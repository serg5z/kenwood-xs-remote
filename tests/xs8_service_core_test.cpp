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
    bool fail_next = false;
    bool fail_gpio_next = false;
    bool fail_root = false;
    bool fail_release = false;
    bool fail_release_gpio = false;
    WaveformEvidence fail_waveform = WaveformEvidence::MISSING;
    ServiceTxEvidence last{}, last_stop{};
    Clock::time_point end_time = at(10);
    bool monitor_healthy() const override { return healthy; }
    ServiceTxEvidence last_evidence() const override { return last; }
    ServiceTxEvidence last_stop_evidence() const override { return last_stop; }
    Clock::time_point last_tx_end_time() const override { return end_time; }
    bool send(uint8_t byte) override {
        sent.push_back(byte);
        last = {};
        last.attempted = true;
        last.byte = byte;
        last.gpio_completed = !fail_gpio_next &&
            !(fail_release_gpio && byte == 0x72);
        fail_gpio_next = false;
        if (byte == 0x09) {
            bool match = stop_matches.empty() ? healthy : stop_matches.front();
            if (!stop_matches.empty()) stop_matches.pop_front();
            last.waveform = match ? WaveformEvidence::MATCH
                                  : WaveformEvidence::MISSING;
            last_stop = last;
            return match;
        }
        const bool match = last.gpio_completed && healthy && !fail_next &&
            !(fail_root && byte == 0x07) &&
            !(fail_release && byte == 0x72);
        fail_next = false;
        last.waveform = match ? WaveformEvidence::MATCH
                                : fail_waveform;
        return match;
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

static void test_source_selection()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(10); });
    core.connected();
    const std::pair<const char*, uint8_t> supported[] = {
        {"CD", 0x84}, {"TUNER", 0x82}, {"PHONO", 0x83}, {"TAPE1", 0x85}
    };
    for (const auto& source : supported) {
        assert(core.command(std::string("SELECT_SOURCE ") + source.first,
                            at(10), at(10)) ==
               "OK SOURCE_TX gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED\n");
        assert(tx.sent.back() == source.second);
    }
    const size_t count = tx.sent.size();
    for (const char* name : {"TV1", "TV2", "TAPE2"})
        assert(core.command(std::string("SELECT_SOURCE ") + name, at(10), at(10)) ==
               "ERR SOURCE_UNVERIFIED\n");
    assert(core.command("SELECT_SOURCE UNKNOWN", at(10), at(10)) ==
           "ERR UNKNOWN_SOURCE\n");
    assert(tx.sent.size() == count);
    tx.fail_next = true;
    assert(core.command("SELECT_SOURCE CD", at(10), at(10)) ==
           "ERR SOURCE_UNCERTAIN gpio_tx=TX_OK waveform=MISSING receiver_execution=UNVERIFIED\n");
    assert(tx.sent.size() == count + 1); // No automatic retransmission.
    tx.healthy = false;
    assert(core.command("SELECT_SOURCE CD", at(11), at(11)) ==
           "ERR MONITOR_FAILED\n");
    assert(tx.sent.size() == count + 1);

    FakeTx inhibited_tx;
    Xs8ServiceCore inhibited(inhibited_tx, Ms(2000), false);
    inhibited.connected();
    assert(inhibited.command("SELECT_SOURCE CD", at(1), at(1)) ==
           "ERR BOOT_INHIBIT\n");
    FakeTx holding_tx;
    Xs8ServiceCore holding(holding_tx, Ms(2000), true);
    holding.connected();
    holding.command("VOLUME_PRESS DOWN", at(1), at(1));
    const size_t holding_count = holding_tx.sent.size();
    assert(holding.command("SELECT_SOURCE CD", at(2), at(2)) ==
           "ERR HOLDING_DOWN\n");
    assert(holding_tx.sent.size() == holding_count);
    holding_tx.stop_matches = {false, false, false};
    holding.command("VOLUME_RELEASE DOWN", at(3), at(3));
    const size_t fault_count = holding_tx.sent.size();
    assert(holding.command("SELECT_SOURCE CD", at(4), at(4)) ==
           "ERR FAULT_LATCHED\n");
    assert(holding_tx.sent.size() == fault_count);
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

