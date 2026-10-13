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

static bool status_has(Xs8ServiceCore& core, int time, const char *field)
{
    return core.command("STATUS", at(time), at(time)).find(field) !=
        std::string::npos;
}

static void test_cd_hold_max_hold()
{
    using Bytes = std::vector<uint8_t>;
    static_assert(Xs8ServiceCore::kCdMaxHold == Ms(10000));

    // Continuous heartbeats never extend the hold past the maximum.
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
    core.connected();
    const auto token = hold_token(core.command(
        "CD_PRESS SEEK_FORWARD", at(10), at(10)));
    for (int time = 510; time <= 9510; time += 500) {
        core.tick(at(time));
        assert(core.command("CD_HEARTBEAT " + token, at(time), at(time)) ==
               "OK CD_LEASE_RENEWED\n");
    }
    core.tick(at(10009));
    assert(tx.sent == (Bytes{0x7A}) && core.cd_release_owed());
    core.tick(at(10010));
    assert(tx.sent == (Bytes{0x7A, 0x72}));
    assert(!core.cd_fault() && !core.cd_release_owed());
    assert(status_has(core, 10010, " cd_state=IDLE "));
    assert(status_has(core, 10010, " cd_last_result=MAX_HOLD "));
    assert(status_has(core, 10010, " cd_lease_expired=0 "));
    assert(status_has(core, 10010, " state=IDLE "));
    assert(status_has(core, 10010, " receiver_execution=UNVERIFIED "));
    // A late heartbeat or release neither renews nor transmits.
    assert(core.command("CD_HEARTBEAT " + token, at(10011), at(10011)) ==
           "ERR CD_NOT_ACTIVE\n");
    assert(core.command("CD_RELEASE " + token, at(10012), at(10012)) ==
           "ERR CD_NOT_ACTIVE\n");
    for (int time = 10020; time <= 30000; time += 20) core.tick(at(time));
    assert(tx.sent == (Bytes{0x7A, 0x72}));

    // A new hold gets a fresh token and its own maximum; the expired token
    // cannot renew or release it.
    const auto fresh = hold_token(core.command(
        "CD_PRESS SEEK_BACKWARD", at(30000), at(30000)));
    assert(fresh != token);
    assert(status_has(core, 30000, " cd_last_result=HOLDING "));
    assert(core.command("CD_HEARTBEAT " + token, at(30001), at(30001)) ==
           "ERR CD_NOT_OWNER\n");
    assert(core.command("CD_RELEASE " + token, at(30001), at(30001)) ==
           "ERR CD_NOT_OWNER\n");
    for (int time = 30500; time <= 39500; time += 500)
        assert(core.command("CD_HEARTBEAT " + fresh, at(time), at(time)) ==
               "OK CD_LEASE_RENEWED\n");
    assert(tx.sent == (Bytes{0x7A, 0x72, 0x6A}));
    core.tick(at(40000));
    assert(tx.sent == (Bytes{0x7A, 0x72, 0x6A, 0x72}));

    // A heartbeat that reaches the service after the maximum, before any
    // control-loop tick, is refused after the single expiry RELEASE.
    FakeTx late_tx;
    Xs8ServiceCore late(late_tx, Ms(2000), true, {}, [] { return at(0); });
    late.connected();
    const auto late_token = hold_token(late.command(
        "CD_PRESS SEEK_FORWARD", at(10), at(10)));
    for (int time = 1010; time <= 9010; time += 1000)
        late.command("CD_HEARTBEAT " + late_token, at(time), at(time));
    assert(late.command("CD_HEARTBEAT " + late_token, at(9990), at(10010)) ==
           "ERR CD_NOT_ACTIVE\n");
    assert(late_tx.sent == (Bytes{0x7A, 0x72}));
    assert(status_has(late, 10010, " cd_last_result=MAX_HOLD "));
    late.tick(at(12010));
    assert(late_tx.sent == (Bytes{0x7A, 0x72}));

    // Normal release before the maximum leaves nothing for expiry to do.
    FakeTx early_tx;
    Xs8ServiceCore early(early_tx, Ms(2000), true, {}, [] { return at(0); });
    early.connected();
    const auto early_token = hold_token(early.command(
        "CD_PRESS SEEK_FORWARD", at(10), at(10)));
    early.command("CD_HEARTBEAT " + early_token, at(1500), at(1500));
    assert(early.command("CD_RELEASE " + early_token, at(3000), at(3000)).find(
           "OK CD_RELEASE_TX reason=CLIENT_RELEASE") == 0);
    for (int time = 3020; time <= 20000; time += 20) early.tick(at(time));
    assert(early_tx.sent == (Bytes{0x7A, 0x72}));
    assert(status_has(early, 20000, " cd_last_result=CLIENT_RELEASE "));

    // Lease expiry before the maximum keeps its own reason, even when the
    // control loop only notices after the maximum has also passed.
    FakeTx lease_tx;
    Xs8ServiceCore lease(lease_tx, Ms(2000), true, {}, [] { return at(0); });
    lease.connected();
    const auto lease_token = hold_token(lease.command(
        "CD_PRESS SEEK_BACKWARD", at(10), at(10)));
    lease.command("CD_HEARTBEAT " + lease_token, at(7000), at(7000));
    lease.tick(at(12000));
    assert(lease_tx.sent == (Bytes{0x6A, 0x72}));
    assert(status_has(lease, 12000, " cd_last_result=LEASE_EXPIRED "));
    assert(status_has(lease, 12000, " cd_lease_expired=1 "));
    lease.tick(at(20000));
    assert(lease_tx.sent == (Bytes{0x6A, 0x72}));

    // A renewed lease that would outlast the maximum is cut at the maximum.
    FakeTx cut_tx;
    Xs8ServiceCore cut(cut_tx, Ms(2000), true, {}, [] { return at(0); },
                       {}, Ms(2000), Ms(1000));
    cut.connected();
    const auto cut_token = hold_token(cut.command(
        "CD_PRESS SEEK_FORWARD", at(10), at(10)));
    cut.command("CD_HEARTBEAT " + cut_token, at(900), at(900));
    cut.tick(at(1009));
    assert(cut_tx.sent == (Bytes{0x7A}));
    cut.tick(at(1010));
    assert(cut_tx.sent == (Bytes{0x7A, 0x72}));
    assert(status_has(cut, 1010, " cd_last_result=MAX_HOLD "));
    assert(status_has(cut, 1010, " cd_lease_expired=0 "));

    // An unconfirmed expiry RELEASE latches the fault and keeps the
    // obligation; only graceful shutdown makes its one extra attempt.
    FakeTx fail_tx;
    Xs8ServiceCore fail(fail_tx, Ms(2000), true, {}, [] { return at(0); });
    fail.connected();
    const auto fail_token = hold_token(fail.command(
        "CD_PRESS SEEK_FORWARD", at(10), at(10)));
    for (int time = 1010; time <= 9010; time += 1000)
        fail.command("CD_HEARTBEAT " + fail_token, at(time), at(time));
    fail_tx.fail_release = true;
    fail.tick(at(10010));
    assert(fail_tx.sent == (Bytes{0x7A, 0x72}));
    assert(fail.cd_fault() && fail.cd_release_owed());
    assert(status_has(fail, 10010, " state=FAULT_LATCHED "));
    assert(status_has(fail, 10010, " cd_state=FAULT_LATCHED "));
    assert(status_has(fail, 10010, " cd_last_result=RELEASE_UNCERTAIN "));
    assert(fail.command("CD_HEARTBEAT " + fail_token, at(10011), at(10011)) ==
           "ERR CD_NOT_ACTIVE\n");
    assert(fail.command("CD_PRESS SEEK_FORWARD", at(10012), at(10012)) ==
           "ERR CD_FAULT_LATCHED\n");
    for (int time = 10020; time <= 20000; time += 20) fail.tick(at(time));
    assert(fail_tx.sent == (Bytes{0x7A, 0x72}));
    assert(!fail.shutdown(at(20001)));
    assert(fail_tx.sent == (Bytes{0x7A, 0x72, 0x72}));
    assert(!fail.shutdown(at(20002)));
    assert(fail_tx.sent == (Bytes{0x7A, 0x72, 0x72}));
}

