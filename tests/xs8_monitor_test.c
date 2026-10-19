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
    struct gpiod_edge_event batches[8][128];
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

static void test_frame_injection(enum injection injection, bool split_batch)
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
        xs8_decoder_edge(&decoder, &e, save_decoded, &decoded);
    }
    assert(!decoder.active && decoder.synced);
    assert(decoded.count > 0);
    static const unsigned int expected_bits[] = {8, 0, 0, 7, 6, 8, 8};
    static const unsigned int expected_stage[] = {4, 1, 2, 2, 2, 4, 4};
    static const char *first_reason[] = {NULL, "EVENT_GAP", "EVENT_GAP",
        "UNEXPECTED_DATA_EDGE", "NEW_BUSY_RISE", "EVENT_GAP", NULL};
    assert(decoder.bits == expected_bits[injection]);
    assert(decoder.stage == expected_stage[injection]);
    assert(first_reason[injection] ? decoded.records[0].reason &&
        strcmp(decoded.records[0].reason, first_reason[injection]) == 0 :
        decoded.records[0].reason == NULL);

    setup(0, 0);
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
    assert(healthy == !gap);
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
    bool passive_valid = injection == CONTROL || injection == MISSING_TAIL_DENSE;
    bool accepted = injection == CONTROL;
    assert(valid == (passive_valid ? 1u : 0u));
    assert(invalid == (passive_valid ? 0u : injection == EXTRA_BUSY ? 2u : 1u));
    assert(gaps == (gap ? 1u : 0u));
    if (passive_valid) assert(records[0].decoded.waveform_lsb == 0x84);
    enum xs8_observation outcome = xs8_monitor_verify_tx(
        monitor, checkpoint, 0x84, start, end, 1);
    assert(outcome == (gap ? XS8_OBS_UNHEALTHY :
                       accepted ? XS8_OBS_MATCH : XS8_OBS_INVALID));
    if (gap) assert(xs8_monitor_poll(monitor, 0) == -1);
    xs8_monitor_stop(monitor);
    const char *log = teardown();
    assert(count_of(log, "monitor FAILED") == (gap ? 1 : 0));
    if (gap) assert(strstr(log, "monitor FAILED reason=SEQUENCE_GAP"));
    if (injection == GLOBAL_ONLY) assert(strstr(log,
        "expected_seqno=3 actual_seqno=5 line_seqno=2 event_index=3"));

    printf("fault-injection case=%s batches=%u health=%s failure=%s "
           "global_breaks=%u line_breaks=%u decoder_active=%u synced=%u "
           "stage=%u bits=%u valid_frames=%u invalid_frames=%u gap_records=%u "
           "first_reason=%s verification=%s rejected=%s\n",
           names[injection], split_batch ? 2 : 1, healthy ? "HEALTHY" : "FAILED",
           gap ? "SEQUENCE_GAP" : "NONE", global_breaks, line_breaks,
           decoder.active, decoder.synced, decoder.stage, decoder.bits,
           valid, invalid, gaps, decoded.records[0].reason ?
           decoded.records[0].reason : "NONE", gap ? "UNHEALTHY" :
           accepted ? "MATCH" : "INVALID", accepted ? "no" : "yes");
}

static void check_tail_trace(const struct gpiod_edge_event *events, size_t count,
                            uint8_t byte, bool passive_valid, bool local_match,
                            bool tail_fell)
{
    setup(0, 0);
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
        check_tail_trace(events, 20, bytes[capture], true, variant == 1, data_first);
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
        check_tail_trace(events, 20, 0x84, cases[i].valid, cases[i].match, true);
    }
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
    test_archived_passive_tails();
    test_incompatible_local_tail_timing();
    for (int injection = CONTROL; injection <= MISSING_TAIL_DENSE; ++injection) {
        test_frame_injection((enum injection)injection, false);
        test_frame_injection((enum injection)injection, true);
    }
    puts("xs8_monitor_test: OK");
    return 0;
}
