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

#define CHIP "/dev/gpiochip0"
#define BUSY_RX 23
#define DATA_RX 27
#define EVENT_BATCH 256
#define INITIAL_EDGES 4096

struct captured_edge {
    uint64_t timestamp_ns;
    unsigned int gpio;
    enum gpiod_edge_event_type type;
};

struct capture {
    struct captured_edge *edges;
    size_t count, capacity;
    unsigned long last_seqno;
    uint64_t kernel_gaps, buffer_dropped;
    bool allocation_failed;
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

    for (int i = 0; i < count; ++i) {
        struct gpiod_edge_event *event =
            gpiod_edge_event_buffer_get_event(buffer, i);
        uint64_t timestamp = gpiod_edge_event_get_timestamp_ns(event);
        if (timestamp > cutoff_ns) return 1;

        unsigned long seqno = gpiod_edge_event_get_global_seqno(event);
        if (seqno > cap->last_seqno + 1)
            cap->kernel_gaps += seqno - cap->last_seqno - 1;
        cap->last_seqno = seqno;

        if (cap->allocation_failed) {
            ++cap->buffer_dropped;
            continue;
        }
        if (cap->count == cap->capacity) {
            size_t next = cap->capacity * 2;
            if (next <= cap->capacity ||
                next > SIZE_MAX / sizeof(*cap->edges)) {
                cap->allocation_failed = true;
                ++cap->buffer_dropped;
                continue;
            }
            struct captured_edge *grown =
                realloc(cap->edges, next * sizeof(*cap->edges));
            if (!grown) {
                cap->allocation_failed = true;
                ++cap->buffer_dropped;
                continue;
            }
            cap->edges = grown;
            cap->capacity = next;
        }
        cap->edges[cap->count++] = (struct captured_edge){
            timestamp,
            gpiod_edge_event_get_line_offset(event),
            gpiod_edge_event_get_event_type(event)
        };
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [duration_seconds]\n", argv[0]);
        return 2;
    }

    double seconds = 10.0;
    if (argc == 2) {
        char *end;
        errno = 0;
        seconds = strtod(argv[1], &end);
        if (errno || end == argv[1] || *end || !isfinite(seconds) ||
            seconds <= 0.0) {
            fprintf(stderr, "Invalid duration\n");
            return 2;
        }
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
    struct capture cap = {0};
    int result = 1;
    int64_t start_ns = -1;
    int64_t end_ns = -1;
    int64_t cutoff_ns = -1;

    chip = gpiod_chip_open(CHIP);
    settings = gpiod_line_settings_new();
    config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();
    buffer = gpiod_edge_event_buffer_new(EVENT_BATCH);
    cap.edges = malloc(INITIAL_EDGES * sizeof(*cap.edges));
    cap.capacity = cap.edges ? INITIAL_EDGES : 0;
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

    start_ns = now_ns();
    if (start_ns < 0 || duration_ns > INT64_MAX - start_ns) {
        fprintf(stderr, "Capture clock or duration error\n");
        goto cleanup;
    }
    end_ns = start_ns + duration_ns;
    request = gpiod_chip_request_lines(chip, request_config, config);
    if (!request) {
        perror("GPIO input request");
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
    }

    cutoff_ns = stop_requested ? now_ns() : end_ns;
    if (cutoff_ns < 0) {
        perror("Capture clock");
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
    puts("time_us,gpio,edge");
    for (size_t i = 0; i < cap.count; ++i) {
        struct captured_edge *edge = &cap.edges[i];
        if (cutoff_ns >= 0 && edge->timestamp_ns > (uint64_t)cutoff_ns)
            continue;
        printf("%lld,%u,%s\n",
               (long long)((int64_t)edge->timestamp_ns - start_ns) / 1000,
               edge->gpio,
               edge->type == GPIOD_EDGE_EVENT_RISING_EDGE
                   ? "rising" : "falling");
    }
    fprintf(stderr, "Captured %zu edges; kernel gaps: %llu; "
            "buffer dropped: %llu\n", cap.count,
            (unsigned long long)cap.kernel_gaps,
            (unsigned long long)cap.buffer_dropped);
    if (cap.kernel_gaps || cap.buffer_dropped || ferror(stdout)) result = 1;

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