static void test_volume_step()
{
    using Bytes = std::vector<uint8_t>;
    const std::string idle_fields = " state=IDLE stop_owed=0 context_may_be_open=0"
                                    " receiver_execution=UNVERIFIED\n";
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
    core.connected(1);
    core.connected(2);
    assert(core.command("VOLUME_STEP UP", at(10), at(10), 2) ==
           "OK VOLUME_STEP_TX direction=UP" + idle_fields);
    assert(core.command("VOLUME_STEP DOWN", at(20), at(20), 2) ==
           "OK VOLUME_STEP_TX direction=DOWN" + idle_fields);
    assert(tx.sent == (Bytes{0x17, 0xF9, 0x09, 0x07, 0x17, 0xE9, 0x09, 0x07}));
    assert(status_has(core, 21, " state=IDLE stop_owed=0 context_may_be_open=0 "));
    assert(status_has(core, 21, " last_stop_waveform=MATCH "));
    for (const char *line : {"VOLUME_STEP", "VOLUME_STEP up", "VOLUME_STEP UP ",
                             "VOLUME_STEP LEFT"})
        assert(core.command(line, at(22), at(22), 2) == "ERR UNKNOWN_COMMAND\n");
    assert(core.command("VOLUME_STEP UP", at(22), at(22), 9) == "ERR NO_CLIENT\n");
    // Disconnecting the stepping client owes nothing.
    core.disconnected(at(23), 2);
    assert(tx.sent.size() == 8);

    // A CEC hold is neither interrupted nor taken over.
    core.connected(2);
    assert(core.command("VOLUME_PRESS UP", at(30), at(30), 1) == "OK VOLUME_PRESS_TX\n");
    assert(core.command("VOLUME_STEP UP", at(40), at(40), 2) == "ERR VOLUME_BUSY\n");
    assert(core.command("VOLUME_STEP DOWN", at(40), at(40), 2) == "ERR VOLUME_BUSY\n");
    core.disconnected(at(41), 2);
    assert(core.state() == VolumeControl::State::HOLDING_UP);
    assert(core.command("VOLUME_PRESS UP", at(50), at(50), 1) == "OK VOLUME_PRESS_TX\n");
    assert(tx.sent.size() == 10);
    core.disconnected(at(60), 1);
    assert(tx.sent == (Bytes{0x17, 0xF9, 0x09, 0x07, 0x17, 0xE9, 0x09, 0x07,
                             0x17, 0xF9, 0x09, 0x07}));

    // Re-arm is preserved: after a CEC watchdog stop a step works, and the
    // CEC press stays blocked until its release, as before.
    core.connected(1);
    core.connected(2);
    assert(core.command("VOLUME_PRESS DOWN", at(100), at(100), 1) == "OK VOLUME_PRESS_TX\n");
    core.tick(at(1300));
    assert(core.state() == VolumeControl::State::IDLE);
    assert(core.command("VOLUME_STEP DOWN", at(1310), at(1310), 2).rfind(
           "OK VOLUME_STEP_TX", 0) == 0);
    assert(core.command("VOLUME_PRESS DOWN", at(1320), at(1320), 1) == "ERR IDLE\n");
    assert(core.command("VOLUME_RELEASE DOWN", at(1330), at(1330), 1) ==
           "OK VOLUME_RELEASE_TX\n");
    assert(core.command("VOLUME_PRESS DOWN", at(1340), at(1340), 1) == "OK VOLUME_PRESS_TX\n");
    assert(core.command("VOLUME_RELEASE DOWN", at(1350), at(1350), 1) ==
           "OK VOLUME_RELEASE_TX\n");
    // Successful steps never leave a re-arm block behind.
    assert(core.command("VOLUME_STEP UP", at(1360), at(1360), 2).rfind(
           "OK VOLUME_STEP_TX", 0) == 0);
    assert(core.command("VOLUME_PRESS UP", at(1370), at(1370), 1) == "OK VOLUME_PRESS_TX\n");
    assert(core.command("VOLUME_RELEASE UP", at(1380), at(1380), 1) ==
           "OK VOLUME_RELEASE_TX\n");
    assert(core.command("MUTE", at(1390), at(1390), 1) == "OK MUTE_TX\n");

    // Refusals transmit nothing.
    FakeTx boot_tx;
    Xs8ServiceCore boot(boot_tx, Ms(2000), false, {}, [] { return at(0); });
    boot.connected();
    assert(boot.command("VOLUME_STEP UP", at(1), at(1)) == "ERR BOOT_INHIBIT\n");
    assert(boot_tx.sent.empty());

    FakeTx cd_tx;
    Xs8ServiceCore cd(cd_tx, Ms(2000), true, {}, [] { return at(0); });
    cd.connected();
    const auto token = hold_token(cd.command("CD_PRESS SEEK_FORWARD", at(1), at(1)));
    assert(cd.command("VOLUME_STEP UP", at(2), at(2)) == "ERR CD_TRACK_BUSY\n");
    cd_tx.fail_release = true;
    assert(cd.command("CD_RELEASE " + token, at(3), at(3)).rfind(
           "ERR CD_RELEASE_UNCERTAIN", 0) == 0);
    assert(cd.command("VOLUME_STEP UP", at(4), at(4)) == "ERR CD_FAULT_LATCHED\n");
    assert(cd_tx.sent == (Bytes{0x7A, 0x72}));

    FakeTx context_tx;
    Xs8ServiceCore context(context_tx, Ms(2000), true, {}, [] { return at(0); });
    context.connected(1);
    context.set_research_context_enabled(true);
    assert(context.command("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED", at(1), at(1), 1)
           .rfind("OK CONTEXT_SESSION_ENTERED", 0) == 0);
    assert(context.command("VOLUME_STEP UP", at(2), at(2), 1) ==
           "ERR RESEARCH_CONTEXT_ACTIVE\n");
    assert(context_tx.sent == (Bytes{0x17}));

    FakeTx monitor_tx;
    Xs8ServiceCore monitor(monitor_tx, Ms(2000), true, {}, [] { return at(0); });
    monitor.connected();
    monitor_tx.healthy = false;
    assert(monitor.command("VOLUME_STEP DOWN", at(1), at(1)) == "ERR MONITOR_FAILED\n");
    assert(monitor_tx.sent.empty());

    bool signalled = true;
    FakeTx stop_tx;
    Xs8ServiceCore stopping(stop_tx, Ms(2000), true, [&] { return signalled; },
                            [] { return at(0); });
    stopping.connected();
    assert(stopping.command("VOLUME_STEP UP", at(1), at(1)) == "ERR SHUTDOWN\n");
    assert(stop_tx.sent.empty());
    signalled = false;
    assert(stopping.command("VOLUME_STEP UP", at(2), at(2)).rfind(
           "OK VOLUME_STEP_TX", 0) == 0);
    assert(stopping.shutdown(at(3)));
    assert(stop_tx.sent == (Bytes{0x17, 0xF9, 0x09, 0x07}));

    // Uncertain STOP: FAULT latched, obligations kept and reported, every
    // later transmission refused, and one more cleanup only at shutdown.
    FakeTx stop_fail_tx;
    Xs8ServiceCore stop_fail(stop_fail_tx, Ms(2000), true, {}, [] { return at(0); });
    stop_fail.connected();
    stop_fail_tx.stop_matches = {false, false, false};
    assert(stop_fail.command("VOLUME_STEP UP", at(1), at(1)) ==
           "ERR VOLUME_STEP_UNCERTAIN direction=UP state=FAULT_LATCHED stop_owed=1"
           " context_may_be_open=1 receiver_execution=UNVERIFIED\n");
    assert(stop_fail_tx.sent == (Bytes{0x17, 0xF9, 0x09, 0x09, 0x09}));
    for (const char *line : {"VOLUME_STEP UP", "VOLUME_STEP DOWN",
                             "VOLUME_PRESS UP", "MUTE"})
        assert(stop_fail.command(line, at(2), at(2)).rfind("ERR ", 0) == 0);
    assert(stop_fail.command("CD_PRESS TRACK_NEXT", at(2), at(2)) == "ERR FAULT_LATCHED\n");
    assert(stop_fail.command("SELECT_SOURCE CD", at(2), at(2)) == "ERR FAULT_LATCHED\n");
    assert(stop_fail_tx.sent.size() == 5);
    assert(!stop_fail.shutdown(at(3)));
    assert(stop_fail_tx.sent == (Bytes{0x17, 0xF9, 0x09, 0x09, 0x09, 0x09, 0x07}));

    // Uncertain CONTEXT exit.
    FakeTx root_fail_tx;
    Xs8ServiceCore root_fail(root_fail_tx, Ms(2000), true, {}, [] { return at(0); });
    root_fail.connected();
    root_fail_tx.fail_root = true;
    assert(root_fail.command("VOLUME_STEP DOWN", at(1), at(1)) ==
           "ERR VOLUME_STEP_UNCERTAIN direction=DOWN state=FAULT_LATCHED stop_owed=0"
           " context_may_be_open=1 receiver_execution=UNVERIFIED\n");
    assert(root_fail_tx.sent == (Bytes{0x17, 0xE9, 0x09, 0x07, 0x07, 0x07}));
    assert(root_fail.command("VOLUME_STEP DOWN", at(2), at(2)) == "ERR FAULT_LATCHED\n");
    assert(status_has(root_fail, 2, " state=FAULT_LATCHED stop_owed=0 context_may_be_open=1 "));
    assert(root_fail_tx.sent.size() == 6);

    // Uncertain entry frame: exit still attempted, FAULT latched, no retry.
    FakeTx start_fail_tx;
    Xs8ServiceCore start_fail(start_fail_tx, Ms(2000), true, {}, [] { return at(0); });
    start_fail.connected();
    start_fail_tx.fail_next = true;
    assert(start_fail.command("VOLUME_STEP UP", at(1), at(1)) ==
           "ERR VOLUME_STEP_UNCERTAIN direction=UP state=FAULT_LATCHED stop_owed=0"
           " context_may_be_open=0 receiver_execution=UNVERIFIED\n");
    assert(start_fail_tx.sent == (Bytes{0x17, 0x07}));
    assert(start_fail.command("VOLUME_STEP UP", at(2), at(2)) == "ERR FAULT_LATCHED\n");
    assert(start_fail_tx.sent == (Bytes{0x17, 0x07}));
}

