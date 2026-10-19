#include "xs8_monitor_fault.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BUSY 23
#define DATA 27
#define ARMED_NS 5000000000ULL

static struct xs8_capture_state armed_state(void)
{
    struct xs8_capture_state state = {0};
    state.armed_ns = ARMED_NS;
    return state;
}

static void expect_line(const struct xs8_capture_state *state, const char *want)
{
    char line[384];
    int n = xs8_capture_format_fault(state, BUSY, DATA, line, sizeof(line));
    if (n < 0 || strcmp(line, want) != 0) {
        fprintf(stderr, "got:  %s\nwant: %s\n", n < 0 ? "(none)" : line, want);
        assert(0);
    }
}

/* The counters must match the original inline checks in xs8_monitor.c. */
struct reference {
    unsigned long long gaps, missing, seq_regressions, ts_regressions;
    unsigned long long last_sequence, last_timestamp;
    int timestamp_seen;
};

static int reference_step(struct reference *r, unsigned long long sequence,
                          unsigned long long timestamp)
{
    int fault = 0;
    if (sequence > r->last_sequence + 1) {
        fault = 1;
        ++r->gaps;
        r->missing += sequence - r->last_sequence - 1;
    } else if (sequence <= r->last_sequence) {
        fault = 1;
        ++r->seq_regressions;
    }
    if (r->timestamp_seen && timestamp < r->last_timestamp) {
        fault = 1;
        ++r->ts_regressions;
    }
    r->last_sequence = sequence;
    r->last_timestamp = timestamp;
    r->timestamp_seen = 1;
    return fault;
}

static void test_in_order_events_record_nothing(void)
{
    struct xs8_capture_state state = armed_state();
    for (uint64_t i = 1; i <= 100; ++i)
        assert(xs8_capture_check(&state, i % 2 ? DATA : BUSY, i, i,
                                 ARMED_NS + i * 1000, i) == 0);
    assert(!state.fault_recorded);
    assert(state.sequence_gaps == 0 && state.missing_events == 0);
    assert(state.sequence_regressions == 0 && state.timestamp_regressions == 0);
    char line[64];
    assert(xs8_capture_format_fault(&state, BUSY, DATA, line, sizeof(line)) == -1);
}

/* The cold-boot shape: three DATA events whose first sequence number is 3. */
static void test_first_event_gap_is_recorded(void)
{
    struct xs8_capture_state state = armed_state();
    assert(xs8_capture_check(&state, DATA, 3, 1, ARMED_NS + 250000, 1) ==
           XS8_CAPTURE_SEQUENCE_GAP);
    assert(xs8_capture_check(&state, DATA, 4, 2, ARMED_NS + 260000, 2) == 0);
    assert(xs8_capture_check(&state, DATA, 5, 3, ARMED_NS + 270000, 3) == 0);
    assert(state.sequence_gaps == 1 && state.missing_events == 2);
    assert(state.fault_recorded && !state.fault_logged);
    expect_line(&state,
        "XS8 OBSERVED_BUS monitor FAILED reason=SEQUENCE_GAP gpio=27 line=DATA "
        "expected_seqno=1 actual_seqno=3 line_seqno=1 event_index=1 "
        "event_mono_us=5000250 previous_event_mono_us=NONE since_arm_us=250");
}

static void test_mid_stream_gap_reports_previous_event(void)
{
    struct xs8_capture_state state = armed_state();
    xs8_capture_check(&state, BUSY, 1, 1, ARMED_NS + 1000000, 1);
    xs8_capture_check(&state, BUSY, 2, 2, ARMED_NS + 2000000, 2);
    assert(xs8_capture_check(&state, BUSY, 9, 3, ARMED_NS + 3000000, 3) ==
           XS8_CAPTURE_SEQUENCE_GAP);
    assert(state.missing_events == 6);
    expect_line(&state,
        "XS8 OBSERVED_BUS monitor FAILED reason=SEQUENCE_GAP gpio=23 line=BUSY "
        "expected_seqno=3 actual_seqno=9 line_seqno=3 event_index=3 "
        "event_mono_us=5003000 previous_event_mono_us=5002000 since_arm_us=3000");
}

