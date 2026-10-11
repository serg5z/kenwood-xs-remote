#ifndef XS8_RT_H
#define XS8_RT_H

#include <pthread.h>
#include <sched.h>
#include <stdbool.h>

/* Priority zero disables RT. Explicit opt-in is limited to FIFO 1..10. */
int xs8_rt_parse_priority(const char *value);

struct xs8_rt_ops {
    int (*get)(pthread_t, int *, struct sched_param *);
    int (*set)(pthread_t, int, const struct sched_param *);
};

struct xs8_rt_guard {
    pthread_t thread;
    int original_policy;
    struct sched_param original_param;
    bool active;
};

/* Return 1 if enabled, 0 if disabled, -1 on failure (errno set). */
int xs8_rt_begin(struct xs8_rt_guard *guard, int priority,
                 const struct xs8_rt_ops *ops);

/* Return 0 on exact restore, 1 after a retry, 2 after SCHED_OTHER fallback,
 * or -1 if demotion failed and the thread may still be running at RT priority. */
int xs8_rt_end(struct xs8_rt_guard *guard, const struct xs8_rt_ops *ops);

#endif
