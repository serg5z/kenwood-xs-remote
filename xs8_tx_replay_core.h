#ifndef XS8_TX_REPLAY_CORE_H
#define XS8_TX_REPLAY_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

#define XS8_REPLAY_MAX_GAP_MS 5000u
struct replay_options {
    bool up, disconnected, dry_run;
    unsigned int gap_ms[3];
};
struct replay_frame {
    uint8_t byte;
    int64_t invoked_ns, start_ns, completed_ns;
    int error;
    bool gpio_ok;
};
struct replay_report {
    struct replay_frame frames[4];
    unsigned int attempted;
    bool cleanup_attempted, cleanup_ok;
};
struct replay_ops {
    void *ctx;
    int (*services_inactive)(void *);
    int (*open_tx_only)(void *);
    int (*send)(void *, uint8_t, int64_t *);
    int (*release)(void *);
    void (*close)(void *);
    int64_t (*now)(void *);
    int (*wait)(void *, int64_t); /* errno value, zero on success */
    bool (*cancelled)(void *);
};
static inline uint8_t replay_byte(const struct replay_options *o, unsigned int i)
{
    const uint8_t bytes[] = {0x17, o->up ? 0xF9 : 0xE9, 0x09, 0x07};
    return bytes[i];
}
/* Two exact fields only; absent/unloaded units or indeterminate state refuse TX. */
static inline bool replay_service_state(const char *text)
{
    return !strcmp(text, "LoadState=loaded\nActiveState=inactive\n") ||
           !strcmp(text, "ActiveState=inactive\nLoadState=loaded\n");
}
static inline int replay_run(const struct replay_options *o,
    const struct replay_ops *ops, struct replay_report *report)
{
    memset(report, 0, sizeof(*report));
    for (unsigned int i = 0; i < 3; ++i)
        if (o->gap_ms[i] > XS8_REPLAY_MAX_GAP_MS) return EINVAL;
    if (o->dry_run) return 0;
    if (!o->disconnected) return EACCES;
    if (ops->cancelled(ops->ctx)) return ECANCELED;
    if (ops->services_inactive(ops->ctx) < 0) return errno ? errno : EACCES;
    if (ops->cancelled(ops->ctx)) return ECANCELED;
    /* open() releases partially acquired resources on failure. */
    if (ops->open_tx_only(ops->ctx) < 0) return errno ? errno : EIO;
    int error = 0;
    for (unsigned int i = 0; i < 4; ++i) {
        if (ops->cancelled(ops->ctx)) { error = ECANCELED; break; }
        if (i && o->gap_ms[i - 1]) {
            int64_t deadline = report->frames[i - 1].completed_ns +
                (int64_t)o->gap_ms[i - 1] * 1000000;
            error = ops->wait(ops->ctx, deadline);
            if (error) break;
        }
        if (ops->cancelled(ops->ctx)) { error = ECANCELED; break; }
        struct replay_frame *f = &report->frames[report->attempted];
        f->byte = replay_byte(o, i);
        f->invoked_ns = ops->now(ops->ctx);
        if (f->invoked_ns < 0) { error = EIO; break; }
        ++report->attempted;
        f->gpio_ok = ops->send(ops->ctx, f->byte, &f->start_ns) == 0;
        f->error = f->gpio_ok ? 0 : (errno ? errno : EIO);
        f->completed_ns = ops->now(ops->ctx);
        if (!f->gpio_ok) { error = f->error; break; }
        if (f->completed_ns < 0) { error = EIO; break; }
    }
    if (!error && ops->cancelled(ops->ctx)) error = ECANCELED;
    report->cleanup_attempted = true;
    report->cleanup_ok = ops->release(ops->ctx) == 0;
    if (!report->cleanup_ok && !error) error = EIO;
    ops->close(ops->ctx);
    return error;
}
#endif
