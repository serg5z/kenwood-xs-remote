#ifndef XS8_MONITOR_FAULT_H
#define XS8_MONITOR_FAULT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capture-integrity checks for the RX edge monitor. Pure bookkeeping: no I/O,
 * locks or clocks, so the per-edge path costs what it did before. The first
 * fault is kept in memory and formatted by the caller outside that path. */
enum {
    XS8_CAPTURE_SEQUENCE_GAP = 1u << 0,
    XS8_CAPTURE_SEQUENCE_REGRESSION = 1u << 1,
    XS8_CAPTURE_TIMESTAMP_REGRESSION = 1u << 2,
    XS8_CAPTURE_DECODED_OVERFLOW = 1u << 3,
    XS8_CAPTURE_LINE_SEQUENCE_GAP = 1u << 4,
    XS8_CAPTURE_LINE_SEQUENCE_REGRESSION = 1u << 5,
    XS8_CAPTURE_UNEXPECTED_LINE = 1u << 6
};

struct xs8_capture_fault {
    unsigned int reasons;           /* XS8_CAPTURE_* bits of the faulting edge */
    bool has_edge;                  /* false for a decoded-batch overflow */
    unsigned int gpio;
    uint64_t expected_sequence;     /* previous global sequence + 1 */
    uint64_t actual_sequence;
    uint64_t line_sequence;
    uint64_t expected_line_sequence;
    uint64_t previous_timestamp_ns; /* 0 when no earlier event */
    uint64_t timestamp_ns;
    uint64_t event_index;           /* 1-based count of events seen */
    uint64_t dropped;               /* decoded results lost (overflow only) */
};

struct xs8_capture_state {
    uint64_t sequence_gaps, missing_events, sequence_regressions;
    uint64_t timestamp_regressions, last_sequence, last_timestamp;
    bool timestamp_seen;
    uint64_t armed_ns;              /* monitor arming time, CLOCK_MONOTONIC */
    bool fault_recorded, fault_logged;
    struct xs8_capture_fault first_fault;
};

/* Checks one edge against the previous one, updates the counters exactly as
 * the monitor always has, and returns this edge's XS8_CAPTURE_* bits (0 when
 * consistent). Only the first faulting edge is recorded. */
unsigned int xs8_capture_check(struct xs8_capture_state *state,
    unsigned int gpio, uint64_t sequence, uint64_t line_sequence,
    uint64_t timestamp_ns, uint64_t event_index);
/* Still counts every raw global discontinuity; only a forward global gap
 * without any other integrity error may be excluded from first-fault storage.
 * The caller must check independent per-line continuity before opting in. */
unsigned int xs8_capture_check_policy(struct xs8_capture_state *state,
    unsigned int gpio, uint64_t sequence, uint64_t line_sequence,
    uint64_t timestamp_ns, uint64_t event_index, bool warn_global_only);
/* Records a decoded-batch overflow as the first fault if none is recorded. */
void xs8_capture_note_overflow(struct xs8_capture_state *state,
    uint64_t dropped, uint64_t timestamp_ns);
/* One log line for the first fault, without a trailing newline. Returns the
 * snprintf length, or -1 when no fault is recorded. */
int xs8_capture_format_fault(const struct xs8_capture_state *state,
    unsigned int busy_gpio, unsigned int data_gpio, char *out, size_t size);
/* One log line for monitor arming. A negative level is reported UNKNOWN. */
int xs8_capture_format_armed(uint64_t armed_ns, int busy_level, int data_level,
    char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
