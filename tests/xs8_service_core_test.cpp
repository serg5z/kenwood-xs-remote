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

static std::string hold_token(const std::string& reply)
{
    const std::string field = " hold_token=";
    const auto start = reply.find(field);
    assert(start != std::string::npos);
    const auto first = start + field.size();
    const auto end = reply.find(' ', first);
    return reply.substr(first, end - first);
}

static std::string wrong_token(const std::string& token)
{
    return token == "999999" ? "999998" : "999999";
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

static void test_cd_hold_press_release_and_interlocks()
{
    for (const auto& trial : {
             std::pair<const char*, uint8_t>{"TRACK_NEXT", 0xFC},
             {"TRACK_PREVIOUS", 0xEC}, {"SEEK_FORWARD", 0x7A},
             {"SEEK_BACKWARD", 0x6A}}) {
        FakeTx tx;
        int waits = 0;
        Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); },
            [&](Clock::time_point) { ++waits; }, Ms(1500));
        core.connected();
        const std::string press = std::string("CD_PRESS ") + trial.first;
        const auto pressed = core.command(press, at(10), at(10));
        assert(pressed.find("OK CD_PRESS_TX") == 0);
        const auto token = hold_token(pressed);
        assert(tx.sent == (std::vector<uint8_t>{trial.second}));
        assert(waits == 0 && core.cd_release_owed());
        assert(core.command("STATUS", at(400), at(400)).find(
               "cd_state=HOLDING") != std::string::npos);
        assert(core.command(press, at(400), at(400)) == "ERR CD_TRACK_BUSY\n");
        assert(core.command("CD_TRACK_NEXT", at(400), at(400)) ==
               "ERR CD_TRACK_BUSY\n");
        assert(core.command("VOLUME_PRESS UP", at(400), at(400)) ==
               "ERR CD_TRACK_BUSY\n");
        assert(core.command("MUTE", at(400), at(400)) ==
               "ERR CD_TRACK_BUSY\n");
        assert(core.research_ready(at(400)) ==
               "ERR RESEARCH reason=CD_TRACK_BUSY\n");
        assert(tx.sent == (std::vector<uint8_t>{trial.second}));
        assert(core.command("CD_RELEASE", at(900), at(900)) ==
               "ERR CD_TOKEN_REQUIRED\n");
        assert(core.command("CD_RELEASE " + wrong_token(token),
                            at(900), at(900)) ==
               "ERR CD_NOT_OWNER\n");
        assert(core.command("CD_RELEASE " + token, at(900), at(900)).find(
               "OK CD_RELEASE_TX") == 0);
        assert(tx.sent == (std::vector<uint8_t>{trial.second, 0x72}));
        assert(!core.cd_release_owed() && !core.cd_fault());
        assert(core.command("CD_RELEASE " + token, at(901), at(901)) ==
               "ERR CD_NOT_ACTIVE\n");
        assert(tx.sent.size() == 2);
    }
}

