#include "xs8_cd_probe.h"

#include <cassert>
#include <initializer_list>
#include <iostream>
#include <string>

namespace {
constexpr uint64_t kMs = 1000000ULL;
constexpr uint64_t kStart = 1000 * kMs;
constexpr uint64_t kEnd = kStart + 20 * kMs;  // Simulated TX completion.
constexpr uint64_t kDeadline = kEnd + Xs8CdProbe::kResponseMaxNs + Xs8CdProbe::kQuietNs;

xs8_monitor_record frame(uint64_t time, uint8_t byte, bool valid = true,
                         uint64_t length = kMs)
{
    xs8_monitor_record record{};
    record.end_ns = time + length;
    record.decoded.kind = XS8_FRAME;
    record.decoded.time_ns = time;
    record.decoded.waveform_lsb = byte;
    record.decoded.valid = valid;
    if (!valid) record.decoded.reason = "BAD_BITS";
    return record;
}

xs8_monitor_record echo() { return frame(kStart, 0xC3, true, 19 * kMs); }

xs8_monitor_record gap()
{
    xs8_monitor_record record{};
    record.decoded.kind = XS8_EVENT_GAP;
    record.decoded.lost_events = 3;
    record.decoded.reason = "EVENT_GAP";
    return record;
}

Xs8CdProbe run(std::initializer_list<xs8_monitor_record> records)
{
    Xs8CdProbe probe(kStart, kEnd);
    for (const auto& record : records) probe.observe(&record, 1);
    return probe;
}

bool has(const std::string& text, const char *part)
{
    return text.find(part) != std::string::npos;
}

using R = Xs8CdProbe::Result;
}