static void test_query_status_gates_and_single_tx()
{
    FakeTx inhibited_tx;
    Xs8ServiceCore inhibited(inhibited_tx, Ms(2000), false);
    inhibited.connected();
    assert(inhibited.begin_query_status(at(1)) == "ERR BOOT_INHIBIT\n");
    assert(inhibited_tx.sent.empty());

    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true);
    assert(core.begin_query_status(at(1)) == "ERR NO_CLIENT\n");
    core.connected();
    assert(core.begin_query_status(at(2)).empty());
    assert(tx.sent == (std::vector<uint8_t>{0x13}));
    tx.healthy = false;
    assert(core.begin_query_status(at(3)) == "ERR MONITOR_FAILED\n");
    assert(tx.sent.size() == 1);

    FakeTx mismatch_tx;
    mismatch_tx.fail_next = true;
    mismatch_tx.fail_waveform = WaveformEvidence::MISMATCH;
    Xs8ServiceCore mismatch(mismatch_tx, Ms(2000), true);
    mismatch.connected();
    assert(mismatch.begin_query_status(at(1)) ==
        "ERR QUERY_STATUS reason=TX_UNCERTAIN gpio_tx=TX_OK "
        "waveform=MISMATCH receiver_execution=UNVERIFIED\n");
    assert(mismatch_tx.sent == (std::vector<uint8_t>{0x13}));

    FakeTx holding_tx;
    Xs8ServiceCore holding(holding_tx, Ms(2000), true,
                           {}, [] { return at(1); });
    holding.connected();
    assert(holding.command("VOLUME_PRESS DOWN", at(1), at(1)) ==
           "OK VOLUME_PRESS_TX\n");
    const size_t before = holding_tx.sent.size();
    assert(holding.begin_query_status(at(2)) == "ERR HOLDING_DOWN\n");
    assert(holding_tx.sent.size() == before);
    assert(holding.begin_query_status(at(2002)) == "ERR HOLDING_DOWN\n");
    assert(holding_tx.sent == (std::vector<uint8_t>{
        0x17, 0xE9, 0x09, 0x07}));
    assert(holding.begin_query_status(at(2003)).empty());
    assert(holding_tx.sent.back() == 0x13);
    assert(holding_tx.sent.size() == 5);

    FakeTx unresolved_tx;
    Xs8ServiceCore unresolved(unresolved_tx, Ms(2000), true,
                              {}, [] { return at(1); });
    unresolved.connected();
    unresolved.command("VOLUME_PRESS DOWN", at(1), at(1));
    unresolved_tx.stop_matches = {false, false, false};
    unresolved.command("VOLUME_RELEASE DOWN", at(3), at(3));
    assert(unresolved.stop_owed());
    const size_t unresolved_count = unresolved_tx.sent.size();
    assert(unresolved.begin_query_status(at(4)) == "ERR FAULT_LATCHED\n");
    assert(unresolved_tx.sent.size() == unresolved_count);

    FakeTx root_tx;
    Xs8ServiceCore root_fault(root_tx, Ms(2000), true,
                              {}, [] { return at(1); });
    root_fault.connected();
    root_fault.command("VOLUME_PRESS DOWN", at(1), at(1));
    root_tx.fail_root = true;
    root_fault.command("VOLUME_RELEASE DOWN", at(2), at(2));
    assert(root_fault.state() == VolumeControl::State::FAULT_LATCHED);
    assert(root_fault.begin_query_status(at(3)) == "ERR FAULT_LATCHED\n");
    assert(root_tx.sent.back() == 0x07);
}