static void test_cd_hold_watchdog_disconnect_and_shutdown()
{
    FakeTx timeout_tx;
    Xs8ServiceCore timeout(timeout_tx, Ms(2000), true, {},
                           [] { return at(0); }, {}, Ms(500));
    timeout.connected();
    timeout.command("CD_PRESS TRACK_NEXT", at(10), at(10));
    timeout.tick(at(509));
    assert(timeout_tx.sent == (std::vector<uint8_t>{0xFC}));
    timeout.tick(at(510));
    assert(timeout_tx.sent == (std::vector<uint8_t>{0xFC, 0x72}));
    assert(!timeout.cd_fault() && !timeout.cd_release_owed());
    assert(timeout.command("STATUS", at(510), at(510)).find("cd_lease_expired=1") != std::string::npos);
    timeout.tick(at(600));
    assert(timeout_tx.sent.size() == 2);
    assert(timeout.command("CD_TRACK_NEXT", at(601), at(601)).find(
           "OK CD_TRACK_TX") == 0);
    assert(timeout.command("STATUS", at(601), at(601)).find("cd_lease_expired=0") != std::string::npos);
    assert(timeout_tx.sent == (std::vector<uint8_t>{0xFC, 0x72, 0xFC, 0x72}));

    FakeTx disconnect_tx;
    Xs8ServiceCore disconnect(disconnect_tx, Ms(2000), true);
    disconnect.connected();
    const auto disconnect_token = hold_token(disconnect.command(
        "CD_PRESS SEEK_BACKWARD", at(10), at(10)));
    disconnect.disconnected(at(100));
    assert(disconnect_tx.sent == (std::vector<uint8_t>{0x6A}));
    assert(disconnect.cd_release_owed());
    disconnect.connected();
    assert(disconnect.command("CD_RELEASE " + disconnect_token,
                              at(101), at(101)).find("OK CD_RELEASE_TX") == 0);
    assert(disconnect_tx.sent == (std::vector<uint8_t>{0x6A, 0x72}));

    FakeTx shutdown_tx;
    Xs8ServiceCore shutdown(shutdown_tx, Ms(2000), true);
    shutdown.connected();
    shutdown.command("CD_PRESS TRACK_PREVIOUS", at(10), at(10));
    assert(shutdown.shutdown(at(50)));
    assert(shutdown_tx.sent == (std::vector<uint8_t>{0xEC, 0x72}));
    assert(!shutdown.cd_release_owed());
    assert(shutdown.command("CD_PRESS TRACK_NEXT", at(51), at(51)) ==
           "ERR SHUTDOWN\n");
}

static void test_cd_lease_renewal_stale_token_and_recovery()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); },
                        {}, Ms(2000));
    core.connected();
    const auto first = hold_token(core.command(
        "CD_PRESS TRACK_NEXT", at(10), at(10)));
    for (int time = 510; time <= 6010; time += 500) {
        core.tick(at(time));
        assert(tx.sent == (std::vector<uint8_t>{0xFC}));
        assert(core.command("CD_HEARTBEAT " + first,
                            at(time), at(time)) ==
               "OK CD_LEASE_RENEWED\n");
    }
    assert(core.command("CD_HEARTBEAT " + wrong_token(first),
                        at(6100), at(6100)) ==
           "ERR CD_NOT_OWNER\n");
    assert(core.command("CD_HEARTBEAT garbage", at(6100), at(6100)) ==
           "ERR CD_TOKEN_REQUIRED\n");
    assert(core.command("CD_RELEASE " + first, at(6500), at(6500)).find(
           "OK CD_RELEASE_TX") == 0);
    assert(tx.sent == (std::vector<uint8_t>{0xFC, 0x72}));

    const auto second = hold_token(core.command(
        "CD_PRESS SEEK_FORWARD", at(6600), at(6600)));
    assert(second != first);
    assert(core.command("CD_HEARTBEAT " + first, at(6700), at(6700)) ==
           "ERR CD_NOT_OWNER\n");
    assert(core.command("CD_RELEASE " + first, at(6700), at(6700)) ==
           "ERR CD_NOT_OWNER\n");
    assert(tx.sent == (std::vector<uint8_t>{0xFC, 0x72, 0x7A}));
    core.disconnected(at(6800));
    assert(tx.sent == (std::vector<uint8_t>{0xFC, 0x72, 0x7A}));
    core.connected();
    assert(core.command("CD_RELEASE " + second, at(6900), at(6900)).find(
           "OK CD_RELEASE_TX") == 0);
    const auto third = hold_token(core.command(
        "CD_PRESS TRACK_PREVIOUS", at(6900), at(6900)));
    assert(third != second);
    assert(core.command("CD_RELEASE " + second, at(7000), at(7000)) ==
           "ERR CD_NOT_OWNER\n");
    assert(core.command("CD_RELEASE " + third, at(7000), at(7000)).find(
           "OK CD_RELEASE_TX") == 0);

    FakeTx expired_tx;
    Xs8ServiceCore expired(expired_tx, Ms(2000), true, {},
                           [] { return at(0); }, {}, Ms(2000));
    expired.connected();
    const auto expired_token = hold_token(expired.command(
        "CD_PRESS TRACK_NEXT", at(10), at(10)));
    expired.tick(at(2010));
    assert(expired_tx.sent == (std::vector<uint8_t>{0xFC, 0x72}));
    assert(!expired.cd_fault() && !expired.cd_release_owed());
    assert(expired.command("CD_HEARTBEAT " + expired_token,
                           at(2011), at(2011)) == "ERR CD_NOT_ACTIVE\n");
    assert(expired.command("CD_RELEASE " + expired_token,
                           at(2011), at(2011)) == "ERR CD_NOT_ACTIVE\n");
    assert(expired.command("STATUS", at(2011), at(2011)).find("cd_last_result=LEASE_EXPIRED") != std::string::npos);
    expired_tx.healthy = false;
    assert(expired.command("CD_PRESS TRACK_NEXT", at(2012), at(2012)) ==
           "ERR MONITOR_FAILED\n");
    assert(expired.command("STATUS", at(2012), at(2012)).find("cd_lease_expired=1") != std::string::npos);
    assert(expired_tx.sent.size() == 2);
    expired_tx.healthy = true;
    expired.disconnected(at(2020));
    expired.connected();
    const auto fresh_token = hold_token(expired.command(
        "CD_PRESS TRACK_NEXT", at(2030), at(2030)));
    assert(fresh_token != expired_token);
    assert(expired.command("STATUS", at(2030), at(2030)).find("cd_lease_expired=0") != std::string::npos);
    assert(expired.command("CD_HEARTBEAT " + expired_token,
                           at(2031), at(2031)) == "ERR CD_NOT_OWNER\n");
    assert(expired.command("CD_RELEASE " + expired_token,
                           at(2031), at(2031)) == "ERR CD_NOT_OWNER\n");
    assert(expired_tx.sent == (std::vector<uint8_t>{0xFC, 0x72, 0xFC}));
    assert(expired.command("CD_RELEASE " + fresh_token,
                           at(2032), at(2032)).find("OK CD_RELEASE_TX") == 0);

    FakeTx uncertain_tx;
    Xs8ServiceCore uncertain(uncertain_tx, Ms(2000), true, {},
                             [] { return at(0); }, {}, Ms(500));
    uncertain.connected();
    uncertain.command("CD_PRESS SEEK_FORWARD", at(10), at(10));
    uncertain_tx.fail_release = true;
    uncertain.tick(at(510));
    assert(uncertain_tx.sent == (std::vector<uint8_t>{0x7A, 0x72}));
    assert(uncertain.cd_fault() && uncertain.cd_release_owed());
    assert(uncertain.command("CD_PRESS TRACK_NEXT", at(511), at(511)) ==
           "ERR CD_FAULT_LATCHED\n");
    uncertain.disconnected(at(512));
    uncertain.connected();
    assert(uncertain.command("CD_PRESS TRACK_NEXT", at(513), at(513)) ==
           "ERR CD_FAULT_LATCHED\n");
}