static void test_cd_transport_keys()
{
    using Bytes = std::vector<uint8_t>;
    struct Key { const char *line; uint8_t byte; const char *action; };
    const Key keys[] = {{"CD_PLAY", 0x41, "PLAY"}, {"CD_PLAY_PAUSE", 0xBC, "PLAY_PAUSE"},
                        {"CD_STOP", 0x9C, "STOP"}};
    for (const auto& key : keys) {
        FakeTx tx;
        Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
        core.connected();
        // Exactly one ROOT frame, no 0x17, no KEY UP, no hold.
        assert(core.command(key.line, at(1), at(1)) ==
               std::string("OK CD_TRANSPORT_TX action=") + key.action +
               " gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED\n");
        assert(tx.sent == (Bytes{key.byte}));
        assert(!core.cd_release_owed() && !core.cd_fault());
        assert(status_has(core, 2, " cd_state=IDLE "));
        assert(status_has(core, 2, (std::string(" cd_action=") + key.action + " ").c_str()));
        assert(status_has(core, 2, " cd_last_result=TX_VERIFIED "));
        // Immediately usable again: no hold or lease was created.
        assert(core.command(key.line, at(3), at(3)).rfind("OK CD_TRANSPORT_TX", 0) == 0);
        assert(core.command("CD_TRACK_NEXT", at(4), at(4)).rfind("OK CD_TRACK_TX", 0) == 0);
        assert(tx.sent == (Bytes{key.byte, key.byte, 0xFC, 0x72}));
        core.tick(at(10000));
        assert(tx.sent.size() == 4);

        // Uncertain GPIO or waveform: reported, never retried, no CD fault.
        for (int mode = 0; mode < 2; ++mode) {
            FakeTx fail_tx;
            Xs8ServiceCore fail(fail_tx, Ms(2000), true, {}, [] { return at(0); });
            fail.connected();
            if (mode == 0) fail_tx.fail_gpio_next = true;
            else fail_tx.fail_next = true;
            assert(fail.command(key.line, at(1), at(1)) ==
                   std::string("ERR CD_TRANSPORT_UNCERTAIN action=") + key.action +
                   (mode == 0 ? " gpio_tx=TX_FAIL" : " gpio_tx=TX_OK") +
                   " waveform=MISSING receiver_execution=UNVERIFIED\n");
            assert(fail_tx.sent == (Bytes{key.byte}));
            assert(!fail.cd_fault() && !fail.cd_release_owed());
            assert(status_has(fail, 2, " cd_last_result=TX_UNCERTAIN "));
            assert(status_has(fail, 2, " state=IDLE "));
            fail.tick(at(5000));
            assert(fail_tx.sent == (Bytes{key.byte}));
        }
    }
    for (const char *line : {"CD_PLAY ", "CD_PAUSE", "cd_stop", "CD_STOP 1", "CD_PLAYPAUSE"}) {
        FakeTx tx;
        Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
        core.connected();
        assert(core.command(line, at(1), at(1)) == "ERR UNKNOWN_COMMAND\n");
        assert(tx.sent.empty());
    }

    // Refusals transmit nothing and leave existing CD and volume state alone.
    FakeTx boot_tx;
    Xs8ServiceCore boot(boot_tx, Ms(2000), false, {}, [] { return at(0); });
    boot.connected();
    assert(boot.command("CD_STOP", at(1), at(1)) == "ERR BOOT_INHIBIT\n");
    assert(boot.command("CD_STOP", at(1), at(1), 7) == "ERR NO_CLIENT\n");
    assert(boot_tx.sent.empty());

    FakeTx hold_tx;
    Xs8ServiceCore hold(hold_tx, Ms(2000), true, {}, [] { return at(0); });
    hold.connected();
    const auto token = hold_token(hold.command("CD_PRESS SEEK_FORWARD", at(1), at(1)));
    for (const char *line : {"CD_PLAY", "CD_PLAY_PAUSE", "CD_STOP"})
        assert(hold.command(line, at(2), at(2)) == "ERR CD_TRACK_BUSY\n");
    assert(status_has(hold, 2, " cd_state=HOLDING cd_action=SEEK_FORWARD "));
    assert(hold.command("CD_RELEASE " + token, at(3), at(3)).rfind("OK CD_RELEASE_TX", 0) == 0);
    assert(hold_tx.sent == (Bytes{0x7A, 0x72}));

    FakeTx fault_tx;
    Xs8ServiceCore fault(fault_tx, Ms(2000), true, {}, [] { return at(0); });
    fault.connected();
    const auto fault_token = hold_token(fault.command("CD_PRESS SEEK_BACKWARD", at(1), at(1)));
    fault_tx.fail_release = true;
    fault.command("CD_RELEASE " + fault_token, at(2), at(2));
    assert(fault.command("CD_STOP", at(3), at(3)) == "ERR CD_FAULT_LATCHED\n");
    assert(fault_tx.sent == (Bytes{0x6A, 0x72}));

    FakeTx volume_tx;
    Xs8ServiceCore volume(volume_tx, Ms(2000), true, {}, [] { return at(0); });
    volume.connected();
    volume.command("VOLUME_PRESS UP", at(1), at(1));
    assert(volume.command("CD_PLAY_PAUSE", at(2), at(2)) == "ERR HOLDING_UP\n");
    assert(volume.state() == VolumeControl::State::HOLDING_UP);
    assert(volume_tx.sent == (Bytes{0x17, 0xF9}));

    FakeTx context_tx;
    Xs8ServiceCore context(context_tx, Ms(2000), true, {}, [] { return at(0); });
    context.connected(1);
    context.set_research_context_enabled(true);
    context.command("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED", at(1), at(1), 1);
    assert(context.command("CD_PLAY_PAUSE", at(2), at(2), 1) ==
           "ERR RESEARCH_CONTEXT_ACTIVE\n");
    assert(context_tx.sent == (Bytes{0x17}));

    FakeTx monitor_tx;
    Xs8ServiceCore monitor(monitor_tx, Ms(2000), true, {}, [] { return at(0); });
    monitor.connected();
    monitor_tx.healthy = false;
    assert(monitor.command("CD_PLAY", at(1), at(1)) == "ERR MONITOR_FAILED\n");
    assert(monitor_tx.sent.empty());

    FakeTx shut_tx;
    Xs8ServiceCore shut(shut_tx, Ms(2000), true, {}, [] { return at(0); });
    shut.connected();
    assert(shut.shutdown(at(1)));
    assert(shut.command("CD_STOP", at(2), at(2)) == "ERR SHUTDOWN\n");
    assert(shut_tx.sent.empty());
}

