#define _POSIX_C_SOURCE 200809L
#include "xs8_gpio.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#define CHIP "/dev/gpiochip0"
#define DATA_TX 4
#define BUSY_TX 22
#define BUSY_RX 23
#define DATA_RX 27

/* Values retained from the validated standalone transmitter. */
#define LEAD_US 1150
#define START_US 9800
#define ONE_US 4900
#define ZERO_US 9800
#define HIGH_US 4900
#define BUS_FREE_TIMEOUT_US 100000
#define BUS_FREE_GUARD_US 500
#define END_GUARD_US 750

int64_t xs8_gpio_now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return -1;
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

int xs8_gpio_wait_until(int64_t deadline_ns)
{
    struct timespec t = {
        .tv_sec = deadline_ns / 1000000000LL,
        .tv_nsec = deadline_ns % 1000000000LL
    };
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
    } while (rc == EINTR);
    return rc;
}

static int tx(struct xs8_gpio *bus, unsigned int pin, int high)
{
    return gpiod_line_request_set_value(
        bus->tx_req, pin, high ? GPIOD_LINE_VALUE_ACTIVE
                               : GPIOD_LINE_VALUE_INACTIVE);
}

int xs8_gpio_release(struct xs8_gpio *bus)
{
    if (!bus || !bus->tx_req) return 0;
    int data = tx(bus, DATA_TX, 0);
    int busy = tx(bus, BUSY_TX, 0);
    return data < 0 || busy < 0 ? -1 : 0;
}

void xs8_gpio_close(struct xs8_gpio *bus)
{
    if (!bus) return;
    xs8_gpio_release(bus);
    if (bus->rx_req) gpiod_line_request_release(bus->rx_req);
    if (bus->tx_req) gpiod_line_request_release(bus->tx_req);
    if (bus->chip) gpiod_chip_close(bus->chip);
    if (bus->lock_ready) pthread_mutex_destroy(&bus->rx_lock);
    memset(bus, 0, sizeof(*bus));
}

int xs8_gpio_open(struct xs8_gpio *bus, bool capture)
{
    if (!bus) { errno = EINVAL; return -1; }
    memset(bus, 0, sizeof(*bus));
    struct gpiod_line_settings *out = NULL, *in = NULL;
    struct gpiod_line_config *tx_cfg = NULL, *rx_cfg = NULL;
    struct gpiod_request_config *rx_req_cfg = NULL;
    int saved_error = 0;

    int lock_error = pthread_mutex_init(&bus->rx_lock, NULL);
    if (lock_error) { errno = lock_error; return -1; }
    bus->lock_ready = true;
    bus->chip = gpiod_chip_open(CHIP);
    out = gpiod_line_settings_new();
    in = gpiod_line_settings_new();
    tx_cfg = gpiod_line_config_new();
    rx_cfg = gpiod_line_config_new();
    if (capture) rx_req_cfg = gpiod_request_config_new();
    if (!bus->chip || !out || !in || !tx_cfg || !rx_cfg ||
        (capture && !rx_req_cfg)) goto fail;

    unsigned int outputs[] = {DATA_TX, BUSY_TX};
    unsigned int inputs[] = {BUSY_RX, DATA_RX};
    if (gpiod_line_settings_set_direction(out, GPIOD_LINE_DIRECTION_OUTPUT) < 0 ||
        gpiod_line_settings_set_output_value(out, GPIOD_LINE_VALUE_INACTIVE) < 0 ||
        gpiod_line_settings_set_direction(in, GPIOD_LINE_DIRECTION_INPUT) < 0)
        goto fail;
    if (capture &&
        (gpiod_line_settings_set_edge_detection(in, GPIOD_LINE_EDGE_BOTH) < 0 ||
         gpiod_line_settings_set_event_clock(in, GPIOD_LINE_CLOCK_MONOTONIC) < 0))
        goto fail;
    if (gpiod_line_config_add_line_settings(tx_cfg, outputs, 2, out) < 0 ||
        gpiod_line_config_add_line_settings(rx_cfg, inputs,
                                            capture ? 2 : 1, in) < 0)
        goto fail;
    if (capture)
        gpiod_request_config_set_event_buffer_size(rx_req_cfg, 4096);
    bus->tx_req = gpiod_chip_request_lines(bus->chip, NULL, tx_cfg);
    if (!bus->tx_req) goto fail;
    bus->rx_req = gpiod_chip_request_lines(bus->chip, rx_req_cfg, rx_cfg);
    if (!bus->rx_req) goto fail;

    if (rx_req_cfg) gpiod_request_config_free(rx_req_cfg);
    gpiod_line_config_free(rx_cfg);
    gpiod_line_config_free(tx_cfg);
    gpiod_line_settings_free(in);
    gpiod_line_settings_free(out);
    return 0;

fail:
    saved_error = errno ? errno : EIO;
    if (rx_req_cfg) gpiod_request_config_free(rx_req_cfg);
    if (rx_cfg) gpiod_line_config_free(rx_cfg);
    if (tx_cfg) gpiod_line_config_free(tx_cfg);
    if (in) gpiod_line_settings_free(in);
    if (out) gpiod_line_settings_free(out);
    xs8_gpio_close(bus);
    errno = saved_error;
    return -1;
}

static int wait_bus_free(struct xs8_gpio *bus)
{
    int64_t start = xs8_gpio_now_ns();
    if (start < 0) return -1;
    int64_t free_since = 0;
    while (xs8_gpio_now_ns() - start < BUS_FREE_TIMEOUT_US * 1000LL) {
        pthread_mutex_lock(&bus->rx_lock);
        int busy = gpiod_line_request_get_value(bus->rx_req, BUSY_RX);
        pthread_mutex_unlock(&bus->rx_lock);
        if (busy < 0) return -1;
        int64_t now = xs8_gpio_now_ns();
        if (now < 0) return -1;
        if (busy) free_since = 0;
        else {
            if (!free_since) free_since = now;
            if (now - free_since >= BUS_FREE_GUARD_US * 1000LL) return 0;
        }
    }
    errno = ETIMEDOUT;
    return -1;
}

int xs8_gpio_send(struct xs8_gpio *bus, uint8_t value, int64_t *tx_start_ns)
{
    if (!bus || !bus->tx_req || !bus->rx_req) { errno = EINVAL; return -1; }
    if (tx_start_ns) *tx_start_ns = 0;
    if (xs8_gpio_release(bus) < 0 || wait_bus_free(bus) < 0) goto fail;
    int64_t deadline = xs8_gpio_now_ns();
    if (deadline < 0) goto fail;
    if (tx_start_ns) *tx_start_ns = deadline;

#define STEP(pin, level, duration) do {                         \
    if (tx(bus, (pin), (level)) < 0) goto fail;                   \
    deadline += (int64_t)(duration) * 1000LL;                    \
    int wait_error = xs8_gpio_wait_until(deadline);              \
    if (wait_error) { errno = wait_error; goto fail; }            \
} while (0)

    STEP(BUSY_TX, 1, LEAD_US);
    STEP(DATA_TX, 1, START_US);
    for (int i = 0; i < 8; ++i) {
        STEP(DATA_TX, 0, (value & (1u << i)) ? ZERO_US : ONE_US);
        STEP(DATA_TX, 1, HIGH_US);
    }
    STEP(DATA_TX, 0, END_GUARD_US);
    if (tx(bus, BUSY_TX, 0) < 0) goto fail;
#undef STEP
    return 0;

fail:
    {
        int saved_error = errno ? errno : EIO;
        xs8_gpio_release(bus);
        errno = saved_error;
    }
    return -1;
}
