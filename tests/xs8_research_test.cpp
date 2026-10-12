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
    std::cout << "XS8 research observation tests passed\n";
}