static void test_cd_hold_uncertain_and_volume_exclusion()
{
    FakeTx press_tx;
    press_tx.fail_next = true;
    Xs8ServiceCore press(press_tx, Ms(2000), true);
    press.connected();
    assert(press.command("CD_PRESS TRACK_NEXT", at(10), at(10)).find(
           "ERR CD_PRESS_UNCERTAIN") == 0);
    assert(press_tx.sent == (std::vector<uint8_t>{0xFC, 0x72}));
    assert(press.cd_fault() && !press.cd_release_owed());

    FakeTx monitor_tx;
    Xs8ServiceCore monitor(monitor_tx, Ms(2000), true);
    monitor.connected();
    monitor.command("CD_PRESS SEEK_BACKWARD", at(10), at(10));
    monitor_tx.healthy = false;
    monitor.tick(at(20));
    assert(monitor_tx.sent == (std::vector<uint8_t>{0x6A, 0x72}));
    assert(monitor.cd_fault() && monitor.cd_release_owed());
    monitor.tick(at(30));
    assert(monitor_tx.sent.size() == 2);

    FakeTx release_tx;
    Xs8ServiceCore release(release_tx, Ms(2000), true);
    release.connected();
    const auto release_token = hold_token(
        release.command("CD_PRESS SEEK_FORWARD", at(10), at(10)));
    release_tx.fail_release = true;
    assert(release.command("CD_RELEASE " + release_token,
                           at(100), at(100)).find(
           "ERR CD_RELEASE_UNCERTAIN") == 0);
    assert(release.cd_fault() && release.cd_release_owed());
    assert(release.command("CD_PRESS TRACK_NEXT", at(101), at(101)) ==
           "ERR CD_FAULT_LATCHED\n");
    assert(!release.shutdown(at(110)));
    assert(release_tx.sent == (std::vector<uint8_t>{0x7A, 0x72, 0x72}));
    FakeTx restarted_tx;
    Xs8ServiceCore restarted(restarted_tx, Ms(2000), false);
    restarted.connected();
    assert(restarted.command("CD_PRESS TRACK_NEXT", at(1), at(1)) ==
           "ERR BOOT_INHIBIT\n");
    assert(restarted_tx.sent.empty());

    FakeTx disconnect_failure_tx;
    Xs8ServiceCore disconnect_failure(disconnect_failure_tx, Ms(2000), true);
    disconnect_failure.connected();
    disconnect_failure.command("CD_PRESS TRACK_PREVIOUS", at(10), at(10));
    disconnect_failure_tx.fail_release = true;
    disconnect_failure.disconnected(at(20));
    disconnect_failure.tick(at(2010));
    assert(disconnect_failure_tx.sent ==
           (std::vector<uint8_t>{0xEC, 0x72}));
    assert(disconnect_failure.cd_fault() &&
           disconnect_failure.cd_release_owed());

    FakeTx volume_tx;
    Xs8ServiceCore volume(volume_tx, Ms(2000), true);
    volume.connected();
    volume.command("VOLUME_PRESS DOWN", at(10), at(10));
    assert(volume.command("CD_PRESS TRACK_NEXT", at(11), at(11)) ==
           "ERR HOLDING_DOWN\n");
    assert(volume_tx.sent == (std::vector<uint8_t>{0x17, 0xE9}));
    assert(volume.command("VOLUME_RELEASE DOWN", at(20), at(20)) ==
           "OK VOLUME_RELEASE_TX\n");
}