static void test_repeated_and_backward_sequences_are_regressions(void)
{
    struct xs8_capture_state state = armed_state();
    xs8_capture_check(&state, DATA, 1, 1, ARMED_NS + 10000, 1);
    xs8_capture_check(&state, DATA, 2, 2, ARMED_NS + 20000, 2);
    assert(xs8_capture_check(&state, DATA, 2, 3, ARMED_NS + 30000, 3) ==
           XS8_CAPTURE_SEQUENCE_REGRESSION);
    assert(xs8_capture_check(&state, DATA, 1, 4, ARMED_NS + 40000, 4) ==
           XS8_CAPTURE_SEQUENCE_REGRESSION);
    assert(state.sequence_regressions == 2 && state.sequence_gaps == 0);
    expect_line(&state,
        "XS8 OBSERVED_BUS monitor FAILED reason=SEQUENCE_REGRESSION gpio=27 "
        "line=DATA expected_seqno=3 actual_seqno=2 line_seqno=3 event_index=3 "
        "event_mono_us=5000030 previous_event_mono_us=5000020 since_arm_us=30");
}

static void test_timestamp_regression_and_combined_reasons(void)
{
    struct xs8_capture_state state = armed_state();
    xs8_capture_check(&state, DATA, 1, 1, ARMED_NS + 50000, 1);
    assert(xs8_capture_check(&state, DATA, 2, 2, ARMED_NS + 40000, 2) ==
           XS8_CAPTURE_TIMESTAMP_REGRESSION);
    assert(state.timestamp_regressions == 1);
    expect_line(&state,
        "XS8 OBSERVED_BUS monitor FAILED reason=TIMESTAMP_REGRESSION gpio=27 "
        "line=DATA expected_seqno=2 actual_seqno=2 line_seqno=2 event_index=2 "
        "event_mono_us=5000040 previous_event_mono_us=5000050 since_arm_us=40");

    struct xs8_capture_state both = armed_state();
    xs8_capture_check(&both, BUSY, 1, 1, ARMED_NS + 50000, 1);
    assert(xs8_capture_check(&both, BUSY, 4, 2, ARMED_NS + 40000, 2) ==
           (XS8_CAPTURE_SEQUENCE_GAP | XS8_CAPTURE_TIMESTAMP_REGRESSION));
    char line[384];
    xs8_capture_format_fault(&both, BUSY, DATA, line, sizeof(line));
    assert(strstr(line, "reason=SEQUENCE_GAP+TIMESTAMP_REGRESSION gpio=23"));
}

static void test_only_the_first_fault_is_kept(void)
{
    struct xs8_capture_state state = armed_state();
    xs8_capture_check(&state, DATA, 2, 1, ARMED_NS + 10, 1);
    xs8_capture_check(&state, BUSY, 2, 1, ARMED_NS + 5, 2);
    xs8_capture_note_overflow(&state, 7, ARMED_NS + 20);
    assert(state.first_fault.reasons == XS8_CAPTURE_SEQUENCE_GAP);
    assert(state.first_fault.actual_sequence == 2);
    assert(state.sequence_gaps == 1 && state.sequence_regressions == 1);
    assert(state.timestamp_regressions == 1);
}

static void test_overflow_without_edge_fault(void)
{
    struct xs8_capture_state state = armed_state();
    xs8_capture_check(&state, DATA, 1, 1, ARMED_NS + 1000, 1);
    xs8_capture_note_overflow(&state, 3, ARMED_NS + 2000);
    expect_line(&state,
        "XS8 OBSERVED_BUS monitor FAILED reason=DECODED_OVERFLOW dropped=3 "
        "event_mono_us=5000002 since_arm_us=2");
}

