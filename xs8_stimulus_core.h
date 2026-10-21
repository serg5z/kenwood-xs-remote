#ifndef XS8_STIMULUS_CORE_H
#define XS8_STIMULUS_CORE_H

/* Standalone, disconnected-fixture stimulus; not a receiver command API.
 * Timings mirror xs8_gpio.c; this does not link or modify the production TX.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define STIM_DATA 6
#define STIM_BUSY 19
#define STIM_MAX_FRAMES 256
#define STIM_MAX_TRANSITIONS 10000
#define STIM_MAX_RUN_NS 60000000000LL
enum stimulus_line { STIM_COMBINED, STIM_DATA_ONLY, STIM_BUSY_ONLY };

struct stimulus_options {
    unsigned int frames, gap_ms;
    bool disconnected_ack;
    enum stimulus_line line;
    unsigned int transitions, interval_us;
    bool hold_busy_high;
    bool hold_busy_high_only;
    unsigned int duration_ms;
};
struct stimulus_frame {
    uint8_t byte;
    int64_t start_ns, end_ns, max_late_ns;
    unsigned int over_500us, over_1000us, samples;
    bool completed;
};
struct stimulus_report {
    struct stimulus_frame frames[STIM_MAX_FRAMES];
    unsigned int attempted, completed;
    bool cleanup_attempted, cleanup_ok;
    struct {
        int64_t write_ns, deadline_ns, wake_ns;
        bool written;
    } transitions[STIM_MAX_TRANSITIONS];
    unsigned int transition_attempted, transition_written;
    struct stimulus_frame single;
    bool busy_high_attempted, busy_high_written, busy_low_written;
    int64_t busy_high_write_ns, busy_high_done_ns, busy_lead_wake_ns;
    int64_t busy_low_write_ns;
    int64_t busy_low_done_ns, busy_hold_deadline_ns, busy_hold_wake_ns;
    bool busy_hold_completed;
};
struct stimulus_ops {
    void *ctx;
    int (*service_inactive)(void *);
    int (*open)(void *);
    int (*set)(void *, unsigned int gpio, int level);
    int64_t (*now)(void *);
    /* Absolute CLOCK_MONOTONIC wait; return errno, including EINTR on cancel. */
    int (*wait)(void *, int64_t deadline);
    bool (*cancelled)(void *);
    void (*close)(void *);
};

static inline uint8_t stimulus_byte(unsigned int frame)
{
    static const uint8_t pattern[] = {0x00, 0xff, 0x55, 0xaa};
    return pattern[frame % 4];
}

static inline int64_t stimulus_frame_ns(uint8_t byte)
{
    int64_t us = 1150 + 9800 + 750;
    for (unsigned int bit = 0; bit < 8; ++bit)
        us += (byte & (1u << bit) ? 9800 : 4900) + 4900;
    return us * 1000;
}