static void test_cd_hold_interleaved_clients_and_volume_owner()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true);
    constexpr uint64_t cec = 1, cd = 2, web = 3;
    core.connected(cec);
    core.connected(cd);
    const auto token = hold_token(core.command(
        "CD_PRESS TRACK_NEXT", at(10), at(10), cd));
    core.disconnected(at(20), cd); // Short-lived CD PRESS socket.
    assert(tx.sent == (std::vector<uint8_t>{0xFC}));
    core.connected(web);
    assert(core.command("STATUS", at(30), at(30), web).find(
           "cd_state=HOLDING") != std::string::npos);
    assert(core.command("VOLUME_PRESS UP", at(40), at(40), cec) ==
           "ERR CD_TRACK_BUSY\n");
    assert(core.command("MUTE", at(40), at(40), cec) ==
           "ERR CD_TRACK_BUSY\n");
    assert(core.command("CD_HEARTBEAT " + token, at(510), at(510), web) ==
           "OK CD_LEASE_RENEWED\n");
    assert(tx.sent == (std::vector<uint8_t>{0xFC}));
    core.disconnected(at(520), web);
    core.connected(cd);
    assert(core.command("CD_RELEASE " + token, at(530), at(530), cd).find(
           "OK CD_RELEASE_TX") == 0);
    assert(tx.sent == (std::vector<uint8_t>{0xFC, 0x72}));
    assert(core.command("VOLUME_PRESS UP", at(540), at(540), cec) ==
           "OK VOLUME_PRESS_TX\n");
    core.disconnected(at(550), cd); // Other client cannot stop CEC volume.
    assert(tx.sent == (std::vector<uint8_t>{0xFC, 0x72, 0x17, 0xF9}));
    core.disconnected(at(560), cec); // CEC owner disconnect still sends STOP.
    assert(tx.sent ==
           (std::vector<uint8_t>{0xFC, 0x72, 0x17, 0xF9, 0x09, 0x07}));
}

// ---- Attended research CONTEXT session ----

static std::string context_token(const std::string& reply)
{
    const std::string field = " token=";
    const auto start = reply.find(field);
    assert(start != std::string::npos);
    const auto first = start + field.size();
    return reply.substr(first, reply.find(' ', first) - first);
}

static bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

