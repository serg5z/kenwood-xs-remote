#define _POSIX_C_SOURCE 200809L
#include "xs8_monitor.h"
#include "xs8_gpio.h"
#include "xs8_decode.h"
#include "xs8_monitor_fault.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVENT_BATCH 128
#define BUSY_GPIO 26
#define DATA_GPIO 13
#define OBS_HISTORY 64
#define OBS_TAIL_NS 2000000ULL

/* Opt-in, fixed storage only. No formatting, allocation or output on failure.
 * Full retained delivery history includes pre/post TX context, not sorted edges. */
#define FAILURE_RAW_EVENTS 2048
#define FAILURE_SNAPSHOTS 16
struct failure_edge {
    uint64_t serial, timestamp_ns, global, line, batch;
    unsigned int gpio, index, batch_size, stage, bits;
    bool rising, active, tail_fell;
    int64_t interval_ns;
    const char *error, *emitted;
};
struct failure_snapshot {
    uint64_t id, checkpoint, start_ns, end_ns, decision_event;
    uint8_t byte;
    const char *rejection;
};
struct failure_history {
    struct failure_edge edges[FAILURE_RAW_EVENTS];
    struct failure_snapshot failures[FAILURE_SNAPSHOTS];
    uint64_t events, batches, failures_seen;
    bool initial_busy_low;
};

