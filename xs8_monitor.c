#define _POSIX_C_SOURCE 200809L
#include "xs8_monitor.h"
#include "xs8_decode.h"

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVENT_BATCH 128
#define BUSY_GPIO 23
#define DATA_GPIO 27

struct xs8_monitor {
    struct xs8_gpio *bus;
    struct gpiod_edge_event_buffer *buffer;
    struct xs8_decoder decoder;
    pthread_t thread;
    atomic_bool stop;
    uint64_t total_events, busy_edges, data_edges;
    uint64_t sequence_gaps, missing_events, sequence_regressions;
    uint64_t timestamp_regressions, last_sequence, last_timestamp;
    uint64_t incomplete_gap_frames, dropped_frame_logs;
    unsigned int max_batch;
    bool timestamp_seen;
};

struct decoded_batch {
    struct xs8_decoded results[EVENT_BATCH * 2];
    size_t count;
    uint64_t dropped;
};

static void collect_decoded(void *user, const struct xs8_decoded *result)
{
    struct decoded_batch *batch = user;
    if (batch->count < EVENT_BATCH * 2)
        batch->results[batch->count++] = *result;
    else ++batch->dropped;
}

static void log_decoded_batch(struct xs8_monitor *monitor,
                              struct decoded_batch *batch)
{
    for (size_t i = 0; i < batch->count; ++i) {
        const struct xs8_decoded *result = &batch->results[i];
        if (result->kind == XS8_EVENT_GAP) continue;
        if (!result->valid && strcmp(result->reason, "EVENT_GAP") == 0) {
            ++monitor->incomplete_gap_frames;
            continue;
        }
        if (result->valid) {
            fprintf(stderr, "mono_us=%llu XS8 OBSERVED_BUS source=UNATTRIBUTED "
                            "status=VALID waveform_lsb=0x%02X "
                            "legacy_msb_short1=0x%02X\n",
                    (unsigned long long)(result->time_ns / 1000),
                    result->waveform_lsb, result->legacy_msb_short1);
        } else {
            fprintf(stderr, "mono_us=%llu XS8 OBSERVED_BUS source=UNATTRIBUTED "
                            "status=INCOMPLETE bits=%u reason=%s\n",
                    (unsigned long long)(result->time_ns / 1000),
                    result->bits, result->reason);
        }
    }
    monitor->dropped_frame_logs += batch->dropped;
    batch->count = 0;
    batch->dropped = 0;
}

static void *monitor_events(void *argument)
{
    struct xs8_monitor *monitor = argument;
    int fd = gpiod_line_request_get_fd(monitor->bus->rx_req);
    unsigned int stop_drains = 0;
    struct decoded_batch decoded = {0};
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
        if ((unsigned int)count > monitor->max_batch)
            monitor->max_batch = (unsigned int)count;
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
            ++monitor->total_events;
            if (edge.gpio == BUSY_GPIO) ++monitor->busy_edges;
            else if (edge.gpio == DATA_GPIO) ++monitor->data_edges;
            if (edge.sequence > monitor->last_sequence + 1) {
                ++monitor->sequence_gaps;
                monitor->missing_events +=
                    edge.sequence - monitor->last_sequence - 1;
            } else if (edge.sequence <= monitor->last_sequence) {
                ++monitor->sequence_regressions;
            }
            if (monitor->timestamp_seen &&
                edge.time_ns < monitor->last_timestamp)
                ++monitor->timestamp_regressions;
            monitor->last_sequence = edge.sequence;
            monitor->last_timestamp = edge.time_ns;
            monitor->timestamp_seen = true;
            xs8_decoder_edge(&monitor->decoder, &edge,
                             collect_decoded, &decoded);
        }
        log_decoded_batch(monitor, &decoded);
    }
    xs8_decoder_flush(&monitor->decoder, collect_decoded, &decoded);
    log_decoded_batch(monitor, &decoded);
    fprintf(stderr, "XS8 OBSERVED_BUS summary total_events=%llu "
                    "busy_edges=%llu data_edges=%llu global_sequence_gaps=%llu "
                    "missing_events=%llu sequence_regressions=%llu "
                    "timestamp_regressions=%llu max_batch=%u "
                    "incomplete_gap_frames=%llu dropped_frame_logs=%llu\n",
            (unsigned long long)monitor->total_events,
            (unsigned long long)monitor->busy_edges,
            (unsigned long long)monitor->data_edges,
            (unsigned long long)monitor->sequence_gaps,
            (unsigned long long)monitor->missing_events,
            (unsigned long long)monitor->sequence_regressions,
            (unsigned long long)monitor->timestamp_regressions,
            monitor->max_batch,
            (unsigned long long)monitor->incomplete_gap_frames,
            (unsigned long long)monitor->dropped_frame_logs);
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