static std::string enter(Xs8ServiceCore& core, int ms, uint64_t client = 1)
{
    const std::string reply = core.command("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED",
                                           at(ms), at(ms), client);
    assert(reply.rfind("OK CONTEXT_SESSION_ENTERED token=", 0) == 0);
    assert(has(reply, " byte=0x17 gpio_tx=TX_OK waveform=MATCH context_may_be_open=1"));
    return context_token(reply);
}

static void test_context_gates_and_authorization()
{
    FakeTx tx;
    Xs8ServiceCore inhibited(tx, Ms(2000), false, {}, [] { return at(0); });
    inhibited.connected(1);
    inhibited.set_research_context_enabled(true);
    assert(inhibited.command("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED", at(1), at(1), 1) ==
           "ERR CONTEXT_SESSION reason=BOOT_INHIBIT\n");
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
    core.connected(1);
    assert(core.command("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED", at(1), at(1), 1) ==
           "ERR CONTEXT_SESSION reason=DISABLED\n");
    core.set_research_context_enabled(true);
    for (const char *line : {"CONTEXT_SESSION_ENTER", "CONTEXT_SESSION_ENTER YES",
                             "CONTEXT_SESSION_ENTER CONFIRM_ATTENDED EXTRA",
                             "CONTEXT_SESSION_SEND 1 0x78", "CONTEXT_SESSION_BOGUS"})
        assert(core.command(line, at(2), at(2), 1).rfind("ERR CONTEXT_SESSION reason=", 0) == 0);
    assert(core.command("CONTEXT_SESSION_EXIT 1", at(2), at(2), 1) ==
           "ERR CONTEXT_SESSION reason=NOT_ACTIVE\n");
    assert(tx.sent.empty());
    // An existing CD hold blocks entry; CD safety keeps priority.
    assert(core.command("CD_PRESS TRACK_NEXT", at(3), at(3), 1).rfind("OK CD_PRESS_TX", 0) == 0);
    assert(core.command("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED", at(4), at(4), 1) ==
           "ERR CONTEXT_SESSION reason=CD_BUSY\n");
    assert(tx.sent == (std::vector<uint8_t>{0xFC}));
}

static void test_context_send_exit_and_token_ownership()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
    core.connected(1);
    core.connected(2);
    core.set_research_context_enabled(true);
    const std::string token = enter(core, 10);
    const std::string status = core.command("STATUS", at(11), at(11), 2);
    assert(has(status, "state=RESEARCH_CONTEXT ") && has(status, "context_may_be_open=1"));
    assert(core.command("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED", at(12), at(12), 2) ==
           "ERR CONTEXT_SESSION reason=BUSY\n");
    // Token and owner connection are both required; neither alone suffices.
    assert(core.command("CONTEXT_SESSION_SEND " + token + " 0x78", at(13), at(13), 2) ==
           "ERR CONTEXT_SESSION reason=NOT_OWNER\n");
    assert(core.command("CONTEXT_SESSION_SEND " + wrong_token(token) + " 0x78", at(14), at(14), 1) ==
           "ERR CONTEXT_SESSION reason=NOT_OWNER\n");
    assert(core.command("CONTEXT_SESSION_EXIT " + token, at(15), at(15), 2) ==
           "ERR CONTEXT_SESSION reason=NOT_OWNER\n");
    assert(tx.sent == (std::vector<uint8_t>{0x17}));
    const std::string sent = core.command("CONTEXT_SESSION_SEND " + token + " 0x78", at(16), at(16), 1);
    assert(sent == "OK CONTEXT_SESSION_SENT byte=0x78 gpio_tx=TX_OK waveform=MATCH "
                   "context_may_be_open=1 receiver_execution=UNVERIFIED\n");
    for (const char *line : {"0x17", "0x07", "0xE9", "0xF9", "0x7", "78", "0xZZ"}) {
        const std::string reply = core.command(
            "CONTEXT_SESSION_SEND " + token + " " + line, at(17), at(17), 1);
        assert(reply.rfind("ERR CONTEXT_SESSION reason=", 0) == 0);
    }
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0x78}));
    const std::string exited = core.command("CONTEXT_SESSION_EXIT " + token, at(18), at(18), 1);
    assert(exited.rfind("OK CONTEXT_SESSION_EXITED byte=0x07 root_attempts=1 "
                        "root_gpio_tx=TX_OK root_waveform=MATCH context_may_be_open=0 "
                        "state=IDLE root_verified=NOT_OBSERVED", 0) == 0);
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0x78, 0x07}));
    assert(!core.context_may_be_open() && !core.research_context_open());
    assert(core.command("CONTEXT_SESSION_SEND " + token + " 0x78", at(19), at(19), 1) ==
           "ERR CONTEXT_SESSION reason=NOT_ACTIVE\n");
    // A new session gets a new token; the old one never revives it.
    const std::string second = enter(core, 20);
    assert(second != token);
    assert(core.command("CONTEXT_SESSION_EXIT " + token, at(21), at(21), 1) ==
           "ERR CONTEXT_SESSION reason=NOT_OWNER\n");
    assert(has(core.command("CONTEXT_SESSION_STATUS", at(22), at(22), 2), "open=1"));
}

