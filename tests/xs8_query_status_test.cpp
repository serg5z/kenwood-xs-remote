#include "xs8_query_status.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

static xs8_monitor_record frame(uint64_t serial, uint64_t start,
                                uint64_t end, uint8_t lsb, uint8_t legacy,
                                bool valid = true, const char *reason = nullptr)
{
    xs8_monitor_record record{};
    record.serial = serial;
    record.end_ns = end;
    record.decoded.kind = XS8_FRAME;
    record.decoded.time_ns = start;
    record.decoded.waveform_lsb = lsb;
    record.decoded.legacy_msb_short1 = legacy;
    record.decoded.valid = valid;
    record.decoded.reason = reason;
    return record;
}

static Xs8QueryStatus query()
{
    return Xs8QueryStatus(1000, 2500);
}

static void assert_contains(const std::string& text, const char *part)
{
    assert(text.find(part) != std::string::npos);
}

static void test_all_known_pairs()
{
    const struct { uint8_t legacy, lsb; const char *name; } sources[] = {
        {0x11, 0x77, "VIDEO2"}, {0x13, 0x37, "PHONO"},
        {0x19, 0x67, "TAPE1"}, {0x1A, 0xA7, "VIDEO1"},
        {0x1B, 0x27, "TUNER"}, {0x1D, 0x47, "CD"}
    };
    const struct { uint8_t legacy, lsb; const char *name; } monitors[] = {
        {0x18, 0xE7, "OFF"}, {0x14, 0xD7, "ON"}
    };
    for (const auto& source : sources) {
        for (const auto& monitor : monitors) {
            Xs8QueryStatus result = query();
            const xs8_monitor_record records[] = {
                frame(1, 1100, 1500, 0x13, 0x37),
                // Both responses start before the TX function returns at 2500.
                frame(2, 1600, 1700, source.lsb, source.legacy),
                frame(3, 1800, 1900, monitor.lsb, monitor.legacy),
            };
            result.observe(records, 3);
            assert(!result.done(3000)); // Keep watching for extra frames.
            assert(result.done(5000002500ULL));
            std::string response = result.response(5000002500ULL);
            assert_contains(response, "OK QUERY_STATUS");
            assert_contains(response, "tx=0x13/0x37@1100-1500:VALID");
            char raw_source[48], raw_monitor[48];
            std::snprintf(raw_source, sizeof(raw_source),
                          "rx1=0x%02X/0x%02X@1600-1700:VALID",
                          source.lsb, source.legacy);
            std::snprintf(raw_monitor, sizeof(raw_monitor),
                          "rx2=0x%02X/0x%02X@1800-1900:VALID",
                          monitor.lsb, monitor.legacy);
            assert_contains(response, raw_source);
            assert_contains(response, raw_monitor);
            assert_contains(response, "attribution=TEMPORAL");
            assert_contains(response, "receiver_execution=UNVERIFIED");
            assert_contains(response, (std::string(" source_inferred=") +
                                       source.name).c_str());
            assert_contains(response, (std::string(" monitor_inferred=") +
                                       monitor.name).c_str());
        }
    }
}

