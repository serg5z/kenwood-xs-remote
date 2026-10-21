#define _POSIX_C_SOURCE 200809L
#include <gpiod.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "xs8_sniff_diag.h"

#define CHIP "/dev/gpiochip0"
#define BUSY_RX 26
#define DATA_RX 13
#define EVENT_BATCH 256
#define INITIAL_EDGES 4096
#define DIAGNOSTIC_EDGES 262144

struct capture {
    struct sniff_record *edges;
    size_t count, capacity;
    struct sniff_window window;
    uint64_t buffer_dropped, batches;
    bool allocation_failed, event_order;
};

static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return -1;
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int collect_events(struct gpiod_line_request *req,
                          struct gpiod_edge_event_buffer *buffer,
                          struct capture *cap, uint64_t cutoff_ns)
{
    int count = gpiod_line_request_read_edge_events(req, buffer, EVENT_BATCH);
    if (count < 0) return -1;
    int64_t read_ns = now_ns();
    if (read_ns < 0) return -1;
    ++cap->batches;

    for (int i = 0; i < count; ++i) {
        struct gpiod_edge_event *event =
            gpiod_edge_event_buffer_get_event(buffer, i);
        uint64_t timestamp = gpiod_edge_event_get_timestamp_ns(event);
        /* Diagnostic mode retains every delivered event in original order.
         * A regressing kernel timestamp must not truncate a read batch. */
        if (!cap->event_order && timestamp > cutoff_ns) return 1;

        unsigned long seqno = gpiod_edge_event_get_global_seqno(event);
        sniff_note_sequence(&cap->window, (uint32_t)seqno, (uint64_t)read_ns);

        if (cap->allocation_failed) {
            ++cap->buffer_dropped;
            continue;
        }
        if (cap->count == cap->capacity) {
            if (cap->event_order) {
                cap->allocation_failed = true;
                ++cap->buffer_dropped;
                continue;
            }
            size_t next = cap->capacity * 2;
            if (next <= cap->capacity ||
                next > SIZE_MAX / sizeof(*cap->edges)) {
                cap->allocation_failed = true;
                ++cap->buffer_dropped;
                continue;
            }
            struct sniff_record *grown =
                realloc(cap->edges, next * sizeof(*cap->edges));
            if (!grown) {
                cap->allocation_failed = true;
                ++cap->buffer_dropped;
                continue;
            }
            cap->edges = grown;
            cap->capacity = next;
        }
        cap->edges[cap->count++] = (struct sniff_record){
            .timestamp_ns = timestamp,
            .read_ns = (uint64_t)read_ns,
            .batch = cap->batches,
            .gpio = gpiod_edge_event_get_line_offset(event),
            .index = (unsigned int)i,
            .batch_size = (unsigned int)count,
            .global_seqno = (uint32_t)seqno,
            .line_seqno = (uint32_t)gpiod_edge_event_get_line_seqno(event),
            .rising = gpiod_edge_event_get_event_type(event) ==
                       GPIOD_EDGE_EVENT_RISING_EDGE
        };
    }
    return 0;
}