static void test_context_excludes_every_other_transmission()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
    core.connected(1);
    core.connected(2);
    core.set_research_context_enabled(true);
    enter(core, 10);
    for (const char *line : {"MUTE", "VOLUME_PRESS UP", "SELECT_SOURCE CD",
                             "CD_TRACK_NEXT", "CD_PRESS SEEK_FORWARD"})
        assert(core.command(line, at(11), at(11), 2) == "ERR RESEARCH_CONTEXT_ACTIVE\n");
    assert(core.begin_query_status(at(12)) == "ERR RESEARCH_CONTEXT\n");
    assert(core.research_ready(at(12)) == "ERR RESEARCH reason=RESEARCH_CONTEXT\n");
    assert(core.research_send(0x84, at(12)) == "ERR RESEARCH reason=RESEARCH_CONTEXT\n");
    // RELEASE remains answerable and transmits nothing without a hold.
    assert(core.command("VOLUME_RELEASE UP", at(13), at(13), 2) == "OK VOLUME_RELEASE_TX\n");
    assert(tx.sent == (std::vector<uint8_t>{0x17}));
}

static void test_context_lease_lifetime_and_disconnect()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
    core.connected(1);
    core.connected(2);
    core.set_research_context_enabled(true);
    const std::string token = enter(core, 0);
    core.tick(at(2999));
    assert(core.research_context_open());
    assert(core.command("CONTEXT_SESSION_HEARTBEAT " + token, at(2999), at(2999), 1) ==
           "OK CONTEXT_SESSION_LEASE_RENEWED\n");
    core.tick(at(5998));
    assert(core.research_context_open());
    core.tick(at(5999));
    assert(!core.research_context_open() && !core.context_may_be_open());
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0x07}));
    assert(has(core.command("CONTEXT_SESSION_STATUS", at(6000), at(6000), 2),
               "last=LEASE_EXPIRED_ROOT_TX_OK state=IDLE"));
    assert(core.command("CONTEXT_SESSION_HEARTBEAT " + token, at(6001), at(6001), 1) ==
           "ERR CONTEXT_SESSION reason=NOT_ACTIVE\n");

    // Heartbeats cannot extend the absolute lifetime.
    const std::string lived = enter(core, 10000);
    for (int ms = 12000; ms < 310000; ms += 2000)
        assert(core.command("CONTEXT_SESSION_HEARTBEAT " + lived, at(ms), at(ms), 1) ==
               "OK CONTEXT_SESSION_LEASE_RENEWED\n");
    core.tick(at(310000));
    assert(!core.research_context_open());
    assert(has(core.command("CONTEXT_SESSION_STATUS", at(310001), at(310001), 2),
               "last=MAX_LIFETIME_ROOT_TX_OK"));

    // Only the owner's disconnect exits, and no further client is needed.
    enter(core, 320000);
    core.disconnected(at(320001), 2);
    assert(core.research_context_open());
    core.disconnected(at(320002), 1);
    assert(!core.research_context_open() && !core.context_may_be_open());
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0x07, 0x17, 0x07, 0x17, 0x07}));
}