static void test_failures_and_unknown()
{
    const xs8_monitor_record tx = frame(1, 1100, 1500, 0x13, 0x37);
    const xs8_monitor_record source = frame(2, 1600, 1700, 0x47, 0x1D);
    const xs8_monitor_record monitor = frame(3, 1800, 1900, 0xE7, 0x18);
    const uint64_t deadline = 5000002500ULL;

    Xs8QueryStatus unknown = query();
    const xs8_monitor_record unknown_pair[] = {
        tx, frame(2, 1600, 1700, 0x22, 0x44), monitor
    };
    unknown.observe(unknown_pair, 3);
    assert_contains(unknown.response(deadline), "source_inferred=UNKNOWN");
    assert_contains(unknown.response(deadline), "monitor_inferred=OFF");

    // One frame after the echo is PARTIAL, not silence.
    Xs8QueryStatus missing = query();
    const xs8_monitor_record partial[] = {tx, source};
    missing.observe(partial, 2);
    assert(missing.done(deadline) && !missing.successful(deadline));
    assert_contains(missing.response(deadline), "ERR QUERY_STATUS reason=PARTIAL ");
    assert_contains(missing.response(deadline), "rx1=0x47/0x1D");
    assert(missing.response(deadline).find(" rx2=") == std::string::npos);
    assert_contains(missing.response(deadline), "source_inferred=UNKNOWN");
    assert_contains(missing.response(deadline), "monitor_inferred=UNKNOWN");

    // A verified echo and a silent window: NO_REPLY, distinct from every fault.
    Xs8QueryStatus silent = query();
    silent.observe(&tx, 1);
    assert(!silent.done(silent.deadline_ns() - 1));
    assert(silent.done(silent.deadline_ns()) && !silent.successful(silent.deadline_ns()));
    assert_contains(silent.response(deadline), "ERR QUERY_STATUS reason=NO_REPLY ");
    assert_contains(silent.response(deadline), " tx=0x13/");
    assert(silent.response(deadline).find(" rx1=") == std::string::npos);
    assert_contains(silent.response(deadline), "gap_events=0 ");
    // A frame after the deadline does not turn silence into PARTIAL.
    Xs8QueryStatus late = query();
    const uint64_t window_end = late.deadline_ns();
    const xs8_monitor_record after[] = {tx, frame(2, window_end + 1, window_end + 100, 0x47, 0x1D)};
    late.observe(after, 2);
    assert_contains(late.response(window_end + 200), "reason=NO_REPLY ");
    // Silence plus lost events is EVENT_GAP, never NO_REPLY.
    Xs8QueryStatus silent_gap = query();
    xs8_monitor_record lost{};
    lost.decoded.kind = XS8_EVENT_GAP;
    lost.decoded.lost_events = 1;
    lost.decoded.reason = "EVENT_GAP";
    const xs8_monitor_record with_gap[] = {tx, lost};
    silent_gap.observe(with_gap, 2);
    assert_contains(silent_gap.response(deadline), "reason=EVENT_GAP ");
    // Overflow and monitor failure are faults, never silence.
    Xs8QueryStatus overflowed = query();
    overflowed.observe(&tx, 1);
    overflowed.overflow();
    assert_contains(overflowed.response(deadline), "reason=OVERFLOW ");
    Xs8QueryStatus failed = query();
    failed.observe(&tx, 1);
    failed.monitor_failed();
    assert_contains(failed.response(deadline), "reason=MONITOR_FAILED ");

    Xs8QueryStatus no_tx = query();
    assert_contains(no_tx.response(deadline), "reason=TX_MISSING");

    Xs8QueryStatus extra = query();
    const xs8_monitor_record four[] = {
        tx, source, monitor, frame(4, 2000, 2100, 0x99, 0x55)
    };
    extra.observe(four, 4);
    assert_contains(extra.response(3000), "reason=EXTRA");
    assert_contains(extra.response(3000), "rx3=0x99/0x55");

    Xs8QueryStatus truncated = query();
    const xs8_monitor_record invalid[] = {
        tx, frame(2, 1600, 1700, 0x47, 0x1D, false, "TRUNCATED")
    };
    truncated.observe(invalid, 2);
    assert_contains(truncated.response(3000), "reason=INCOMPLETE");
    assert_contains(truncated.response(3000), "INCOMPLETE:TRUNCATED");

    Xs8QueryStatus unfinished = query();
    xs8_monitor_record partial_frame =
        frame(0, 1600, deadline, 0x07, 0x33, false,
              "CAPTURE_WINDOW_ENDED");
    partial_frame.decoded.bits = 4;
    const xs8_monitor_record unfinished_frames[] = {tx, partial_frame};
    unfinished.observe(unfinished_frames, 2);
    assert_contains(unfinished.response(deadline), "reason=INCOMPLETE");
    assert_contains(unfinished.response(deadline),
                    "CAPTURE_WINDOW_ENDED:bits=4");

    Xs8QueryStatus swapped = query();
    const xs8_monitor_record reversed[] = {
        tx, frame(2, 1600, 1700, 0xE7, 0x18),
        frame(3, 1800, 1900, 0x47, 0x1D)
    };
    swapped.observe(reversed, 3);
    assert_contains(swapped.response(3000), "reason=OUT_OF_ORDER");

    Xs8QueryStatus overlap = query();
    const xs8_monitor_record crossed[] = {
        tx, frame(2, 1400, 1700, 0x47, 0x1D)
    };
    overlap.observe(crossed, 2);
    assert_contains(overlap.response(3000), "reason=OVERLAP");

    Xs8QueryStatus mismatch = query();
    const xs8_monitor_record wrong_tx =
        frame(1, 1100, 1500, 0x14, 0xD7);
    mismatch.observe(&wrong_tx, 1);
    assert_contains(mismatch.response(3000), "reason=TX_MISMATCH");

    Xs8QueryStatus gap = query();
    xs8_monitor_record event_gap{};
    event_gap.serial = 2;
    event_gap.decoded.kind = XS8_EVENT_GAP;
    event_gap.decoded.lost_events = 3;
    const xs8_monitor_record gapped[] = {tx, event_gap};
    gap.observe(gapped, 2);
    assert_contains(gap.response(3000), "reason=EVENT_GAP");
    assert_contains(gap.response(3000), "gap_events=3");

    Xs8QueryStatus interrupted = query();
    xs8_monitor_record broken =
        frame(2, 1600, 1700, 0x00, 0x00, false, "EVENT_GAP");
    event_gap.decoded.time_ns = 1700;
    event_gap.decoded.reason = "MISSING_EVENTS";
    const xs8_monitor_record interrupted_frames[] = {tx, broken, event_gap};
    interrupted.observe(interrupted_frames, 3);
    assert_contains(interrupted.response(3000), "reason=EVENT_GAP");
    assert_contains(interrupted.response(3000), "gap_ns=1700");
    assert_contains(interrupted.response(3000), "gap_reason=MISSING_EVENTS");

    Xs8QueryStatus wrapped = query();
    wrapped.observe(&tx, 1);
    wrapped.overflow();
    assert_contains(wrapped.response(3000), "reason=OVERFLOW");

    Xs8QueryStatus failed_monitor = query();
    failed_monitor.monitor_failed();
    assert_contains(failed_monitor.response(3000), "reason=MONITOR_FAILED");

    Xs8QueryStatus cancelled = query();
    cancelled.cancel();
    assert_contains(cancelled.response(3000), "reason=CANCELLED");
    Xs8QueryStatus between = query();
    between.capture_warning();
    assert_contains(between.response(between.deadline_ns()), "reason=GLOBAL_GAP_WARNING");
    Xs8QueryStatus warned = query();
    auto warned_tx = tx;
    warned_tx.decoded.global_gap_warnings = 1;
    const xs8_monitor_record warned_pair[] = {warned_tx, source, monitor};
    warned.observe(warned_pair, 3);
    assert(!warned.successful(warned.deadline_ns()));
    assert_contains(warned.response(warned.deadline_ns()), "reason=GLOBAL_GAP_WARNING");
    assert_contains(warned.response(warned.deadline_ns()), "source_inferred=UNKNOWN");
}

int main()
{
    test_all_known_pairs();
    test_failures_and_unknown();
    std::cout << "XS8 query status offline tests passed\n";
}
