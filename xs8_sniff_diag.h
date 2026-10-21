#ifndef XS8_SNIFF_DIAG_H
#define XS8_SNIFF_DIAG_H

/* Passive diagnostic only. Never reorder records, including timestamp regressions. */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct sniff_record {
    uint64_t timestamp_ns, read_ns, batch;
    unsigned int gpio, index, batch_size;
    uint32_t global_seqno, line_seqno;
    bool rising;
};

struct sniff_range { uint32_t first, last; };
struct sniff_window {
    uint32_t highest;
    uint64_t gaps, first_gap_read_ns;
};

static inline void sniff_note_sequence(struct sniff_window *window,
                                       uint32_t sequence, uint64_t read_ns)
{
    if ((uint64_t)sequence > (uint64_t)window->highest + 1) {
        window->gaps += sequence - window->highest - 1;
        if (!window->first_gap_read_ns) window->first_gap_read_ns = read_ns;
    }
    if (sequence > window->highest) window->highest = sequence;
}

static inline int64_t sniff_deadline(const struct sniff_window *window,
                                     int64_t initial, uint64_t post_gap_ms)
{
    if (!window->first_gap_read_ns) return initial;
    uint64_t deadline = window->first_gap_read_ns + post_gap_ms * 1000000;
    return deadline > INT64_MAX ? INT64_MAX : (int64_t)deadline;
}

struct sniff_summary {
    uint64_t gaps, initially_missing, late, unresolved, regressions;
    uint64_t timestamp_regressions;
};

/* Analyze after capture: no printing/allocation in the collection loop.
 * A late event removes its number from the missing-range ledger. Repeated
 * older numbers not in that ledger are regressions, not new losses.
 * Sequence wrap/reset is not reconstructed; zero is flagged as a regression.
 */
static inline int sniff_report(FILE *csv, FILE *diagnostics,
                               const struct sniff_record *events, size_t count,
                               struct sniff_summary *summary)
{
    *summary = (struct sniff_summary){0};
    if (count > SIZE_MAX / sizeof(struct sniff_range) - 1) return -1;
    struct sniff_range *missing = calloc(count + 1, sizeof(*missing));
    if (!missing) return -1;
    size_t ranges = 0;
    uint32_t highest = 0;
    uint64_t last_time = 0;
    fprintf(csv, "timestamp_ns,gpio,edge,global_seqno,line_seqno,batch,"
                 "batch_index,batch_size,read_ns,sequence_status\n");
    for (size_t i = 0; i < count; ++i) {
        const struct sniff_record *e = &events[i];
        const char *status = "IN_ORDER";
        if (e->global_seqno > highest) {
            if ((uint64_t)e->global_seqno > (uint64_t)highest + 1) {
                missing[ranges++] = (struct sniff_range){highest + 1,
                                                         e->global_seqno - 1};
                ++summary->gaps;
                summary->initially_missing += e->global_seqno - highest - 1;
                summary->unresolved += e->global_seqno - highest - 1;
                status = "GAP";
            }
            highest = e->global_seqno;
        } else {
            status = "REGRESSION_OR_DUPLICATE";
            for (size_t r = 0; r < ranges; ++r) {
                struct sniff_range *m = &missing[r];
                if (!e->global_seqno || e->global_seqno < m->first ||
                    e->global_seqno > m->last) continue;
                uint32_t seq = e->global_seqno;
                if (m->first == m->last) {
                    missing[r] = missing[--ranges];
                } else if (seq == m->first) {
                    ++m->first;
                } else if (seq == m->last) {
                    --m->last;
                } else {
                    missing[ranges++] = (struct sniff_range){seq + 1, m->last};
                    m->last = seq - 1;
                }
                status = "LATE";
                ++summary->late;
                --summary->unresolved;
                break;
            }
            if (status[0] != 'L') ++summary->regressions;
        }
        if (i && e->timestamp_ns < last_time) ++summary->timestamp_regressions;
        last_time = e->timestamp_ns;
        fprintf(csv, "%" PRIu64 ",%u,%s,%" PRIu32 ",%" PRIu32
                ",%" PRIu64 ",%u,%u,%" PRIu64 ",%s\n",
                e->timestamp_ns, e->gpio, e->rising ? "rising" : "falling",
                e->global_seqno, e->line_seqno, e->batch, e->index,
                e->batch_size, e->read_ns, status);
    }
    fprintf(diagnostics, "ORDER_SUMMARY events=%zu gaps=%" PRIu64
            " initially_missing=%" PRIu64 " late_arrivals=%" PRIu64
            " unresolved_at_end=%" PRIu64 " regressions_or_duplicates=%" PRIu64
            " timestamp_regressions=%" PRIu64 "\n", count, summary->gaps,
            summary->initially_missing, summary->late, summary->unresolved,
            summary->regressions, summary->timestamp_regressions);
    for (size_t r = 0; r < ranges; ++r)
        fprintf(diagnostics, "UNRESOLVED_SEQUENCE_RANGE=%" PRIu32 "-%" PRIu32
                " (not observed before capture ended; cause unknown)\n",
                missing[r].first, missing[r].last);
    free(missing);
    return ferror(csv) || ferror(diagnostics) ? -1 : 0;
}

#endif
