#define _POSIX_C_SOURCE 200809L
#include "xs8_rt.h"

#include <errno.h>
#include <stdlib.h>

static const struct xs8_rt_ops real_ops = {
    .get = pthread_getschedparam,
    .set = pthread_setschedparam
};

int xs8_rt_parse_priority(const char *value)
{
    if (!value || !*value) return 0;
    char *end;
    errno = 0;
    long priority = strtol(value, &end, 10);
    if (errno || end == value || *end || priority < 1 || priority > 10) {
        errno = EINVAL;
        return -1;
    }
    return (int)priority;
}

int xs8_rt_begin(struct xs8_rt_guard *guard, int priority,
                 const struct xs8_rt_ops *ops)
{
    if (!guard || priority < 0 || priority > 10) { errno = EINVAL; return -1; }
    guard->active = false;
    if (!priority) return 0;
    if (!ops) ops = &real_ops;
    if (!ops->get || !ops->set) { errno = EINVAL; return -1; }
    guard->thread = pthread_self();
    int error = ops->get(guard->thread, &guard->original_policy,
                         &guard->original_param);
    if (error) { errno = error; return -1; }
    struct sched_param fifo = {.sched_priority = priority};
    error = ops->set(guard->thread, SCHED_FIFO, &fifo);
    if (error) { errno = error; return -1; }
    guard->active = true;
    return 1;
}

int xs8_rt_end(struct xs8_rt_guard *guard, const struct xs8_rt_ops *ops)
{
    if (!guard) { errno = EINVAL; return -1; }
    if (!guard->active) return 0;
    if (!ops) ops = &real_ops;
    if (!ops->set) { errno = EINVAL; return -1; }
    int error = ops->set(guard->thread, guard->original_policy,
                         &guard->original_param);
    if (!error) { guard->active = false; return 0; }
    int first_error = error;
    error = ops->set(guard->thread, guard->original_policy,
                     &guard->original_param);
    if (!error) { guard->active = false; errno = first_error; return 1; }
    struct sched_param normal = {.sched_priority = 0};
    error = ops->set(guard->thread, SCHED_OTHER, &normal);
    if (!error) { guard->active = false; errno = first_error; return 2; }
    errno = error;
    return -1;
}
