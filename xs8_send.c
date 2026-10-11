#define _POSIX_C_SOURCE 200809L
#include "xs8_gpio.h"
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CAPTURE_NS 1000000000LL
#define EVENT_BATCH 256
#define INITIAL_EDGES 4096
#define VOLUME_HOLD_NS 150000000LL
#define STOP_ATTEMPTS 3

static struct xs8_gpio bus;
static volatile sig_atomic_t probe_interrupted;

static int64_t now_ns(void) { return xs8_gpio_now_ns(); }
static int wait_until(int64_t deadline) {
    return xs8_gpio_wait_until(deadline);
}

static void on_probe_signal(int signal_number)
{
    (void)signal_number;
    probe_interrupted = 1;
}

struct captured_edge {
    uint64_t timestamp_ns;
    unsigned int gpio;
    enum gpiod_edge_event_type type;
};

struct capture {
    int event_fd;
    struct gpiod_edge_event_buffer *buffer;
    struct captured_edge *edges;
    size_t count, capacity;
    unsigned long last_seqno, lost;
    int error;
    bool out_of_memory;
    int64_t end_ns;
    atomic_bool stop;
};

static void *capture_events(void *arg)
{
    struct capture *cap = arg;

    for (;;) {
        bool stopping = atomic_load(&cap->stop);
        /* Poll the fd outside libgpiod so BUSY reads need only lock reads. */
        struct pollfd fd = {.fd = cap->event_fd, .events = POLLIN};
        int ready = poll(&fd, 1, stopping ? 0 : 20);
        if (ready < 0) {
            if (errno == EINTR) continue;
            cap->error = errno;
            break;
        }
        if (!ready) {
            if (stopping) break;
            continue;
        }
        if (!(fd.revents & POLLIN)) {
            cap->error = EIO;
            break;
        }

        pthread_mutex_lock(&bus.rx_lock);
        int n = gpiod_line_request_read_edge_events(
            bus.rx_req, cap->buffer, EVENT_BATCH);
        int read_error = errno;
        pthread_mutex_unlock(&bus.rx_lock);
        if (n < 0) {
            if (read_error == EINTR) continue;
            cap->error = read_error;
            break;
        }

        bool past_end = false;
        for (int i = 0; i < n; ++i) {
            struct gpiod_edge_event *event =
                gpiod_edge_event_buffer_get_event(cap->buffer, i);
            uint64_t timestamp = gpiod_edge_event_get_timestamp_ns(event);
            if (stopping && timestamp > (uint64_t)cap->end_ns) {
                past_end = true;
                break;
            }
            unsigned long seqno = gpiod_edge_event_get_global_seqno(event);
            if (seqno > cap->last_seqno + 1)
                cap->lost += seqno - cap->last_seqno - 1;
            cap->last_seqno = seqno;
            if (cap->out_of_memory) continue;
            if (cap->count == cap->capacity) {
                size_t next = cap->capacity ? cap->capacity * 2 : EVENT_BATCH;
                if (next < cap->capacity ||
                    next > SIZE_MAX / sizeof(*cap->edges)) {
                    cap->out_of_memory = true;
                    continue;
                }
                struct captured_edge *grown = realloc(
                    cap->edges, next * sizeof(*cap->edges));
                if (!grown) {
                    cap->out_of_memory = true;
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
        if (past_end) break;
    }
    return NULL;
}

static int wait_probe_until(int64_t deadline)
{
    struct timespec t = {
        .tv_sec = deadline / 1000000000LL,
        .tv_nsec = deadline % 1000000000LL
    };
    while (!probe_interrupted) {
        int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                 &t, NULL);
        if (rc == 0) return 0;
        if (rc != EINTR) return -1;
    }
    return -1;
}

static int run_volume_probe(int64_t *tx_start_ns)
{
    int64_t ignored_start = 0;
    bool context_may_be_open = false;
    bool volume_may_be_active = false;
    bool volume_started = false;
    bool stop_completed = false;
    int64_t stop_complete_ns = 0;
    int result = -1;

    if (probe_interrupted) goto finish;
    if (xs8_gpio_send(&bus, 0x17, tx_start_ns) < 0) {
        context_may_be_open = *tx_start_ns != 0;
        goto finish;
    }
    context_may_be_open = true;
    if (probe_interrupted) goto finish;

    /* Set the guard before TX: a partly transmitted VOL- is uncertain. */
    volume_may_be_active = true;
    if (xs8_gpio_send(&bus, 0xE9, &ignored_start) < 0) goto finish;
    volume_started = true;
    if (wait_probe_until(now_ns() + VOLUME_HOLD_NS) < 0) goto finish;
    result = 0;

finish:
    if (volume_may_be_active) {
        for (int attempt = 0; attempt < STOP_ATTEMPTS; ++attempt) {
            if (xs8_gpio_send(&bus, 0x09, &ignored_start) == 0) {
                stop_completed = true;
                stop_complete_ns = now_ns();
                fprintf(stderr, "VOL STOP transmission completed\n");
                break;
            }
            fprintf(stderr, "VOL STOP attempt %d/%d failed\n",
                    attempt + 1, STOP_ATTEMPTS);
        }
        if (!stop_completed) {
            fprintf(stderr, "WARNING: VOL STOP FAILED; VOLUME CONTROL "
                    "MAY REMAIN ACTIVE. Stop it at the receiver.\n");
            result = -1;
        }
    }

    if (volume_started && stop_completed && result == 0 &&
        wait_probe_until(stop_complete_ns + CAPTURE_NS) < 0)
        result = -1;

    /* Never exit the context ahead of a possibly outstanding VOL STOP. */
    if (context_may_be_open && (!volume_may_be_active || stop_completed)) {
        if (xs8_gpio_send(&bus, 0x07, &ignored_start) < 0) {
            fprintf(stderr, "WARNING: ROOT exit transmission failed; "
                    "receiver context may remain open\n");
            result = -1;
        }
    }
    if (probe_interrupted) {
        fprintf(stderr, "Volume probe interrupted\n");
        result = -1;
    }
    return result;
}

int main(int argc, char **argv)
{
    bool volume_probe = argc == 2 &&
                        strcmp(argv[1], "--volume-probe") == 0;
    bool capturing = volume_probe ||
                     (argc == 3 && strcmp(argv[1], "--capture") == 0);
    if (!volume_probe && ((argc != 2 && !capturing) ||
        (argc == 2 && argv[1][0] == '-'))) {
        fprintf(stderr, "Usage: %s [--capture 0xNN | --volume-probe | 0xNN]\n",
                argv[0]);
        return 2;
    }

    unsigned long parsed = 0;
    if (!volume_probe) {
        char *end;
        const char *byte_arg = argv[capturing ? 2 : 1];
        parsed = strtoul(byte_arg, &end, 0);
        if (end == byte_arg || *end || parsed > 255) {
            fprintf(stderr, "Invalid byte\n");
            return 2;
        }
    }

    struct capture cap = {0};
    pthread_t capture_thread;
    bool thread_started = false;
    int64_t tx_start_ns = 0;
    int64_t capture_start_ns = 0;
    int result = -1;

    if (volume_probe) {
        struct sigaction action = {.sa_handler = on_probe_signal};
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, NULL) < 0 ||
            sigaction(SIGTERM, &action, NULL) < 0) {
            perror("Probe signal setup");
            goto cleanup;
        }
    }

    if (xs8_gpio_open(&bus, capturing) < 0) {
        perror("GPIO setup failed");
        goto cleanup;
    }

    if (capturing) {
        cap.event_fd = gpiod_line_request_get_fd(bus.rx_req);
        cap.buffer = gpiod_edge_event_buffer_new(EVENT_BATCH);
        cap.edges = malloc(INITIAL_EDGES * sizeof(*cap.edges));
        if (!cap.buffer || !cap.edges) {
            perror("Capture allocation");
            goto cleanup;
        }
        cap.capacity = INITIAL_EDGES;
        atomic_init(&cap.stop, false);
        int thread_error = pthread_create(
            &capture_thread, NULL, capture_events, &cap);
        if (thread_error) {
            errno = thread_error;
            perror("Capture thread");
            goto cleanup;
        }
        thread_started = true;
        capture_start_ns = now_ns();
    } else {
        printf("XS8 TX: 0x%02lX\n", parsed);
        fflush(stdout);
    }

    result = volume_probe ? run_volume_probe(&tx_start_ns) :
             xs8_gpio_send(&bus, (uint8_t)parsed, &tx_start_ns);
    int64_t tx_complete_ns = now_ns();

    /* Always release both transistor outputs. */
    if (xs8_gpio_release(&bus) < 0)
        result = -1;

    if (capturing) {
        if (!volume_probe && result == 0) {
            cap.end_ns = tx_complete_ns + CAPTURE_NS;
            if (wait_until(cap.end_ns) != 0)
                result = -1;
        } else {
            cap.end_ns = now_ns();
        }
        atomic_store(&cap.stop, true);
        pthread_join(capture_thread, NULL);
        thread_started = false;
        if (cap.error || cap.out_of_memory || cap.lost) {
            fprintf(stderr, "Capture incomplete: read error=%d, "
                    "allocation failure=%d, missing events=%lu\n",
                    cap.error, cap.out_of_memory, cap.lost);
            result = -1;
        }
        if (tx_start_ns || volume_probe) {
            int64_t origin_ns = tx_start_ns ? tx_start_ns : capture_start_ns;
            puts("time_us,gpio,edge");
            for (size_t i = 0; i < cap.count; ++i) {
                const struct captured_edge *edge = &cap.edges[i];
                if (edge->timestamp_ns > (uint64_t)cap.end_ns)
                    continue;
                printf("%lld,%u,%s\n",
                       (long long)((int64_t)edge->timestamp_ns -
                                   origin_ns) / 1000,
                       edge->gpio,
                       edge->type == GPIOD_EDGE_EVENT_RISING_EDGE
                           ? "rising" : "falling");
            }
            if (ferror(stdout)) result = -1;
        }
    }

cleanup:
    if (thread_started) {
        cap.end_ns = now_ns();
        atomic_store(&cap.stop, true);
        pthread_join(capture_thread, NULL);
    }
    if (xs8_gpio_release(&bus) < 0) result = -1;
    xs8_gpio_close(&bus);
    if (cap.buffer) gpiod_edge_event_buffer_free(cap.buffer);
    free(cap.edges);

    if (result < 0) {
        fprintf(stderr, "XS8 transmission failed\n");
        return 1;
    }

    if (!capturing) puts("XS8 transmission complete");
    return 0;
}