int main(int argc, char **argv)
{
    bool event_order = false, duration_set = false, post_gap_set = false;
    uint64_t post_gap_ms = 1000;
    double seconds = 10.0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--event-order")) {
            event_order = true;
        } else if (!strcmp(argv[i], "--post-gap-ms") && i + 1 < argc) {
            const char *value = argv[++i];
            char *end;
            errno = 0;
            unsigned long ms = strtoul(value, &end, 10);
            if (errno || end == value || *end || value[0] == '-' ||
                ms < 1 || ms > 60000) {
                fprintf(stderr, "Post-gap window must be 1..60000 ms\n");
                return 2;
            }
            post_gap_ms = ms;
            post_gap_set = true;
        } else if (!strcmp(argv[i], "--help")) {
            printf("Usage: %s [--event-order [--post-gap-ms N]] "
                   "[duration_seconds]\n", argv[0]);
            return 0;
        } else if (!duration_set) {
            char *end;
            errno = 0;
            seconds = strtod(argv[i], &end);
            if (errno || end == argv[i] || *end || !isfinite(seconds) ||
                seconds <= 0.0) {
                fprintf(stderr, "Invalid duration or option: %s\n", argv[i]);
                return 2;
            }
            duration_set = true;
        } else {
            fprintf(stderr, "Unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }
    if ((post_gap_set && !event_order) || (event_order && seconds > 3600)) {
        fprintf(stderr, "--post-gap-ms requires --event-order; "
                "diagnostic duration must be <=3600 seconds\n");
        return 2;
    }
    long double duration_ns_ld = (long double)seconds * 1000000000.0L;
    if (duration_ns_ld < 1.0L || duration_ns_ld >= (long double)INT64_MAX) {
        fprintf(stderr, "Invalid duration\n");
        return 2;
    }
    int64_t duration_ns = (int64_t)duration_ns_ld;

    struct sigaction action = {.sa_handler = on_signal};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    struct gpiod_chip *chip = NULL;
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *config = NULL;
    struct gpiod_request_config *request_config = NULL;
    struct gpiod_line_request *request = NULL;
    struct gpiod_edge_event_buffer *buffer = NULL;
    struct capture cap = {.event_order = event_order};
    int result = 1;
    int64_t start_ns = -1;
    int64_t end_ns = -1;
    int64_t cutoff_ns = -1;

    chip = gpiod_chip_open(CHIP);
    settings = gpiod_line_settings_new();
    config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();
    buffer = gpiod_edge_event_buffer_new(EVENT_BATCH);
    size_t initial = event_order ? DIAGNOSTIC_EDGES : INITIAL_EDGES;
    cap.edges = malloc(initial * sizeof(*cap.edges));
    cap.capacity = cap.edges ? initial : 0;
    if (!chip || !settings || !config || !request_config ||
        !buffer || !cap.edges) {
        perror("Capture setup");
        goto cleanup;
    }

    if (gpiod_line_settings_set_direction(
            settings, GPIOD_LINE_DIRECTION_INPUT) < 0 ||
        gpiod_line_settings_set_edge_detection(
            settings, GPIOD_LINE_EDGE_BOTH) < 0 ||
        gpiod_line_settings_set_event_clock(
            settings, GPIOD_LINE_CLOCK_MONOTONIC) < 0) {
        perror("GPIO input settings");
        goto cleanup;
    }

    unsigned int inputs[] = {BUSY_RX, DATA_RX};
    if (gpiod_line_config_add_line_settings(
            config, inputs, 2, settings) < 0) {
        perror("GPIO line configuration");
        goto cleanup;
    }
    gpiod_request_config_set_event_buffer_size(request_config, 4096);
    gpiod_request_config_set_consumer(request_config, "xs8-sniff");

    start_ns = now_ns();
    if (start_ns < 0 || duration_ns > INT64_MAX - start_ns) {
        fprintf(stderr, "Capture clock or duration error\n");
        goto cleanup;
    }
    end_ns = start_ns + duration_ns;
    request = gpiod_chip_request_lines(chip, request_config, config);
    if (!request) {
        perror("GPIO input request");
        fprintf(stderr, "Exclusive GPIO13/26 ownership required. "
                "Do not run alongside kenwood-xs8.service or another GPIO owner.\n");
        goto cleanup;
    }

    int fd = gpiod_line_request_get_fd(request);
    while (!stop_requested) {
        int64_t current_ns = now_ns();
        if (current_ns < 0) {
            perror("Capture clock");
            goto output;
        }
        if (current_ns >= end_ns) break;

        int64_t remaining_ns = end_ns - current_ns;
        int timeout_ms = remaining_ns > 1000000000LL ? 1000 :
            (int)(remaining_ns / 1000000LL +
                  (remaining_ns % 1000000LL != 0));
        struct pollfd pollfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pollfd, 1, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("GPIO poll");
            goto output;
        }
        if (!ready) continue;
        if (!(pollfd.revents & POLLIN)) {
            fprintf(stderr, "GPIO event request closed unexpectedly\n");
            goto output;
        }
        int collected = collect_events(request, buffer, &cap,
                                       (uint64_t)end_ns);
        if (collected < 0) {
            perror("GPIO event read");
            goto output;
        }
        if (collected > 0) break;
        if (event_order) {
            /* Bound from userspace detection of the FIRST gap, not from a
             * potentially regressing event timestamp. Never renew this window. */
            end_ns = sniff_deadline(&cap.window, end_ns, post_gap_ms);
        }
    }

    cutoff_ns = stop_requested ? now_ns() : end_ns;
    if (cutoff_ns < 0) {
        perror("Capture clock");
        goto output;
    }
    /* Diagnostic mode stops at its wall-clock deadline: no unbounded drain
     * and no discarding later-delivered events based on timestamp ordering. */
    if (event_order) {
        result = 0;
        goto output;
    }
    /* Drain events already queued at the cutoff without extending capture. */
    for (;;) {
        struct pollfd pollfd = {.fd = fd, .events = POLLIN};
        int ready = poll(&pollfd, 1, 0);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("GPIO drain poll");
            goto output;
        }
        if (!ready) break;
        if (!(pollfd.revents & POLLIN)) {
            fprintf(stderr, "GPIO event request closed unexpectedly\n");
            goto output;
        }
        int collected = collect_events(request, buffer, &cap,
                                       (uint64_t)cutoff_ns);
        if (collected < 0) {
            perror("GPIO drain read");
            goto output;
        }
        if (collected > 0) break;
    }
    result = 0;

