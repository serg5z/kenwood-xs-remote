#include "xs8_service_core.h"

#include <cassert>
#include <deque>
#include <iostream>
#include <vector>

using Clock = VolumeControl::Clock;
using Ms = VolumeControl::Ms;

static Clock::time_point at(int milliseconds)
{
    return Clock::time_point(Ms(milliseconds));
}

struct FakeTx final : ServiceTransmitter {
    std::vector<uint8_t> sent;
    std::deque<bool> stop_matches;
    bool healthy = true;
    ServiceTxEvidence last{}, last_stop{};
    bool monitor_healthy() const override { return healthy; }
    ServiceTxEvidence last_evidence() const override { return last; }
    ServiceTxEvidence last_stop_evidence() const override { return last_stop; }
    bool send(uint8_t byte) override {
        sent.push_back(byte);
        last = {};
        last.attempted = true;
        last.byte = byte;
        last.gpio_completed = true;
        if (byte == 0x09) {
            bool match = stop_matches.empty() ? healthy : stop_matches.front();
            if (!stop_matches.empty()) stop_matches.pop_front();
            last.waveform = match ? WaveformEvidence::MATCH
                                  : WaveformEvidence::MISSING;
            last_stop = last;
            return match;
        }
        last.waveform = healthy ? WaveformEvidence::MATCH
                                : WaveformEvidence::MISSING;
        return healthy;
    }
};

static void test_startup_and_parsing()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), false, {}, [] { return at(0); });
    core.connected();
    assert(core.command("STATUS", at(0), at(0)).find("BOOT_INHIBIT") !=
           std::string::npos);
    assert(core.command("VOLUME_PRESS UP", at(1), at(1)) ==
           "ERR BOOT_INHIBIT\n");
    assert(core.command("VOLUME_PRESS SIDEWAYS", at(1), at(1)) ==
           "ERR UNKNOWN_COMMAND\n");
    assert(core.command("MUTE EXTRA", at(1), at(1)) ==
           "ERR UNKNOWN_COMMAND\n");
    assert(tx.sent.empty());
}

static void test_serialized_commands_and_disconnect()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(30); });
    core.connected();
    assert(core.command("VOLUME_PRESS UP", at(10), at(10)) ==
           "OK VOLUME_PRESS_TX\n");
    assert(core.command("VOLUME_PRESS UP", at(20), at(20)) ==
           "OK VOLUME_PRESS_TX\n");
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0xF9}));
    core.disconnected(at(30));
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0xF9, 0x09, 0x07}));
    assert(!core.stop_owed());
    assert(core.command("STATUS", at(31), at(31)).find(
               "last_stop_waveform=MATCH") != std::string::npos);
    assert(core.command("VOLUME_PRESS DOWN", at(32), at(32)) ==
           "ERR NO_CLIENT\n");
}

static void test_down_hold_disconnect_before_max_hold()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(20); });
    core.connected();
    assert(core.command("VOLUME_PRESS DOWN", at(10), at(10)) ==
           "OK VOLUME_PRESS_TX\n");
    core.disconnected(at(20));
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0xE9, 0x09, 0x07}));
    assert(core.state() == VolumeControl::State::IDLE);
    assert(!core.stop_owed());
    assert(core.command("STATUS", at(21), at(21)).find(
               "last_stop_waveform=MATCH") != std::string::npos);
}

static void test_release_and_mute_order()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(40); });
    core.connected();
    assert(core.command("VOLUME_PRESS DOWN", at(10), at(10)) ==
           "OK VOLUME_PRESS_TX\n");
    assert(core.command("VOLUME_RELEASE DOWN", at(20), at(20)) ==
           "OK VOLUME_RELEASE_TX\n");
    assert(core.command("MUTE", at(30), at(30)) == "OK MUTE_TX\n");
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0xE9, 0x09, 0x07,
                                            0x17, 0xD8, 0x07}));
}

static void test_release_and_watchdog()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(500), true, {}, [] { return at(510); });
    core.connected();
    assert(core.command("VOLUME_PRESS DOWN", at(10), at(10)) ==
           "OK VOLUME_PRESS_TX\n");
    core.tick(at(510));
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0xE9, 0x09, 0x07}));
    assert(core.command("VOLUME_PRESS DOWN", at(511), at(511)) !=
           "OK VOLUME_PRESS_TX\n");
    core.command("VOLUME_RELEASE DOWN", at(512), at(512));
    assert(core.command("VOLUME_PRESS DOWN", at(513), at(513)) ==
           "OK VOLUME_PRESS_TX\n");
    assert(core.shutdown(at(514)));
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0xE9, 0x09, 0x07,
                                            0x17, 0xE9, 0x09, 0x07}));
}

static void test_failed_stop_verification_and_monitor()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(20); });
    core.connected();
    core.command("VOLUME_PRESS UP", at(10), at(10));
    tx.stop_matches = {false, false, false};
    assert(core.command("VOLUME_RELEASE UP", at(20), at(20)) ==
           "ERR VOLUME_STOP_UNCERTAIN\n");
    assert(core.state() == VolumeControl::State::FAULT_LATCHED);
    assert(core.stop_owed());
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0xF9,
                                            0x09, 0x09, 0x09}));
    assert(core.command("VOLUME_PRESS DOWN", at(21), at(21)) !=
           "OK VOLUME_PRESS_TX\n");

    FakeTx disconnected_tx;
    Xs8ServiceCore disconnected(disconnected_tx, Ms(2000), true,
                                {}, [] { return at(20); });
    disconnected.connected();
    disconnected.command("VOLUME_PRESS DOWN", at(10), at(10));
    disconnected_tx.stop_matches = {false, false, false};
    disconnected.disconnected(at(20));
    assert(disconnected.state() == VolumeControl::State::FAULT_LATCHED);
    assert(disconnected.stop_owed());
    assert(disconnected_tx.sent == (std::vector<uint8_t>{
        0x17, 0xE9, 0x09, 0x09, 0x09}));

    FakeTx tx2;
    Xs8ServiceCore core2(tx2, Ms(2000), true, {}, [] { return at(1); });
    core2.connected();
    tx2.healthy = false;
    assert(core2.command("VOLUME_PRESS UP", at(1), at(1)) ==
           "ERR MONITOR_FAILED\n");
    assert(tx2.sent.empty());

    FakeTx tx3;
    Xs8ServiceCore core3(tx3, Ms(2000), true, {}, [] { return at(2); });
    core3.connected();
    core3.command("VOLUME_PRESS DOWN", at(1), at(1));
    tx3.healthy = false;
    core3.tick(at(2));
    assert(core3.state() == VolumeControl::State::FAULT_LATCHED);
    assert(core3.stop_owed());
    size_t attempts = tx3.sent.size();
    core3.tick(at(3));
    assert(tx3.sent.size() == attempts);
}

int main()
{
    test_startup_and_parsing();
    test_serialized_commands_and_disconnect();
    test_down_hold_disconnect_before_max_hold();
    test_release_and_mute_order();
    test_release_and_watchdog();
    test_failed_stop_verification_and_monitor();
    std::cout << "XS8 service core tests passed\n";
}