static void test_context_uncertain_send_exits_and_latches()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
    core.connected(1);
    core.set_research_context_enabled(true);
    const std::string token = enter(core, 10);
    tx.fail_next = true;
    const std::string reply = core.command("CONTEXT_SESSION_SEND " + token + " 0x88",
                                           at(11), at(11), 1);
    assert(reply.rfind("ERR CONTEXT_SESSION_SEND_UNCERTAIN byte=0x88 gpio_tx=TX_OK "
                       "waveform=MISSING root_attempts=1 root_gpio_tx=TX_OK "
                       "root_waveform=MATCH context_may_be_open=0 state=FAULT_LATCHED", 0) == 0);
    // Exit attempted once, candidate never retried, fault never silently cleared.
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0x88, 0x07}));
    assert(core.state() == VolumeControl::State::FAULT_LATCHED);
    assert(core.command("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED", at(12), at(12), 1) ==
           "ERR CONTEXT_SESSION reason=FAULT_LATCHED\n");
    assert(core.command("MUTE", at(13), at(13), 1) == "ERR MUTE_UNCERTAIN\n");
    assert(tx.sent.size() == 3);
}

static void test_context_entry_busy_and_exit_failure()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
    core.connected(1);
    core.set_research_context_enabled(true);
    tx.fail_gpio_next = true; // BUSY or GPIO failure on the entry frame.
    const std::string entered = core.command("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED",
                                             at(10), at(10), 1);
    assert(entered.rfind("ERR CONTEXT_SESSION_ENTER_UNCERTAIN byte=0x17 root_attempts=1 "
                         "context_may_be_open=0 state=FAULT_LATCHED", 0) == 0);
    assert(tx.sent == (std::vector<uint8_t>{0x17, 0x07}));
    assert(!core.research_context_open());

    FakeTx stuck;
    Xs8ServiceCore other(stuck, Ms(2000), true, {}, [] { return at(0); });
    other.connected(1);
    other.set_research_context_enabled(true);
    const std::string token = enter(other, 10);
    stuck.fail_root = true;
    const std::string exited = other.command("CONTEXT_SESSION_EXIT " + token, at(11), at(11), 1);
    assert(exited.rfind("ERR CONTEXT_SESSION_EXIT_UNCERTAIN byte=0x07 root_attempts=3 "
                        "root_gpio_tx=TX_OK root_waveform=MISSING context_may_be_open=1 "
                        "state=FAULT_LATCHED", 0) == 0);
    assert(stuck.sent == (std::vector<uint8_t>{0x17, 0x07, 0x07, 0x07}));
    const std::string status = other.command("STATUS", at(12), at(12), 1);
    assert(has(status, "state=FAULT_LATCHED") && has(status, "context_may_be_open=1"));
}

static void test_context_monitor_failure_and_shutdown()
{
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
    core.connected(1);
    core.set_research_context_enabled(true);
    enter(core, 10);
    tx.healthy = false;
    core.tick(at(11));
    assert(!core.research_context_open());
    assert(core.state() == VolumeControl::State::FAULT_LATCHED);
    assert(tx.sent.size() >= 2 && tx.sent[1] == 0x07);

    FakeTx clean;
    Xs8ServiceCore stopping(clean, Ms(2000), true, {}, [] { return at(0); });
    stopping.connected(1);
    stopping.set_research_context_enabled(true);
    enter(stopping, 10);
    assert(stopping.shutdown(at(11)));
    assert(clean.sent == (std::vector<uint8_t>{0x17, 0x07}));
    assert(!stopping.context_may_be_open());
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
    test_cd_hold_press_release_and_interlocks();
    test_cd_hold_watchdog_disconnect_and_shutdown();
    test_cd_lease_renewal_stale_token_and_recovery();
    test_cd_hold_uncertain_and_volume_exclusion();
    test_cd_hold_interleaved_clients_and_volume_owner();
    test_context_gates_and_authorization();
    test_context_send_exit_and_token_ownership();
    test_context_excludes_every_other_transmission();
    test_context_lease_lifetime_and_disconnect();
    test_context_uncertain_send_exits_and_latches();
    test_context_entry_busy_and_exit_failure();
    test_context_monitor_failure_and_shutdown();
    std::cout << "XS8 service core tests passed\n";
}
