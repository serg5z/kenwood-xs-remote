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
#define OBS_HISTORY 64
#define OBS_TAIL_NS 2000000ULL

struct observation_record {
    uint64_t serial, end_ns;
    struct xs8_decoded decoded;
};

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
    pthread_mutex_t observation_lock;
    pthread_cond_t observation_changed;
    struct observation_record observations[OBS_HISTORY];
    uint64_t observation_serial;
    atomic_bool healthy;
    bool threaded;
};

struct decoded_batch {
    struct {
        struct xs8_decoded decoded;
        uint64_t end_ns;
    } results[EVENT_BATCH * 2];
    size_t count;
    uint64_t dropped;
    uint64_t current_event_ns;
};

static void collect_decoded(void *user, const struct xs8_decoded *result)
{
    struct decoded_batch *batch = user;
    if (batch->count < EVENT_BATCH * 2) {
        batch->results[batch->count].decoded = *result;
        batch->results[batch->count].end_ns = batch->current_event_ns;
        ++batch->count;
    } else ++batch->dropped;
}

static void record_observation(struct xs8_monitor *monitor,
                               const struct xs8_decoded *result,
                               uint64_t end_ns)
{
    pthread_mutex_lock(&monitor->observation_lock);
    uint64_t serial = ++monitor->observation_serial;
    monitor->observations[serial % OBS_HISTORY] =
        (struct observation_record){serial, end_ns, *result};
    pthread_cond_broadcast(&monitor->observation_changed);
    pthread_mutex_unlock(&monitor->observation_lock);
}

static void monitor_failed(struct xs8_monitor *monitor)
{
    atomic_store(&monitor->healthy, false);
    pthread_mutex_lock(&monitor->observation_lock);
    pthread_cond_broadcast(&monitor->observation_changed);
    pthread_mutex_unlock(&monitor->observation_lock);
}

static void log_decoded_batch(struct xs8_monitor *monitor,
                              struct decoded_batch *batch)
{
    if (batch->dropped) monitor_failed(monitor);
    for (size_t i = 0; i < batch->count; ++i) {
        const struct xs8_decoded *result = &batch->results[i].decoded;
        record_observation(monitor, result, batch->results[i].end_ns);
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

static int read_events(struct xs8_monitor *monitor)
{
    struct decoded_batch decoded = {0};
    pthread_mutex_lock(&monitor->bus->rx_lock);
    int count = gpiod_line_request_read_edge_events(
        monitor->bus->rx_req, monitor->buffer, EVENT_BATCH);
    int read_error = errno;
    pthread_mutex_unlock(&monitor->bus->rx_lock);
    if (count < 0) {
        errno = read_error;
        return -1;
    }
    if ((unsigned int)count > monitor->max_batch)
        monitor->max_batch = (unsigned int)count;
    bool capture_fault = false;
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
            capture_fault = true;
            ++monitor->sequence_gaps;
            monitor->missing_events +=
                edge.sequence - monitor->last_sequence - 1;
        } else if (edge.sequence <= monitor->last_sequence) {
            capture_fault = true;
            ++monitor->sequence_regressions;
        }
        if (monitor->timestamp_seen &&
            edge.time_ns < monitor->last_timestamp) {
            capture_fault = true;
            ++monitor->timestamp_regressions;
        }
        monitor->last_sequence = edge.sequence;
        monitor->last_timestamp = edge.time_ns;
        monitor->timestamp_seen = true;
        decoded.current_event_ns = edge.time_ns;
        xs8_decoder_edge(&monitor->decoder, &edge,
                         collect_decoded, &decoded);
    }
    log_decoded_batch(monitor, &decoded);
    if (capture_fault) monitor_failed(monitor);
    return 0;
}

int xs8_monitor_poll(struct xs8_monitor *monitor, int timeout_ms)
{
    if (!monitor || (!monitor->threaded && !xs8_monitor_healthy(monitor))) {
        errno = EIO;
        return -1;
    }
    struct pollfd input = {
        .fd = gpiod_line_request_get_fd(monitor->bus->rx_req),
        .events = POLLIN
    };
    int ready = poll(&input, 1, timeout_ms);
    if (ready < 0) {
        if (errno == EINTR) return 0;
        fprintf(stderr, "XS8 OBSERVED_BUS monitor poll failed: %d\n", errno);
        monitor_failed(monitor);
        return -1;
    }
    if (!ready) return 0;
    if (!(input.revents & POLLIN)) {
        fprintf(stderr, "XS8 OBSERVED_BUS monitor fd closed\n");
        monitor_failed(monitor);
        errno = EIO;
        return -1;
    }
    if (read_events(monitor) < 0) {
        fprintf(stderr, "XS8 OBSERVED_BUS event read failed: %d\n", errno);
        monitor_failed(monitor);
        return -1;
    }
    return 0;
}