output:
    /* Release exclusive input ownership before potentially slow output. */
    gpiod_line_request_release(request);
    request = NULL;
    if (event_order) {
        struct sniff_summary summary;
        if (sniff_report(stdout, stderr, cap.edges, cap.count, &summary) < 0) {
            fprintf(stderr, "Diagnostic reporting failed\n");
            result = 1;
        }
        fprintf(stderr, "CAPTURE_START_NS=%lld batches=%" PRIu64
                " first_gap_read_ns=%" PRIu64 " buffer_dropped=%" PRIu64
                " collection_complete=%s stop_reason=%s\n",
                (long long)start_ns, cap.batches,
                cap.window.first_gap_read_ns, cap.buffer_dropped,
                result == 0 && !cap.buffer_dropped ? "yes" : "no",
                result ? "ERROR" : stop_requested ? "SIGNAL" :
                cap.window.first_gap_read_ns ? "FIRST_GAP_WINDOW" : "DURATION");
        if (cap.buffer_dropped)
            fprintf(stderr, "WARNING: local storage exhausted; sequence analysis "
                    "is incomplete (limit %u events).\n", DIAGNOSTIC_EDGES);
    } else {
        puts("time_us,gpio,edge");
        for (size_t i = 0; i < cap.count; ++i) {
            struct sniff_record *edge = &cap.edges[i];
            if (cutoff_ns >= 0 && edge->timestamp_ns > (uint64_t)cutoff_ns)
                continue;
            printf("%lld,%u,%s\n",
                   (long long)((int64_t)edge->timestamp_ns - start_ns) / 1000,
                   edge->gpio, edge->rising ? "rising" : "falling");
        }
        fprintf(stderr, "Captured %zu edges; kernel gaps: %llu; "
                "buffer dropped: %llu\n", cap.count,
                (unsigned long long)cap.window.gaps,
                (unsigned long long)cap.buffer_dropped);
    }
    if (fflush(stdout) == EOF) {
        perror("CSV output");
        result = 1;
    }
    if (cap.window.gaps || cap.buffer_dropped || ferror(stdout)) result = 1;

cleanup:
    if (request) gpiod_line_request_release(request);
    if (buffer) gpiod_edge_event_buffer_free(buffer);
    if (request_config) gpiod_request_config_free(request_config);
    if (config) gpiod_line_config_free(config);
    if (settings) gpiod_line_settings_free(settings);
    if (chip) gpiod_chip_close(chip);
    free(cap.edges);
    return result;
}
