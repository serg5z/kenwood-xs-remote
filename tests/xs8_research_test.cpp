#include "xs8_research.h"

#include <cassert>
#include <iostream>

static xs8_monitor_record frame(uint64_t serial, uint64_t start,
                                uint8_t byte, bool valid = true)
{
    xs8_monitor_record record{};
    record.serial = serial;
    record.end_ns = start + 100;
    record.decoded.kind = XS8_FRAME;
    record.decoded.time_ns = start;
    record.decoded.valid = valid;
    record.decoded.waveform_lsb = byte;
    record.decoded.legacy_msb_short1 = 0xC6;
    record.decoded.bits = valid ? 8 : 3;
    record.decoded.reason = valid ? nullptr : "CAPTURE_WINDOW_ENDED";
    return record;
}

int main()
{
    std::vector<uint8_t> parsed;
    long gap_ms = 0, window_ms = 0;
    assert(xs8_parse_research_chain(
        "RESEARCH_CHAIN 2 200 1500 0xFC 0xEC", parsed,
        gap_ms, window_ms));
    assert((parsed == std::vector<uint8_t>{0xFC, 0xEC}));
    assert(gap_ms == 200 && window_ms == 1500);
    for (const char *invalid : {
        "RESEARCH_CHAIN 1 200 1500 0xFC",
        "RESEARCH_CHAIN 5 200 1500 0xFC 0xEC 0x01 0x02 0x03",
        "RESEARCH_CHAIN 2 0 1500 0xFC 0xEC",
        "RESEARCH_CHAIN 2 1001 1500 0xFC 0xEC",
        "RESEARCH_CHAIN 2 200 5001 0xFC 0xEC",
        "RESEARCH_CHAIN 2 200 1500 0xFC",
        "RESEARCH_CHAIN 2 200 1500 0xFC 0xEC 0xAA",
        "RESEARCH_CHAIN 2 200 1500 0xFC 0xGG",
        "RESEARCH_CHAIN 2 200 1500 0xFC 0xEC extra"})
        assert(!xs8_parse_research_chain(invalid, parsed,
                                         gap_ms, window_ms));
    Xs8ResearchSession session(7, 10, 1000, 2000, true, 0x9C, 1050, 1200);
    assert(session.active() && session.cursor() == 10);
    auto tx = frame(11, 1100, 0x9C);
    auto rx = frame(12, 1400, 0xD6);
    session.observe(&tx, 1);
    session.observe(&rx, 1);
    assert(session.read(0).find("waveform_lsb=0x9C") != std::string::npos);
    assert(session.read(0).find("relation=TX_WINDOW") != std::string::npos);
    assert(session.read(1).find("origin=UNATTRIBUTED") != std::string::npos);
    assert(session.read(1).find("relation=OTHER_TRAFFIC") !=
           std::string::npos);
    assert(session.read(2).find("RESEARCH_WAIT") != std::string::npos);
    session.finish(1999);
    assert(session.active());
    session.finish(2000);
    assert(!session.active());
    assert(session.read(2).find("requested=0x9C") != std::string::npos);

    Xs8ResearchSession bad(8, 0, 0, 1000, false);
    xs8_monitor_record gap{};
    gap.decoded.kind = XS8_EVENT_GAP;
    gap.decoded.time_ns = 200;
    gap.decoded.lost_events = 3;
    gap.decoded.reason = "EVENT_GAP";
    auto partial = frame(2, 400, 0x00, false);
    bad.observe(&gap, 1);
    bad.overflow();
    bad.monitor_failed();
    bad.finish(1000, &partial);
    assert(bad.read(0).find("kind=GAP") != std::string::npos);
    assert(bad.read(0).find("lost_events=3") != std::string::npos);
    assert(bad.read(1).find("valid=0") != std::string::npos);
    assert(bad.read(2).find("overflow=1 monitor_failed=1") !=
           std::string::npos);
    Xs8ResearchSession boundary(10, 0, 100, 500, false);
    auto crossing = frame(1, 450, 0x42);
    boundary.observe(&crossing, 1);
    boundary.finish(500);
    assert(boundary.read(0).find("valid=0") != std::string::npos);
    assert(boundary.read(0).find("reason=CAPTURE_WINDOW_ENDED") !=
           std::string::npos);
    Xs8ResearchSession late_deadline(12, 0, 0, UINT64_MAX, false);
    auto late_crossing = frame(1, 450, 0x42);
    auto too_late = frame(2, 600, 0x43);
    late_deadline.observe(&late_crossing, 1);
    late_deadline.observe(&too_late, 1);
    late_deadline.set_deadline(500);
    late_deadline.finish(500);
    assert(late_deadline.read(0).find("reason=CAPTURE_WINDOW_ENDED") !=
           std::string::npos);
    assert(late_deadline.read(1).find("count=1") != std::string::npos);
    Xs8ResearchSession capped(11, 0, 0, 1000, false);
    auto at_start = frame(1, 1, 0x13);
    for (size_t i = 0; i <= Xs8ResearchSession::kMaxRecords; ++i)
        capped.observe(&at_start, 1);
    capped.finish(1000);
    assert(capped.read(Xs8ResearchSession::kMaxRecords).find("overflow=1") !=
           std::string::npos);
    Xs8ResearchSession cancelled(9, 0, 0, 5000000000ULL, false);
    cancelled.cancel();
    assert(cancelled.read(0).find("cancelled=1") != std::string::npos);

    constexpr uint64_t ms = 1000000ULL;
    Xs8ResearchSession sequence(20, 0, 100, UINT64_MAX, false);
    sequence.set_chain();
    Xs8ResearchChain chain(20, {0xFC, 0xEC}, 200 * ms, 1500 * ms);
    int sends = 0;
    auto ready = [] { return std::string(); };
    auto send = [&](uint8_t byte) {
        ++sends;
        uint64_t start = sends == 1 ? 100 : 100 + 200 * ms + 100;
        return Xs8ResearchChain::Attempt{byte, start, start + 100,
                                         true, "MATCH"};
    };
    chain.tick(100, ready, send, sequence);
    assert(sends == 1 && chain.active());
    assert(chain.read(0).find("requested=0xFC") != std::string::npos);
    auto first = frame(1, 120, 0xFC);
    sequence.observe(&first, 1);
    auto between = frame(2, 1000, 0xD6);
    sequence.observe(&between, 1);
    chain.tick(100 + 200 * ms + 99, ready, send, sequence);
    assert(sends == 1); // Gap starts at the previous TX end, not its start.
    chain.tick(100 + 200 * ms + 100, ready, send, sequence);
    assert(sends == 2 && !chain.active());
    auto second = frame(3, 100 + 200 * ms + 120, 0xEC);
    sequence.observe(&second, 1);
    assert(sequence.read(0).find("relation=TX_WINDOW") != std::string::npos);
    assert(sequence.read(1).find("relation=OTHER_TRAFFIC") !=
           std::string::npos);
    assert(sequence.read(2).find("relation=TX_WINDOW") != std::string::npos);
    assert(chain.read(1).find("requested=0xEC") != std::string::npos);
    assert(chain.read(2).find("planned=2 aborted=0") != std::string::npos);
    sequence.finish(sequence.deadline_ns());
    assert(sequence.read(3).find("kind=CHAIN") != std::string::npos);

    Xs8ResearchSession failed_capture(21, 0, 0, UINT64_MAX, false);
    Xs8ResearchChain failed(21, {0xFC, 0xEC}, 200 * ms, 1500 * ms);
    int failed_sends = 0;
    failed.tick(1, ready, [&](uint8_t byte) {
        ++failed_sends;
        return Xs8ResearchChain::Attempt{byte, 1, 100, true, "MISSING"};
    }, failed_capture);
    failed.tick(300 * ms, ready, send, failed_capture);
    assert(failed_sends == 1 && failed.uncertain());
    assert(failed.read(1).find("aborted=1 reason=TX_UNCERTAIN") !=
           std::string::npos);
    Xs8ResearchSession denied_capture(22, 0, 0, UINT64_MAX, false);
    Xs8ResearchChain denied(22, {0xFC, 0xEC}, 200 * ms, 1500 * ms);
    denied.tick(1, [] { return std::string("ERR FAULT"); }, send,
                denied_capture);
    assert(denied.read(0).find("count=0 planned=2 aborted=1") !=
           std::string::npos);
    Xs8ResearchSession stopped_capture(23, 0, 0, UINT64_MAX, false);
    Xs8ResearchChain stopped(23, {0xFC, 0xEC}, 200 * ms, 1500 * ms);
    stopped.abort(1, "CANCELLED", stopped_capture);
    stopped.tick(1000 * ms, ready, send, stopped_capture);
    assert(stopped.read(0).find("reason=CANCELLED") != std::string::npos);
    std::cout << "XS8 research observation tests passed\n";
}
