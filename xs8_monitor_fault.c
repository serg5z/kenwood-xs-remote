#include "xs8_monitor_fault.h"

#include <inttypes.h>
#include <stdio.h>

unsigned int xs8_capture_check(struct xs8_capture_state *state,
    unsigned int gpio, uint64_t sequence, uint64_t line_sequence,
    uint64_t timestamp_ns, uint64_t event_index)
{
    return xs8_capture_check_policy(state, gpio, sequence, line_sequence,
                                    timestamp_ns, event_index, false);
}

unsigned int xs8_capture_check_policy(struct xs8_capture_state *state,
    unsigned int gpio, uint64_t sequence, uint64_t line_sequence,
    uint64_t timestamp_ns, uint64_t event_index, bool warn_global_only)
{
    unsigned int reasons = 0;
    const uint64_t expected = state->last_sequence + 1;
    if (sequence > expected) {
        reasons |= XS8_CAPTURE_SEQUENCE_GAP;
        ++state->sequence_gaps;
        state->missing_events += sequence - expected;
    } else if (sequence < expected) {
        reasons |= XS8_CAPTURE_SEQUENCE_REGRESSION;
        ++state->sequence_regressions;
    }
    const bool had_timestamp = state->timestamp_seen;
    const uint64_t previous_ns = state->last_timestamp;
    if (had_timestamp && timestamp_ns < previous_ns) {
        reasons |= XS8_CAPTURE_TIMESTAMP_REGRESSION;
        ++state->timestamp_regressions;
    }
    if (reasons && !state->fault_recorded &&
        !(warn_global_only && reasons == XS8_CAPTURE_SEQUENCE_GAP)) {
        state->fault_recorded = true;
        state->first_fault = (struct xs8_capture_fault){
            .reasons = reasons,
            .has_edge = true,
            .gpio = gpio,
            .expected_sequence = expected,
            .actual_sequence = sequence,
            .line_sequence = line_sequence,
            .previous_timestamp_ns = had_timestamp ? previous_ns : 0,
            .timestamp_ns = timestamp_ns,
            .event_index = event_index
        };
    }
    state->last_sequence = sequence;
    state->last_timestamp = timestamp_ns;
    state->timestamp_seen = true;
    return reasons;
}

void xs8_capture_note_overflow(struct xs8_capture_state *state,
    uint64_t dropped, uint64_t timestamp_ns)
{
    if (state->fault_recorded) return;
    state->fault_recorded = true;
    state->first_fault = (struct xs8_capture_fault){
        .reasons = XS8_CAPTURE_DECODED_OVERFLOW,
        .timestamp_ns = timestamp_ns,
        .dropped = dropped
    };
}

static const char *line_name(unsigned int gpio, unsigned int busy_gpio,
                             unsigned int data_gpio)
{
    return gpio == busy_gpio ? "BUSY" : gpio == data_gpio ? "DATA" : "OTHER";
}

static const char *level_name(int level)
{
    return level < 0 ? "UNKNOWN" : level ? "HIGH" : "LOW";
}

/* Microseconds from arming, signed: kernel edge detection starts when the RX
 * lines are requested, slightly before the monitor is armed. */
static long long since_arm_us(const struct xs8_capture_state *state,
                              uint64_t timestamp_ns)
{
    return ((long long)timestamp_ns - (long long)state->armed_ns) / 1000;
}

int xs8_capture_format_fault(const struct xs8_capture_state *state,
    unsigned int busy_gpio, unsigned int data_gpio, char *out, size_t size)
{
    if (!state->fault_recorded) return -1;
    const struct xs8_capture_fault *fault = &state->first_fault;
    char reasons[192] = "";
    static const struct { unsigned int bit; const char *name; } names[] = {
        {XS8_CAPTURE_SEQUENCE_GAP, "SEQUENCE_GAP"},
        {XS8_CAPTURE_SEQUENCE_REGRESSION, "SEQUENCE_REGRESSION"},
        {XS8_CAPTURE_TIMESTAMP_REGRESSION, "TIMESTAMP_REGRESSION"},
        {XS8_CAPTURE_DECODED_OVERFLOW, "DECODED_OVERFLOW"},
        {XS8_CAPTURE_LINE_SEQUENCE_GAP, "LINE_SEQUENCE_GAP"},
        {XS8_CAPTURE_LINE_SEQUENCE_REGRESSION, "LINE_SEQUENCE_REGRESSION"},
        {XS8_CAPTURE_UNEXPECTED_LINE, "UNEXPECTED_LINE"}
    };
    size_t used = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!(fault->reasons & names[i].bit)) continue;
        int n = snprintf(reasons + used, sizeof(reasons) - used, "%s%s",
                         used ? "+" : "", names[i].name);
        if (n > 0) used += (size_t)n;
    }
    if (!fault->has_edge)
        return snprintf(out, size,
            "XS8 OBSERVED_BUS monitor FAILED reason=%s dropped=%" PRIu64
            " event_mono_us=%" PRIu64 " since_arm_us=%lld",
            reasons, fault->dropped, fault->timestamp_ns / 1000,
            since_arm_us(state, fault->timestamp_ns));
    char previous[32] = "NONE";
    if (fault->previous_timestamp_ns)
        snprintf(previous, sizeof(previous), "%" PRIu64,
                 fault->previous_timestamp_ns / 1000);
    char line_expected[64] = "";
    if (fault->reasons & (XS8_CAPTURE_LINE_SEQUENCE_GAP |
                          XS8_CAPTURE_LINE_SEQUENCE_REGRESSION))
        snprintf(line_expected, sizeof(line_expected),
                 " expected_line_seqno=%" PRIu64, fault->expected_line_sequence);
    return snprintf(out, size,
        "XS8 OBSERVED_BUS monitor FAILED reason=%s gpio=%u line=%s "
        "expected_seqno=%" PRIu64 " actual_seqno=%" PRIu64
        " line_seqno=%" PRIu64 " event_index=%" PRIu64
        " event_mono_us=%" PRIu64 " previous_event_mono_us=%s"
        " since_arm_us=%lld%s",
        reasons, fault->gpio, line_name(fault->gpio, busy_gpio, data_gpio),
        fault->expected_sequence, fault->actual_sequence,
        fault->line_sequence, fault->event_index,
        fault->timestamp_ns / 1000, previous,
        since_arm_us(state, fault->timestamp_ns), line_expected);
}

int xs8_capture_format_armed(uint64_t armed_ns, int busy_level, int data_level,
    char *out, size_t size)
{
    return snprintf(out, size,
        "XS8 OBSERVED_BUS monitor armed mono_us=%" PRIu64 " busy=%s data=%s",
        armed_ns / 1000, level_name(busy_level), level_name(data_level));
}
