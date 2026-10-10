#define _POSIX_C_SOURCE 200809L
#include <gpiod.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHIP "/dev/gpiochip0"
#define DATA_TX 4
#define BUSY_TX 22
#define BUSY_RX 23

#define LEAD_US 1150
#define START_US 9800
#define ONE_US 4900
#define ZERO_US 9800
#define HIGH_US 4900
#define BUS_FREE_TIMEOUT_US 100000
#define BUS_FREE_GUARD_US 500
#define END_GUARD_US 750

static struct gpiod_line_request *req;

static int64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static int wait_until(int64_t deadline)
{
    struct timespec t = {
        .tv_sec = deadline / 1000000000LL,
        .tv_nsec = deadline % 1000000000LL
    };
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC,
                             TIMER_ABSTIME, &t, NULL);
    } while (rc == EINTR);
    return rc;
}

static int tx(unsigned int pin, int high)
{
    return gpiod_line_request_set_value(
        req, pin, high ? GPIOD_LINE_VALUE_ACTIVE
                       : GPIOD_LINE_VALUE_INACTIVE);
}

static int wait_bus_free(void)
{
    int64_t start = now_ns();
    int64_t free_since = 0;

    while (now_ns() - start < BUS_FREE_TIMEOUT_US * 1000LL) {
        int busy = gpiod_line_request_get_value(req, BUSY_RX);
        if (busy < 0) return -1;

        int64_t now = now_ns();
        if (busy) {
            free_since = 0;
        } else {
            if (!free_since) free_since = now;
            if (now - free_since >= BUS_FREE_GUARD_US * 1000LL)
                return 0;
        }
    }
    return -1;
}

static int send_xs(uint8_t value)
{
    int64_t deadline;

    if (tx(DATA_TX, 0) < 0 || tx(BUSY_TX, 0) < 0)
        return -1;

    if (wait_bus_free() < 0) {
        fprintf(stderr, "BUSY timeout\n");
        return -1;
    }

    deadline = now_ns();

#define STEP(pin, level, duration) do {                         \
    if (tx((pin), (level)) < 0) return -1;                       \
    deadline += (int64_t)(duration) * 1000LL;                    \
    if (wait_until(deadline) != 0) return -1;                    \
} while (0)

    STEP(BUSY_TX, 1, LEAD_US);
    STEP(DATA_TX, 1, START_US);

    for (int i = 0; i < 8; ++i) {
        STEP(DATA_TX, 0,
             (value & (1u << i)) ? ZERO_US : ONE_US);
        STEP(DATA_TX, 1, HIGH_US);
    }

    STEP(DATA_TX, 0, END_GUARD_US);

    if (tx(BUSY_TX, 0) < 0)
        return -1;

#undef STEP
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s 0xNN\n", argv[0]);
        return 2;
    }

    char *end;
    unsigned long parsed = strtoul(argv[1], &end, 0);
    if (*end || parsed > 255) {
        fprintf(stderr, "Invalid byte\n");
        return 2;
    }

    struct gpiod_chip *chip = gpiod_chip_open(CHIP);
    struct gpiod_line_settings *out = gpiod_line_settings_new();
    struct gpiod_line_settings *in = gpiod_line_settings_new();
    struct gpiod_line_config *cfg = gpiod_line_config_new();

    if (!chip || !out || !in || !cfg) {
        fprintf(stderr, "GPIO setup failed\n");
        return 1;
    }

    unsigned int outputs[] = {DATA_TX, BUSY_TX};
    unsigned int inputs[] = {BUSY_RX};

    gpiod_line_settings_set_direction(
        out, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(
        out, GPIOD_LINE_VALUE_INACTIVE);
    gpiod_line_settings_set_direction(
        in, GPIOD_LINE_DIRECTION_INPUT);

    if (gpiod_line_config_add_line_settings(cfg, outputs, 2, out) < 0 ||
        gpiod_line_config_add_line_settings(cfg, inputs, 1, in) < 0) {
        fprintf(stderr, "Line configuration failed\n");
        return 1;
    }

    req = gpiod_chip_request_lines(chip, NULL, cfg);
    if (!req) {
        perror("GPIO request");
        return 1;
    }

    printf("XS8 TX: 0x%02lX\n", parsed);
    fflush(stdout);

    int result = send_xs((uint8_t)parsed);

    /* Always release both transistor outputs. */
    if (tx(DATA_TX, 0) < 0 || tx(BUSY_TX, 0) < 0)
        result = -1;

    gpiod_line_request_release(req);
    gpiod_line_config_free(cfg);
    gpiod_line_settings_free(out);
    gpiod_line_settings_free(in);
    gpiod_chip_close(chip);

    if (result < 0) {
        fprintf(stderr, "XS8 transmission failed\n");
        return 1;
    }

    puts("XS8 transmission complete");
    return 0;
}
