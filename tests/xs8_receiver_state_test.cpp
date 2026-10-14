#include "xs8_receiver_state.h"

#include <cassert>
#include <iostream>
#include <string>

static xs8_monitor_record frame(uint64_t time, uint8_t legacy,
                                bool valid = true)
{
    xs8_monitor_record record{};
    record.end_ns = time + 100;
    record.decoded.kind = XS8_FRAME;
    record.decoded.time_ns = time;
    record.decoded.legacy_msb_short1 = legacy;
    if (legacy == 0x37) record.decoded.waveform_lsb = 0x13;
    record.decoded.valid = valid;
    return record;
}

static void has(const std::string& status, const char *field)
{
    assert(status.find(field) != std::string::npos);
}

int main()
{
    Xs8ReceiverState state;
    has(state.status_fields(), "receiver_source=UNKNOWN");
    has(state.status_fields(), "receiver_monitor=UNKNOWN");
    const struct { uint8_t byte; const char *name; } sources[] = {
        {0x71, "TV2"}, {0x73, "PHONO"}, {0x79, "TAPE1"},
        {0x7A, "TV1"}, {0x7B, "TUNER"}, {0x7D, "CD"}
    };
    uint64_t time = 1000;
    for (const auto& source : sources) {
        state.observe(frame(time, source.byte), false);
        has(state.status_fields(), (std::string("receiver_source=") + source.name).c_str());
        time += 1000;
    }
    state.observe(frame(time++, 0x14), false);
    has(state.status_fields(), "receiver_monitor=ON");
    state.observe(frame(time++, 0x18), false);
    has(state.status_fields(), "receiver_monitor=OFF");
    has(state.status_fields(), "receiver_source=CD");
    state.observe(frame(time++, 0x71, false), false);
    state.observe(frame(time++, 0x55), false);
    has(state.status_fields(), "receiver_source=CD");

    // Locally transmitted frames and pending-query frames cannot be passive state.
    state.note_tx(10000, 11000);
    state.observe(frame(10500, 0x71), false);
    state.observe(frame(12000, 0x73), true);
    has(state.status_fields(), "receiver_source=CD");
    state.observe(frame(12000, 0x73), false);
    has(state.status_fields(), "receiver_source=PHONO");
    has(state.status_fields(), "receiver_source_evidence=AFTER_LOCAL_TX");
    state.observe(frame(11500, 0x7D), false);
    has(state.status_fields(), "receiver_source=PHONO"); // Older observation is stale.

    // Back-to-back CONTEXT selection 0x17 -> 0x78 -> 0x07 (TV1): only each local
    // frame's own window is excluded. The receiver's source broadcast between
    // the key and the exit frame is observed although the exit ends later.
    {
        Xs8ReceiverState tv;
        tv.observe(frame(1000, 0x73), false);
        has(tv.status_fields(), "receiver_source=PHONO");
        tv.note_tx(100000, 101000); // 0x17
        tv.note_tx(101500, 102500); // 0x78
        tv.note_tx(103000, 104000); // 0x07 (the service processes frames after it)
        tv.observe(frame(100000, 0x7D), false); // Local frames: never state.
        tv.observe(frame(101500, 0x7D), false);
        tv.observe(frame(104000, 0x7D), false);
        has(tv.status_fields(), "receiver_source=PHONO");
        tv.observe(frame(102700, 0x7A), false);
        has(tv.status_fields(), "receiver_source=TV1");
        has(tv.status_fields(), "receiver_source_evidence=AFTER_LOCAL_TX");
        // TV2 the same way; a broadcast long after the last TX is PASSIVE.
        tv.note_tx(200000, 201000);
        tv.note_tx(201500, 202500);
        tv.note_tx(203000, 204000);
        tv.observe(frame(202600, 0x71), false);
        has(tv.status_fields(), "receiver_source=TV2");
        tv.observe(frame(900000000, 0x7B), false);
        has(tv.status_fields(), "receiver_source=TUNER");
        has(tv.status_fields(), "receiver_source_evidence=PASSIVE");
        // Bounded memory: once a window is evicted, everything up to its end
        // is excluded, as the single horizon did before.
        Xs8ReceiverState many;
        for (uint64_t i = 0; i < 17; ++i) many.note_tx(1000000 + i * 10000, 1005000 + i * 10000);
        many.observe(frame(1000500, 0x7D), false); // Up to the evicted end:
        many.observe(frame(1004000, 0x7D), false); // excluded.
        has(many.status_fields(), "receiver_source=UNKNOWN");
        many.observe(frame(1018000, 0x7D), false); // Between retained windows.
        has(many.status_fields(), "receiver_source=CD");
        // CD_STOP is ROOT 0xA1, the TV1 broadcast value (legacy 0x7A): the
        // service's own STOP frame never reads as TV1; a later broadcast does.
        Xs8ReceiverState stop;
        stop.observe(frame(1000, 0x7D), false);
        stop.note_tx(300000, 301000);
        stop.observe(frame(300000, 0x7A), false);
        has(stop.status_fields(), "receiver_source=CD");
        stop.observe(frame(302000, 0x7A), false);
        has(stop.status_fields(), "receiver_source=TV1");
        // Pending-query and suppressed frames stay excluded.
        Xs8ReceiverState queried;
        queried.observe(frame(5000, 0x7A), true);
        queried.suppress_until(9000);
        queried.observe(frame(8000, 0x7A), false);
        has(queried.status_fields(), "receiver_source=UNKNOWN");
    }

    Xs8QueryStatus query(20000, 22000);
    const xs8_monitor_record observations[] = {
        frame(20100, 0x37), frame(21000, 0x11), frame(23000, 0x14)
    };
    query.observe(observations, 3); // First response precedes TX function return.
    state.apply_query(query, query.deadline_ns());
    has(state.status_fields(), "receiver_source=TV2");
    has(state.status_fields(), "receiver_source_evidence=QUERY");
    has(state.status_fields(), "receiver_monitor=ON");

    Xs8QueryStatus failed(30000, 32000);
    const xs8_monitor_record bad[] = {
        frame(30100, 0x37), frame(33000, 0x1D)
    };
    failed.observe(bad, 2);
    state.apply_query(failed, failed.deadline_ns());
    has(state.status_fields(), "receiver_source=TV2");
    Xs8QueryStatus unknown(34000, 35000);
    const xs8_monitor_record unknown_frames[] = {
        frame(34100, 0x37), frame(36000, 0x55), frame(38000, 0x99)
    };
    unknown.observe(unknown_frames, 3);
    state.apply_query(unknown, unknown.deadline_ns());
    has(state.status_fields(), "receiver_source=TV2");
    has(state.status_fields(), "receiver_monitor=ON");

    xs8_monitor_record gap{};
    gap.decoded.kind = XS8_EVENT_GAP;
    gap.decoded.lost_events = 2;
    state.observe(gap, false);
    state.observe(frame(40000, 0x7D), false);
    has(state.status_fields(), "receiver_source=TV2");
    has(state.status_fields(), "receiver_integrity=UNCERTAIN");
    state.monitor_failed();
    state.apply_query(query, query.deadline_ns());
    has(state.status_fields(), "receiver_integrity=UNCERTAIN");
    Xs8ReceiverState overflowed;
    overflowed.history_overflow();
    overflowed.apply_query(query, query.deadline_ns());
    has(overflowed.status_fields(), "receiver_integrity=HEALTHY");
    has(overflowed.status_fields(), "receiver_source=TV2");
    Xs8ReceiverState restarted;
    has(restarted.status_fields(), "receiver_source=UNKNOWN");
    has(restarted.status_fields(), "receiver_monitor=UNKNOWN");
    std::cout << "XS8 receiver state offline tests passed\n";
}