static void finish_monitor(struct xs8_monitor *monitor)
{
    struct decoded_batch decoded = {0};
    decoded.current_event_ns = monitor->last_timestamp;
    xs8_decoder_flush(&monitor->decoder, collect_decoded, &decoded);
    log_decoded_batch(monitor, &decoded);
    monitor_failed(monitor);
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
}

static void *monitor_events(void *argument)
{
    struct xs8_monitor *monitor = argument;
    unsigned int stop_drains = 0;
    for (;;) {
        bool stopping = atomic_load(&monitor->stop);
        if (stopping && ++stop_drains > 32) {
            fprintf(stderr, "XS8 OBSERVED_BUS monitor drain limit reached\n");
            break;
        }
        if (xs8_monitor_poll(monitor, stopping ? 0 : 20) < 0) break;
        if (stopping) {
            struct pollfd input = {
                .fd = gpiod_line_request_get_fd(monitor->bus->rx_req),
                .events = POLLIN
            };
            if (poll(&input, 1, 0) <= 0) break;
        }
    }
    finish_monitor(monitor);
    return NULL;
}

static int start_monitor(struct xs8_gpio *bus, struct xs8_monitor **out,
                         bool threaded)
{
    if (!bus || !bus->rx_req || !out) { errno = EINVAL; return -1; }
    *out = NULL;
    struct xs8_monitor *monitor = calloc(1, sizeof(*monitor));
    if (!monitor) return -1;
    monitor->bus = bus;
    monitor->threaded = threaded;
    if (pthread_mutex_init(&monitor->observation_lock, NULL) != 0) {
        free(monitor);
        errno = EIO;
        return -1;
    }
    pthread_condattr_t condition_attr;
    if (pthread_condattr_init(&condition_attr) != 0) {
        pthread_mutex_destroy(&monitor->observation_lock);
        free(monitor);
        errno = EIO;
        return -1;
    }
    int condition_error = 0;
#ifndef __APPLE__
    condition_error = pthread_condattr_setclock(&condition_attr,
                                                CLOCK_MONOTONIC);
#endif
    if (!condition_error)
        condition_error = pthread_cond_init(&monitor->observation_changed,
                                            &condition_attr);
    pthread_condattr_destroy(&condition_attr);
    if (condition_error) {
        pthread_mutex_destroy(&monitor->observation_lock);
        free(monitor);
        errno = condition_error;
        return -1;
    }
    monitor->buffer = gpiod_edge_event_buffer_new(EVENT_BATCH);
    if (!monitor->buffer) goto start_fail;
    if (gpiod_line_request_get_fd(bus->rx_req) < 0) {
        errno = EIO;
        goto start_fail;
    }
    pthread_mutex_lock(&bus->rx_lock);
    int busy = gpiod_line_request_get_value(bus->rx_req, BUSY_GPIO);
    pthread_mutex_unlock(&bus->rx_lock);
    if (busy < 0) {
        goto start_fail;
    }
    xs8_decoder_init(&monitor->decoder, busy == 0);
    atomic_init(&monitor->stop, false);
    atomic_init(&monitor->healthy, true);
    int error = 0;
    if (threaded) {
        error = pthread_create(&monitor->thread, NULL, monitor_events, monitor);
        if (error) {
            errno = error;
            goto start_fail;
        }
    }
    *out = monitor;
    return 0;
start_fail:
    error = errno;
    if (monitor->buffer) gpiod_edge_event_buffer_free(monitor->buffer);
    pthread_cond_destroy(&monitor->observation_changed);
    pthread_mutex_destroy(&monitor->observation_lock);
    free(monitor);
    errno = error;
    return -1;
}

int xs8_monitor_start(struct xs8_gpio *bus, struct xs8_monitor **out)
{
    return start_monitor(bus, out, true);
}

int xs8_monitor_start_manual(struct xs8_gpio *bus, struct xs8_monitor **out)
{
    return start_monitor(bus, out, false);
}

