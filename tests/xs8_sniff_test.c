#include "xs8_sniff_diag.h"
#include <assert.h>
#include <string.h>

static struct sniff_summary analyze(const uint32_t *sequences, size_t count)
{
    struct sniff_record *events = calloc(count + 1, sizeof(*events));
    assert(events);
    for (size_t i = 0; i < count; ++i)
        events[i] = (struct sniff_record){
            .timestamp_ns = 1000 + i, .read_ns = 2000 + i,
            .batch = i / 2 + 1, .index = i % 2, .batch_size = 2,
            .global_seqno = sequences[i], .line_seqno = (uint32_t)i + 1,
            .gpio = i % 2 ? 26 : 13, .rising = i % 2 == 0
        };
    FILE *csv = tmpfile(), *diag = tmpfile();
    assert(csv && diag);
    struct sniff_summary summary;
    assert(sniff_report(csv, diag, events, count, &summary) == 0);
    rewind(csv);
    char line[512];
    assert(fgets(line, sizeof(line), csv));
    assert(!strcmp(line, "timestamp_ns,gpio,edge,global_seqno,line_seqno,batch,"
                         "batch_index,batch_size,read_ns,sequence_status\n"));
    for (size_t i = 0; i < count; ++i) {
        unsigned long long timestamp, batch, read_time;
        unsigned gpio, global, per_line, index, batch_size;
        char edge[16], status[40];
        assert(fgets(line, sizeof(line), csv));
        assert(sscanf(line, "%llu,%u,%15[^,],%u,%u,%llu,%u,%u,%llu,%39s",
            &timestamp, &gpio, edge, &global, &per_line, &batch, &index,
            &batch_size, &read_time, status) == 10);
        assert(timestamp == events[i].timestamp_ns && global == sequences[i]);
        assert(gpio == events[i].gpio && per_line == events[i].line_seqno);
        assert(batch == events[i].batch && index == events[i].index);
        assert(batch_size == events[i].batch_size && read_time == events[i].read_ns);
        assert(!strcmp(edge, events[i].rising ? "rising" : "falling"));
    }
    assert(!fgets(line, sizeof(line), csv));
    fclose(csv);
    fclose(diag);
    free(events);
    return summary;
}

int main(void)
{
    struct sniff_summary s = analyze(NULL, 0);
    assert(s.gaps == 0 && s.unresolved == 0);
    const uint32_t ordered[] = {1, 2, 3, 4};
    s = analyze(ordered, 4);
    assert(!s.gaps && !s.late && !s.regressions);

    /* Connected incident: 3 initially absent, then arrives in a later batch. */
    const uint32_t late[] = {1, 2, 4, 3, 5};
    s = analyze(late, 5);
    assert(s.gaps == 1 && s.initially_missing == 1 && s.late == 1);
    assert(!s.unresolved && !s.regressions);
    const uint32_t absent[] = {1, 2, 4, 5};
    s = analyze(absent, 4);
    assert(s.gaps == 1 && s.unresolved == 1 && !s.late);

    /* Disconnected incident: missing 243 subsequently appears after 244. */
    uint32_t disconnected[245];
    for (uint32_t i = 0; i < 245; ++i) disconnected[i] = i + 1;
    disconnected[242] = 244;
    disconnected[243] = 243;
    s = analyze(disconnected, 245);
    assert(s.initially_missing == 1 && s.late == 1 && !s.unresolved);

    const uint32_t split_ranges[] = {8, 4, 2, 6, 1, 3, 5, 7, 9};
    s = analyze(split_ranges, 9);
    assert(s.initially_missing == 7 && s.late == 7 && !s.unresolved);
    const uint32_t duplicates[] = {1, 3, 2, 2, 1, 4};
    s = analyze(duplicates, 6);
    assert(s.late == 1 && s.regressions == 2 && !s.unresolved);
    const uint32_t separate[] = {1, 4, 8, 2, 6};
    s = analyze(separate, 5);
    assert(s.gaps == 2 && s.initially_missing == 5 && s.late == 2);
    assert(s.unresolved == 3);
    const uint32_t huge_gap[] = {UINT32_MAX, 1, UINT32_MAX - 1, 0};
    s = analyze(huge_gap, 4);
    assert(s.initially_missing == (uint64_t)UINT32_MAX - 1);
    assert(s.late == 2 && s.unresolved == (uint64_t)UINT32_MAX - 3);
    assert(s.regressions == 1);

    /* Timestamp regressions are retained, not used as capture cutoffs. */
    const struct sniff_record regression[] = {
        {.timestamp_ns = 20, .global_seqno = 1},
        {.timestamp_ns = 10, .global_seqno = 2}
    };
    FILE *csv = tmpfile(), *diag = tmpfile();
    assert(csv && diag);
    assert(sniff_report(csv, diag, regression, 2, &s) == 0);
    assert(s.timestamp_regressions == 1);
    fclose(csv);
    fclose(diag);
    /* Exact CSV contract: retain late event after the newer event/batch. */
    const struct sniff_record example[] = {
        {1000000, 2000000, 1, 13, 0, 2, 1, 1, true},
        {1029000, 2000000, 1, 26, 1, 2, 3, 1, false},
        {1010000, 2500000, 2, 13, 0, 1, 2, 2, false}
    };
    csv = tmpfile();
    diag = tmpfile();
    assert(csv && diag);
    assert(sniff_report(csv, diag, example, 3, &s) == 0);
    rewind(csv);
    char text[2048] = {0};
    assert(fread(text, 1, sizeof(text) - 1, csv) > 0);
    assert(strstr(text, "1029000,26,falling,3,1,1,1,2,2000000,GAP\n"
                        "1010000,13,falling,2,2,2,0,1,2500000,LATE\n"));
    rewind(diag);
    memset(text, 0, sizeof(text));
    assert(fread(text, 1, sizeof(text) - 1, diag) > 0);
    assert(strstr(text, "late_arrivals=1 unresolved_at_end=0"));
    assert(!strstr(text, "UNRESOLVED_SEQUENCE_RANGE"));
    fclose(csv);
    fclose(diag);
    struct sniff_window window = {0};
    sniff_note_sequence(&window, 1, 1000000000);
    sniff_note_sequence(&window, 2, 2000000000);
    assert(sniff_deadline(&window, 10000000000LL, 1000) == 10000000000LL);
    sniff_note_sequence(&window, 4, 3000000000);
    assert(window.gaps == 1 && window.first_gap_read_ns == 3000000000);
    assert(sniff_deadline(&window, 10000000000LL, 1000) == 4000000000);
    sniff_note_sequence(&window, 3, 3500000000);
    sniff_note_sequence(&window, 5, 3600000000);
    sniff_note_sequence(&window, 7, 3700000000);
    assert(window.highest == 7 && window.gaps == 2);
    assert(sniff_deadline(&window, 10000000000LL, 1000) == 4000000000);
    /* First gap near the original deadline gets the full requested window. */
    window.first_gap_read_ns = 9900000000;
    assert(sniff_deadline(&window, 10000000000LL, 1000) == 10900000000LL);
    puts("xs8_sniff diagnostic tests passed (13 synthetic scenarios)");
    return 0;
}