int main()
{
    // A valid 0x94 ~3 ms after TX completion, alone in the window: RESPONSE,
    // reported only after the quiet window has closed.
    {
        Xs8CdProbe probe = run({echo(), frame(kEnd + 3 * kMs, 0x94)});
        assert(!probe.done(kEnd + 10 * kMs));
        assert(probe.result(kEnd + 10 * kMs) == R::FAILED);
        assert(probe.done(kDeadline));
        assert(probe.result(kDeadline) == R::RESPONSE);
        assert(probe.response_ns() == kEnd + 3 * kMs);
        const std::string reply = probe.response(kDeadline);
        assert(reply == "OK CD_PROBE result=RESPONSE latency=3000us frames=1 "
                        "rx=0x94@+3000us gpio_tx=TX_OK waveform=MATCH "
                        "receiver_execution=UNVERIFIED\n");
    }
    // Window edges: 1 ms and 20 ms inclusive.
    for (uint64_t offset : {Xs8CdProbe::kResponseMinNs, Xs8CdProbe::kResponseMaxNs})
        assert(run({echo(), frame(kEnd + offset, 0x94)}).result(kDeadline) == R::RESPONSE);
    for (uint64_t offset : {Xs8CdProbe::kResponseMinNs - 1, Xs8CdProbe::kResponseMaxNs + 1})
        assert(run({echo(), frame(kEnd + offset, 0x94)}).result(kDeadline) == R::AMBIGUOUS);

    // Silence after a verified echo: NO_RESPONSE, never OFF.
    {
        Xs8CdProbe probe = run({echo()});
        assert(probe.result(kDeadline) == R::NO_RESPONSE);
        assert(probe.response(kDeadline) ==
               "OK CD_PROBE result=NO_RESPONSE latency=NONE frames=0 rx=NONE "
               "gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED\n");
    }
    // History before the TX is ignored; a frame after the deadline (for example
    // a delayed report ~320 ms later) is outside the transaction.
    assert(run({frame(kStart - 50 * kMs, 0x94), echo()}).result(kDeadline) == R::NO_RESPONSE);
    assert(run({echo(), frame(kEnd + 320 * kMs, 0x94)}).result(kDeadline) == R::NO_RESPONSE);
    assert(run({echo(), frame(kDeadline + 1, 0x94)}).result(kDeadline) == R::NO_RESPONSE);

    // Late (in the quiet window), early, unexpected or multiple frames: AMBIGUOUS.
    {
        Xs8CdProbe late = run({echo(), frame(kEnd + 30 * kMs, 0x94)});
        assert(late.result(kDeadline) == R::AMBIGUOUS);
        assert(has(late.response(kDeadline), "result=AMBIGUOUS latency=NONE frames=1 "
                                             "rx=0x94@+30000us"));
        assert(run({echo(), frame(kEnd + 3 * kMs, 0x94), frame(kEnd + 10 * kMs, 0x94)})
                   .result(kDeadline) == R::AMBIGUOUS);
        assert(run({echo(), frame(kEnd + 3 * kMs, 0x94), frame(kEnd + 40 * kMs, 0xE4)})
                   .result(kDeadline) == R::AMBIGUOUS);
        assert(run({echo(), frame(kEnd + 3 * kMs, 0xE4)}).result(kDeadline) == R::AMBIGUOUS);
        assert(run({echo(), frame(kEnd + 2 * kMs, 0xE4), frame(kEnd + 5 * kMs, 0x94)})
                   .result(kDeadline) == R::AMBIGUOUS);
        Xs8CdProbe many = run({echo(), frame(kEnd + 2 * kMs, 0x94), frame(kEnd + 4 * kMs, 0x94),
                               frame(kEnd + 6 * kMs, 0x94), frame(kEnd + 8 * kMs, 0x94),
                               frame(kEnd + 10 * kMs, 0x94)});
        assert(many.result(kDeadline) == R::AMBIGUOUS);
        assert(has(many.response(kDeadline), "frames=5 "));
    }

    // Our own TX must be the first frame in the TX window and a valid 0xC3.
    {
        Xs8CdProbe missing = run({});
        assert(missing.result(kDeadline) == R::FAILED);
        assert(missing.response(kDeadline) ==
               "ERR CD_PROBE reason=TX_MISSING rx=NONE receiver_execution=UNVERIFIED\n");
        // A 0x94 with no echo before it is never a response.
        assert(has(run({frame(kEnd + 3 * kMs, 0x94)}).response(kDeadline), "reason=TX_MISSING"));
        assert(has(run({frame(kStart, 0xD3)}).response(kDeadline), "reason=TX_MISMATCH"));
        assert(has(run({frame(kStart, 0xC3, false)}).response(kDeadline), "reason=TX_INVALID"));
    }
    // Faults: overlap, invalid frames, lost events, overflow, monitor, cancel.
    {
        assert(has(run({frame(kStart - kMs, 0x94, true, 2 * kMs), echo()}).response(kDeadline),
                   "reason=OVERLAP"));
        // A frame starting before the echo has ended.
        assert(has(run({echo(), frame(kStart + 5 * kMs, 0x94)}).response(kDeadline),
                   "reason=OVERLAP"));
        Xs8CdProbe invalid = run({echo(), frame(kEnd + 3 * kMs, 0x94, false)});
        assert(invalid.done(kEnd + 4 * kMs));  // Faults end the transaction early.
        assert(invalid.result(kDeadline) == R::FAILED);
        assert(has(invalid.response(kDeadline), "reason=INVALID_FRAME rx=INVALID@+3000us"));
        assert(has(run({echo(), frame(kEnd + 3 * kMs, 0x94), gap()}).response(kDeadline),
                   "reason=EVENT_GAP"));
        assert(has(run({gap(), echo(), frame(kEnd + 3 * kMs, 0x94)}).response(kDeadline),
                   "reason=EVENT_GAP"));
        auto warned = echo();
        warned.decoded.global_gap_warnings = 1;
        assert(has(run({warned, frame(kEnd + 3 * kMs, 0x94)}).response(kDeadline),
                   "reason=GLOBAL_GAP_WARNING"));
        Xs8CdProbe between = run({echo(), frame(kEnd + 3 * kMs, 0x94)});
        between.capture_warning();
        assert(has(between.response(kDeadline), "reason=GLOBAL_GAP_WARNING"));
        Xs8CdProbe overflow = run({echo(), frame(kEnd + 3 * kMs, 0x94)});
        overflow.overflow();
        assert(has(overflow.response(kDeadline), "reason=HISTORY_OVERFLOW"));
        Xs8CdProbe monitor = run({echo(), frame(kEnd + 3 * kMs, 0x94)});
        monitor.monitor_failed();
        assert(monitor.result(kDeadline) == R::FAILED);
        assert(has(monitor.response(kDeadline), "reason=MONITOR_FAILED"));
        Xs8CdProbe cancelled = run({echo()});
        cancelled.cancel();
        assert(cancelled.done(0));
        assert(has(cancelled.response(kDeadline), "reason=CANCELLED"));
    }
    // Delivery in one batch or one record at a time gives the same result.
    {
        const xs8_monitor_record batch[] = {frame(kStart - 50 * kMs, 0x7D), echo(),
                                            frame(kEnd + 3 * kMs, 0x94)};
        Xs8CdProbe probe(kStart, kEnd);
        probe.observe(batch, 3);
        assert(probe.result(kDeadline) == R::RESPONSE);
    }

    // Presence: only RESPONSE sets PRESENT, only NO_RESPONSE sets NOT_DETECTED;
    // AMBIGUOUS and FAILED keep the last observation. ABSENT is never produced.
    {
        Xs8CdPresence presence;
        assert(presence.status_fields() ==
               " cd_changer=UNKNOWN cd_changer_ns=0 cd_changer_last_probe=NONE");
        presence.apply(R::AMBIGUOUS, 5, 0);
        presence.apply(R::FAILED, 6, 0);
        assert(presence.status_fields() ==
               " cd_changer=UNKNOWN cd_changer_ns=0 cd_changer_last_probe=FAILED");
        presence.apply(R::RESPONSE, 100, 90);
        assert(presence.status_fields() ==
               " cd_changer=PRESENT cd_changer_ns=90 cd_changer_last_probe=RESPONSE");
        presence.apply(R::AMBIGUOUS, 200, 0);
        presence.apply(R::FAILED, 210, 0);
        assert(presence.status_fields() ==
               " cd_changer=PRESENT cd_changer_ns=90 cd_changer_last_probe=FAILED");
        presence.apply(R::NO_RESPONSE, 300, 0);
        assert(presence.status_fields() ==
               " cd_changer=NOT_DETECTED cd_changer_ns=300 cd_changer_last_probe=NO_RESPONSE");
        assert(!has(presence.status_fields(), "ABSENT"));
    }
    std::cout << "XS8 CD presence probe offline tests passed\n";
}