static void test_cd_key_holds()
{
    using Bytes = std::vector<uint8_t>;
    const std::string ok_up = " gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED\n";
    // Exact frames; keepalives never retransmit; owner KEY_UP sends 0x72.
    for (const auto& trial : {std::pair<const char*, uint8_t>{"SEEK_FORWARD", 0x7A},
                              {"SEEK_BACKWARD", 0x6A}}) {
        const std::string action = trial.first;
        FakeTx tx;
        Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
        core.connected(1);
        assert(core.command("KEY_DOWN " + action, at(10), at(10), 1) ==
               "OK KEY_DOWN_TX action=" + action + ok_up);
        for (int time = 400; time <= 3600; time += 400) {
            core.tick(at(time));
            assert(core.command("KEY_DOWN " + action, at(time), at(time), 1) ==
                   "OK KEY_ALIVE action=" + action + "\n");
        }
        assert(tx.sent == (Bytes{trial.second}));
        assert(status_has(core, 3601, (" cd_state=HOLDING cd_action=" + action + " ").c_str()));
        assert(core.cd_release_owed());
        assert(core.command("KEY_UP " + action, at(3700), at(3700), 1) ==
               "OK KEY_UP_TX action=" + action + ok_up);
        assert(tx.sent == (Bytes{trial.second, 0x72}));
        assert(!core.cd_release_owed() && !core.cd_fault());
        assert(status_has(core, 3701, " cd_last_result=CLIENT_RELEASE "));
        // Late KEY_UP and repeated KEY_UP transmit nothing.
        assert(core.command("KEY_UP " + action, at(3800), at(3800), 1) ==
               "OK KEY_UP_NONE last=CLIENT_RELEASE\n");
        core.tick(at(20000));
        assert(tx.sent.size() == 2);
        // A normal KEY_UP leaves no re-arm block: the owner may press again.
        assert(core.command("KEY_DOWN " + action, at(20001), at(20001), 1).rfind(
               "OK KEY_DOWN_TX", 0) == 0);
    }
    for (const char *line : {"KEY_DOWN TRACK_NEXT", "KEY_DOWN seek_forward", "KEY_UP ",
                             "KEY_DOWN SEEK_FORWARD ", "KEY_UP VOLUME_UP"}) {
        FakeTx tx;
        Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
        core.connected(1);
        assert(core.command(line, at(1), at(1), 1) == "ERR KEY_UNKNOWN_ACTION\n");
        assert(tx.sent.empty());
    }

    // Non-owners cannot touch the hold; the owner's disconnect releases at once.
    FakeTx tx;
    Xs8ServiceCore core(tx, Ms(2000), true, {}, [] { return at(0); });
    core.connected(1);
    core.connected(2);
    core.command("KEY_DOWN SEEK_FORWARD", at(10), at(10), 1);
    assert(core.command("KEY_UP SEEK_FORWARD", at(20), at(20), 2) == "ERR KEY_NOT_OWNER\n");
    assert(core.command("KEY_DOWN SEEK_FORWARD", at(20), at(20), 2) == "ERR KEY_BUSY\n");
    assert(core.command("KEY_DOWN SEEK_BACKWARD", at(20), at(20), 1) == "ERR KEY_BUSY\n");
    assert(core.command("KEY_UP SEEK_BACKWARD", at(20), at(20), 1) ==
           "ERR KEY_ACTION_MISMATCH\n");
    core.disconnected(at(30), 2);
    assert(tx.sent == (Bytes{0x7A}) && core.cd_release_owed());
    core.disconnected(at(40), 1);
    assert(tx.sent == (Bytes{0x7A, 0x72}));
    assert(!core.cd_release_owed() && !core.cd_fault());
    assert(status_has(core, 41, " cd_state=IDLE "));
    assert(status_has(core, 41, " cd_last_result=CLIENT_DISCONNECT "));

    // Token and connection holds never release each other.
    core.connected(3);
    const auto token = hold_token(core.command("CD_PRESS SEEK_BACKWARD", at(50), at(50), 3));
    assert(core.command("KEY_UP SEEK_BACKWARD", at(60), at(60), 3) == "ERR KEY_NOT_OWNER\n");
    assert(core.command("KEY_DOWN SEEK_BACKWARD", at(60), at(60), 3) == "ERR KEY_BUSY\n");
    core.disconnected(at(70), 3);
    core.tick(at(1500)); // Token holds keep the 2000 ms lease, not 1200 ms.
    assert(tx.sent == (Bytes{0x7A, 0x72, 0x6A}));
    core.connected(4);
    assert(core.command("CD_RELEASE " + token, at(1600), at(1600), 4).rfind(
           "OK CD_RELEASE_TX", 0) == 0);
    const auto reply = core.command("KEY_DOWN SEEK_FORWARD", at(1700), at(1700), 4);
    assert(reply.find("hold_token") == std::string::npos);
    for (const std::string& guess : {std::string("1"), std::string("999999"), token})
        for (const char *verb : {"CD_RELEASE ", "CD_HEARTBEAT "})
            assert(core.command(verb + guess, at(1800), at(1800), 4) == "ERR CD_NOT_OWNER\n");
    assert(tx.sent == (Bytes{0x7A, 0x72, 0x6A, 0x72, 0x7A}));
    assert(core.command("KEY_UP SEEK_FORWARD", at(1900), at(1900), 4).rfind("OK KEY_UP_TX", 0) == 0);

    // Inactivity: 1200 ms after the last KEY_DOWN; a late keepalive is refused
    // (never a new press) until the owner sends KEY_UP or disconnects.
    FakeTx idle_tx;
    Xs8ServiceCore idle(idle_tx, Ms(2000), true, {}, [] { return at(0); });
    idle.connected(1);
    idle.connected(2);
    idle.command("KEY_DOWN SEEK_FORWARD", at(10), at(10), 1);
    idle.command("KEY_DOWN SEEK_FORWARD", at(1000), at(1000), 1);
    idle.tick(at(2199));
    assert(idle_tx.sent == (Bytes{0x7A}));
    assert(idle.command("KEY_DOWN SEEK_FORWARD", at(2250), at(2250), 1) ==
           "ERR KEY_EXPIRED last=INACTIVITY\n");
    assert(idle_tx.sent == (Bytes{0x7A, 0x72}));
    assert(status_has(idle, 2251, " cd_lease_expired=1 cd_last_result=INACTIVITY "));
    assert(!idle.cd_fault() && !idle.cd_release_owed());
    assert(idle.command("KEY_DOWN SEEK_BACKWARD", at(2260), at(2260), 1) ==
           "ERR KEY_EXPIRED last=INACTIVITY\n");
    // Another client is not blocked.
    assert(idle.command("KEY_DOWN SEEK_BACKWARD", at(2300), at(2300), 2).rfind(
           "OK KEY_DOWN_TX", 0) == 0);
    assert(idle.command("KEY_UP SEEK_FORWARD", at(2310), at(2310), 1) == "ERR KEY_NOT_OWNER\n");
    assert(idle.command("KEY_UP SEEK_BACKWARD", at(2320), at(2320), 2).rfind("OK KEY_UP_TX", 0) == 0);
    assert(idle.command("KEY_UP SEEK_FORWARD", at(2330), at(2330), 1) ==
           "OK KEY_UP_NONE last=CLIENT_RELEASE\n");
    assert(idle.command("KEY_DOWN SEEK_FORWARD", at(2400), at(2400), 1).rfind(
           "OK KEY_DOWN_TX", 0) == 0);
    assert(idle_tx.sent == (Bytes{0x7A, 0x72, 0x6A, 0x72, 0x7A}));
    idle.tick(at(3600));
    assert(idle_tx.sent.size() == 6);
    idle.disconnected(at(3700), 1); // Disconnect also clears the block.
    idle.connected(1);
    assert(idle.command("KEY_DOWN SEEK_FORWARD", at(3800), at(3800), 1).rfind(
           "OK KEY_DOWN_TX", 0) == 0);

    // MAX_HOLD: 10 s from the press, regardless of keepalives.
    FakeTx max_tx;
    Xs8ServiceCore longest(max_tx, Ms(2000), true, {}, [] { return at(0); });
    longest.connected(1);
    longest.command("KEY_DOWN SEEK_BACKWARD", at(10), at(10), 1);
    for (int time = 500; time <= 10000; time += 500)
        assert(longest.command("KEY_DOWN SEEK_BACKWARD", at(time), at(time), 1) ==
               "OK KEY_ALIVE action=SEEK_BACKWARD\n");
    assert(max_tx.sent == (Bytes{0x6A}));
    assert(longest.command("KEY_DOWN SEEK_BACKWARD", at(10010), at(10010), 1) ==
           "ERR KEY_EXPIRED last=MAX_HOLD\n");
    assert(max_tx.sent == (Bytes{0x6A, 0x72}));
    assert(status_has(longest, 10011, " cd_lease_expired=0 cd_last_result=MAX_HOLD "));
    assert(longest.command("KEY_UP SEEK_BACKWARD", at(10020), at(10020), 1) ==
           "OK KEY_UP_NONE last=MAX_HOLD\n");
    longest.tick(at(30000));
    assert(max_tx.sent.size() == 2);

    // Uncertain press, KEY_UP, disconnect and inactivity releases latch faults
    // and keep the obligation; nothing is retried before shutdown.
    FakeTx press_tx;
    Xs8ServiceCore press(press_tx, Ms(2000), true, {}, [] { return at(0); });
    press.connected(1);
    press_tx.fail_next = true;
    assert(press.command("KEY_DOWN SEEK_FORWARD", at(1), at(1), 1).rfind(
           "ERR KEY_DOWN_UNCERTAIN action=SEEK_FORWARD gpio_tx=TX_OK waveform=MISSING", 0) == 0);
    assert(press_tx.sent == (Bytes{0x7A, 0x72}));
    assert(press.cd_fault() && !press.cd_release_owed());
    assert(press.command("KEY_DOWN SEEK_FORWARD", at(2), at(2), 1) == "ERR CD_FAULT_LATCHED\n");
    assert(press.command("KEY_UP SEEK_FORWARD", at(3), at(3), 1) ==
           "OK KEY_UP_NONE last=PRESS_UNCERTAIN\n");
    assert(press_tx.sent.size() == 2);
    for (int path = 0; path < 3; ++path) {
        FakeTx fail_tx;
        Xs8ServiceCore fail(fail_tx, Ms(2000), true, {}, [] { return at(0); });
        fail.connected(1);
        fail.command("KEY_DOWN SEEK_FORWARD", at(10), at(10), 1);
        fail_tx.fail_release = true;
        if (path == 0)
            assert(fail.command("KEY_UP SEEK_FORWARD", at(20), at(20), 1) ==
                   "ERR KEY_UP_UNCERTAIN action=SEEK_FORWARD gpio_tx=TX_OK waveform=MISSING"
                   " receiver_execution=UNVERIFIED\n");
        else if (path == 1) fail.disconnected(at(20), 1);
        else fail.tick(at(1300));
        assert(fail_tx.sent == (Bytes{0x7A, 0x72}));
        assert(fail.cd_fault() && fail.cd_release_owed());
        assert(status_has(fail, 1400, " cd_state=FAULT_LATCHED "));
        assert(status_has(fail, 1400, " cd_last_result=RELEASE_UNCERTAIN "));
        fail.connected(2);
        assert(fail.command("KEY_DOWN SEEK_BACKWARD", at(1400), at(1400), 2) ==
               "ERR CD_FAULT_LATCHED\n");
        assert(fail.command("KEY_UP SEEK_FORWARD", at(1400), at(1400), 2) ==
               "OK KEY_UP_NONE last=RELEASE_UNCERTAIN\n");
        fail.tick(at(20000));
        assert(fail_tx.sent.size() == 2);
        assert(!fail.shutdown(at(20001)));
        assert(fail_tx.sent == (Bytes{0x7A, 0x72, 0x72}));
    }

    // Mutual exclusion with volume, CONTEXT and the other CD actions.
    FakeTx busy_tx;
    Xs8ServiceCore busy(busy_tx, Ms(2000), true, {}, [] { return at(0); });
    busy.connected(1);
    busy.connected(2);
    busy.command("VOLUME_PRESS UP", at(1), at(1), 1);
    assert(busy.command("KEY_DOWN SEEK_FORWARD", at(2), at(2), 2) == "ERR HOLDING_UP\n");
    assert(busy.state() == VolumeControl::State::HOLDING_UP);
    busy.command("VOLUME_RELEASE UP", at(3), at(3), 1);
    assert(busy.command("KEY_DOWN SEEK_FORWARD", at(4), at(4), 2).rfind("OK KEY_DOWN_TX", 0) == 0);
    for (const char *line : {"VOLUME_PRESS UP", "VOLUME_STEP DOWN", "MUTE", "CD_TRACK_NEXT",
                             "CD_PRESS SEEK_BACKWARD", "CD_STOP", "CD_PLAY_PAUSE",
                             "SELECT_SOURCE CD"})
        assert(busy.command(line, at(5), at(5), 1) == "ERR CD_TRACK_BUSY\n");
    busy.set_research_context_enabled(true);
    assert(busy.command("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED", at(6), at(6), 1) ==
           "ERR CONTEXT_SESSION reason=CD_BUSY\n");
    assert(busy.command("KEY_UP SEEK_FORWARD", at(7), at(7), 2).rfind("OK KEY_UP_TX", 0) == 0);
    assert(busy_tx.sent == (Bytes{0x17, 0xF9, 0x09, 0x07, 0x7A, 0x72}));

    FakeTx context_tx;
    Xs8ServiceCore context(context_tx, Ms(2000), true, {}, [] { return at(0); });
    context.connected(1);
    context.set_research_context_enabled(true);
    context.command("CONTEXT_SESSION_ENTER CONFIRM_ATTENDED", at(1), at(1), 1);
    assert(context.command("KEY_DOWN SEEK_FORWARD", at(2), at(2), 1) ==
           "ERR RESEARCH_CONTEXT_ACTIVE\n");
    assert(context.command("KEY_UP SEEK_FORWARD", at(2), at(2), 1) ==
           "OK KEY_UP_NONE last=NONE\n");
    assert(context_tx.sent == (Bytes{0x17}));

    FakeTx boot_tx;
    Xs8ServiceCore boot(boot_tx, Ms(2000), false, {}, [] { return at(0); });
    boot.connected(1);
    assert(boot.command("KEY_DOWN SEEK_FORWARD", at(1), at(1), 1) == "ERR BOOT_INHIBIT\n");
    assert(boot.command("KEY_DOWN SEEK_FORWARD", at(1), at(1), 9) == "ERR NO_CLIENT\n");
    assert(boot_tx.sent.empty());

    // Monitor failure and shutdown keep their existing release paths.
    FakeTx monitor_tx;
    Xs8ServiceCore monitor(monitor_tx, Ms(2000), true, {}, [] { return at(0); });
    monitor.connected(1);
    monitor_tx.healthy = false;
    assert(monitor.command("KEY_DOWN SEEK_FORWARD", at(1), at(1), 1) == "ERR MONITOR_FAILED\n");
    monitor_tx.healthy = true;
    monitor.command("KEY_DOWN SEEK_FORWARD", at(2), at(2), 1);
    monitor_tx.healthy = false;
    monitor.tick(at(3));
    assert(monitor_tx.sent == (Bytes{0x7A, 0x72}) && monitor.cd_fault());

    FakeTx shut_tx;
    Xs8ServiceCore shut(shut_tx, Ms(2000), true, {}, [] { return at(0); });
    shut.connected(1);
    shut.command("KEY_DOWN SEEK_BACKWARD", at(1), at(1), 1);
    assert(shut.shutdown(at(2)));
    assert(shut_tx.sent == (Bytes{0x6A, 0x72}));
    assert(shut.command("KEY_DOWN SEEK_BACKWARD", at(3), at(3), 1) == "ERR SHUTDOWN\n");
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
    test_cd_hold_max_hold();
    test_volume_step();
    test_cd_transport_keys();
    test_cd_key_holds();
    test_context_gates_and_authorization();
    test_context_send_exit_and_token_ownership();
    test_context_excludes_every_other_transmission();
    test_context_lease_lifetime_and_disconnect();
    test_context_uncertain_send_exits_and_latches();
    test_context_entry_busy_and_exit_failure();
    test_context_monitor_failure_and_shutdown();
    std::cout << "XS8 service core tests passed\n";
}