void xs8_monitor_stop(struct xs8_monitor *monitor)
{
    if (!monitor) return;
    atomic_store(&monitor->stop, true);
    if (monitor->threaded) pthread_join(monitor->thread, NULL);
    else {
        for (unsigned int i = 0; i < 32; ++i) {
            struct pollfd input = {
                .fd = gpiod_line_request_get_fd(monitor->bus->rx_req),
                .events = POLLIN
            };
            if (poll(&input, 1, 0) <= 0 || !xs8_monitor_healthy(monitor))
                break;
            if (xs8_monitor_poll(monitor, 0) < 0) break;
        }
        finish_monitor(monitor);
    }
    gpiod_edge_event_buffer_free(monitor->buffer);
    pthread_cond_destroy(&monitor->observation_changed);
    pthread_mutex_destroy(&monitor->observation_lock);
    free(monitor);
}

bool xs8_monitor_healthy(struct xs8_monitor *monitor)
{
    return monitor && atomic_load(&monitor->healthy);
}

uint64_t xs8_monitor_checkpoint(struct xs8_monitor *monitor)
{
    if (!monitor) return 0;
    pthread_mutex_lock(&monitor->observation_lock);
    uint64_t serial = monitor->observation_serial;
    pthread_mutex_unlock(&monitor->observation_lock);
    return serial;
}

enum xs8_observation xs8_monitor_verify_tx(struct xs8_monitor *monitor,
    uint64_t checkpoint, uint8_t byte, uint64_t start_ns, uint64_t end_ns,
    unsigned int timeout_ms)
{
    if (!monitor || !xs8_monitor_healthy(monitor)) return XS8_OBS_UNHEALTHY;
    struct timespec deadline;
#ifdef __APPLE__
    const clockid_t wait_clock = CLOCK_REALTIME;
#else
    const clockid_t wait_clock = CLOCK_MONOTONIC;
#endif
    if (clock_gettime(wait_clock, &deadline) < 0)
        return XS8_OBS_UNHEALTHY;
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&monitor->observation_lock);
    uint64_t next = checkpoint + 1;
    for (;;) {
        if (!atomic_load(&monitor->healthy) ||
            monitor->observation_serial - checkpoint >= OBS_HISTORY) {
            pthread_mutex_unlock(&monitor->observation_lock);
            return XS8_OBS_UNHEALTHY;
        }
        while (next <= monitor->observation_serial) {
            struct observation_record record =
                monitor->observations[next % OBS_HISTORY];
            ++next;
            if (record.serial != next - 1 ||
                record.decoded.kind == XS8_EVENT_GAP) {
                pthread_mutex_unlock(&monitor->observation_lock);
                return XS8_OBS_INVALID;
            }
            if (record.decoded.time_ns < start_ns) {
                if (record.end_ns > start_ns) {
                    pthread_mutex_unlock(&monitor->observation_lock);
                    return XS8_OBS_INVALID;
                }
                continue;
            }
            if (record.decoded.time_ns > end_ns) continue;
            if (record.end_ns > end_ns + OBS_TAIL_NS) {
                pthread_mutex_unlock(&monitor->observation_lock);
                return XS8_OBS_INVALID;
            }
            enum xs8_observation result = !record.decoded.valid
                ? XS8_OBS_INVALID
                : record.decoded.waveform_lsb == byte
                    ? XS8_OBS_MATCH : XS8_OBS_MISMATCH;
            pthread_mutex_unlock(&monitor->observation_lock);
            return result;
        }
        if (!monitor->threaded) {
            pthread_mutex_unlock(&monitor->observation_lock);
            struct timespec current;
            if (clock_gettime(wait_clock, &current) < 0)
                return XS8_OBS_UNHEALTHY;
            int64_t remaining_ns =
                (int64_t)(deadline.tv_sec - current.tv_sec) * 1000000000LL +
                deadline.tv_nsec - current.tv_nsec;
            if (remaining_ns <= 0) return XS8_OBS_MISSING;
            int wait_ms = (int)((remaining_ns + 999999) / 1000000);
            if (wait_ms > 20) wait_ms = 20;
            if (xs8_monitor_poll(monitor, wait_ms) < 0)
                return XS8_OBS_UNHEALTHY;
            pthread_mutex_lock(&monitor->observation_lock);
            continue;
        }
        int rc = pthread_cond_timedwait(&monitor->observation_changed,
                                       &monitor->observation_lock, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&monitor->observation_lock);
            return XS8_OBS_MISSING;
        }
        if (rc) {
            pthread_mutex_unlock(&monitor->observation_lock);
            return XS8_OBS_UNHEALTHY;
        }
    }
}