static void test_research_tx_interlocks()
{
    FakeTx inhibited_tx;
    Xs8ServiceCore inhibited(inhibited_tx, Ms(2000), false);
    inhibited.connected();
    assert(inhibited.research_send(0x9C, at(1)) ==
           "ERR RESEARCH reason=BOOT_INHIBIT\n");
    assert(inhibited.research_send(0x9C, at(1), true) ==
           "ERR RESEARCH reason=BOOT_INHIBIT\n");
    assert(inhibited_tx.sent.empty());

    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true);
    core.connected();
    assert(core.research_send(0x9C, at(1)) ==
           "OK RESEARCH_TX gpio_tx=TX_OK waveform=MATCH "
           "receiver_execution=UNVERIFIED");
    assert(tx.sent == (std::vector<uint8_t>{0x9C}));
    tx.fail_next = true;
    assert(core.research_send(0x42, at(2)) ==
           "ERR RESEARCH_TX gpio_tx=TX_OK waveform=MISSING "
           "receiver_execution=UNVERIFIED");
    assert(tx.sent == (std::vector<uint8_t>{0x9C, 0x42}));
    core.disconnected(at(2));
    assert(core.research_send(0x11, at(3)) ==
           "ERR RESEARCH reason=NO_CLIENT\n");
    assert(core.research_send(0x11, at(3), true) ==
           "OK RESEARCH_TX gpio_tx=TX_OK waveform=MATCH "
           "receiver_execution=UNVERIFIED");
    assert(tx.sent.back() == 0x11);
    tx.healthy = false;
    assert(core.research_send(0xBC, at(3), true) ==
           "ERR RESEARCH reason=MONITOR_FAILED\n");
    assert(tx.sent.size() == 3);

    FakeTx holding_tx;
    Xs8ServiceCore holding(holding_tx, Ms(2000), true);
    holding.connected();
    holding.command("VOLUME_PRESS DOWN", at(10), at(10));
    const size_t count = holding_tx.sent.size();
    assert(holding.research_send(0x9C, at(20)) ==
           "ERR RESEARCH reason=HOLDING_DOWN\n");
    assert(holding_tx.sent.size() == count);
    holding_tx.stop_matches = {false, false, false};
    holding.command("VOLUME_RELEASE DOWN", at(30), at(30));
    assert(holding.research_send(0x9C, at(31)) ==
           "ERR RESEARCH reason=FAULT_LATCHED\n");
}

static void test_cd_track_success_timing_and_serialization()
{
    for (const auto& trial : {std::pair<const char*, uint8_t>{"CD_TRACK_NEXT", 0xFC},
                              {"CD_TRACK_PREVIOUS", 0xEC}}) {
        FakeTx tx;
        Xs8ServiceCore *owner = nullptr;
        int waits = 0;
        Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); },
            [&](Clock::time_point deadline) {
                ++waits;
                assert(deadline == at(110)); // TX end at 10 ms + 100 ms.
                assert(tx.sent == (std::vector<uint8_t>{trial.second}));
                assert(owner->command("CD_TRACK_NEXT", at(1), at(1)) ==
                       "ERR CD_TRACK_BUSY\n");
                assert(owner->command("MUTE", at(1), at(1)) ==
                       "ERR CD_TRACK_BUSY\n");
                assert(owner->research_ready(at(1)) ==
                       "ERR RESEARCH reason=CD_TRACK_BUSY\n");
                assert(owner->command("STATUS", at(1), at(1)).find(
                       "cd_release_owed=1") != std::string::npos);
            });
        owner = &core;
        core.connected();
        const std::string result = core.command(trial.first, at(1), at(1));
        assert(result.find("OK CD_TRACK_TX") == 0);
        assert(result.find("release_waveform=MATCH") != std::string::npos);
        assert(result.find("receiver_execution=UNVERIFIED") !=
               std::string::npos);
        assert(tx.sent == (std::vector<uint8_t>{trial.second, 0x72}));
        assert(waits == 1 && !core.cd_release_owed() && !core.cd_fault());
        assert(core.command("STATUS", at(2), at(2)).find(
               "cd_last_result=TX_VERIFIED") != std::string::npos);
    }
}

static void test_cd_track_uncertain_press_still_releases()
{
    FakeTx tx;
    tx.fail_next = true;
    int waits = 0;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); },
        [&](Clock::time_point) { ++waits; });
    core.connected();
    const auto result = core.command("CD_TRACK_NEXT", at(1), at(1));
    assert(result.find("ERR CD_TRACK_UNCERTAIN") == 0);
    assert(result.find("cd_result=PRESS_UNCERTAIN") != std::string::npos);
    assert(tx.sent == (std::vector<uint8_t>{0xFC, 0x72}));
    assert(waits == 0 && core.cd_fault() && !core.cd_release_owed());
    assert(core.command("STATUS", at(2), at(2)).find(
           "state=FAULT_LATCHED") != std::string::npos);
    assert(core.command("CD_TRACK_PREVIOUS", at(2), at(2)) ==
           "ERR CD_FAULT_LATCHED\n");
    assert(core.research_ready(at(2)) ==
           "ERR RESEARCH reason=CD_FAULT_LATCHED\n");

    FakeTx gpio_tx;
    gpio_tx.fail_gpio_next = true;
    Xs8ServiceCore gpio_core(gpio_tx, Ms(2000), true);
    gpio_core.connected();
    const auto gpio_result = gpio_core.command("CD_TRACK_PREVIOUS", at(1), at(1));
    assert(gpio_result.find("press_gpio_tx=TX_FAIL") != std::string::npos);
    assert(gpio_tx.sent == (std::vector<uint8_t>{0xEC, 0x72}));
    assert(gpio_core.cd_fault() && !gpio_core.cd_release_owed());
}