/* Kernel edge detection starts at the line request, just before arming. */
static void test_event_before_arming_is_negative(void)
{
    struct xs8_capture_state state = armed_state();
    xs8_capture_check(&state, DATA, 2, 1, ARMED_NS - 1500, 1);
    char line[384];
    xs8_capture_format_fault(&state, BUSY, DATA, line, sizeof(line));
    assert(strstr(line, " since_arm_us=-1"));
    assert(strstr(line, "line=DATA"));
}

static void test_counters_match_original_algorithm(void)
{
    struct xs8_capture_state state = armed_state();
    struct reference reference = {0};
    unsigned int seed = 12345u;
    unsigned long long sequence = 0, timestamp = ARMED_NS;
    for (int i = 0; i < 5000; ++i) {
        seed = seed * 1103515245u + 12345u;
        unsigned int roll = (seed >> 16) % 10;
        if (roll == 0) sequence += 3;
        else if (roll == 1 && sequence > 1) sequence -= 1;
        else if (roll != 2) sequence += 1;
        timestamp = roll == 3 ? timestamp - 7 : timestamp + 1000;
        int expected = reference_step(&reference, sequence, timestamp);
        int got = xs8_capture_check(&state, DATA, sequence, (uint64_t)i,
                                    timestamp, (uint64_t)i + 1) != 0;
        assert(expected == got);
    }
    assert(state.sequence_gaps == reference.gaps);
    assert(state.missing_events == reference.missing);
    assert(state.sequence_regressions == reference.seq_regressions);
    assert(state.timestamp_regressions == reference.ts_regressions);
    assert(state.last_sequence == reference.last_sequence);
    assert(state.last_timestamp == reference.last_timestamp);
    assert(reference.gaps && reference.seq_regressions && reference.ts_regressions);
}

static void test_armed_line(void)
{
    char line[128];
    xs8_capture_format_armed(ARMED_NS, 0, 1, line, sizeof(line));
    assert(strcmp(line, "XS8 OBSERVED_BUS monitor armed mono_us=5000000 "
                        "busy=LOW data=HIGH") == 0);
    xs8_capture_format_armed(ARMED_NS, 1, -1, line, sizeof(line));
    assert(strcmp(line, "XS8 OBSERVED_BUS monitor armed mono_us=5000000 "
                        "busy=HIGH data=UNKNOWN") == 0);
}

static void test_warning_does_not_hide_later_integrity_fault(void)
{
    struct xs8_capture_state state = armed_state();
    xs8_capture_check_policy(&state, DATA, 1, 1, ARMED_NS + 1000, 1, true);
    assert(xs8_capture_check_policy(&state, DATA, 4, 2,
        ARMED_NS + 2000, 2, true) == XS8_CAPTURE_SEQUENCE_GAP);
    assert(!state.fault_recorded && state.sequence_gaps == 1 && state.missing_events == 2);
    xs8_capture_note_overflow(&state, 3, ARMED_NS + 3000);
    assert(state.fault_recorded && state.first_fault.reasons == XS8_CAPTURE_DECODED_OVERFLOW);
    struct xs8_capture_state mixed = armed_state();
    xs8_capture_check_policy(&mixed, DATA, 1, 1, ARMED_NS + 2000, 1, true);
    assert(xs8_capture_check_policy(&mixed, DATA, 3, 2, ARMED_NS + 1000, 2, true) ==
        (XS8_CAPTURE_SEQUENCE_GAP | XS8_CAPTURE_TIMESTAMP_REGRESSION));
    assert(mixed.fault_recorded);
}

int main(void)
{
    test_in_order_events_record_nothing();
    test_first_event_gap_is_recorded();
    test_mid_stream_gap_reports_previous_event();
    test_repeated_and_backward_sequences_are_regressions();
    test_timestamp_regression_and_combined_reasons();
    test_only_the_first_fault_is_kept();
    test_overflow_without_edge_fault();
    test_event_before_arming_is_negative();
    test_counters_match_original_algorithm();
    test_armed_line();
    test_warning_does_not_hide_later_integrity_fault();
    puts("xs8_monitor_fault_test: OK");
    return 0;
}
