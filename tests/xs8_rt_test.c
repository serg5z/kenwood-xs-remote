#include "xs8_rt.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

static int policy, priority, get_calls, set_calls;
static int get_error, fail_set_1, fail_set_2, fail_set_3, fail_set_4;

static int fake_get(pthread_t thread, int *out_policy,
                    struct sched_param *out_param)
{
    (void)thread;
    ++get_calls;
    if (get_error) return get_error;
    *out_policy = policy;
    out_param->sched_priority = priority;
    return 0;
}

static int fake_set(pthread_t thread, int next_policy,
                    const struct sched_param *next_param)
{
    (void)thread;
    ++set_calls;
    if ((set_calls == 1 && fail_set_1) ||
        (set_calls == 2 && fail_set_2) ||
        (set_calls == 3 && fail_set_3) ||
        (set_calls == 4 && fail_set_4)) return EPERM;
    policy = next_policy;
    priority = next_param->sched_priority;
    return 0;
}

static const struct xs8_rt_ops fake_ops = {
    .get = fake_get, .set = fake_set
};

static void reset_fake(void)
{
    policy = SCHED_OTHER;
    priority = 0;
    get_calls = set_calls = 0;
    get_error = fail_set_1 = fail_set_2 = fail_set_3 = fail_set_4 = 0;
}

int main(void)
{
    assert(xs8_rt_parse_priority(NULL) == 0);
    assert(xs8_rt_parse_priority("") == 0);
    assert(xs8_rt_parse_priority("5") == 5);
    assert(xs8_rt_parse_priority("0") == -1);
    assert(xs8_rt_parse_priority("11") == -1);
    assert(xs8_rt_parse_priority("5x") == -1);

    struct xs8_rt_guard guard = {0};
    reset_fake();
    assert(xs8_rt_begin(&guard, 0, &fake_ops) == 0);
    assert(xs8_rt_end(&guard, &fake_ops) == 0);
    assert(get_calls == 0 && set_calls == 0);

    reset_fake();
    assert(xs8_rt_begin(&guard, 5, &fake_ops) == 1);
    assert(guard.active && policy == SCHED_FIFO && priority == 5);
    assert(xs8_rt_end(&guard, &fake_ops) == 0);
    assert(!guard.active && policy == SCHED_OTHER && priority == 0);

    reset_fake();
    get_error = EIO;
    assert(xs8_rt_begin(&guard, 5, &fake_ops) == -1 && errno == EIO);
    assert(!guard.active && set_calls == 0 && policy == SCHED_OTHER);

    reset_fake();
    fail_set_1 = 1;
    assert(xs8_rt_begin(&guard, 5, &fake_ops) == -1 && errno == EPERM);
    assert(!guard.active && policy == SCHED_OTHER);

    reset_fake();
    assert(xs8_rt_begin(&guard, 5, &fake_ops) == 1);
    fail_set_2 = 1;
    assert(xs8_rt_end(&guard, &fake_ops) == 1);
    assert(!guard.active && policy == SCHED_OTHER && set_calls == 3);

    reset_fake();
    assert(xs8_rt_begin(&guard, 5, &fake_ops) == 1);
    fail_set_2 = fail_set_3 = 1;
    assert(xs8_rt_end(&guard, &fake_ops) == 2);
    assert(!guard.active && policy == SCHED_OTHER && set_calls == 4);

    reset_fake();
    assert(xs8_rt_begin(&guard, 5, &fake_ops) == 1);
    fail_set_2 = fail_set_3 = fail_set_4 = 1;
    assert(xs8_rt_end(&guard, &fake_ops) == -1 && errno == EPERM);
    assert(guard.active && policy == SCHED_FIFO && set_calls == 4);

    puts("XS8 RT scheduling tests passed");
    return 0;
}
