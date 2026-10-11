#define _POSIX_C_SOURCE 200809L
#include "xs8_monitor.h"
#include "xs8_decode.h"

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define EVENT_BATCH 128
#define BUSY_GPIO 23

struct xs8_monitor {
    struct xs8_gpio *bus;
    struct gpiod_edge_event_buffer *buffer;
    struct xs8_decoder decoder;
    pthread_t thread;
    atomic_bool stop;
};

static void log_decoded(void *user, const struct xs8_decoded *result)
{
    (void)user;
    if (result->kind == XS8_EVENT_GAP) {
        fprintf(stderr, "mono_us=%llu XS8 OBSERVED_BUS event_gap=%llu "
                        "reason=%s\n",
                (unsigned long long)(result->time_ns / 1000),
                (unsigned long long)result->lost_events, result->reason);
    } else if (result->valid) {
        fprintf(stderr, "mono_us=%llu XS8 OBSERVED_BUS source=UNATTRIBUTED "
                        "status=VALID "
                        "waveform_lsb=0x%02X legacy_msb_short1=0x%02X\n",
                (unsigned long long)(result->time_ns / 1000),
                result->waveform_lsb, result->legacy_msb_short1);
    } else {
        fprintf(stderr, "mono_us=%llu XS8 OBSERVED_BUS source=UNATTRIBUTED "
                        "status=INCOMPLETE "
                        "bits=%u reason=%s\n",
                (unsigned long long)(result->time_ns / 1000),
                result->bits, result->reason);
    }
}

static void *monitor_events(void *argument)
{
    struct xs8_monitor *monitor = argument;
    int fd = gpiod_line_request_get_fd(monitor->bus->rx_req);
    unsigned int stop_drains = 0;
    for (;;) {
        bool stopping = atomic_load(&monitor->stop);
        if (stopping && ++stop_drains > 32) {
            fprintf(stderr, "XS8 OBSERVED_BUS monitor drain limit reached\n");
            break;
        }
        struct pollfd input = {.fd = fd, .events = POLLIN};
        int ready = poll(&input, 1, stopping ? 0 : 20);
        if (ready < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "XS8 OBSERVED_BUS monitor poll failed: %d\n", errno);
            break;
        }
        if (!ready) {
            if (stopping) break;
            continue;
        }
        if (!(input.revents & POLLIN)) {
            fprintf(stderr, "XS8 OBSERVED_BUS monitor fd closed\n");
            break;
        }
        pthread_mutex_lock(&monitor->bus->rx_lock);
        int count = gpiod_line_request_read_edge_events(
            monitor->bus->rx_req, monitor->buffer, EVENT_BATCH);
        int read_error = errno;
        pthread_mutex_unlock(&monitor->bus->rx_lock);
        if (count < 0) {
            if (read_error == EINTR) continue;
            fprintf(stderr, "XS8 OBSERVED_BUS event read failed: %d\n",
                    read_error);
            break;
        }
        for (int i = 0; i < count; ++i) {
            struct gpiod_edge_event *event =
                gpiod_edge_event_buffer_get_event(monitor->buffer, i);
            struct xs8_edge edge = {
                .time_ns = gpiod_edge_event_get_timestamp_ns(event),
                .sequence = gpiod_edge_event_get_global_seqno(event),
                .gpio = gpiod_edge_event_get_line_offset(event),
                .kind = gpiod_edge_event_get_event_type(event) ==
                        GPIOD_EDGE_EVENT_RISING_EDGE ? XS8_RISING : XS8_FALLING
            };
            xs8_decoder_edge(&monitor->decoder, &edge, log_decoded, NULL);
        }
    }
    xs8_decoder_flush(&monitor->decoder, log_decoded, NULL);
    return NULL;
}

int xs8_monitor_start(struct xs8_gpio *bus, struct xs8_monitor **out)
{
    if (!bus || !bus->rx_req || !out) { errno = EINVAL; return -1; }
    *out = NULL;
    struct xs8_monitor *monitor = calloc(1, sizeof(*monitor));
    if (!monitor) return -1;
    monitor->bus = bus;
    monitor->buffer = gpiod_edge_event_buffer_new(EVENT_BATCH);
    if (!monitor->buffer) { free(monitor); return -1; }
    if (gpiod_line_request_get_fd(bus->rx_req) < 0) {
        gpiod_edge_event_buffer_free(monitor->buffer);
        free(monitor);
        errno = EIO;
        return -1;
    }
    pthread_mutex_lock(&bus->rx_lock);
    int busy = gpiod_line_request_get_value(bus->rx_req, BUSY_GPIO);
    pthread_mutex_unlock(&bus->rx_lock);
    if (busy < 0) {
        gpiod_edge_event_buffer_free(monitor->buffer);
        free(monitor);
        return -1;
    }
    xs8_decoder_init(&monitor->decoder, busy == 0);
    atomic_init(&monitor->stop, false);
    int error = pthread_create(&monitor->thread, NULL, monitor_events, monitor);
    if (error) {
        gpiod_edge_event_buffer_free(monitor->buffer);
        free(monitor);
        errno = error;
        return -1;
    }
    *out = monitor;
    return 0;
}

void xs8_monitor_stop(struct xs8_monitor *monitor)
{
    if (!monitor) return;
    atomic_store(&monitor->stop, true);
    pthread_join(monitor->thread, NULL);
    gpiod_edge_event_buffer_free(monitor->buffer);
    free(monitor);
}
