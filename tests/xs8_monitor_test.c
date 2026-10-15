/* Runs the real xs8_monitor.c against a scripted libgpiod: no GPIO, no bus. */
#define _POSIX_C_SOURCE 200809L
#include "xs8_gpio.h"
#include "xs8_monitor.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUSY 23
#define DATA 27
#define ARMED_NS 7000000000ULL

struct gpiod_edge_event {
    enum gpiod_edge_event_type type;
    uint64_t timestamp_ns;
    unsigned int offset;
    unsigned long global_seqno, line_seqno;
};

struct gpiod_edge_event_buffer {
    struct gpiod_edge_event events[256];
    size_t capacity, count;
};

static struct {
    struct gpiod_edge_event batches[8][16];
    size_t sizes[8];
    size_t queued, next;
    int pipe[2];
    int levels[64];
} fake;

int64_t xs8_gpio_now_ns(void) { return (int64_t)ARMED_NS; }

enum gpiod_line_value
gpiod_line_request_get_value(struct gpiod_line_request *request,
                             unsigned int offset)
{
    (void)request;
    return offset < 64 ? (enum gpiod_line_value)fake.levels[offset]
                       : GPIOD_LINE_VALUE_ERROR;
}

int gpiod_line_request_get_fd(struct gpiod_line_request *request)
{
    (void)request;
    return fake.pipe[0];
}

int gpiod_line_request_read_edge_events(struct gpiod_line_request *request,
                                        struct gpiod_edge_event_buffer *buffer,
                                        size_t max_events)
{
    (void)request;
    assert(fake.next < fake.queued);
    char byte;
    assert(read(fake.pipe[0], &byte, 1) == 1);
    size_t count = fake.sizes[fake.next];
    assert(count <= max_events && count <= buffer->capacity);
    memcpy(buffer->events, fake.batches[fake.next], count * sizeof(buffer->events[0]));
    buffer->count = count;
    ++fake.next;
    return (int)count;
}

enum gpiod_edge_event_type
gpiod_edge_event_get_event_type(struct gpiod_edge_event *event) { return event->type; }
uint64_t gpiod_edge_event_get_timestamp_ns(struct gpiod_edge_event *event)
{ return event->timestamp_ns; }
unsigned int gpiod_edge_event_get_line_offset(struct gpiod_edge_event *event)
{ return event->offset; }
unsigned long gpiod_edge_event_get_global_seqno(struct gpiod_edge_event *event)
{ return event->global_seqno; }
unsigned long gpiod_edge_event_get_line_seqno(struct gpiod_edge_event *event)
{ return event->line_seqno; }

struct gpiod_edge_event_buffer *gpiod_edge_event_buffer_new(size_t capacity)
{
    struct gpiod_edge_event_buffer *buffer = calloc(1, sizeof(*buffer));
    if (buffer) buffer->capacity = capacity < 256 ? capacity : 256;
    return buffer;
}

void gpiod_edge_event_buffer_free(struct gpiod_edge_event_buffer *buffer)
{
    free(buffer);
}

struct gpiod_edge_event *
gpiod_edge_event_buffer_get_event(struct gpiod_edge_event_buffer *buffer,
                                  unsigned long index)
{
    assert(index < buffer->count);
    return &buffer->events[index];
}

/* --- harness ------------------------------------------------------------ */

static char dummy_request;
static struct xs8_gpio bus;
static FILE *log_file;
static int saved_stderr = -1;

static void setup(int busy_level, int data_level)
{
    memset(&fake, 0, sizeof(fake));
    assert(pipe(fake.pipe) == 0);
    fake.levels[BUSY] = busy_level;
    fake.levels[DATA] = data_level;
    memset(&bus, 0, sizeof(bus));
    bus.rx_req = (struct gpiod_line_request *)&dummy_request;
    assert(pthread_mutex_init(&bus.rx_lock, NULL) == 0);
    fflush(stderr);
    log_file = tmpfile();
    assert(log_file);
    saved_stderr = dup(STDERR_FILENO);
    assert(dup2(fileno(log_file), STDERR_FILENO) >= 0);
}