static inline int stimulus_validate(const struct stimulus_options *options)
{
    if (!options->disconnected_ack) { errno = EPERM; return -1; }
    if (options->hold_busy_high_only) {
        if (options->hold_busy_high || options->line != STIM_COMBINED ||
            !options->duration_ms || options->duration_ms > 60000) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (options->duration_ms) { errno = EINVAL; return -1; }
    if (options->hold_busy_high && options->line != STIM_DATA_ONLY) {
        errno = EINVAL;
        return -1;
    }
    if (options->line != STIM_COMBINED) {
        if ((options->line != STIM_DATA_ONLY && options->line != STIM_BUSY_ONLY) ||
            options->transitions < 2 || options->transitions > STIM_MAX_TRANSITIONS ||
            options->transitions % 2 || options->interval_us < 1000 ||
            options->interval_us > 20000 ||
            ((int64_t)options->transitions + options->hold_busy_high) *
                options->interval_us * 1000 >
                STIM_MAX_RUN_NS) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    if (!options->frames || options->frames > STIM_MAX_FRAMES ||
        !options->gap_ms || options->gap_ms > 1000) {
        errno = EINVAL;
        return -1;
    }
    int64_t total = (int64_t)(options->frames - 1) * options->gap_ms * 1000000;
    for (unsigned int i = 0; i < options->frames; ++i)
        total += stimulus_frame_ns(stimulus_byte(i));
    if (total > STIM_MAX_RUN_NS) { errno = EINVAL; return -1; }
    return 0;
}

/* Require a known, loaded unit explicitly inactive, not failed/not-found.
 * Reject extra/duplicate/malformed fields rather than guessing its state. */
static inline bool stimulus_service_state(const char *text)
{
    return !strcmp(text, "LoadState=loaded\nActiveState=inactive\n") ||
           !strcmp(text, "ActiveState=inactive\nLoadState=loaded\n");
}

static inline int stimulus_step(const struct stimulus_ops *ops,
                                struct stimulus_frame *frame, int64_t *deadline,
                                int64_t run_limit, unsigned int gpio, int level,
                                unsigned int us)
{
    if (ops->cancelled(ops->ctx)) { errno = ECANCELED; return -1; }
    if (*deadline > run_limit - (int64_t)us * 1000) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (ops->set(ops->ctx, gpio, level) < 0) return -1;
    *deadline += (int64_t)us * 1000;
    int error = ops->wait(ops->ctx, *deadline);
    if (error) { errno = error; return -1; }
    int64_t woke = ops->now(ops->ctx);
    if (woke < 0) return -1;
    int64_t late = woke - *deadline;
    ++frame->samples;
    if (late > frame->max_late_ns) frame->max_late_ns = late;
    if (late > 500000) ++frame->over_500us;
    if (late > 1000000) ++frame->over_1000us;
    if (woke >= run_limit || woke - frame->start_ns > 250000000) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static inline int stimulus_run(const struct stimulus_options *options,
                               const struct stimulus_ops *ops,
                               struct stimulus_report *report)
{
    memset(report, 0, sizeof(*report));
    if (stimulus_validate(options) < 0) return -1;
    if (ops->service_inactive(ops->ctx) < 0) return -1;
    bool opened = false;
    int error = 0;
    if (ops->cancelled(ops->ctx)) { errno = ECANCELED; return -1; }
    if (ops->open(ops->ctx) < 0) { error = errno; goto done; }
    opened = true;
    /* A second check narrows the startup race. Exclusive TX ownership remains
     * the arbitration mechanism; no service management is performed. */
    if (ops->service_inactive(ops->ctx) < 0) { error = errno; goto done; }
    if (ops->set(ops->ctx, STIM_DATA, 0) < 0 ||
        ops->set(ops->ctx, STIM_BUSY, 0) < 0) { error = errno; goto done; }
    int64_t started = ops->now(ops->ctx);
    if (started < 0) { error = errno; goto done; }
    if (started > INT64_MAX - STIM_MAX_RUN_NS) { error = EOVERFLOW; goto done; }
    int64_t run_limit = started + STIM_MAX_RUN_NS;
    if (options->hold_busy_high || options->hold_busy_high_only) {
        if (ops->cancelled(ops->ctx)) { error = ECANCELED; goto done; }
        report->busy_high_write_ns = ops->now(ops->ctx);
        if (report->busy_high_write_ns < 0) { error = errno; goto done; }
        report->busy_high_attempted = true;
        if (ops->set(ops->ctx, STIM_BUSY, 1) < 0) { error = errno; goto done; }
        report->busy_high_written = true;
        report->busy_high_done_ns = ops->now(ops->ctx);
        if (report->busy_high_done_ns < 0) { error = errno; goto done; }
        if (options->hold_busy_high_only) {
            int64_t hold_ns = (int64_t)options->duration_ms * 1000000;
            if (report->busy_high_done_ns > INT64_MAX - hold_ns) {
                error = EOVERFLOW; goto done;
            }
            /* Requested duration starts after the HIGH write completed.
             * No DATA toggles, refresh writes or intermediate GPIO reads. */
            report->busy_hold_deadline_ns = report->busy_high_done_ns + hold_ns;
            int wait_error = ops->wait(ops->ctx, report->busy_hold_deadline_ns);
            if (wait_error) { error = wait_error; goto done; }
            report->busy_hold_wake_ns = ops->now(ops->ctx);
            if (report->busy_hold_wake_ns < 0) { error = errno; goto done; }
            int64_t late = report->busy_hold_wake_ns - report->busy_hold_deadline_ns;
            if (late < 0 || late > 250000000) { error = ETIMEDOUT; goto done; }
            report->busy_hold_completed = true;
            goto done;
        }
        if (report->busy_high_done_ns >
            run_limit - (int64_t)options->interval_us * 1000) {
            error = ETIMEDOUT; goto done;
        }
        /* Separate BUSY assertion from DATA activity by one configured
         * interval. The normal final DATA LOW already has the same tail. */
        int64_t lead_deadline = report->busy_high_done_ns +
                                (int64_t)options->interval_us * 1000;
        int wait_error = ops->wait(ops->ctx, lead_deadline);
        if (wait_error) { error = wait_error; goto done; }
        report->busy_lead_wake_ns = ops->now(ops->ctx);
        if (report->busy_lead_wake_ns < 0) { error = errno; goto done; }
        if (report->busy_lead_wake_ns > run_limit ||
            report->busy_lead_wake_ns - lead_deadline > 250000000) {
            error = ETIMEDOUT; goto done;
        }
        started = report->busy_lead_wake_ns;
    }
    if (options->line != STIM_COMBINED) {
        unsigned int gpio = options->line == STIM_DATA_ONLY ? STIM_DATA : STIM_BUSY;
        struct stimulus_frame *stats = &report->single;
        stats->start_ns = started;
        int64_t deadline = started;
        for (unsigned int i = 0; i < options->transitions; ++i) {
            if (ops->cancelled(ops->ctx)) { error = ECANCELED; goto done; }
            ++report->transition_attempted;
            report->transitions[i].write_ns = ops->now(ops->ctx);
            if (report->transitions[i].write_ns < 0) { error = errno; goto done; }
            if (report->transitions[i].write_ns >= run_limit) {
                error = ETIMEDOUT; goto done;
            }
            if (ops->set(ops->ctx, gpio, i % 2 == 0) < 0) {
                error = errno; goto done;
            }
            report->transitions[i].written = true;
            ++report->transition_written;
            deadline += (int64_t)options->interval_us * 1000;
            report->transitions[i].deadline_ns = deadline;
            int wait_error = ops->wait(ops->ctx, deadline);
            if (wait_error) { error = wait_error; goto done; }
            int64_t woke = ops->now(ops->ctx);
            if (woke < 0) { error = errno; goto done; }
            report->transitions[i].wake_ns = woke;
            int64_t late = woke - deadline;
            ++stats->samples;
            if (late > stats->max_late_ns) stats->max_late_ns = late;
            if (late > 500000) ++stats->over_500us;
            if (late > 1000000) ++stats->over_1000us;
            if (woke > run_limit || late > 250000000) {
                error = ETIMEDOUT; goto done;
            }
        }
        stats->end_ns = ops->now(ops->ctx);
        if (stats->end_ns < 0) { error = errno; goto done; }
        stats->completed = true;
        goto done;
    }
    for (unsigned int i = 0; i < options->frames; ++i) {
        if (ops->cancelled(ops->ctx)) { error = ECANCELED; break; }
        struct stimulus_frame *frame = &report->frames[report->attempted++];
        frame->byte = stimulus_byte(i);
        frame->start_ns = ops->now(ops->ctx);
        if (frame->start_ns < 0) { error = errno; break; }
        if (frame->start_ns >= run_limit) { error = ETIMEDOUT; break; }
        int64_t deadline = frame->start_ns;
#define STIM_STEP(pin, level, us) do { \
    if (stimulus_step(ops, frame, &deadline, run_limit, pin, level, us) < 0) { \
        error = errno; goto done; \
    } \
} while (0)
        STIM_STEP(STIM_BUSY, 1, 1150);
        STIM_STEP(STIM_DATA, 1, 9800);
        for (unsigned int bit = 0; bit < 8; ++bit) {
            STIM_STEP(STIM_DATA, 0, frame->byte & (1u << bit) ? 9800 : 4900);
            STIM_STEP(STIM_DATA, 1, 4900);
        }
        STIM_STEP(STIM_DATA, 0, 750);
#undef STIM_STEP
        if (ops->cancelled(ops->ctx)) { error = ECANCELED; break; }
        if (ops->set(ops->ctx, STIM_BUSY, 0) < 0) { error = errno; break; }
        frame->end_ns = ops->now(ops->ctx);
        if (frame->end_ns < 0) { error = errno; break; }
        frame->completed = true;
        ++report->completed;
        if (i + 1 < options->frames) {
            int wait_error = ops->wait(ops->ctx,
                frame->end_ns + (int64_t)options->gap_ms * 1000000);
            if (wait_error) { error = wait_error; break; }
        }
    }
done:
    if (opened) {
        report->cleanup_attempted = true;
        /* Always attempt BOTH outputs, even when the first write fails. */
        int data = ops->set(ops->ctx, STIM_DATA, 0);
        /* A timestamp failure must never skip the following LOW write. */
        if (options->hold_busy_high || options->hold_busy_high_only)
            report->busy_low_write_ns = ops->now(ops->ctx);
        int busy = ops->set(ops->ctx, STIM_BUSY, 0);
        report->busy_low_written = busy == 0;
        if (options->hold_busy_high_only) {
            report->busy_low_done_ns = ops->now(ops->ctx);
            if ((report->busy_low_write_ns < 0 || report->busy_low_done_ns < 0) &&
                !error) error = EIO;
        }
        report->cleanup_ok = data == 0 && busy == 0;
        if (!report->cleanup_ok && !error) error = EIO;
    }
    ops->close(ops->ctx);
    if (error) { errno = error; return -1; }
    return 0;
}
#endif
