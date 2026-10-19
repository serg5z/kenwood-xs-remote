#define _POSIX_C_SOURCE 200809L
#include "xs8_decode.h"
#ifndef XS8_SEND_VERIFY_TEST
#include "xs8_gpio.h"
/* The standalone Makefile target compiles this utility as one C unit. */
#include "xs8_decode.c"
#endif
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

#ifndef XS8_SEND_VERIFY_TEST
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
#endif

struct gpiod_edge_event_buffer;

struct captured_edge {
    uint64_t timestamp_ns;
    uint64_t sequence;
    unsigned int gpio;
    enum xs8_edge_kind type;
};

struct capture {
    int event_fd;
    struct gpiod_edge_event_buffer *buffer;
    struct captured_edge *edges;
    size_t count, capacity;
    unsigned long last_seqno, lost, regressions, timestamp_regressions;
    uint64_t last_timestamp_ns;
    int error;
    bool out_of_memory;
    bool started, window_error;
    int64_t end_ns;
    bool initial_busy_low;
    atomic_bool stop;
};

#ifndef XS8_SEND_VERIFY_TEST
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
            else if (seqno <= cap->last_seqno)
                ++cap->regressions;
            cap->last_seqno = seqno;
            if (cap->last_timestamp_ns && timestamp < cap->last_timestamp_ns)
                ++cap->timestamp_regressions;
            cap->last_timestamp_ns = timestamp;
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
                seqno,
                gpiod_edge_event_get_line_offset(event),
                gpiod_edge_event_get_event_type(event) ==
                    GPIOD_EDGE_EVENT_RISING_EDGE ? XS8_RISING : XS8_FALLING
            };
        }
        if (past_end) break;
    }
    return NULL;
}
#endif

struct tx_verification {
    uint64_t start_ns, end_ns;
    uint64_t current_event_ns;
    unsigned int additional_frames;
    uint8_t observed_lsb, observed_legacy;
    bool candidate_seen, candidate_valid;
    bool ambiguous, incomplete_additional;
    bool log_frames;
};

enum capture_result { CAPTURE_COMPLETE, CAPTURE_INCOMPLETE, CAPTURE_INVALID };
enum match_result { MATCH, MISMATCH, MATCH_UNKNOWN };

struct self_capture_result {
    enum capture_result capture;
    enum match_result match;
    unsigned int additional_frames;
    uint8_t observed_lsb, observed_legacy;
    bool observed_byte;
};

static void inspect_frame(void *user, const struct xs8_decoded *decoded)
{
    struct tx_verification *verification = user;
    if (decoded->kind != XS8_FRAME) return;
    if (verification->log_frames && decoded->valid)
        fprintf(stderr, "mono_us=%llu OBSERVED_BUS source=UNATTRIBUTED "
                        "status=VALID waveform_lsb=0x%02X "
                        "legacy_msb_short1=0x%02X\n",
                (unsigned long long)(decoded->time_ns / 1000),
                decoded->waveform_lsb, decoded->legacy_msb_short1);
    else if (verification->log_frames)
        fprintf(stderr, "mono_us=%llu OBSERVED_BUS source=UNATTRIBUTED "
                        "status=INCOMPLETE bits=%u reason=%s\n",
                (unsigned long long)(decoded->time_ns / 1000),
                decoded->bits, decoded->reason);
    if (decoded->time_ns < verification->start_ns) {
        if (verification->current_event_ns > verification->start_ns)
            verification->ambiguous = true;
        return;
    }
    if (!verification->candidate_seen) {
        if (decoded->time_ns > verification->end_ns) return;
        verification->candidate_seen = true;
        verification->candidate_valid = xs8_decoded_local_tx_tail_valid(decoded);
        if (!decoded->valid) return;
        verification->observed_lsb = decoded->waveform_lsb;
        verification->observed_legacy = decoded->legacy_msb_short1;
        return;
    }
    ++verification->additional_frames;
    if (!decoded->valid) verification->incomplete_additional = true;
}

static struct self_capture_result verify_self_capture(
    const struct capture *cap, uint8_t requested, bool tx_ok,
    int64_t tx_start_ns, int64_t tx_complete_ns, bool log_frames)
{
    struct xs8_decoder decoder;
    struct tx_verification verification = {
        .start_ns = tx_start_ns > 0 ? (uint64_t)tx_start_ns : UINT64_MAX,
        .end_ns = tx_complete_ns > 0 ? (uint64_t)tx_complete_ns : 0,
        .log_frames = log_frames
    };
    xs8_decoder_init(&decoder, cap->initial_busy_low);
    for (size_t i = 0; i < cap->count; ++i) {
        const struct captured_edge *captured = &cap->edges[i];
        if (captured->timestamp_ns > (uint64_t)cap->end_ns) continue;
        struct xs8_edge edge = {
            .time_ns = captured->timestamp_ns,
            .sequence = captured->sequence,
            .gpio = captured->gpio,
            .kind = captured->type
        };
        verification.current_event_ns = edge.time_ns;
        xs8_decoder_edge(&decoder, &edge, inspect_frame, &verification);
    }
    xs8_decoder_flush(&decoder, inspect_frame, &verification);

    bool invalid = !cap->started || cap->window_error || cap->error ||
                   cap->out_of_memory || cap->lost || cap->regressions ||
                   cap->timestamp_regressions || verification.ambiguous;
    struct self_capture_result result = {
        .capture = invalid ? CAPTURE_INVALID :
            verification.candidate_valid && !verification.incomplete_additional
                ? CAPTURE_COMPLETE : CAPTURE_INCOMPLETE,
        .match = MATCH_UNKNOWN,
        .additional_frames = verification.additional_frames,
        .observed_lsb = verification.observed_lsb,
        .observed_legacy = verification.observed_legacy,
        .observed_byte = verification.candidate_valid
    };
    if (tx_ok && result.capture == CAPTURE_COMPLETE)
        result.match = result.observed_lsb == requested ? MATCH : MISMATCH;
    return result;
}