/* Restores stderr and returns everything the monitor logged. */
static char *teardown(void)
{
    fflush(stderr);
    assert(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);
    static char text[8192];
    rewind(log_file);
    size_t n = fread(text, 1, sizeof(text) - 1, log_file);
    text[n] = '\0';
    fclose(log_file);
    close(fake.pipe[0]);
    close(fake.pipe[1]);
    pthread_mutex_destroy(&bus.rx_lock);
    return text;
}

/* What has been logged so far, read without moving the shared file offset. */
static const char *logged_so_far(void)
{
    static char text[8192];
    fflush(stderr);
    ssize_t n = pread(fileno(log_file), text, sizeof(text) - 1, 0);
    assert(n >= 0);
    text[n] = '\0';
    return text;
}

static void queue(const struct gpiod_edge_event *events, size_t count)
{
    assert(fake.queued < 8 && count <= 16);
    memcpy(fake.batches[fake.queued], events, count * sizeof(events[0]));
    fake.sizes[fake.queued++] = count;
    assert(write(fake.pipe[1], "x", 1) == 1);
}

static struct gpiod_edge_event edge(unsigned int offset, unsigned long seq,
                                    unsigned long line_seq, uint64_t after_arm_ns,
                                    enum gpiod_edge_event_type type)
{
    return (struct gpiod_edge_event){type, ARMED_NS + after_arm_ns, offset,
                                     seq, line_seq};
}

static int count_of(const char *text, const char *needle)
{
    int n = 0;
    for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle)) ++n;
    return n;
}

/* --- tests -------------------------------------------------------------- */

static void test_arming_logs_levels_and_clean_events_stay_healthy(void)
{
    setup(0, 1);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    struct gpiod_edge_event events[] = {
        edge(DATA, 1, 1, 1000, GPIOD_EDGE_EVENT_FALLING_EDGE),
        edge(DATA, 2, 2, 2000, GPIOD_EDGE_EVENT_RISING_EDGE),
    };
    queue(events, 2);
    assert(xs8_monitor_poll(monitor, 0) == 0);
    assert(xs8_monitor_healthy(monitor));
    xs8_monitor_stop(monitor);
    char *log = teardown();
    assert(strstr(log, "XS8 OBSERVED_BUS monitor armed mono_us=7000000 "
                       "busy=LOW data=HIGH\n"));
    assert(!strstr(log, "monitor FAILED"));
    assert(strstr(log, "total_events=2 busy_edges=0 data_edges=2 "
                       "global_sequence_gaps=0 missing_events=0 "
                       "sequence_regressions=0 timestamp_regressions=0 "));
}