static void test_cd_release_failure_and_bounded_shutdown()
{
    FakeTx tx;
    tx.fail_release = true;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); },
        [](Clock::time_point) {});
    core.connected();
    const auto result = core.command("CD_TRACK_PREVIOUS", at(1), at(1));
    assert(result.find("cd_result=RELEASE_UNCERTAIN") != std::string::npos);
    assert(tx.sent == (std::vector<uint8_t>{0xEC, 0x72}));
    assert(core.cd_fault() && core.cd_release_owed());
    assert(!core.shutdown(at(2)));
    assert(tx.sent == (std::vector<uint8_t>{0xEC, 0x72, 0x72}));
    assert(!core.shutdown(at(3)));
    assert(tx.sent.size() == 3); // No unbounded shutdown retries.

    FakeTx gpio_tx;
    gpio_tx.fail_release_gpio = true;
    Xs8ServiceCore gpio_core(gpio_tx, Ms(2000), true, {},
                             [] { return at(0); }, [](Clock::time_point) {});
    gpio_core.connected();
    const auto gpio_result = gpio_core.command("CD_TRACK_NEXT", at(1), at(1));
    assert(gpio_result.find("release_gpio_tx=TX_FAIL") != std::string::npos);
    assert(gpio_core.cd_fault() && gpio_core.cd_release_owed());
}

static void test_cd_shutdown_during_gap_releases_once()
{
    FakeTx tx;
    Xs8ServiceCore *owner = nullptr;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); },
        [&](Clock::time_point) {
            assert(owner->shutdown(at(20)));
            assert(tx.sent == (std::vector<uint8_t>{0xFC, 0x72}));
        });
    owner = &core;
    core.connected();
    const auto result = core.command("CD_TRACK_NEXT", at(1), at(1));
    assert(result.find("OK CD_TRACK_TX") == 0);
    assert(tx.sent == (std::vector<uint8_t>{0xFC, 0x72}));
    assert(!core.cd_release_owed());
    assert(core.command("CD_TRACK_NEXT", at(2), at(2)) == "ERR SHUTDOWN\n");
}

static void test_cd_interlocks_and_monitor_failure()
{
    FakeTx inhibited_tx;
    Xs8ServiceCore inhibited(inhibited_tx, Ms(2000), false);
    inhibited.connected();
    assert(inhibited.command("CD_TRACK_NEXT", at(1), at(1)) ==
           "ERR BOOT_INHIBIT\n");
    assert(inhibited_tx.sent.empty());

    FakeTx holding_tx;
    Xs8ServiceCore holding(holding_tx, Ms(2000), true);
    holding.connected();
    holding.command("VOLUME_PRESS DOWN", at(1), at(1));
    assert(holding.command("CD_TRACK_NEXT", at(2), at(2)) ==
           "ERR HOLDING_DOWN\n");
    assert(holding_tx.sent == (std::vector<uint8_t>{0x17, 0xE9}));

    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); },
        [&](Clock::time_point) { tx.healthy = false; });
    core.connected();
    const auto result = core.command("CD_TRACK_NEXT", at(1), at(1));
    assert(result.find("cd_result=RELEASE_UNCERTAIN") != std::string::npos);
    assert(tx.sent == (std::vector<uint8_t>{0xFC, 0x72}));
    assert(core.cd_fault() && core.cd_release_owed());
}

int main()
{
    test_startup_and_parsing();
    test_source_selection();
    test_serialized_commands_and_disconnect();
    test_down_hold_disconnect_before_max_hold();
    test_release_and_mute_order();
    test_release_and_watchdog();
    test_failed_stop_verification_and_monitor();
    test_query_status_gates_and_single_tx();
    test_research_tx_interlocks();
    test_cd_track_success_timing_and_serialization();
    test_cd_track_uncertain_press_still_releases();
    test_cd_release_failure_and_bounded_shutdown();
    test_cd_shutdown_during_gap_releases_once();
    test_cd_interlocks_and_monitor_failure();
    std::cout << "XS8 service core tests passed\n";
}
