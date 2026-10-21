#include "xs8_stimulus_core.h"
#include "xs8_sniff_diag.h"
#include <assert.h>
#include <stdio.h>

struct write_event { int64_t time; unsigned int gpio; int level; };
struct fake {
    int64_t now, late;
    struct write_event writes[10016];
    unsigned int count, waits, opens, closes, checks;
    unsigned int fail_write, fail_wait, cancel_wait, fail_check;
    unsigned int now_calls, fail_now;
    bool open_failure, now_failure, cancel;
};
static int fake_check(void *p)
{
    struct fake *f = p;
    if (++f->checks == f->fail_check) { errno = EACCES; return -1; }
    return 0;
}
static int fake_open(void *p)
{
    struct fake *f = p;
    ++f->opens;
    if (f->open_failure) { errno = EBUSY; return -1; }
    return 0;
}
static void fake_close(void *p) { ++((struct fake *)p)->closes; }
static bool fake_cancel(void *p) { return ((struct fake *)p)->cancel; }
static int64_t fake_now(void *p)
{
    struct fake *f = p;
    if (++f->now_calls == f->fail_now || f->now_failure) {
        errno = EIO; return -1;
    }
    return f->now;
}
static int fake_set(void *p, unsigned int gpio, int level)
{
    struct fake *f = p;
    assert(gpio == 6 || gpio == 19); /* No RX GPIO operations. */
    assert(f->count < sizeof(f->writes) / sizeof(f->writes[0]));
    f->writes[f->count++] = (struct write_event){f->now, gpio, level};
    if (f->count == f->fail_write) { errno = EIO; return -1; }
    return 0;
}
static int fake_wait(void *p, int64_t deadline)
{
    struct fake *f = p;
    ++f->waits;
    if (f->waits == f->cancel_wait) { f->cancel = true; return ECANCELED; }
    if (f->waits == f->fail_wait) return EIO;
    assert(deadline >= f->now);
    f->now = deadline + f->late;
    return 0;
}
static int run(struct fake *f, struct stimulus_options options,
               struct stimulus_report *r)
{
    const struct stimulus_ops ops = {f, fake_check, fake_open, fake_set,
                                    fake_now, fake_wait, fake_cancel, fake_close};
    return stimulus_run(&options, &ops, r);
}
static void cleanup(const struct fake *f, const struct stimulus_report *r)
{
    assert(r->cleanup_attempted && f->closes == 1 && f->count >= 2);
    assert(f->writes[f->count - 2].gpio == 6);
    assert(f->writes[f->count - 1].gpio == 19);
    assert(!f->writes[f->count - 2].level && !f->writes[f->count - 1].level);
}
static void single_line_tests(void)
{
    struct stimulus_report r;
    for (enum stimulus_line line = STIM_DATA_ONLY; line <= STIM_BUSY_ONLY; ++line) {
        struct stimulus_options o = {.disconnected_ack=true, .line=line,
                                     .transitions=320, .interval_us=5000};
        struct fake f = {.now=1000000000};
        assert(run(&f, o, &r) == 0);
        assert(r.single.completed && !r.attempted && !r.completed);
        assert(r.transition_attempted == 320 && r.transition_written == 320);
        assert(f.count == 324 && f.checks == 2 && f.opens == 1);
        assert(r.single.end_ns - r.single.start_ns == 1600000000);
        for (unsigned int i = 0; i < 320; ++i) {
            assert(f.writes[2+i].gpio == (line == STIM_DATA_ONLY ? 6u : 19u));
            assert(f.writes[2+i].level == (i % 2 == 0));
            assert(f.writes[2+i].time == 1000000000LL + i * 5000000LL);
            assert(r.transitions[i].written);
            assert(r.transitions[i].write_ns == f.writes[2+i].time);
            assert(r.transitions[i].wake_ns - r.transitions[i].write_ns == 5000000);
        }
        /* Other GPIO is only ever written LOW at initialization/cleanup. */
        for (unsigned int i = 0; i < f.count; ++i)
            if (f.writes[i].gpio != (line == STIM_DATA_ONLY ? 6u : 19u))
                assert(!f.writes[i].level);
        cleanup(&f, &r);
        for (unsigned int wait = 1; wait <= 320; ++wait) {
            f = (struct fake){.now=1000000000, .cancel_wait=wait};
            assert(run(&f, o, &r) < 0 && errno == ECANCELED);
            assert(!r.single.completed && r.transition_written == wait);
            cleanup(&f, &r);
        }
        f = (struct fake){.now=1000000000, .fail_write=12};
        assert(run(&f, o, &r) < 0 && !r.single.completed);
        assert(r.transition_attempted == 10 && r.transition_written == 9);
        cleanup(&f, &r);
        f = (struct fake){.now=1000000000, .fail_wait=8};
        assert(run(&f, o, &r) < 0 && r.transition_written == 8);
        cleanup(&f, &r);
        f = (struct fake){.now=1000000000, .fail_write=323};
        assert(run(&f, o, &r) < 0 && !r.cleanup_ok);
        cleanup(&f, &r);
        for (unsigned int check = 1; check <= 2; ++check) {
            f = (struct fake){.fail_check=check};
            assert(run(&f, o, &r) < 0 && !r.transition_attempted);
            if (check == 1) assert(!f.opens && !f.count);
            else cleanup(&f, &r);
        }
        o.transitions = 2;
        o.interval_us = 1000;
        f = (struct fake){.now=1000000000};
        assert(run(&f, o, &r) == 0 && r.single.end_ns == 1002000000);
        o.interval_us = 20000;
        f = (struct fake){.now=1000000000};
        assert(run(&f, o, &r) == 0 && r.single.end_ns == 1040000000);
        o.transitions = 10000;
        o.interval_us = 6000;
        f = (struct fake){.now=1000000000};
        assert(run(&f, o, &r) == 0 && r.single.end_ns == 61000000000LL);
        assert(r.transition_written == 10000);
        cleanup(&f, &r);
        o.transitions = 2;
        o.interval_us = 20000;
        for (unsigned int bad = 0; bad < 8; ++bad) {
            struct stimulus_options invalid = o;
            switch (bad) {
            case 0: invalid.transitions=0; break;
            case 1: invalid.transitions=3; break;
            case 2: invalid.transitions=10002; break;
            case 3: invalid.interval_us=999; break;
            case 4: invalid.interval_us=20001; break;
            case 5: invalid.transitions=10000; break; /* >60 s at 20 ms */
            case 6: invalid.disconnected_ack=false; break;
            case 7: invalid.line=(enum stimulus_line)99; break;
            }
            f = (struct fake){0};
            assert(run(&f, invalid, &r) < 0 && !f.opens && !f.checks);
        }
    }
}
static void busy_hold_tests(void)
{
    struct stimulus_options o = {.disconnected_ack=true, .line=STIM_DATA_ONLY,
        .transitions=320, .interval_us=5000, .hold_busy_high=true};
    struct stimulus_report r;
    struct fake f = {.now=1000000000};
    assert(run(&f, o, &r) == 0);
    assert(f.checks == 2 && f.opens == 1 && f.count == 325);
    assert(r.single.completed && r.transition_written == 320);
    assert(r.busy_high_attempted && r.busy_high_written && r.busy_low_written);
    assert(r.busy_high_write_ns == 1000000000 && r.busy_high_done_ns == 1000000000);
    assert(r.busy_lead_wake_ns == 1005000000);
    assert(r.transitions[0].write_ns == 1005000000);
    assert(r.busy_low_write_ns == 2605000000);
    assert(r.single.end_ns - r.single.start_ns == 1600000000);
    assert(f.writes[2].gpio == 19 && f.writes[2].level == 1);
    unsigned int levels[2] = {0}, edges[2] = {0}, rising[2] = {0}, falling[2] = {0};
    struct sniff_record observed[322];
    unsigned int count = 0;
    for (unsigned int i = 0; i < f.count; ++i) {
        const struct write_event *w = &f.writes[i];
        unsigned int line = w->gpio == 6 ? 0 : 1;
        if ((unsigned int)w->level == levels[line]) continue;
        if (line == 0) assert(levels[1] == 1); /* BUSY held for every DATA edge. */
        levels[line] = (unsigned int)w->level;
        ++edges[line];
        if (w->level) ++rising[line]; else ++falling[line];
        assert(count < 322);
        observed[count] = (struct sniff_record){
            .timestamp_ns=(uint64_t)w->time, .read_ns=(uint64_t)w->time,
            .gpio=line ? 26 : 13, .global_seqno=count+1, .line_seqno=edges[line],
            .rising=w->level != 0, .batch=count+1, .batch_size=1
        };
        ++count;
    }
    assert(edges[0] == 320 && rising[0] == 160 && falling[0] == 160);
    assert(edges[1] == 2 && rising[1] == 1 && falling[1] == 1);
    assert(!levels[0] && !levels[1] && count == 322);
    FILE *csv = tmpfile(), *diag = tmpfile();
    struct sniff_summary summary;
    assert(csv && diag && sniff_report(csv, diag, observed, count, &summary) == 0);
    assert(!summary.gaps && !summary.regressions && !summary.timestamp_regressions);
    fclose(csv); fclose(diag);
    cleanup(&f, &r);

    /* Lead interval and every transition wait can be interrupted safely. */
    for (unsigned int wait = 1; wait <= 321; ++wait) {
        f = (struct fake){.now=1000000000, .cancel_wait=wait};
        assert(run(&f, o, &r) < 0 && errno == ECANCELED);
        assert(r.busy_high_written && r.busy_low_written && !r.single.completed);
        cleanup(&f, &r);
    }
    const unsigned int failures[] = {3, 4, 100, 323, 324, 325};
    for (size_t i = 0; i < sizeof(failures)/sizeof(failures[0]); ++i) {
        f = (struct fake){.now=1000000000, .fail_write=failures[i]};
        assert(run(&f, o, &r) < 0);
        cleanup(&f, &r);
        if (failures[i] == 3) {
            assert(r.busy_high_attempted && !r.busy_high_written);
            assert(!r.transition_attempted);
        }
        if (failures[i] >= 324) assert(!r.cleanup_ok);
        if (failures[i] == 325) assert(!r.busy_low_written);
    }
    f = (struct fake){.now=1000000000, .fail_wait=1};
    assert(run(&f, o, &r) < 0 && !r.transition_attempted);
    cleanup(&f, &r);
    f = (struct fake){.now=1000000000, .late=300000000};
    assert(run(&f, o, &r) < 0 && errno == ETIMEDOUT && !r.transition_attempted);
    cleanup(&f, &r);
    for (unsigned int check = 1; check <= 2; ++check) {
        f = (struct fake){.fail_check=check};
        assert(run(&f, o, &r) < 0 && !r.busy_high_attempted);
        if (check == 1) assert(!f.opens && !f.count);
        else cleanup(&f, &r);
    }
    for (unsigned int invalid = 0; invalid < 4; ++invalid) {
        struct stimulus_options bad = o;
        if (invalid == 0) bad.line=STIM_COMBINED;
        if (invalid == 1) bad.line=STIM_BUSY_ONLY;
        if (invalid == 2) bad.disconnected_ack=false;
        if (invalid == 3) { bad.transitions=10000; bad.interval_us=6000; }
        f = (struct fake){0};
        assert(run(&f, bad, &r) < 0 && !f.opens && !f.checks && !f.count);
    }
}
static void busy_high_only_tests(void)
{
    struct stimulus_options o = {.disconnected_ack=true,
        .hold_busy_high_only=true, .duration_ms=3000};
    struct stimulus_report r;
    struct fake f = {.now=1000000000};
    assert(run(&f, o, &r) == 0);
    assert(f.checks == 2 && f.opens == 1 && f.waits == 1 && f.count == 5);
    assert(r.busy_high_attempted && r.busy_high_written && r.busy_low_written);
    assert(r.busy_hold_completed && r.cleanup_ok);
    assert(!r.transition_attempted && !r.transition_written && !r.attempted);
    assert(r.busy_high_write_ns == 1000000000 && r.busy_high_done_ns == 1000000000);
    assert(r.busy_hold_deadline_ns == 4000000000 && r.busy_hold_wake_ns == 4000000000);
    assert(r.busy_low_write_ns == 4000000000 && r.busy_low_done_ns == 4000000000);
    assert(f.writes[2].gpio == 19 && f.writes[2].level == 1);
    unsigned int levels[2] = {0}, edges[2] = {0};
    struct sniff_record observed[2];
    unsigned int count = 0;
    for (unsigned int i = 0; i < f.count; ++i) {
        const struct write_event *w = &f.writes[i];
        unsigned int line = w->gpio == 6 ? 0 : 1;
        if (line == 0) assert(!w->level); /* DATA never HIGH, even transiently. */
        if ((unsigned int)w->level == levels[line]) continue;
        levels[line] = (unsigned int)w->level;
        ++edges[line];
        assert(count < 2);
        observed[count] = (struct sniff_record){
            .timestamp_ns=(uint64_t)w->time, .read_ns=(uint64_t)w->time,
            .gpio=line ? 26 : 13, .global_seqno=count+1, .line_seqno=edges[line],
            .rising=w->level != 0, .batch=count+1, .batch_size=1
        };
        ++count;
    }
    assert(edges[0] == 0 && edges[1] == 2 && !levels[0] && !levels[1]);
    assert(observed[0].rising && !observed[1].rising);
    FILE *csv = tmpfile(), *diag = tmpfile();
    struct sniff_summary summary;
    assert(csv && diag && sniff_report(csv, diag, observed, count, &summary) == 0);
    assert(!summary.gaps && !summary.late && !summary.regressions);
    fclose(csv); fclose(diag);
    cleanup(&f, &r);

    const unsigned int valid[] = {1, 60000};
    for (size_t i = 0; i < sizeof(valid)/sizeof(valid[0]); ++i) {
        o.duration_ms = valid[i];
        f = (struct fake){.now=1000000000};
        assert(run(&f, o, &r) == 0);
        assert(r.busy_low_write_ns - r.busy_high_done_ns ==
               (int64_t)o.duration_ms * 1000000);
        cleanup(&f, &r);
    }
    o.duration_ms = 3000;
    f = (struct fake){.now=1000000000, .cancel_wait=1};
    assert(run(&f, o, &r) < 0 && errno == ECANCELED);
    assert(r.busy_high_written && r.busy_low_written && !r.busy_hold_completed);
    cleanup(&f, &r);
    f = (struct fake){.now=1000000000, .fail_wait=1};
    assert(run(&f, o, &r) < 0 && r.busy_low_written && !r.busy_hold_completed);
    cleanup(&f, &r);
    f = (struct fake){.now=1000000000, .late=300000000};
    assert(run(&f, o, &r) < 0 && errno == ETIMEDOUT && !r.busy_hold_completed);
    cleanup(&f, &r);
    for (unsigned int write = 1; write <= 5; ++write) {
        f = (struct fake){.now=1000000000, .fail_write=write};
        assert(run(&f, o, &r) < 0);
        cleanup(&f, &r);
        if (write == 3) {
            assert(r.busy_high_attempted && !r.busy_high_written && !f.waits);
        }
        if (write >= 4) assert(!r.cleanup_ok);
        if (write == 5) assert(!r.busy_low_written);
    }
    for (unsigned int now = 1; now <= 6; ++now) {
        f = (struct fake){.now=1000000000, .fail_now=now};
        assert(run(&f, o, &r) < 0);
        cleanup(&f, &r); /* Clock errors cannot skip either LOW write. */
    }
    for (unsigned int check = 1; check <= 2; ++check) {
        f = (struct fake){.fail_check=check};
        assert(run(&f, o, &r) < 0 && !r.busy_high_attempted);
        if (check == 1) assert(!f.opens && !f.count);
        else cleanup(&f, &r);
    }
    f = (struct fake){.open_failure=true};
    assert(run(&f, o, &r) < 0 && f.closes == 1 && !f.count);
    f = (struct fake){.cancel=true};
    assert(run(&f, o, &r) < 0 && !f.opens);
    for (unsigned int invalid = 0; invalid < 6; ++invalid) {
        struct stimulus_options bad = o;
        if (invalid == 0) bad.duration_ms=0;
        if (invalid == 1) bad.duration_ms=60001;
        if (invalid == 2) bad.line=STIM_DATA_ONLY;
        if (invalid == 3) bad.hold_busy_high=true;
        if (invalid == 4) bad.disconnected_ack=false;
        if (invalid == 5) bad.hold_busy_high_only=false; /* Duration without mode. */
        f = (struct fake){0};
        assert(run(&f, bad, &r) < 0 && !f.opens && !f.checks && !f.count);
    }
}
int main(void)
{
    struct stimulus_options options = {.frames=4, .gap_ms=100, .disconnected_ack=true};
    struct stimulus_report report;
    struct fake f = {.now = 1000000000};
    assert(run(&f, options, &report) == 0);
    assert(report.completed == 4 && f.checks == 2 && f.opens == 1);
    assert(f.count == 2 + 4 * 20 + 2);
    assert(!f.writes[0].level && !f.writes[1].level);
    for (unsigned int i = 0; i < 4; ++i) {
        const struct stimulus_frame *frame = &report.frames[i];
        const struct write_event *w = &f.writes[2 + i * 20];
        assert(frame->completed && frame->samples == 19);
        assert(frame->end_ns - frame->start_ns == stimulus_frame_ns(frame->byte));
        assert(w[0].gpio == 19 && w[0].level == 1);
        assert(w[1].gpio == 6 && w[1].level == 1);
        assert(w[1].time - w[0].time == 1150000);
        assert(w[2].time - w[1].time == 9800000);
        for (unsigned int bit = 0; bit < 8; ++bit) {
            const struct write_event *low = &w[2 + bit * 2];
            const struct write_event *high = low + 1;
            assert(low->gpio == 6 && !low->level);
            assert(high->gpio == 6 && high->level);
            assert(high->time - low->time ==
                   (frame->byte & (1u << bit) ? 9800000 : 4900000));
            assert(w[4 + bit * 2].time - high->time == 4900000);
        }
        assert(w[18].gpio == 6 && !w[18].level);
        assert(w[19].gpio == 19 && !w[19].level);
        assert(w[19].time - w[18].time == 750000);
        if (i) assert(frame->start_ns - report.frames[i - 1].end_ns == 100000000);
    }
    assert(report.frames[0].byte == 0x00 && report.frames[1].byte == 0xff);
    assert(report.frames[2].byte == 0x55 && report.frames[3].byte == 0xaa);
    cleanup(&f, &report);

    /* Feed only synthetic level changes to the existing sniffer CSV analyzer.
     * This assumes ideal non-inverting loopback, not actual H11 validation. */
    struct sniff_record edges[100] = {0};
    unsigned int levels[2] = {0}, line_seq[2] = {0};
    size_t count = 0;
    for (unsigned int i = 0; i < f.count; ++i) {
        const struct write_event *w = &f.writes[i];
        unsigned int line = w->gpio == 6 ? 0 : 1;
        if ((unsigned int)w->level == levels[line]) continue;
        levels[line] = (unsigned int)w->level;
        edges[count] = (struct sniff_record){
            .timestamp_ns = (uint64_t)w->time, .read_ns = (uint64_t)w->time,
            .gpio = line ? 26 : 13, .global_seqno = (uint32_t)count + 1,
            .line_seqno = ++line_seq[line], .rising = w->level != 0,
            .batch = count / 8 + 1, .index = count % 8, .batch_size = 8
        };
        ++count;
    }
    assert(count == 80 && line_seq[0] == 72 && line_seq[1] == 8);
    FILE *csv = tmpfile(), *diag = tmpfile();
    struct sniff_summary summary;
    assert(csv && diag && sniff_report(csv, diag, edges, count, &summary) == 0);
    assert(!summary.gaps && !summary.regressions && !summary.timestamp_regressions);
    fclose(csv); fclose(diag);

    const struct stimulus_options invalid[] = {
        {.frames=1, .gap_ms=100, .disconnected_ack=false}, {.frames=0, .gap_ms=100, .disconnected_ack=true}, {.frames=257, .gap_ms=100, .disconnected_ack=true},
        {.frames=1, .gap_ms=0, .disconnected_ack=true}, {.frames=1, .gap_ms=1001, .disconnected_ack=true}, {.frames=256, .gap_ms=1000, .disconnected_ack=true}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        f = (struct fake){0};
        assert(run(&f, invalid[i], &report) < 0);
        assert(!f.opens && !f.checks && !f.count);
    }
    options = (struct stimulus_options){.frames=256, .gap_ms=100, .disconnected_ack=true};
    f = (struct fake){.now = 1000000000};
    assert(run(&f, options, &report) == 0 && report.completed == 256);
    assert(f.now - 1000000000 <= STIM_MAX_RUN_NS);
    cleanup(&f, &report);

    options = (struct stimulus_options){.frames=2, .gap_ms=1, .disconnected_ack=true};
    f = (struct fake){.now = 1000000000};
    assert(run(&f, options, &report) == 0 && report.completed == 2);
    assert(report.frames[1].start_ns - report.frames[0].end_ns == 1000000);
    /* Every initial/frame write failure aborts, then attempts both LOWs. */
    for (unsigned int i = 1; i <= 22; ++i) {
        f = (struct fake){.now = 1000000000, .fail_write = i};
        assert(run(&f, options, &report) < 0);
        assert(report.completed == 0 && report.attempted <= 1);
        cleanup(&f, &report);
        assert(report.cleanup_ok);
    }
    for (unsigned int i = 1; i <= 20; ++i) {
        f = (struct fake){.now = 1000000000, .fail_wait = i};
        assert(run(&f, options, &report) < 0);
        cleanup(&f, &report);
        f = (struct fake){.now = 1000000000, .cancel_wait = i};
        assert(run(&f, options, &report) < 0 && errno == ECANCELED);
        cleanup(&f, &report);
    }
    options = (struct stimulus_options){.frames=1, .gap_ms=100, .disconnected_ack=true};
    f = (struct fake){.now = 1000000000, .fail_write = 23};
    assert(run(&f, options, &report) < 0 && !report.cleanup_ok);
    cleanup(&f, &report); /* DATA LOW failure must not skip BUSY LOW/release. */
    f = (struct fake){.open_failure = true};
    assert(run(&f, options, &report) < 0 && f.closes == 1 && !f.count);
    for (unsigned int i = 1; i <= 2; ++i) {
        f = (struct fake){.fail_check = i};
        assert(run(&f, options, &report) < 0 && !report.attempted);
        if (i == 1) assert(!f.opens && !f.count);
        else cleanup(&f, &report);
    }
    f = (struct fake){.now_failure = true};
    assert(run(&f, options, &report) < 0);
    cleanup(&f, &report);
    f = (struct fake){.now = 1000000000, .late = 600000};
    assert(run(&f, options, &report) == 0);
    assert(report.frames[0].max_late_ns == 600000);
    assert(report.frames[0].over_500us == 19 && !report.frames[0].over_1000us);
    f = (struct fake){.now = 1000000000, .late = 300000000};
    assert(run(&f, options, &report) < 0 && errno == ETIMEDOUT);
    cleanup(&f, &report);
    f = (struct fake){.cancel = true};
    assert(run(&f, options, &report) < 0 && !f.opens);

    assert(stimulus_service_state("LoadState=loaded\nActiveState=inactive\n"));
    assert(stimulus_service_state("ActiveState=inactive\nLoadState=loaded\n"));
    const char *unsafe[] = {
        "", "LoadState=not-found\nActiveState=inactive\n",
        "LoadState=loaded\nActiveState=active\n",
        "LoadState=loaded\nActiveState=failed\n",
        "LoadState=loaded\nActiveState=activating\n",
        "LoadState=loaded\nActiveState=inactive\nActiveState=inactive\n",
        "ActiveState=inactive\n", "garbage"
    };
    for (size_t i = 0; i < sizeof(unsafe) / sizeof(unsafe[0]); ++i)
        assert(!stimulus_service_state(unsafe[i]));
    single_line_tests();
    busy_hold_tests();
    busy_high_only_tests();
    puts("xs8_stimulus combined/single-line/BUSY-hold/BUSY-only fake-backend tests passed");
    return 0;
}