/* The observed cold-boot failure: one batch of three DATA edges, first seqno 3. */
static void test_cold_boot_gap_fails_closed_and_logs_once(void)
{
    setup(0, 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    struct gpiod_edge_event events[] = {
        edge(DATA, 3, 1, 120000, GPIOD_EDGE_EVENT_FALLING_EDGE),
        edge(DATA, 4, 2, 125000, GPIOD_EDGE_EVENT_RISING_EDGE),
        edge(DATA, 5, 3, 130000, GPIOD_EDGE_EVENT_FALLING_EDGE),
    };
    queue(events, 3);
    assert(xs8_monitor_poll(monitor, 0) == 0);
    assert(!xs8_monitor_healthy(monitor));
    /* Logged when detected, not only in the shutdown summary. */
    assert(strstr(logged_so_far(), "monitor FAILED reason=SEQUENCE_GAP"));
    assert(!strstr(logged_so_far(), "OBSERVED_BUS summary"));
    /* Fail-closed is unchanged: a failed manual monitor refuses further polls. */
    assert(xs8_monitor_poll(monitor, 0) == -1);
    assert(xs8_monitor_verify_tx(monitor, 0, 0x13, 0, 1, 10) == XS8_OBS_UNHEALTHY);
    xs8_monitor_stop(monitor);
    char *log = teardown();
    assert(count_of(log, "monitor FAILED") == 1);
    assert(strstr(log,
        "XS8 OBSERVED_BUS monitor FAILED reason=SEQUENCE_GAP gpio=27 line=DATA "
        "expected_seqno=1 actual_seqno=3 line_seqno=1 event_index=1 "
        "event_mono_us=7000120 previous_event_mono_us=NONE since_arm_us=120\n"));
    assert(strstr(log, "total_events=3 busy_edges=0 data_edges=3 "
                       "global_sequence_gaps=1 missing_events=2 "
                       "sequence_regressions=0 timestamp_regressions=0 max_batch=3 "));
    /* The FAILED line precedes the shutdown summary. */
    assert(strstr(log, "monitor FAILED") < strstr(log, "OBSERVED_BUS summary"));
}

static void test_sequence_regression_across_batches(void)
{
    setup(0, 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    struct gpiod_edge_event first[] = {
        edge(BUSY, 1, 1, 1000, GPIOD_EDGE_EVENT_RISING_EDGE),
        edge(BUSY, 2, 2, 2000, GPIOD_EDGE_EVENT_FALLING_EDGE),
    };
    struct gpiod_edge_event second[] = {
        edge(BUSY, 2, 3, 3000, GPIOD_EDGE_EVENT_RISING_EDGE),
    };
    queue(first, 2);
    assert(xs8_monitor_poll(monitor, 0) == 0);
    assert(xs8_monitor_healthy(monitor));
    queue(second, 1);
    assert(xs8_monitor_poll(monitor, 0) == 0);
    assert(!xs8_monitor_healthy(monitor));
    xs8_monitor_stop(monitor);
    char *log = teardown();
    assert(strstr(log, "reason=SEQUENCE_REGRESSION gpio=23 line=BUSY "
                       "expected_seqno=3 actual_seqno=2 line_seqno=3 event_index=3 "
                       "event_mono_us=7000003 previous_event_mono_us=7000002 "
                       "since_arm_us=3\n"));
    assert(strstr(log, "global_sequence_gaps=0 missing_events=0 "
                       "sequence_regressions=1 timestamp_regressions=0 "));
}

static void test_timestamp_regression(void)
{
    setup(0, 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    struct gpiod_edge_event events[] = {
        edge(DATA, 1, 1, 9000, GPIOD_EDGE_EVENT_RISING_EDGE),
        edge(DATA, 2, 2, 8000, GPIOD_EDGE_EVENT_FALLING_EDGE),
    };
    queue(events, 2);
    assert(xs8_monitor_poll(monitor, 0) == 0);
    assert(!xs8_monitor_healthy(monitor));
    xs8_monitor_stop(monitor);
    char *log = teardown();
    assert(count_of(log, "monitor FAILED") == 1);
    assert(strstr(log, "reason=TIMESTAMP_REGRESSION gpio=27 line=DATA "));
    assert(strstr(log, "sequence_regressions=0 timestamp_regressions=1 "));
}

/* A shutdown also marks the monitor unhealthy; that is not a capture fault. */
static void test_clean_shutdown_logs_no_fault(void)
{
    setup(1, 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    xs8_monitor_stop(monitor);
    char *log = teardown();
    assert(strstr(log, "busy=HIGH data=LOW\n"));
    assert(!strstr(log, "monitor FAILED"));
    assert(strstr(log, "OBSERVED_BUS summary total_events=0 "));
}

static void test_unreadable_data_level_does_not_block_start(void)
{
    setup(0, GPIOD_LINE_VALUE_ERROR);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    assert(xs8_monitor_healthy(monitor));
    xs8_monitor_stop(monitor);
    char *log = teardown();
    assert(strstr(log, "busy=LOW data=UNKNOWN\n"));
}

/* An unreadable BUSY level still refuses to start, exactly as before. */
static void test_unreadable_busy_level_still_fails_start(void)
{
    setup(GPIOD_LINE_VALUE_ERROR, 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == -1);
    assert(monitor == NULL);
    char *log = teardown();
    assert(!strstr(log, "monitor armed"));
}

int main(void)
{
    test_arming_logs_levels_and_clean_events_stay_healthy();
    test_cold_boot_gap_fails_closed_and_logs_once();
    test_sequence_regression_across_batches();
    test_timestamp_regression();
    test_clean_shutdown_logs_no_fault();
    test_unreadable_data_level_does_not_block_start();
    test_unreadable_busy_level_still_fails_start();
    puts("xs8_monitor_test: OK");
    return 0;
}
