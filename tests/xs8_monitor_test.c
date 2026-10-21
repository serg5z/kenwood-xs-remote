/* Runs the real xs8_monitor.c against a scripted libgpiod: no GPIO, no bus. */
#define _POSIX_C_SOURCE 200809L
#include "xs8_gpio.h"
#include "xs8_monitor.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUSY 26
#define DATA 13
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
    struct gpiod_edge_event batches[8][128];
    size_t sizes[8];
    size_t queued, next;
    int pipe[2];
    int levels[64];
    bool read_error;
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
    if (fake.read_error) { errno = EIO; return -1; }
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
    assert(unsetenv("XS8_GLOBAL_GAP_POLICY") == 0);
    assert(unsetenv("XS8_FAILURE_SNAPSHOTS") == 0);
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
    static char text[65536];
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
    static char text[65536];
    fflush(stderr);
    ssize_t n = pread(fileno(log_file), text, sizeof(text) - 1, 0);
    assert(n >= 0);
    text[n] = '\0';
    return text;
}

static void queue(const struct gpiod_edge_event *events, size_t count)
{
    /* Streaming CSV replay drains one original batch before queuing the next. */
    if (fake.next == fake.queued) fake.next = fake.queued = 0;
    assert(fake.queued < 8 && count <= 128);
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
        "XS8 OBSERVED_BUS monitor FAILED reason=SEQUENCE_GAP gpio=13 line=DATA "
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
    assert(strstr(log, "reason=SEQUENCE_REGRESSION gpio=26 line=BUSY "
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
    assert(strstr(log, "reason=TIMESTAMP_REGRESSION gpio=13 line=DATA "));
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

/* Counter continuity is assessed independently of the production decoder.
 * Dense numbering models an edge absent before event numbering; retaining the
 * original counters models a record lost after numbering. Neither proves the
 * electrical cause of an actual kernel discontinuity. */
static void number_trace(struct gpiod_edge_event *events, size_t count)
{
    unsigned long lines[64] = {0};
    for (size_t i = 0; i < count; ++i) {
        events[i].global_seqno = i + 1;
        events[i].line_seqno = ++lines[events[i].offset];
    }
}

static size_t frame_trace(struct gpiod_edge_event *events, uint8_t byte)
{
    uint64_t t = 1000000;
    size_t n = 0;
    events[n++] = edge(BUSY, 0, 0, t, GPIOD_EDGE_EVENT_RISING_EDGE);
    t += 1150000;
    events[n++] = edge(DATA, 0, 0, t, GPIOD_EDGE_EVENT_RISING_EDGE);
    t += 9800000;
    events[n++] = edge(DATA, 0, 0, t, GPIOD_EDGE_EVENT_FALLING_EDGE);
    for (unsigned int bit = 0; bit < 8; ++bit) {
        t += (byte & (1u << bit)) ? 9800000 : 4900000;
        events[n++] = edge(DATA, 0, 0, t, GPIOD_EDGE_EVENT_RISING_EDGE);
        t += 4900000;
        events[n++] = edge(DATA, 0, 0, t, GPIOD_EDGE_EVENT_FALLING_EDGE);
    }
    events[n++] = edge(BUSY, 0, 0, t + 750000,
                       GPIOD_EDGE_EVENT_FALLING_EDGE);
    number_trace(events, n);
    assert(n == 20);
    return n;
}

struct decoded_trace {
    struct xs8_decoded records[8];
    size_t count;
};

static void save_decoded(void *user, const struct xs8_decoded *record)
{
    struct decoded_trace *trace = user;
    assert(trace->count < 8);
    trace->records[trace->count++] = *record;
}

enum injection { CONTROL, GLOBAL_ONLY, MISSING_DATA_GAP, MISSING_DATA_DENSE,
                 EXTRA_BUSY, MISSING_TAIL_GAP, MISSING_TAIL_DENSE };

static void test_frame_injection(enum injection injection, bool split_batch, bool warn)
{
    static const char *names[] = {"control", "global-only-2-to-5",
        "missing-data-retained-counters", "missing-data-dense-counters",
        "extra-busy-rise", "missing-tail-retained-counters",
        "missing-tail-dense-counters"};
    struct gpiod_edge_event events[21];
    size_t n = frame_trace(events, 0x84);
    const uint64_t start = events[0].timestamp_ns;
    const uint64_t end = events[n - 1].timestamp_ns;
    if (injection == GLOBAL_ONLY) {
        for (size_t i = 2; i < n; ++i) events[i].global_seqno += 2;
        assert(events[2].global_seqno == 5 && events[2].line_seqno == 2);
    } else if (injection == MISSING_DATA_GAP || injection == MISSING_DATA_DENSE ||
               injection == MISSING_TAIL_GAP || injection == MISSING_TAIL_DENSE) {
        size_t removed = (injection == MISSING_TAIL_GAP ||
                          injection == MISSING_TAIL_DENSE) ? n - 2 : 3;
        assert(events[removed].offset == DATA);
        memmove(events + removed, events + removed + 1,
                (n - removed - 1) * sizeof(events[0]));
        --n;
        if (injection == MISSING_DATA_DENSE || injection == MISSING_TAIL_DENSE)
            number_trace(events, n);
    } else if (injection == EXTRA_BUSY) {
        /* Deliberately inconsistent BUSY rising event while BUSY is HIGH. */
        memmove(events + 5, events + 4, (n - 4) * sizeof(events[0]));
        events[4] = edge(BUSY, 0, 0, events[3].timestamp_ns - ARMED_NS + 1000000,
                         GPIOD_EDGE_EVENT_RISING_EDGE);
        number_trace(events, ++n);
    }

    unsigned long global = 0, lines[64] = {0};
    unsigned int global_breaks = 0, line_breaks = 0;
    for (size_t i = 0; i < n; ++i) {
        global_breaks += events[i].global_seqno != global + 1;
        line_breaks += events[i].line_seqno != lines[events[i].offset] + 1;
        global = events[i].global_seqno;
        lines[events[i].offset] = events[i].line_seqno;
    }
    bool gap = injection == GLOBAL_ONLY || injection == MISSING_DATA_GAP ||
               injection == MISSING_TAIL_GAP;
    bool fatal_gap = gap && (!warn || injection == MISSING_DATA_GAP);
    assert(global_breaks == (gap ? 1u : 0u));
    /* A missing final DATA event has no later DATA counter to expose its loss. */
    assert(line_breaks == (injection == MISSING_DATA_GAP ? 1u : 0u));

    /* Replay unchanged events through the real decoder to inspect its final
     * internal state, then compare every emitted record to the real monitor. */
    struct xs8_decoder decoder;
    struct decoded_trace decoded = {0};
    xs8_decoder_init(&decoder, true);
    for (size_t i = 0; i < n; ++i) {
        struct xs8_edge e = {events[i].timestamp_ns, events[i].global_seqno,
            events[i].offset, events[i].type == GPIOD_EDGE_EVENT_RISING_EDGE
                ? XS8_RISING : XS8_FALLING};
        if (warn && gap && !fatal_gap && e.sequence > decoder.last_sequence + 1)
            decoder.last_sequence = e.sequence - 1;
        xs8_decoder_edge(&decoder, &e, save_decoded, &decoded);
    }
    assert(!decoder.active && decoder.synced);
    assert(decoded.count > 0);
    static const unsigned int expected_bits[] = {8, 0, 0, 7, 6, 8, 8};
    static const unsigned int expected_stage[] = {4, 1, 2, 2, 2, 4, 4};
    static const char *first_reason[] = {NULL, "EVENT_GAP", "EVENT_GAP",
        "UNEXPECTED_DATA_EDGE", "NEW_BUSY_RISE", "EVENT_GAP", NULL};
    bool bridged = warn && gap && !fatal_gap;
    const char *reason = bridged ? NULL : first_reason[injection];
    assert(decoder.bits == (bridged ? 8u : expected_bits[injection]));
    assert(decoder.stage == (bridged ? 4u : expected_stage[injection]));
    assert(reason ? decoded.records[0].reason &&
        strcmp(decoded.records[0].reason, reason) == 0 :
        decoded.records[0].reason == NULL);

    setup(0, 0);
    if (warn) assert(setenv("XS8_GLOBAL_GAP_POLICY", "warn", 1) == 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    uint64_t checkpoint = xs8_monitor_checkpoint(monitor);
    if (split_batch) {
        queue(events, 2);
        assert(xs8_monitor_poll(monitor, 0) == 0);
        assert(xs8_monitor_healthy(monitor));
    }
    queue(events + (split_batch ? 2 : 0), n - (split_batch ? 2 : 0));
    assert(xs8_monitor_poll(monitor, 0) == 0);
    bool healthy = xs8_monitor_healthy(monitor);
    assert(healthy == !fatal_gap);
    struct xs8_monitor_record records[8];
    size_t count = 0;
    uint64_t cursor = checkpoint;
    assert(xs8_monitor_snapshot(monitor, &cursor, records, 8, &count) == 0);
    assert(count == decoded.count);
    unsigned int valid = 0, invalid = 0, gaps = 0;
    for (size_t i = 0; i < count; ++i) {
        const struct xs8_decoded *actual = &records[i].decoded;
        const struct xs8_decoded *expected = &decoded.records[i];
        assert(actual->kind == expected->kind && actual->valid == expected->valid);
        assert(actual->time_ns == expected->time_ns && actual->bits == expected->bits);
        assert(actual->waveform_lsb == expected->waveform_lsb);
        assert(actual->legacy_msb_short1 == expected->legacy_msb_short1);
        assert(actual->lost_events == expected->lost_events);
        if (actual->kind == XS8_FRAME && bridged)
            assert(actual->global_gap_warnings == 1);
        assert(actual->tail_fell == expected->tail_fell);
        assert(actual->final_data_rise_ns == expected->final_data_rise_ns);
        assert(actual->final_data_fall_ns == expected->final_data_fall_ns);
        assert(actual->busy_fall_ns == expected->busy_fall_ns);
        assert((!actual->reason && !expected->reason) ||
               (actual->reason && expected->reason &&
                strcmp(actual->reason, expected->reason) == 0));
        if (actual->kind == XS8_EVENT_GAP) ++gaps;
        else if (actual->valid) ++valid;
        else ++invalid;
    }
    bool passive_valid = injection == CONTROL || injection == MISSING_TAIL_DENSE || bridged;
    bool accepted = injection == CONTROL || (warn && injection == GLOBAL_ONLY);
    assert(valid == (passive_valid ? 1u : 0u));
    assert(invalid == (passive_valid ? 0u : injection == EXTRA_BUSY ? 2u : 1u));
    assert(gaps == (fatal_gap ? 1u : 0u));
    if (passive_valid) assert(records[0].decoded.waveform_lsb == 0x84);
    enum xs8_observation outcome = xs8_monitor_verify_tx(
        monitor, checkpoint, 0x84, start, end, 1);
    assert(outcome == (fatal_gap ? XS8_OBS_UNHEALTHY :
                       accepted ? XS8_OBS_MATCH : XS8_OBS_INVALID));
    if (fatal_gap) assert(xs8_monitor_poll(monitor, 0) == -1);
    xs8_monitor_stop(monitor);
    const char *log = teardown();
    assert(count_of(log, "monitor FAILED") == (fatal_gap ? 1 : 0));
    if (fatal_gap) assert(strstr(log, "monitor FAILED reason=SEQUENCE_GAP"));
    assert(count_of(log, "WARNING reason=GLOBAL_ONLY_SEQUENCE_GAP") == (bridged ? 1 : 0));
    if (injection == GLOBAL_ONLY && !warn) assert(strstr(log,
        "expected_seqno=3 actual_seqno=5 line_seqno=2 event_index=3"));

    printf("fault-injection case=%s policy=%s batches=%u health=%s failure=%s "
           "global_breaks=%u line_breaks=%u decoder_active=%u synced=%u "
           "stage=%u bits=%u valid_frames=%u invalid_frames=%u gap_records=%u "
           "first_reason=%s verification=%s rejected=%s\n",
           names[injection], warn ? "warn" : "strict", split_batch ? 2 : 1,
           healthy ? "HEALTHY" : "FAILED",
           fatal_gap ? "SEQUENCE_GAP" : "NONE", global_breaks, line_breaks,
           decoder.active, decoder.synced, decoder.stage, decoder.bits,
           valid, invalid, gaps, decoded.records[0].reason ?
           decoded.records[0].reason : "NONE", fatal_gap ? "UNHEALTHY" :
           accepted ? "MATCH" : "INVALID", accepted ? "no" : "yes");
}

static void check_tail_trace(const struct gpiod_edge_event *input, size_t count,
                            uint8_t byte, bool passive_valid, bool local_match,
                            bool tail_fell, bool warn, bool global_gap)
{
    struct gpiod_edge_event events[20];
    assert(count <= 20);
    memcpy(events, input, count * sizeof(events[0]));
    if (global_gap)
        for (size_t i = 2; i < count; ++i) events[i].global_seqno += 2;
    setup(0, 0);
    if (warn) assert(setenv("XS8_GLOBAL_GAP_POLICY", "warn", 1) == 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    queue(events, count);
    assert(xs8_monitor_poll(monitor, 0) == 0);
    assert(xs8_monitor_healthy(monitor));
    struct xs8_monitor_record records[2];
    uint64_t cursor = 0;
    size_t copied = 0;
    assert(xs8_monitor_snapshot(monitor, &cursor, records, 2, &copied) == 0);
    assert(copied == 1);
    const struct xs8_decoded *frame = &records[0].decoded;
    assert(frame->valid == passive_valid && frame->bits == 8);
    assert(frame->waveform_lsb == byte && frame->tail_fell == tail_fell);
    assert(frame->final_data_rise_ns == events[17].timestamp_ns);
    assert(frame->final_data_fall_ns == (tail_fell ? events[18].timestamp_ns : 0));
    assert(frame->busy_fall_ns == events[tail_fell ? 19 : 18].timestamp_ns);
    assert(xs8_decoded_local_tx_tail_valid(frame) == local_match);
    assert(xs8_monitor_verify_tx(monitor, 0, byte, events[0].timestamp_ns,
        events[count - 1].timestamp_ns, 1) ==
        (local_match ? XS8_OBS_MATCH : XS8_OBS_INVALID));
    xs8_monitor_stop(monitor);
    assert(!strstr(teardown(), "monitor FAILED"));
}

static void test_archived_passive_tails(void)
{
    /* Full sampled timings, us relative to the first BUSY rise:
     * power-off-on.csv lines 3..22 (BUSY-first), 23..42 (DATA-first),
     * xs-session2.csv lines 3..21 (both falls in the same ADC sample).
     * A joint sample does not establish electrically simultaneous edges;
     * replay both possible delivery orders with equal timestamps. */
    static const unsigned int us[3][20] = {
        {0,1168,11064,15816,20784,25764,30728,40400,45360,50328,
         55296,64968,69936,74904,79888,84632,89600,94568,99312,100024},
        {0,936,10840,20520,25264,34952,39928,49600,54568,59304,
         64272,69032,74016,78760,83720,93392,98136,103104,107856,108576},
        {0,1160,11056,16024,20984,25720,30696,40600,45568,50312,
         55280,60248,65216,69952,74912,79880,84848,94520,99488,99488}
    };
    static const uint8_t bytes[] = {0x14, 0x47, 0x84};
    for (unsigned int variant = 0; variant < 4; ++variant) {
        unsigned int capture = variant < 3 ? variant : 2;
        struct gpiod_edge_event events[20];
        assert(frame_trace(events, bytes[capture]) == 20);
        bool data_first = variant == 1 || variant == 2;
        if (!data_first) {
            struct gpiod_edge_event swap = events[18];
            events[18] = events[19];
            events[19] = swap;
        }
        for (size_t i = 0; i < 20; ++i)
            events[i].timestamp_ns = ARMED_NS + 1000000 + (uint64_t)us[capture][i] * 1000;
        number_trace(events, 20);
        check_tail_trace(events, 20, bytes[capture], true, variant == 1, data_first, false, false);
        check_tail_trace(events, 20, bytes[capture], true, variant == 1, data_first, true, false);
        check_tail_trace(events, 20, bytes[capture], true, variant == 1, data_first, true, true);
    }
}

static void test_incompatible_local_tail_timing(void)
{
    /* HIGH outside the existing 3..7 ms decoder tolerance, absent/reversed
     * guard (archived tests above), or guard beyond local verification's 2 ms.
     * Positive guard boundary cases document the verification tolerance. */
    static const struct { uint64_t high, guard; bool valid, match; } cases[] = {
        {2999999, 750000, false, false},
        {7000001, 750000, false, false},
        {4900000, 0, true, false},
        {4900000, 2000001, true, false},
        {4900000, 1, true, true},
        {4900000, 2000000, true, true}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct gpiod_edge_event events[20];
        assert(frame_trace(events, 0x84) == 20);
        events[18].timestamp_ns = events[17].timestamp_ns + cases[i].high;
        events[19].timestamp_ns = events[18].timestamp_ns + cases[i].guard;
        check_tail_trace(events, 20, 0x84, cases[i].valid, cases[i].match, true, false, false);
        check_tail_trace(events, 20, 0x84, cases[i].valid, cases[i].match, true, true, false);
        check_tail_trace(events, 20, 0x84, cases[i].valid, cases[i].match, true, true, true);
    }
}

static void test_integrity_failures_in_both_policies(void)
{
    static const char *reason[] = {"LINE_SEQUENCE_GAP", "LINE_SEQUENCE_GAP",
        "LINE_SEQUENCE_REGRESSION", "SEQUENCE_REGRESSION", "TIMESTAMP_REGRESSION",
        "SEQUENCE_GAP+TIMESTAMP_REGRESSION", "event read failed", "UNEXPECTED_LINE"};
    for (unsigned int warn = 0; warn < 2; ++warn) {
        for (unsigned int fault = 0; fault < 8; ++fault) {
            struct gpiod_edge_event events[20];
            assert(frame_trace(events, 0x84) == 20);
            if (fault == 0) {
                for (size_t i = 2; i < 20; ++i)
                    if (events[i].offset == DATA) ++events[i].line_seqno;
            } else if (fault == 1) ++events[19].line_seqno;
            else if (fault == 2) {
                for (size_t i = 2; i < 20; ++i)
                    if (events[i].offset == DATA) --events[i].line_seqno;
            } else if (fault == 3) {
                for (size_t i = 2; i < 20; ++i) --events[i].global_seqno;
            } else if (fault == 4 || fault == 5) {
                events[2].timestamp_ns = events[1].timestamp_ns - 1;
                if (fault == 5)
                    for (size_t i = 2; i < 20; ++i) events[i].global_seqno += 2;
            } else if (fault == 7) events[2].offset = 99;
            setup(0, 0);
            if (warn) assert(setenv("XS8_GLOBAL_GAP_POLICY", "warn", 1) == 0);
            struct xs8_monitor *monitor = NULL;
            assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
            queue(events, 20);
            if (fault == 6) fake.read_error = true;
            assert(xs8_monitor_poll(monitor, 0) == (fault == 6 ? -1 : 0));
            assert(!xs8_monitor_healthy(monitor));
            assert(xs8_monitor_verify_tx(monitor, 0, 0x84,
                events[0].timestamp_ns, events[19].timestamp_ns, 1) == XS8_OBS_UNHEALTHY);
            xs8_monitor_stop(monitor);
            const char *log = teardown();
            assert(strstr(log, reason[fault]));
            assert(!strstr(log, "WARNING reason=GLOBAL_ONLY_SEQUENCE_GAP"));
            if (fault <= 2) assert(strstr(log, "expected_line_seqno="));
        }
    }
}

static void test_policy_configuration(void)
{
    setup(0, 0);
    assert(setenv("XS8_GLOBAL_GAP_POLICY", "strict", 1) == 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    assert(xs8_monitor_healthy(monitor));
    xs8_monitor_stop(monitor);
    assert(strstr(teardown(), "global_gap_policy=strict\n"));
    setup(0, 0);
    assert(setenv("XS8_GLOBAL_GAP_POLICY", "WARN", 1) == 0);
    monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == -1 && errno == EINVAL);
    assert(monitor == NULL);
    assert(strstr(teardown(), "invalid XS8_GLOBAL_GAP_POLICY"));
}

static void test_warn_gap_does_not_hide_malformed_or_later_fault(void)
{
    for (unsigned int variant = 0; variant < 4; ++variant) {
        struct gpiod_edge_event events[21];
        size_t count = frame_trace(events, 0x84);
        if (variant == 0) {
            memmove(events + 3, events + 4, (count - 4) * sizeof(events[0]));
            number_trace(events, --count); /* Missing DATA edge before numbering. */
        } else if (variant == 1) {
            memmove(events + 5, events + 4, (count - 4) * sizeof(events[0]));
            events[4] = edge(BUSY, 0, 0, events[3].timestamp_ns - ARMED_NS + 1000000,
                             GPIOD_EDGE_EVENT_RISING_EDGE);
            number_trace(events, ++count);
        } else if (variant == 2) count = 10; /* No frame end. */
        else ++events[18].line_seqno; /* Later per-line failure after a warning. */
        for (size_t i = 2; i < count; ++i) events[i].global_seqno += 2;
        setup(0, 0);
        assert(setenv("XS8_GLOBAL_GAP_POLICY", "warn", 1) == 0);
        struct xs8_monitor *monitor = NULL;
        assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
        queue(events, count);
        assert(xs8_monitor_poll(monitor, 0) == 0);
        assert(xs8_monitor_healthy(monitor) == (variant != 3));
        enum xs8_observation result = xs8_monitor_verify_tx(monitor, 0, 0x84,
            events[0].timestamp_ns, events[count - 1].timestamp_ns, 1);
        assert(result == (variant == 2 ? XS8_OBS_MISSING :
                          variant == 3 ? XS8_OBS_UNHEALTHY : XS8_OBS_INVALID));
        xs8_monitor_stop(monitor);
        const char *log = teardown();
        assert(count_of(log, "WARNING reason=GLOBAL_ONLY_SEQUENCE_GAP") == 1);
        assert(strstr(log, "global_gap_warnings=1"));
        assert(strstr(log, "global_sequence_gaps=1 missing_events=2"));
        assert(strstr(log, "frame_start_ns=7001000000 bits=0 stage=1"));
        assert(count_of(log, "monitor FAILED") == (variant == 3 ? 1 : 0));
        if (variant == 3) assert(strstr(log, "reason=LINE_SEQUENCE_GAP"));
    }
}

static void test_tx_attribution_snapshot(void)
{
    /* Split batches are deliberately queued before verify: a MATCH in the first
     * batch must not hide a second relevant frame/partial in the next batch. */
    for (unsigned int warn = 0; warn < 2; ++warn) {
        for (unsigned int variant = 0; variant < 8; ++variant) {
            struct gpiod_edge_event events[40];
            assert(frame_trace(events, 0x84) == 20);
            uint64_t end = events[19].timestamp_ns;
            assert(frame_trace(events + 20, 0x84) == 20);
            uint64_t shift = end - ARMED_NS + 2000000;
            for (size_t i = 20; i < 40; ++i) events[i].timestamp_ns += shift;
            if (variant == 2 || variant == 5)
                events[38].timestamp_ns = events[37].timestamp_ns + 1000000;
            size_t count = (variant == 3 || variant == 6) ? 21 :
                           variant == 0 || variant == 7 ? 20 : 40;
            if (variant >= 1 && variant <= 3) end = events[count - 1].timestamp_ns;
            else end += 1000000; /* Second frame, if any, begins outside TX. */
            number_trace(events, count);
            if (warn)
                for (size_t i = 2; i < count; ++i) events[i].global_seqno += 2;
            setup(0, 0);
            if (warn) assert(setenv("XS8_GLOBAL_GAP_POLICY", "warn", 1) == 0);
            struct xs8_monitor *monitor = NULL;
            assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
            queue(events, 20);
            if (count > 20) queue(events + 20, count - 20);
            enum xs8_observation expected = variant >= 1 && variant <= 3
                ? XS8_OBS_INVALID : variant == 7 ? XS8_OBS_MISMATCH : XS8_OBS_MATCH;
            assert(xs8_monitor_verify_tx(monitor, 0, variant == 7 ? 0x82 : 0x84,
                events[0].timestamp_ns, end, 1) == expected);
            struct xs8_monitor_diagnostics info;
            assert(xs8_monitor_get_diagnostics(monitor, &info));
            assert(info.healthy && info.warn_global_gaps == (warn != 0));
            assert(info.global_gap_warnings == warn);
            xs8_monitor_stop(monitor);
            assert(!strstr(teardown(), "monitor FAILED"));
        }
    }
}

/* Real event-order capture replay, not synthesized timings or renumbered edges.
 * Only this offline backend supplies levels: initial BUSY/DATA LOW is assumed
 * for the idle disconnected fixture, not recorded as an electrical measurement.
 * read_ns is retained as metadata; wall-clock scheduling is not reproduced. */
struct csv_edge {
    struct gpiod_edge_event event;
    uint64_t batch, read_ns;
    unsigned int batch_index, batch_size;
};

static void test_csv_replay(const char *path, bool warn)
{
    FILE *input = fopen(path, "r");
    assert(input);
    char line[512];
    assert(fgets(line, sizeof(line), input));
    assert(!strcmp(line, "timestamp_ns,gpio,edge,global_seqno,line_seqno,batch,"
                         "batch_index,batch_size,read_ns,sequence_status\n"));
    struct csv_edge rows[1024];
    size_t count = 0, first_gap = SIZE_MAX;
    unsigned int gaps = 0;
    uint64_t previous_global = 0, previous_line[64] = {0};
    while (fgets(line, sizeof(line), input)) {
        assert(count < sizeof(rows) / sizeof(rows[0]));
        struct csv_edge *r = &rows[count];
        char polarity[8], annotation[32];
        int used = 0;
        assert(sscanf(line, "%" SCNu64 ",%u,%7[^,],%lu,%lu,%" SCNu64
                           ",%u,%u,%" SCNu64 ",%31[^\r\n]%n",
            &r->event.timestamp_ns, &r->event.offset, polarity,
            &r->event.global_seqno, &r->event.line_seqno, &r->batch,
            &r->batch_index, &r->batch_size, &r->read_ns, annotation, &used) == 10);
        assert(strspn(line + used, "\r\n") == strlen(line + used));
        assert(!strcmp(polarity, "rising") || !strcmp(polarity, "falling"));
        r->event.type = !strcmp(polarity, "rising")
            ? GPIOD_EDGE_EVENT_RISING_EDGE : GPIOD_EDGE_EVENT_FALLING_EDGE;
        assert(r->event.offset == BUSY || r->event.offset == DATA);
        assert(r->event.timestamp_ns <= INT64_MAX && r->read_ns >= r->event.timestamp_ns);
        assert(r->event.global_seqno <= UINT32_MAX && r->event.line_seqno <= UINT32_MAX);
        assert(r->event.global_seqno > previous_global);
        if (r->event.global_seqno != previous_global + 1) {
            if (first_gap == SIZE_MAX) first_gap = count;
            ++gaps;
        }
        assert(r->event.line_seqno == previous_line[r->event.offset] + 1);
        previous_line[r->event.offset] = r->event.line_seqno;
        previous_global = r->event.global_seqno;
        assert(!count || r->event.timestamp_ns >= rows[count - 1].event.timestamp_ns);
        assert(r->batch_size > 0 && r->batch_size <= 128 && r->batch_index < r->batch_size);
        ++count;
    }
    assert(!ferror(input));
    fclose(input);
    assert(count == 80 && gaps == 3 && first_gap == 6);
    assert(previous_line[DATA] == 72 && previous_line[BUSY] == 8);

    setup(0, 0);
    if (warn) assert(setenv("XS8_GLOBAL_GAP_POLICY", "warn", 1) == 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    uint64_t cursor = xs8_monitor_checkpoint(monitor);
    struct xs8_monitor_record decoded[16];
    size_t decoded_count = 0, delivered = 0, batches = 0;
    bool sequence_matches = true;
    uint64_t last_batch = 0;
    for (size_t i = 0; i < count;) {
        const struct csv_edge *first = &rows[i];
        size_t batch_size = first->batch_size;
        assert(first->batch_index == 0 && first->batch > last_batch && i + batch_size <= count);
        struct gpiod_edge_event events[128];
        for (size_t j = 0; j < batch_size; ++j) {
            const struct csv_edge *r = &rows[i + j];
            assert(r->batch == first->batch && r->batch_index == j &&
                   r->batch_size == batch_size && r->read_ns == first->read_ns);
            events[j] = r->event; /* Preserve timestamp, GPIO, polarity and both counters. */
        }
        last_batch = first->batch;
        queue(events, batch_size);
        assert(xs8_monitor_poll(monitor, 0) == 0);
        assert(fake.next == fake.queued);
        i += batch_size;
        delivered = i;
        ++batches;
        size_t copied = 0;
        assert(decoded_count < 16);
        assert(xs8_monitor_snapshot(monitor, &cursor, decoded + decoded_count,
                                   16 - decoded_count, &copied) == 0);
        decoded_count += copied;
        if (!xs8_monitor_healthy(monitor)) {
            assert(!warn && i - batch_size <= first_gap && i > first_gap);
            assert(xs8_monitor_poll(monitor, 0) == -1 && errno == EIO);
            break; /* Production manual mode stops consuming after the fatal batch. */
        }
    }
    struct xs8_monitor_diagnostics info;
    assert(xs8_monitor_get_diagnostics(monitor, &info));
    if (warn) {
        assert(info.healthy && info.global_gap_warnings == 3);
        assert(delivered == count && decoded_count == 4);
        /* Operator confirmed this captured experiment was UP, not DOWN. */
        const uint8_t expected[] = {0x17, 0xF9, 0x09, 0x07};
        uint64_t frame_warnings = 0;
        for (size_t i = 0; i < decoded_count; ++i) {
            const struct xs8_decoded *frame = &decoded[i].decoded;
            frame_warnings += frame->global_gap_warnings;
            printf("csv_frame policy=warn byte=0x%02X legacy=0x%02X start_ns=%" PRIu64
                   " end_ns=%" PRIu64 " bits=%u global_warnings=%" PRIu64 "\n",
                   frame->waveform_lsb, frame->legacy_msb_short1,
                   frame->time_ns, decoded[i].end_ns, frame->bits, frame->global_gap_warnings);
        }
        fflush(stdout);
        for (size_t i = 0; i < decoded_count; ++i) {
            const struct xs8_decoded *frame = &decoded[i].decoded;
            assert(frame->kind == XS8_FRAME && frame->valid && frame->bits == 8);
            assert(!frame->reason);
            if (frame->waveform_lsb != expected[i]) {
                printf("csv_sequence_mismatch frame=%zu expected=0x%02X actual=0x%02X\n",
                       i + 1, expected[i], frame->waveform_lsb);
                sequence_matches = false;
            }
            assert(xs8_decoded_local_tx_tail_valid(frame));
        }
        assert(frame_warnings == 3);
        struct xs8_monitor_record partial;
        assert(!xs8_monitor_partial(monitor, rows[count - 1].read_ns, &partial));
    } else {
        assert(!info.healthy && info.global_gap_warnings == 0);
        assert(delivered == first_gap + 1); /* This fixture has one event per batch. */
        assert(xs8_monitor_verify_tx(monitor, 0, 0x17,
            rows[0].event.timestamp_ns, rows[19].event.timestamp_ns, 1) == XS8_OBS_UNHEALTHY);
    }
    xs8_monitor_stop(monitor);
    const char *log = teardown();
    assert(count_of(log, "monitor FAILED") == (warn ? 0 : 1));
    assert(count_of(log, "WARNING reason=GLOBAL_ONLY_SEQUENCE_GAP") == (warn ? 3 : 0));
    if (warn) {
        assert(!strstr(log, "status=INCOMPLETE"));
        assert(strstr(log, "line_sequence_gaps=0 line_missing_events=0 line_sequence_regressions=0"));
        assert(strstr(log, "global_sequence_gaps=3 missing_events=3"));
    } else {
        assert(strstr(log, "reason=SEQUENCE_GAP gpio=13 line=DATA expected_seqno=7 actual_seqno=8 line_seqno=6 event_index=7"));
    }
    printf("csv_replay policy=%s delivered=%zu/%zu batches=%zu health=%s global_warnings=%" PRIu64
           " capture_fault=%s\n", warn ? "warn" : "strict", delivered, count, batches,
           info.healthy ? "HEALTHY" : "FAILED", info.global_gap_warnings,
           warn ? "NONE" : "SEQUENCE_GAP");
    fflush(stdout);
    assert(sequence_matches);
}

/* Replay the first shutdown failure block. Never reconstruct lost history or
 * renumber captured events. --split changes read batches only, not edge order.
 * Records after decision_event are diagnostic context, not inputs to that
 * original verification decision. */
static int replay_failure_log(const char *path, unsigned int split)
{
    FILE *input = fopen(path, "r");
    if (!input) { perror(path); return 2; }
    char text[1024], rejection[64];
    uint64_t id, start, end, checkpoint, decision, first, last;
    unsigned int byte, busy_low;
    bool warn = false;
    bool found = false;
    struct csv_edge rows[2048];
    size_t count = 0;
    while (fgets(text, sizeof(text), input)) {
        if (!found) {
            if (strncmp(text, "XS8_FAILURE_BEGIN ", 18)) continue;
            if (sscanf(text, "XS8_FAILURE_BEGIN id=%" SCNu64 " byte=0x%x start_ns=%" SCNu64
                       " end_ns=%" SCNu64 " checkpoint=%" SCNu64 " decision_event=%" SCNu64
                       " rejection=%63s raw_first=%" SCNu64 " raw_last=%" SCNu64
                       " initial_busy_low=%u", &id, &byte, &start, &end, &checkpoint,
                       &decision, rejection, &first, &last, &busy_low) != 10 ||
                first != 1 || last < decision || decision > 2048 || byte > 255 || busy_low > 1) {
                fprintf(stderr, "Failure replay requires complete history from monitor start; "
                        "overwritten/invalid metadata cannot be reconstructed.\n");
                fclose(input); return 2;
            }
            if (!strstr(text, " policy=strict ") && !strstr(text, " policy=warn ")) {
                fclose(input); return 2;
            }
            warn = strstr(text, " policy=warn ") != NULL;
            found = true;
            continue;
        }
        if (!strncmp(text, "XS8_FAILURE_END ", 16)) break;
        if (strncmp(text, "XS8_RAW,", 8)) continue;
        uint64_t serial;
        struct csv_edge r = {0};
        char polarity[8];
        if (sscanf(text, "XS8_RAW,%" SCNu64 ",%" SCNu64 ",%u,%7[^,],%lu,%lu,%" SCNu64
                   ",%u,%u,", &serial, &r.event.timestamp_ns, &r.event.offset, polarity,
                   &r.event.global_seqno, &r.event.line_seqno, &r.batch,
                   &r.batch_index, &r.batch_size) != 9) {
            fclose(input); return 2;
        }
        if (serial > decision) continue;
        if (serial != count + 1 || count == 2048 || !r.batch_size || r.batch_size > 128 ||
            r.batch_index >= r.batch_size ||
            (strcmp(polarity, "rising") && strcmp(polarity, "falling"))) {
            fclose(input); return 2;
        }
        r.event.type = !strcmp(polarity, "rising") ? GPIOD_EDGE_EVENT_RISING_EDGE
                                                  : GPIOD_EDGE_EVENT_FALLING_EDGE;
        rows[count++] = r;
    }
    fclose(input);
    if (!found || !count || count != decision) return 2;
    setup(!busy_low, 0);
    if (warn) assert(setenv("XS8_GLOBAL_GAP_POLICY", "warn", 1) == 0);
    assert(setenv("XS8_FAILURE_SNAPSHOTS", "1", 1) == 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    for (size_t i = 0; i < count;) {
        size_t n = split ? split : rows[i].batch_size;
        if (n > count - i) n = count - i;
        struct gpiod_edge_event events[128];
        if (!split && (rows[i].batch_index || n != rows[i].batch_size)) {
            xs8_monitor_stop(monitor); teardown(); return 2;
        }
        for (size_t j = 0; j < n; ++j) {
            if (!split && (rows[i+j].batch != rows[i].batch || rows[i+j].batch_index != j ||
                            rows[i+j].batch_size != n)) {
                xs8_monitor_stop(monitor); teardown(); return 2;
            }
            events[j] = rows[i+j].event;
        }
        queue(events, n);
        assert(xs8_monitor_poll(monitor, 0) == 0);
        i += n;
    }
    enum xs8_observation result = xs8_monitor_verify_tx(monitor, checkpoint,
                                                       (uint8_t)byte, start, end, 1);
    xs8_monitor_stop(monitor);
    const char *log = teardown();
    char expected[96];
    snprintf(expected, sizeof(expected), "rejection=%s ", rejection);
    bool reproduced = result == XS8_OBS_INVALID && strstr(log, expected);
    printf("failure_replay byte=0x%02X events=%zu split=%u rejection=%s result=%s\n",
           byte, count, split, rejection, reproduced ? "REPRODUCED" : "DIFFERENT");
    return reproduced ? 0 : 1;
}

static void test_failure_snapshots(void)
{
    const char *reasons[] = {"LOW_TIMING", "TRUNCATED", "NONE", "NONE"};
    const char *rejects[] = {"DECODE_INVALID", "DECODE_INVALID", "MULTIPLE_CANDIDATES",
                             "LOCAL_TAIL_INVALID"};
    for (unsigned int variant = 0; variant < 4; ++variant) {
        for (unsigned int enabled = 0; enabled < 2; ++enabled) {
            struct gpiod_edge_event events[40];
            size_t n = frame_trace(events, 0x09);
            if (variant == 0) {
                uint64_t shift = events[3].timestamp_ns - events[2].timestamp_ns - 2000000;
                for (size_t i = 3; i < n; ++i) events[i].timestamp_ns -= shift;
            } else if (variant == 1) {
                /* Remove final bit's DATA rise and final DATA fall; BUSY closes
                 * an otherwise error-free 7-bit frame. Dense counters. */
                events[17] = events[19]; n = 18;
            } else if (variant == 3) {
                events[19].timestamp_ns = events[18].timestamp_ns + 2500000;
            } else {
                assert(frame_trace(events + 20, 0x09) == 20);
                uint64_t shift = events[19].timestamp_ns - events[20].timestamp_ns + 2000000;
                for (size_t i = 20; i < 40; ++i) events[i].timestamp_ns += shift;
                n = 40;
            }
            number_trace(events, n);
            setup(0, 0);
            if (enabled) assert(setenv("XS8_FAILURE_SNAPSHOTS", "1", 1) == 0);
            if (variant == 0) {
                assert(setenv("XS8_GLOBAL_GAP_POLICY", "warn", 1) == 0);
                for (size_t i = 2; i < n; ++i) events[i].global_seqno += 1;
            }
            struct xs8_monitor *monitor = NULL;
            assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
            queue(events, n);
            enum xs8_observation result = xs8_monitor_verify_tx(monitor, 0, 0x09,
                events[0].timestamp_ns, events[n - 1].timestamp_ns, 1);
            assert(result == XS8_OBS_INVALID && xs8_monitor_healthy(monitor));
            assert(!strstr(logged_so_far(), "XS8_FAILURE_BEGIN")); /* No failure-path I/O. */
            xs8_monitor_stop(monitor);
            const char *log = teardown();
            if (!enabled) { assert(!strstr(log, "XS8_RAW,")); continue; }
            assert(strstr(log, rejects[variant]) && strstr(log, reasons[variant]));
            if (variant == 0) assert(strstr(log, ",2000000,3000000,12000000,LOW_TIMING,"));
            if (variant == 1) assert(strstr(log, ",NONE,TRUNCATED"));
            if (variant == 3) assert(strstr(log, ",2500000,1,2000000,NONE,NONE,1"));
            assert(count_of(log, "XS8_RAW,") == (int)n);
            assert(strstr(log, "overwritten_events=0 overwritten_snapshots=0"));
            char name[] = "/tmp/xs8-failure-replay-XXXXXX";
            int fd = mkstemp(name);
            assert(fd >= 0);
            FILE *output = fdopen(fd, "w");
            assert(output && fputs(log, output) >= 0 && fclose(output) == 0);
            const unsigned int splits[] = {0, 1, 7, 128};
            for (size_t i = 0; i < sizeof(splits)/sizeof(splits[0]); ++i)
                assert(replay_failure_log(name, splits[i]) == 0);
            assert(unlink(name) == 0);
        }
    }
}

static void test_failure_retained_after_match(void)
{
    struct gpiod_edge_event events[40];
    assert(frame_trace(events, 0x09) == 20);
    uint64_t shift = events[3].timestamp_ns - events[2].timestamp_ns - 2000000;
    for (size_t i = 3; i < 20; ++i) events[i].timestamp_ns -= shift;
    assert(frame_trace(events + 20, 0x09) == 20);
    shift = events[19].timestamp_ns - events[20].timestamp_ns + 2000000;
    for (size_t i = 20; i < 40; ++i) events[i].timestamp_ns += shift;
    number_trace(events, 40);
    setup(0, 0);
    assert(setenv("XS8_FAILURE_SNAPSHOTS", "1", 1) == 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    queue(events, 20);
    assert(xs8_monitor_verify_tx(monitor, 0, 0x09, events[0].timestamp_ns,
                                events[19].timestamp_ns, 1) == XS8_OBS_INVALID);
    uint64_t checkpoint = xs8_monitor_checkpoint(monitor);
    queue(events + 20, 20);
    assert(xs8_monitor_verify_tx(monitor, checkpoint, 0x09, events[20].timestamp_ns,
                                events[39].timestamp_ns, 1) == XS8_OBS_MATCH);
    assert(!strstr(logged_so_far(), "XS8_FAILURE_BEGIN"));
    xs8_monitor_stop(monitor);
    const char *log = teardown();
    assert(count_of(log, "XS8_FAILURE_BEGIN") == 1);
    assert(strstr(log, "rejection=DECODE_INVALID"));
    assert(strstr(log, "decision_event=20") && strstr(log, "raw_last=40"));
    assert(strstr(log, "post_margin_reached=1"));
}

static void test_failure_history_bounds(void)
{
    setup(0, 0);
    assert(setenv("XS8_FAILURE_SNAPSHOTS", "1", 1) == 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    for (unsigned int i = 0; i < 110; ++i) {
        struct gpiod_edge_event events[20];
        assert(frame_trace(events, 0x09) == 20);
        uint64_t shift = events[3].timestamp_ns - events[2].timestamp_ns - 2000000;
        for (size_t j = 3; j < 20; ++j) events[j].timestamp_ns -= shift;
        for (size_t j = 0; j < 20; ++j) {
            events[j].timestamp_ns += (uint64_t)i * 200000000;
            events[j].global_seqno += i * 20;
            events[j].line_seqno += i * (events[j].offset == BUSY ? 2 : 18);
        }
        uint64_t checkpoint = xs8_monitor_checkpoint(monitor);
        queue(events, 20);
        assert(xs8_monitor_verify_tx(monitor, checkpoint, 0x09, events[0].timestamp_ns,
                                    events[19].timestamp_ns, 1) == XS8_OBS_INVALID);
    }
    assert(xs8_monitor_healthy(monitor));
    assert(!strstr(logged_so_far(), "XS8_FAILURE_BEGIN"));
    xs8_monitor_stop(monitor);
    const char *log = teardown();
    assert(strstr(log, "overwritten_events=152 overwritten_snapshots=94"));
    assert(strstr(log, "raw_first=153"));
    char name[] = "/tmp/xs8-truncated-history-XXXXXX";
    int fd = mkstemp(name);
    FILE *output = fdopen(fd, "w");
    assert(output && fputs(log, output) >= 0 && fclose(output) == 0);
    assert(replay_failure_log(name, 0) == 2);
    assert(unlink(name) == 0);
}

static int benchmark_snapshots(void)
{
    for (unsigned int enabled = 0; enabled < 2; ++enabled) {
        setup(0, 0);
        if (enabled) assert(setenv("XS8_FAILURE_SNAPSHOTS", "1", 1) == 0);
        struct xs8_monitor *monitor = NULL;
        assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
        struct timespec begin, finish;
        assert(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
        for (unsigned int i = 0; i < 10000; ++i) {
            struct gpiod_edge_event events[20];
            assert(frame_trace(events, 0x09) == 20);
            for (size_t j = 0; j < 20; ++j) {
                events[j].timestamp_ns += (uint64_t)i * 200000000;
                events[j].global_seqno += i * 20;
                events[j].line_seqno += i * (events[j].offset == BUSY ? 2 : 18);
            }
            queue(events, 20);
            assert(xs8_monitor_poll(monitor, 0) == 0);
        }
        assert(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
        int64_t ns = (finish.tv_sec - begin.tv_sec) * 1000000000LL +
                     finish.tv_nsec - begin.tv_nsec;
        xs8_monitor_stop(monitor);
        const char *log = teardown();
        printf("snapshot_benchmark enabled=%u events=200000 elapsed_ns=%" PRId64
               " ns_per_event=%.2f\n", enabled, ns, (double)ns / 200000);
        const char *memory = strstr(log, "XS8 failure snapshots enabled");
        if (memory) printf("%.*s\n", (int)strcspn(memory, "\n"), memory);
    }
    return 0;
}

static int example_failure(void)
{
    struct gpiod_edge_event events[20];
    assert(frame_trace(events, 0x09) == 20);
    uint64_t shift = events[3].timestamp_ns - events[2].timestamp_ns - 2000000;
    for (size_t i = 3; i < 20; ++i) events[i].timestamp_ns -= shift;
    setup(0, 0);
    assert(setenv("XS8_FAILURE_SNAPSHOTS", "1", 1) == 0);
    struct xs8_monitor *monitor = NULL;
    assert(xs8_monitor_start_manual(&bus, &monitor) == 0);
    queue(events, 20);
    assert(xs8_monitor_verify_tx(monitor, 0, 0x09, events[0].timestamp_ns,
                                events[19].timestamp_ns, 1) == XS8_OBS_INVALID);
    xs8_monitor_stop(monitor);
    puts(teardown());
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--example-failure")) return example_failure();
    if (argc == 2 && !strcmp(argv[1], "--benchmark-snapshots")) return benchmark_snapshots();
    if (argc == 3 && !strcmp(argv[1], "--replay-failure"))
        return replay_failure_log(argv[2], 0);
    if (argc == 5 && !strcmp(argv[1], "--replay-failure") && !strcmp(argv[3], "--split")) {
        char *end;
        unsigned long split = strtoul(argv[4], &end, 10);
        if (!*argv[4] || *end || split < 1 || split > 128) return 2;
        return replay_failure_log(argv[2], (unsigned int)split);
    }
    if (argc != 1) {
        if (argc != 5 || strcmp(argv[1], "--replay-csv") || strcmp(argv[3], "--policy") ||
            (strcmp(argv[4], "strict") && strcmp(argv[4], "warn"))) {
            fprintf(stderr, "Usage: %s [--replay-csv H12_CSV --policy strict|warn]\n", argv[0]);
            return 2;
        }
        test_csv_replay(argv[2], !strcmp(argv[4], "warn"));
        return 0;
    }
    test_failure_snapshots();
    test_failure_retained_after_match();
    test_failure_history_bounds();
    test_arming_logs_levels_and_clean_events_stay_healthy();
    test_cold_boot_gap_fails_closed_and_logs_once();
    test_sequence_regression_across_batches();
    test_timestamp_regression();
    test_clean_shutdown_logs_no_fault();
    test_unreadable_data_level_does_not_block_start();
    test_unreadable_busy_level_still_fails_start();
    test_archived_passive_tails();
    test_incompatible_local_tail_timing();
    test_integrity_failures_in_both_policies();
    test_policy_configuration();
    test_warn_gap_does_not_hide_malformed_or_later_fault();
    test_tx_attribution_snapshot();
    for (int injection = CONTROL; injection <= MISSING_TAIL_DENSE; ++injection) {
        test_frame_injection((enum injection)injection, false, false);
        test_frame_injection((enum injection)injection, true, false);
        test_frame_injection((enum injection)injection, false, true);
        test_frame_injection((enum injection)injection, true, true);
    }
    test_csv_replay("tests/fixtures/h12-rx-replay.csv", false);
    test_csv_replay("tests/fixtures/h12-rx-replay.csv", true);
    puts("xs8_monitor_test: OK");
    return 0;
}