struct xs8_monitor {
    struct xs8_gpio *bus;
    struct gpiod_edge_event_buffer *buffer;
    struct xs8_decoder decoder;
    pthread_t thread;
    atomic_bool stop;
    uint64_t total_events, busy_edges, data_edges;
    struct xs8_capture_state capture;
    uint64_t incomplete_gap_frames, dropped_frame_logs;
    unsigned int max_batch;
    pthread_mutex_t observation_lock;
    pthread_mutex_t processing_lock; /* batch, decoder and diagnostic snapshot */
    pthread_cond_t observation_changed;
    struct xs8_monitor_record observations[OBS_HISTORY];
    uint64_t observation_serial;
    atomic_bool healthy;
    bool threaded;
    struct failure_history *failure_history;
    bool warn_global_gaps;
    uint64_t last_line_sequence[2]; /* BUSY, DATA; independent of global seqno */
    uint64_t line_gaps, line_missing, line_regressions, global_gap_warnings;
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

/* Called under processing_lock (and, in verify, observation_lock).
 * A descriptor references the ring: even repeated STOP failures do not copy
 * arrays or perform I/O. Overwrites are explicitly reported at shutdown. */
static void remember_failure(struct xs8_monitor *m, uint64_t checkpoint,
    uint8_t byte, uint64_t start, uint64_t end, const char *rejection)
{
    struct failure_history *h = m->failure_history;
    if (!h) return;
    uint64_t id = ++h->failures_seen;
    h->failures[(id - 1) % FAILURE_SNAPSHOTS] =
        (struct failure_snapshot){id, checkpoint, start, end, h->events, byte, rejection};
}

/* Only called after the reader has stopped, never by TX verification/cleanup. */
static void dump_failures(struct xs8_monitor *m)
{
    struct failure_history *h = m->failure_history;
    if (!h || !h->failures_seen) return;
    uint64_t first = h->events > FAILURE_RAW_EVENTS ? h->events - FAILURE_RAW_EVENTS + 1 : 1;
    uint64_t first_failure = h->failures_seen > FAILURE_SNAPSHOTS
        ? h->failures_seen - FAILURE_SNAPSHOTS + 1 : 1;
    fprintf(stderr, "XS8_FAILURE_STORAGE bytes=%zu events=%" PRIu64
            " overwritten_events=%" PRIu64 " overwritten_snapshots=%" PRIu64 "\n",
            sizeof(*h), h->events, first - 1, first_failure - 1);
    for (uint64_t id = first_failure; id <= h->failures_seen; ++id) {
        const struct failure_snapshot *f = &h->failures[(id - 1) % FAILURE_SNAPSHOTS];
        fprintf(stderr, "XS8_FAILURE_BEGIN id=%" PRIu64 " byte=0x%02X start_ns=%" PRIu64
                " end_ns=%" PRIu64 " checkpoint=%" PRIu64 " decision_event=%" PRIu64
                " rejection=%s raw_first=%" PRIu64 " raw_last=%" PRIu64
                " initial_busy_low=%u policy=%s post_margin_reached=%u\n",
                id, f->byte, f->start_ns, f->end_ns,
                f->checkpoint, f->decision_event, f->rejection, first, h->events,
                h->initial_busy_low, m->warn_global_gaps ? "warn" : "strict",
                h->events && h->edges[(h->events - 1) % FAILURE_RAW_EVENTS].timestamp_ns
                    >= f->end_ns &&
                h->edges[(h->events - 1) % FAILURE_RAW_EVENTS].timestamp_ns - f->end_ns >= 5000000);
        fprintf(stderr, "XS8_RAW_HEADER serial,timestamp_ns,gpio,edge,global_seqno,line_seqno,"
                "batch,batch_index,batch_size,stage,bits,active,interval_ns,"
                "expected_min_ns,expected_max_ns,error,emitted_reason,tail_fell\n");
        for (uint64_t serial = first; serial <= h->events; ++serial) {
            const struct failure_edge *e = &h->edges[(serial - 1) % FAILURE_RAW_EVENTS];
            /* Retain every event through the decision, plus available post-TX
             * context (5 ms). Never wait for future edges to fill a margin. */
            if (serial > f->decision_event && e->timestamp_ns > f->end_ns &&
                e->timestamp_ns - f->end_ns > 5000000) continue;
            uint64_t low = 0, high = 0;
            if (e->active && e->gpio == DATA_GPIO) {
                if (e->rising && e->stage == 0) { low = 300000; high = 5000000; }
                else if (e->rising && e->stage == 2) { low = 3000000; high = 12000000; }
                else if (!e->rising && e->stage == 1) { low = 7000000; high = 13000000; }
                else if (!e->rising && (e->stage == 3 || e->stage == 4)) {
                    low = 3000000; high = 7000000;
                }
            } else if (e->active && e->gpio == BUSY_GPIO && !e->rising &&
                       e->stage == 4 && e->tail_fell) {
                low = 1; high = OBS_TAIL_NS; /* strict local-TX guard, not passive validity */
            }
            fprintf(stderr, "XS8_RAW,%" PRIu64 ",%" PRIu64 ",%u,%s,%" PRIu64
                    ",%" PRIu64 ",%" PRIu64 ",%u,%u,%u,%u,%u,%" PRId64
                    ",%" PRIu64 ",%" PRIu64 ",%s,%s,%u\n", e->serial,
                    e->timestamp_ns, e->gpio, e->rising ? "rising" : "falling",
                    e->global, e->line, e->batch, e->index, e->batch_size,
                    e->stage, e->bits, e->active, e->interval_ns, low, high,
                    e->error ? e->error : "NONE", e->emitted ? e->emitted : "NONE", e->tail_fell);
        }
        fprintf(stderr, "XS8_FAILURE_END id=%" PRIu64 "\n", id);
    }
}

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
        (struct xs8_monitor_record){serial, end_ns, *result};
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

/* Reports the first capture fault once, outside the per-edge loop. */
static void log_capture_fault(struct xs8_monitor *monitor)
{
    if (!monitor->capture.fault_recorded || monitor->capture.fault_logged)
        return;
    monitor->capture.fault_logged = true;
    char line[384];
    if (xs8_capture_format_fault(&monitor->capture, BUSY_GPIO, DATA_GPIO,
                                 line, sizeof(line)) > 0)
        fprintf(stderr, "%s\n", line);
}

static void log_decoded_batch(struct xs8_monitor *monitor,
                              struct decoded_batch *batch)
{
    if (batch->dropped) {
        xs8_capture_note_overflow(&monitor->capture, batch->dropped,
                                  batch->current_event_ns);
        monitor_failed(monitor);
    }
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
    struct {
        uint64_t expected, actual, line_sequence, timestamp_ns, previous_ns;
        uint64_t event_index, frame_start_ns;
        unsigned int gpio, bits, stage;
    } warnings[EVENT_BATCH];
    size_t warning_count = 0;
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
    if (monitor->failure_history) ++monitor->failure_history->batches;
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
        uint64_t line_sequence = gpiod_edge_event_get_line_seqno(event);
        int line = edge.gpio == BUSY_GPIO ? 0 : edge.gpio == DATA_GPIO ? 1 : -1;
        uint64_t expected_line = line < 0 ? 0 : monitor->last_line_sequence[line] + 1;
        unsigned int line_fault = 0;
        if (line < 0) line_fault = XS8_CAPTURE_UNEXPECTED_LINE;
        else {
            if (line_sequence > expected_line) {
                line_fault = XS8_CAPTURE_LINE_SEQUENCE_GAP;
                ++monitor->line_gaps;
                monitor->line_missing += line_sequence - expected_line;
            } else if (line_sequence < expected_line) {
                line_fault = XS8_CAPTURE_LINE_SEQUENCE_REGRESSION;
                ++monitor->line_regressions;
            }
            monitor->last_line_sequence[line] = line_sequence;
        }
        const uint64_t expected_global = monitor->capture.last_sequence + 1;
        const uint64_t previous_ns = monitor->capture.last_timestamp;
        unsigned int reasons = xs8_capture_check_policy(&monitor->capture,
            edge.gpio, edge.sequence, line_sequence, edge.time_ns,
            monitor->total_events, monitor->warn_global_gaps && !line_fault);
        if (line_fault) {
            if (!monitor->capture.fault_recorded) {
                monitor->capture.fault_recorded = true;
                monitor->capture.first_fault = (struct xs8_capture_fault){
                    .has_edge = true, .gpio = edge.gpio,
                    .expected_sequence = expected_global,
                    .actual_sequence = edge.sequence, .line_sequence = line_sequence,
                    .previous_timestamp_ns = previous_ns, .timestamp_ns = edge.time_ns,
                    .event_index = monitor->total_events
                };
            }
            if (monitor->capture.first_fault.event_index == monitor->total_events) {
                monitor->capture.first_fault.reasons |= line_fault;
                monitor->capture.first_fault.expected_line_sequence = expected_line;
            }
        }
        if (monitor->warn_global_gaps && !line_fault &&
            reasons == XS8_CAPTURE_SEQUENCE_GAP) {
            ++monitor->global_gap_warnings;
            warnings[warning_count].expected = expected_global;
            warnings[warning_count].actual = edge.sequence;
            warnings[warning_count].line_sequence = line_sequence;
            warnings[warning_count].timestamp_ns = edge.time_ns;
            warnings[warning_count].previous_ns = previous_ns;
            warnings[warning_count].event_index = monitor->total_events;
            warnings[warning_count].frame_start_ns = monitor->decoder.active
                ? monitor->decoder.frame_start_ns : 0;
            warnings[warning_count].gpio = edge.gpio;
            warnings[warning_count].bits = monitor->decoder.bits;
            warnings[warning_count++].stage = monitor->decoder.stage;
            /* Explicit experimental exemption from the decoder's global-gap
             * reset. Keep the raw event/counters untouched; never bypass a
             * line fault, regression, timestamp fault or waveform check. */
            monitor->decoder.last_sequence = edge.sequence - 1;
            ++monitor->decoder.global_gap_warnings;
        } else if (reasons || line_fault) {
            capture_fault = true;
        }
        struct failure_edge *raw = NULL;
        size_t decoded_before = decoded.count;
        if (monitor->failure_history) {
            struct failure_history *h = monitor->failure_history;
            uint64_t serial = ++h->events;
            raw = &h->edges[(serial - 1) % FAILURE_RAW_EVENTS];
            *raw = (struct failure_edge){
                .serial = serial, .timestamp_ns = edge.time_ns,
                .global = edge.sequence, .line = line_sequence, .batch = h->batches,
                .gpio = edge.gpio, .index = (unsigned int)i, .batch_size = (unsigned int)count,
                .stage = monitor->decoder.stage, .bits = monitor->decoder.bits,
                .rising = edge.kind == XS8_RISING, .active = monitor->decoder.active,
                .tail_fell = monitor->decoder.tail_fell,
                .interval_ns = monitor->decoder.active
                    ? (int64_t)edge.time_ns - (int64_t)monitor->decoder.phase_start_ns : 0
            };
        }
        decoded.current_event_ns = edge.time_ns;
        xs8_decoder_edge(&monitor->decoder, &edge,
                         collect_decoded, &decoded);
        if (raw) {
            raw->error = monitor->decoder.error;
            if (decoded.count > decoded_before)
                raw->emitted = decoded.results[decoded.count - 1].decoded.reason;
        }
        if (monitor->warn_global_gaps && !line_fault &&
            reasons == XS8_CAPTURE_SEQUENCE_GAP &&
            edge.gpio == BUSY_GPIO && edge.kind == XS8_RISING && monitor->decoder.active)
            monitor->decoder.global_gap_warnings = 1;
    }
    /* No logging in the edge-processing loop; preserve delivery order. */
    for (size_t i = 0; i < warning_count; ++i)
        fprintf(stderr, "XS8 OBSERVED_BUS WARNING reason=GLOBAL_ONLY_SEQUENCE_GAP "
                "policy=warn gpio=%u expected_seqno=%llu actual_seqno=%llu "
                "line_seqno=%llu event_index=%llu event_mono_ns=%llu "
                "previous_event_mono_ns=%llu frame_start_ns=%llu bits=%u stage=%u\n",
                warnings[i].gpio, (unsigned long long)warnings[i].expected,
                (unsigned long long)warnings[i].actual,
                (unsigned long long)warnings[i].line_sequence,
                (unsigned long long)warnings[i].event_index,
                (unsigned long long)warnings[i].timestamp_ns,
                (unsigned long long)warnings[i].previous_ns,
                (unsigned long long)warnings[i].frame_start_ns,
                warnings[i].bits, warnings[i].stage);
    log_decoded_batch(monitor, &decoded);
    if (capture_fault) monitor_failed(monitor);
    log_capture_fault(monitor);
    pthread_mutex_lock(&monitor->observation_lock);
    pthread_cond_broadcast(&monitor->observation_changed);
    pthread_mutex_unlock(&monitor->observation_lock);
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
    pthread_mutex_lock(&monitor->processing_lock);
    int read_result = read_events(monitor);
    if (read_result < 0) monitor_failed(monitor);
    pthread_mutex_unlock(&monitor->processing_lock);
    if (read_result < 0) {
        fprintf(stderr, "XS8 OBSERVED_BUS event read failed: %d\n", errno);
        return -1;
    }
    return 0;
}

