/* Synthetic edge source for the REAL service/monitor/decoder/verifier. No GPIO
 * driver is linked. Virtual monotonic TX completion avoids hardware or sleeps. */
#define _POSIX_C_SOURCE 200809L
#include "xs8_gpio.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DATA 13
#define BUSY 26
struct gpiod_edge_event {
    uint64_t timestamp, global, line;
    unsigned int gpio;
    enum gpiod_edge_event_type edge;
};
struct gpiod_edge_event_buffer { struct gpiod_edge_event events[128]; size_t count; };
static struct gpiod_edge_event pending[80];
static size_t queued, consumed;
static int fds[2];
static uint64_t global, lines[2], floor_ns;
static int64_t clock_advance;
static unsigned int attempts;
static char request;

int64_t xs8_gpio_now_ns(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec + clock_advance;
}
int xs8_gpio_open(struct xs8_gpio *bus, bool capture)
{
    assert(capture);
    assert(pipe(fds) == 0);
    memset(bus, 0, sizeof(*bus));
    bus->tx_req = bus->rx_req = (struct gpiod_line_request *)&request;
    bus->lock_ready = true;
    return pthread_mutex_init(&bus->rx_lock, NULL);
}
int xs8_gpio_release(struct xs8_gpio *bus) { (void)bus; return 0; }
void xs8_gpio_close(struct xs8_gpio *bus)
{
    close(fds[0]); close(fds[1]); pthread_mutex_destroy(&bus->rx_lock);
}
int xs8_gpio_wait_until(int64_t deadline) { (void)deadline; return 0; }
enum gpiod_line_value gpiod_line_request_get_value(struct gpiod_line_request *r, unsigned int gpio)
{ (void)r; (void)gpio; return GPIOD_LINE_VALUE_INACTIVE; }
int gpiod_line_request_get_fd(struct gpiod_line_request *r) { (void)r; return fds[0]; }
struct gpiod_edge_event_buffer *gpiod_edge_event_buffer_new(size_t n)
{ assert(n <= 128); return calloc(1, sizeof(struct gpiod_edge_event_buffer)); }
void gpiod_edge_event_buffer_free(struct gpiod_edge_event_buffer *b) { free(b); }
struct gpiod_edge_event *gpiod_edge_event_buffer_get_event(struct gpiod_edge_event_buffer *b, unsigned long i)
{ assert(i < b->count); return &b->events[i]; }
enum gpiod_edge_event_type gpiod_edge_event_get_event_type(struct gpiod_edge_event *e) { return e->edge; }
uint64_t gpiod_edge_event_get_timestamp_ns(struct gpiod_edge_event *e) { return e->timestamp; }
unsigned int gpiod_edge_event_get_line_offset(struct gpiod_edge_event *e) { return e->gpio; }
unsigned long gpiod_edge_event_get_global_seqno(struct gpiod_edge_event *e) { return e->global; }
unsigned long gpiod_edge_event_get_line_seqno(struct gpiod_edge_event *e) { return e->line; }
int gpiod_line_request_read_edge_events(struct gpiod_line_request *r,
    struct gpiod_edge_event_buffer *b, size_t max)
{
    (void)r;
    char token;
    assert(read(fds[0], &token, 1) == 1);
    assert(consumed < queued);
    /* One frame per read: verification must inspect the next ready batch. */
    size_t n = queued - consumed;
    if (n > 20) n = 20;
    assert(n <= max);
    memcpy(b->events, pending + consumed, n * sizeof(pending[0]));
    b->count = n; consumed += n;
    return (int)n;
}
static void append(uint64_t t, unsigned int gpio, bool high)
{
    assert(queued < 80);
    pending[queued++] = (struct gpiod_edge_event){.timestamp = t, .gpio = gpio,
        .edge = high ? GPIOD_EDGE_EVENT_RISING_EDGE : GPIOD_EDGE_EVENT_FALLING_EDGE};
}
static uint64_t frame(uint64_t t, uint8_t byte)
{
    append(t, BUSY, true); t += 1150000;
    append(t, DATA, true); t += 9800000;
    append(t, DATA, false);
    for (unsigned int bit = 0; bit < 8; ++bit) {
        t += byte & (1u << bit) ? 9800000 : 4900000;
        append(t, DATA, true); t += 4900000; append(t, DATA, false);
    }
    t += 750000; append(t, BUSY, false);
    return t;
}
int xs8_gpio_send(struct xs8_gpio *bus, uint8_t byte, int64_t *start)
{
    (void)bus;
    assert(consumed == queued);
    queued = consumed = 0;
    const char *mode = getenv("XS8_MONITOR_TEST_TRACE");
    if (!mode) mode = "valid";
    if (++attempts > 1 && !strcmp(mode, "warn_once")) mode = "valid";
    uint64_t now = (uint64_t)xs8_gpio_now_ns();
    if (now < floor_ns) { clock_advance += floor_ns - now; now = floor_ns; }
    *start = (int64_t)now;
    uint64_t end = frame(now, byte);
    bool outside = strstr(mode, "outside") != NULL;
    if (strstr(mode, "two") || strstr(mode, "malformed") || outside)
        end = frame(end + 3000000, outside ? 0x41 : byte);
    if (strstr(mode, "partial")) {
        if (outside) queued = 20;
        append(pending[19].timestamp + 3000000, BUSY, true);
        end = pending[queued - 1].timestamp;
    }
    if (strstr(mode, "malformed")) pending[38].timestamp = pending[37].timestamp + 1000000;
    if (strstr(mode, "missing_tail")) { pending[18] = pending[19]; --queued; }
    uint64_t completion = outside ? pending[19].timestamp + 1 : end + 1;
    for (size_t i = 0; i < queued; ++i) {
        if ((!strncmp(mode, "warn_", 5) && i == 2) ||
            (strstr(mode, "outside_warning") && i == 22)) global += 2;
        unsigned int line = pending[i].gpio == BUSY ? 0 : 1;
        if (strstr(mode, "line_fault") && i == 18) ++lines[line];
        pending[i].global = ++global;
        pending[i].line = ++lines[line];
    }
    floor_ns = pending[queued - 1].timestamp + 1000000;
    clock_advance += completion - now;
    for (size_t i = 0; i < (queued + 19) / 20; ++i) assert(write(fds[1], "x", 1) == 1);
    fprintf(stderr, "FAKE_TX 0x%02X trace=%s\n", byte, mode);
    return 0;
}