#ifndef XS8_SEND_VERIFY_TEST
static bool report_self_capture(const struct capture *cap, uint8_t requested,
                                bool tx_ok, int64_t tx_start_ns,
                                int64_t tx_complete_ns)
{
    struct self_capture_result result = verify_self_capture(
        cap, requested, tx_ok, tx_start_ns, tx_complete_ns, true);
    const char *capture_result = result.capture == CAPTURE_COMPLETE ? "COMPLETE" :
        result.capture == CAPTURE_INCOMPLETE ? "INCOMPLETE" : "INVALID";
    const char *match_result = result.match == MATCH ? "MATCH" :
        result.match == MISMATCH ? "MISMATCH" : "UNKNOWN";
    fprintf(stderr, "TX_RESULT=%s\nCAPTURE_RESULT=%s\nMATCH_RESULT=%s\n",
            tx_ok ? "TX_OK" : "TX_FAIL", capture_result, match_result);
    fprintf(stderr, "ADDITIONAL_FRAMES=%u\n", result.additional_frames);
    if (result.observed_byte)
        fprintf(stderr, "requested_logical=0x%02X observed_waveform_lsb=0x%02X "
                        "observed_legacy_msb_short1=0x%02X\n",
                requested, result.observed_lsb, result.observed_legacy);
    fprintf(stderr, "Capture comparison checks the bus waveform only; "
                    "it is not receiver acknowledgment.\n");
    return result.match == MATCH;
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
    int64_t tx_complete_ns = 0;
    int64_t capture_start_ns = 0;
    int result = -1;
    bool tx_ok = false;

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
        pthread_mutex_lock(&bus.rx_lock);
        int initial_busy = gpiod_line_request_get_value(bus.rx_req, 23);
        pthread_mutex_unlock(&bus.rx_lock);
        if (initial_busy < 0) {
            perror("Initial BUSY read");
            goto cleanup;
        }
        cap.initial_busy_low = initial_busy == 0;
        atomic_init(&cap.stop, false);
        int thread_error = pthread_create(
            &capture_thread, NULL, capture_events, &cap);
        if (thread_error) {
            errno = thread_error;
            perror("Capture thread");
            goto cleanup;
        }
        thread_started = true;
        cap.started = true;
        capture_start_ns = now_ns();
    } else {
        printf("XS8 TX: 0x%02lX\n", parsed);
        fflush(stdout);
    }

    result = volume_probe ? run_volume_probe(&tx_start_ns) :
             xs8_gpio_send(&bus, (uint8_t)parsed, &tx_start_ns);
    tx_ok = !volume_probe && result == 0;
    tx_complete_ns = now_ns();
    if (tx_complete_ns < 0) cap.window_error = true;

    /* Always release both transistor outputs. */
    if (xs8_gpio_release(&bus) < 0) {
        tx_ok = false;
        result = -1;
    }

    if (capturing) {
        if (!volume_probe && result == 0 && tx_complete_ns >= 0 &&
            tx_complete_ns <= INT64_MAX - CAPTURE_NS) {
            cap.end_ns = tx_complete_ns + CAPTURE_NS;
            if (wait_until(cap.end_ns) != 0) {
                cap.window_error = true;
                result = -1;
            }
        } else {
            if (!volume_probe && result == 0) cap.window_error = true;
            cap.end_ns = now_ns();
            if (cap.end_ns < 0) cap.window_error = true;
        }
        atomic_store(&cap.stop, true);
        pthread_join(capture_thread, NULL);
        thread_started = false;
        if (cap.error || cap.out_of_memory || cap.lost || cap.regressions ||
            cap.timestamp_regressions) {
            fprintf(stderr, "Capture incomplete: read error=%d, "
                    "allocation failure=%d, missing events=%lu, "
                    "sequence regressions=%lu, timestamp regressions=%lu\n",
                    cap.error, cap.out_of_memory, cap.lost, cap.regressions,
                    cap.timestamp_regressions);
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
                       edge->type == XS8_RISING
                           ? "rising" : "falling");
            }
            if (ferror(stdout)) {
                cap.window_error = true;
                result = -1;
            }
        }
    }

cleanup:
    if (thread_started) {
        cap.end_ns = now_ns();
        atomic_store(&cap.stop, true);
        pthread_join(capture_thread, NULL);
    }
    if (xs8_gpio_release(&bus) < 0) {
        tx_ok = false;
        result = -1;
    }
    if (capturing && !volume_probe)
        if (!report_self_capture(&cap, (uint8_t)parsed, tx_ok,
                                 tx_start_ns, tx_complete_ns)) result = -1;
    xs8_gpio_close(&bus);
    if (cap.buffer) gpiod_edge_event_buffer_free(cap.buffer);
    free(cap.edges);

    if (result < 0) {
        fprintf(stderr, "%s\n", capturing && !volume_probe
                ? "XS8 self-capture diagnostic failed"
                : "XS8 transmission failed");
        return 1;
    }

    if (!capturing) puts("XS8 transmission complete");
    return 0;
}
#endif