static void finish_monitor(struct xs8_monitor *monitor)
{
    struct decoded_batch decoded = {0};
    decoded.current_event_ns = monitor->capture.last_timestamp;
    xs8_decoder_flush(&monitor->decoder, collect_decoded, &decoded);
    log_decoded_batch(monitor, &decoded);
    log_capture_fault(monitor);
    monitor_failed(monitor);
    fprintf(stderr, "XS8 OBSERVED_BUS summary total_events=%llu "
                    "busy_edges=%llu data_edges=%llu global_sequence_gaps=%llu "
                    "missing_events=%llu sequence_regressions=%llu "
                    "timestamp_regressions=%llu max_batch=%u "
                    "incomplete_gap_frames=%llu dropped_frame_logs=%llu\n",
            (unsigned long long)monitor->total_events,
            (unsigned long long)monitor->busy_edges,
            (unsigned long long)monitor->data_edges,
            (unsigned long long)monitor->capture.sequence_gaps,
            (unsigned long long)monitor->capture.missing_events,
            (unsigned long long)monitor->capture.sequence_regressions,
            (unsigned long long)monitor->capture.timestamp_regressions,
            monitor->max_batch,
            (unsigned long long)monitor->incomplete_gap_frames,
            (unsigned long long)monitor->dropped_frame_logs);
    fprintf(stderr, "XS8 OBSERVED_BUS integrity policy=%s global_gap_warnings=%llu "
                    "line_sequence_gaps=%llu line_missing_events=%llu "
                    "line_sequence_regressions=%llu\n",
            monitor->warn_global_gaps ? "warn" : "strict",
            (unsigned long long)monitor->global_gap_warnings,
            (unsigned long long)monitor->line_gaps,
            (unsigned long long)monitor->line_missing,
            (unsigned long long)monitor->line_regressions);
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
    const char *policy = getenv("XS8_GLOBAL_GAP_POLICY");
    if (policy && strcmp(policy, "strict") && strcmp(policy, "warn")) {
        fprintf(stderr, "XS8 invalid XS8_GLOBAL_GAP_POLICY (expected strict|warn)\n");
        errno = EINVAL;
        return -1;
    }
    struct xs8_monitor *monitor = calloc(1, sizeof(*monitor));
    if (!monitor) return -1;
    monitor->bus = bus;
    monitor->threaded = threaded;
    monitor->warn_global_gaps = policy && !strcmp(policy, "warn");
    if (pthread_mutex_init(&monitor->processing_lock, NULL) != 0) {
        free(monitor);
        errno = EIO;
        return -1;
    }
    if (pthread_mutex_init(&monitor->observation_lock, NULL) != 0) {
        pthread_mutex_destroy(&monitor->processing_lock);
        free(monitor);
        errno = EIO;
        return -1;
    }
    pthread_condattr_t condition_attr;
    if (pthread_condattr_init(&condition_attr) != 0) {
        pthread_mutex_destroy(&monitor->processing_lock);
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
        pthread_mutex_destroy(&monitor->processing_lock);
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
    /* Diagnostic only: an unreadable DATA level is reported, never fatal. */
    int data = busy < 0 ? -1 : gpiod_line_request_get_value(bus->rx_req, DATA_GPIO);
    pthread_mutex_unlock(&bus->rx_lock);
    if (busy < 0) {
        goto start_fail;
    }
    xs8_decoder_init(&monitor->decoder, busy == 0);
    const char *snapshots = getenv("XS8_FAILURE_SNAPSHOTS");
    if (snapshots && !strcmp(snapshots, "1")) {
        monitor->failure_history = calloc(1, sizeof(*monitor->failure_history));
        if (monitor->failure_history) {
            /* Touch storage before accepting commands, not on a STOP retry.
             * This is not mlock or a hard real-time scheduling guarantee. */
            volatile unsigned char *pages = (volatile unsigned char *)monitor->failure_history;
            for (size_t i = 0; i < sizeof(*monitor->failure_history); i += 4096) pages[i] = 0;
            pages[sizeof(*monitor->failure_history) - 1] = 0;
            monitor->failure_history->initial_busy_low = busy == 0;
            fprintf(stderr, "XS8 failure snapshots enabled bytes=%zu raw_capacity=%u "
                    "snapshot_capacity=%u output=MONITOR_SHUTDOWN_ONLY\n",
                    sizeof(*monitor->failure_history), FAILURE_RAW_EVENTS, FAILURE_SNAPSHOTS);
        } else fprintf(stderr, "XS8 failure snapshots unavailable: allocation failed; "
                       "verification unchanged\n");
    }
    int64_t armed_ns = xs8_gpio_now_ns();
    monitor->capture.armed_ns = armed_ns > 0 ? (uint64_t)armed_ns : 0;
    char armed[128];
    if (xs8_capture_format_armed(monitor->capture.armed_ns, busy, data,
                                 armed, sizeof(armed)) > 0)
        fprintf(stderr, "%s\n", armed);
    atomic_init(&monitor->stop, false);
    atomic_init(&monitor->healthy, true);
    fprintf(stderr, "XS8 OBSERVED_BUS global_gap_policy=%s%s\n",
            monitor->warn_global_gaps ? "warn" : "strict",
            monitor->warn_global_gaps ? " EXPERIMENTAL: global gaps do not prove edge completeness" : "");
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
    free(monitor->failure_history);
    if (monitor->buffer) gpiod_edge_event_buffer_free(monitor->buffer);
    pthread_cond_destroy(&monitor->observation_changed);
    pthread_mutex_destroy(&monitor->observation_lock);
    pthread_mutex_destroy(&monitor->processing_lock);
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
    dump_failures(monitor);
    free(monitor->failure_history);
    gpiod_edge_event_buffer_free(monitor->buffer);
    pthread_cond_destroy(&monitor->observation_changed);
    pthread_mutex_destroy(&monitor->observation_lock);
    pthread_mutex_destroy(&monitor->processing_lock);
    free(monitor);
}

bool xs8_monitor_healthy(struct xs8_monitor *monitor)
{
    return monitor && atomic_load(&monitor->healthy);
}

bool xs8_monitor_get_diagnostics(struct xs8_monitor *monitor,
                                struct xs8_monitor_diagnostics *out)
{
    if (!monitor || !out) return false;
    pthread_mutex_lock(&monitor->processing_lock);
    *out = (struct xs8_monitor_diagnostics){monitor->warn_global_gaps,
        atomic_load(&monitor->healthy), monitor->global_gap_warnings};
    pthread_mutex_unlock(&monitor->processing_lock);
    return true;
}

uint64_t xs8_monitor_checkpoint(struct xs8_monitor *monitor)
{
    if (!monitor) return 0;
    pthread_mutex_lock(&monitor->observation_lock);
    uint64_t serial = monitor->observation_serial;
    pthread_mutex_unlock(&monitor->observation_lock);
    return serial;
}

int xs8_monitor_snapshot(struct xs8_monitor *monitor, uint64_t *cursor,
    struct xs8_monitor_record *records, size_t capacity, size_t *count)
{
    if (!monitor || !cursor || !records || !capacity || !count) {
        errno = EINVAL;
        return -1;
    }
    *count = 0;
    pthread_mutex_lock(&monitor->observation_lock);
    uint64_t newest = monitor->observation_serial;
    if (*cursor > newest || newest - *cursor >= OBS_HISTORY) {
        pthread_mutex_unlock(&monitor->observation_lock);
        errno = EOVERFLOW;
        return -1;
    }
    while (*cursor < newest && *count < capacity) {
        uint64_t serial = *cursor + 1;
        struct xs8_monitor_record record =
            monitor->observations[serial % OBS_HISTORY];
        if (record.serial != serial) {
            pthread_mutex_unlock(&monitor->observation_lock);
            errno = EOVERFLOW;
            return -1;
        }
        records[(*count)++] = record;
        *cursor = serial;
    }
    pthread_mutex_unlock(&monitor->observation_lock);
    return 0;
}

bool xs8_monitor_partial(struct xs8_monitor *monitor, uint64_t now_ns,
    struct xs8_monitor_record *record)
{
    if (!monitor || monitor->threaded || !record || !monitor->decoder.active)
        return false;
    *record = (struct xs8_monitor_record){
        .end_ns = now_ns,
        .decoded = {
            .kind = XS8_FRAME,
            .time_ns = monitor->decoder.frame_start_ns,
            .valid = false,
            .waveform_lsb = monitor->decoder.waveform_lsb,
            .legacy_msb_short1 = monitor->decoder.legacy_msb_short1,
            .bits = monitor->decoder.bits,
            .reason = monitor->decoder.error
                ? monitor->decoder.error : "CAPTURE_WINDOW_ENDED",
            .global_gap_warnings = monitor->decoder.global_gap_warnings
        }
    };
    return true;
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
    for (;;) {
        /* Service/manual mode has one reader. Drain currently readable batches,
         * without waiting for future traffic or extending the timeout. */
        if (!monitor->threaded) {
            for (unsigned int drains = 0;; ++drains) {
                struct pollfd fd = {gpiod_line_request_get_fd(monitor->bus->rx_req), POLLIN, 0};
                int ready = poll(&fd, 1, 0);
                if (ready < 0) return XS8_OBS_UNHEALTHY;
                if (!ready) break;
                if (drains == 32) {
                    pthread_mutex_lock(&monitor->processing_lock);
                    remember_failure(monitor, checkpoint, byte, start_ns, end_ns, "DRAIN_LIMIT");
                    pthread_mutex_unlock(&monitor->processing_lock);
                    return XS8_OBS_INVALID;
                }
                if (xs8_monitor_poll(monitor, 0) < 0) return XS8_OBS_UNHEALTHY;
            }
        }
        /* Batch publication, health, and partial decoder state are one snapshot.
         * Lock order matches the event reader: processing, then observation. */
        pthread_mutex_lock(&monitor->processing_lock);
        pthread_mutex_lock(&monitor->observation_lock);
        enum xs8_observation result = XS8_OBS_MISSING;
        unsigned int candidates = 0;
        const char *rejection = NULL;
        if (!atomic_load(&monitor->healthy) || checkpoint > monitor->observation_serial ||
            monitor->observation_serial - checkpoint >= OBS_HISTORY) {
            result = XS8_OBS_UNHEALTHY;
        } else {
            for (uint64_t next = checkpoint + 1; next <= monitor->observation_serial; ++next) {
                struct xs8_monitor_record record = monitor->observations[next % OBS_HISTORY];
                if (record.serial != next || record.decoded.kind == XS8_EVENT_GAP) {
                    rejection = record.serial != next ? "OBSERVATION_SERIAL" : "EVENT_GAP_RECORD";
                    result = XS8_OBS_INVALID;
                    break;
                }
                if (record.decoded.time_ns < start_ns) {
                    if (record.end_ns > start_ns) {
                        rejection = "PRE_TX_OVERLAP";
                        result = XS8_OBS_INVALID; break;
                    }
                    continue;
                }
                if (record.decoded.time_ns > end_ns) continue;
                if (++candidates > 1 || record.end_ns > end_ns + OBS_TAIL_NS ||
                    !xs8_decoded_local_tx_tail_valid(&record.decoded)) {
                    rejection = candidates > 1 ? "MULTIPLE_CANDIDATES" :
                        record.end_ns > end_ns + OBS_TAIL_NS ? "END_OUTSIDE_WINDOW" :
                        !record.decoded.valid ? "DECODE_INVALID" : "LOCAL_TAIL_INVALID";
                    result = XS8_OBS_INVALID;
                    break;
                }
                result = record.decoded.waveform_lsb == byte ? XS8_OBS_MATCH : XS8_OBS_MISMATCH;
            }
            if (monitor->decoder.active && monitor->decoder.frame_start_ns <= end_ns &&
                (candidates || monitor->decoder.frame_start_ns < start_ns)) {
                rejection = "ACTIVE_FRAME_OVERLAP";
                result = XS8_OBS_INVALID;
            }
        }
        struct pollfd fd = {gpiod_line_request_get_fd(monitor->bus->rx_req), POLLIN, 0};
        int pending = poll(&fd, 1, 0);
        if (pending < 0) result = XS8_OBS_UNHEALTHY;
        bool wait = result == XS8_OBS_MISSING ||
            (pending > 0 && (result == XS8_OBS_MATCH || result == XS8_OBS_MISMATCH));
        if (!wait && result == XS8_OBS_INVALID)
            remember_failure(monitor, checkpoint, byte, start_ns, end_ns, rejection);
        pthread_mutex_unlock(&monitor->processing_lock);
        if (!wait) {
            pthread_mutex_unlock(&monitor->observation_lock);
            return result;
        }
        if (!monitor->threaded) {
            pthread_mutex_unlock(&monitor->observation_lock);
            struct timespec current;
            if (clock_gettime(wait_clock, &current) < 0) return XS8_OBS_UNHEALTHY;
            int64_t remaining_ns =
                (int64_t)(deadline.tv_sec - current.tv_sec) * 1000000000LL +
                deadline.tv_nsec - current.tv_nsec;
            if (remaining_ns <= 0) return XS8_OBS_MISSING;
            int wait_ms = pending > 0 ? 0 : (int)((remaining_ns + 999999) / 1000000);
            if (wait_ms > 20) wait_ms = 20;
            if (xs8_monitor_poll(monitor, wait_ms) < 0) return XS8_OBS_UNHEALTHY;
        } else {
            int rc = pthread_cond_timedwait(&monitor->observation_changed,
                                           &monitor->observation_lock, &deadline);
            pthread_mutex_unlock(&monitor->observation_lock);
            if (rc == ETIMEDOUT) return XS8_OBS_MISSING;
            if (rc) return XS8_OBS_UNHEALTHY;
        }
    }
}
