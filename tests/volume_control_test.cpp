#include "volume_control.h"

#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <vector>

using State = VolumeControl::State;
using Direction = VolumeControl::Direction;
using Time = VolumeControl::Time;
using Ms = VolumeControl::Ms;

static Time fake_now{};
static Time at(int milliseconds) {
    fake_now = Time{} + Ms(milliseconds);
    return fake_now;
}

static void check(bool ok, int line) {
    if (!ok) {
        std::cerr << "test failed at line " << line << '\n';
        std::exit(1);
    }
}
#define CHECK(expression) check((expression), __LINE__)

struct FakeTx : VolumeTransmitter {
    std::vector<uint8_t> sent;
    std::deque<bool> outcomes;
    std::function<void(uint8_t)> on_send;
    bool send(uint8_t byte) override {
        sent.push_back(byte);
        if (on_send) on_send(byte);
        if (outcomes.empty()) return true;
        bool result = outcomes.front();
        outcomes.pop_front();
        return result;
    }
};

static void test_boot_and_normal_paths() {
    FakeTx tx;
    VolumeControl ctl(tx, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    CHECK(!ctl.startup(false, false));
    CHECK(ctl.state() == State::BOOT_INHIBIT);
    CHECK(!ctl.press(Direction::UP, at(0), at(0)));
    CHECK(tx.sent.empty());
    CHECK(ctl.startup(false, true));
    tx.on_send = [&](uint8_t byte) {
        State expected = byte == 0x17 ? State::ENTERING
                       : byte == 0xF9 ? State::START_UNCERTAIN
                       : byte == 0x09 ? State::STOP_PENDING
                                      : State::EXIT_PENDING;
        CHECK(ctl.state() == expected);
    };
    CHECK(ctl.press(Direction::UP, at(1), at(1)));
    CHECK(ctl.state() == State::HOLDING_UP);
    CHECK(ctl.stop_owed());
    CHECK(ctl.press(Direction::UP, at(100), at(100)));
    CHECK(tx.sent == (std::vector<uint8_t>{0x17, 0xF9}));
    CHECK(ctl.release(Direction::UP, at(120)));
    CHECK(ctl.state() == State::IDLE);
    CHECK(!ctl.stop_owed());
    CHECK(tx.sent == (std::vector<uint8_t>{0x17, 0xF9, 0x09, 0x07}));
    tx.on_send = {};
    CHECK(ctl.mute(at(130)));
    CHECK(tx.sent == (std::vector<uint8_t>{0x17, 0xF9, 0x09, 0x07,
                                           0x17, 0xD8, 0x07}));
}

static void test_direction_change_and_shutdown() {
    FakeTx tx;
    VolumeControl ctl(tx, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    CHECK(ctl.startup(false, true));
    CHECK(ctl.press(Direction::DOWN, at(10), at(10)));
    CHECK(ctl.press(Direction::UP, at(100), at(100)));
    CHECK(tx.sent == (std::vector<uint8_t>{0x17, 0xE9, 0x09, 0x07,
                                           0x17, 0xF9}));
    CHECK(ctl.shutdown(at(110)));
    CHECK(tx.sent == (std::vector<uint8_t>{0x17, 0xE9, 0x09, 0x07,
                                           0x17, 0xF9, 0x09, 0x07}));

    FakeTx tx2;
    VolumeControl ctl2(tx2, Ms(1200), Ms(2000), {},
                       [] { return fake_now; });
    CHECK(ctl2.startup(false, true));
    CHECK(ctl2.press(Direction::DOWN, at(20), at(20)));
    tx2.outcomes = {false, false, false};
    CHECK(!ctl2.shutdown(at(30)));
    CHECK(ctl2.stop_owed());
    CHECK(ctl2.state() == State::FAULT_LATCHED);
    CHECK(tx2.sent == (std::vector<uint8_t>{0x17, 0xE9,
                                            0x09, 0x09, 0x09}));
}

static void test_partial_start_and_failed_stop() {
    FakeTx tx;
    VolumeControl ctl(tx, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    CHECK(ctl.startup(false, true));
    tx.outcomes = {true, false, true, true};
    tx.on_send = [&](uint8_t byte) {
        if (byte == 0xE9) CHECK(ctl.state() == State::START_UNCERTAIN);
    };
    CHECK(!ctl.press(Direction::DOWN, at(10), at(10)));
    CHECK(ctl.state() == State::FAULT_LATCHED);
    CHECK(tx.sent == (std::vector<uint8_t>{0x17, 0xE9, 0x09, 0x07}));
    CHECK(!ctl.press(Direction::UP, at(20), at(20)));

    FakeTx tx2;
    VolumeControl ctl2(tx2, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    CHECK(ctl2.startup(false, true));
    CHECK(ctl2.press(Direction::DOWN, at(10), at(10)));
    tx2.outcomes = {false, false, false};
    CHECK(!ctl2.release(Direction::DOWN, at(20)));
    CHECK(ctl2.state() == State::FAULT_LATCHED);
    CHECK(ctl2.stop_owed());
    CHECK(tx2.sent == (std::vector<uint8_t>{0x17, 0xE9, 0x09, 0x09, 0x09}));
    CHECK(!ctl2.shutdown(at(30))); // One more cleanup attempt, still faulted.
    CHECK(!ctl2.stop_owed());
    CHECK(tx2.sent == (std::vector<uint8_t>{0x17, 0xE9, 0x09, 0x09, 0x09,
                                            0x09, 0x07}));
}

static void test_exit_failure_and_context_entry_failure() {
    FakeTx tx;
    VolumeControl ctl(tx, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    CHECK(ctl.startup(false, true));
    CHECK(ctl.press(Direction::UP, at(10), at(10)));
    tx.outcomes = {true, false, false, false};
    CHECK(!ctl.release(Direction::UP, at(20)));
    CHECK(ctl.state() == State::FAULT_LATCHED);
    CHECK(!ctl.stop_owed());
    CHECK(ctl.context_may_be_open());
    CHECK(tx.sent == (std::vector<uint8_t>{0x17, 0xF9, 0x09,
                                           0x07, 0x07, 0x07}));

    FakeTx tx2;
    VolumeControl ctl2(tx2, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    CHECK(ctl2.startup(false, true));
    tx2.outcomes = {false, true};
    CHECK(!ctl2.press(Direction::UP, at(10), at(10)));
    CHECK(ctl2.state() == State::FAULT_LATCHED);
    CHECK(tx2.sent == (std::vector<uint8_t>{0x17, 0x07}));
}

static void test_watchdogs_and_stale_queue() {
    FakeTx tx;
    VolumeControl ctl(tx, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    CHECK(ctl.startup(false, true));
    CHECK(ctl.press(Direction::DOWN, at(10), at(10)));
    ctl.tick(at(1210));
    CHECK(ctl.state() == State::IDLE);
    CHECK(ctl.rearm_blocked());
    CHECK(!ctl.press(Direction::DOWN, at(20), at(1211)));
    CHECK(!ctl.press(Direction::DOWN, at(1212), at(1212)));
    CHECK(ctl.release(Direction::DOWN, at(1220)));
    CHECK(ctl.press(Direction::DOWN, at(1221), at(1221)));

    FakeTx tx2;
    VolumeControl ctl2(tx2, Ms(1200), Ms(500), {}, [] { return fake_now; });
    CHECK(ctl2.startup(false, true));
    CHECK(ctl2.press(Direction::UP, at(1), at(1)));
    CHECK(ctl2.press(Direction::UP, at(400), at(400)));
    ctl2.tick(at(501));
    CHECK(ctl2.rearm_blocked());
    CHECK(ctl2.state() == State::IDLE);
    CHECK(tx2.sent == (std::vector<uint8_t>{0x17, 0xF9, 0x09, 0x07}));
    CHECK(!ctl2.press(Direction::UP, at(502), at(502)));
    CHECK(ctl2.release(Direction::UP, at(503)));
    CHECK(ctl2.press(Direction::UP, at(504), at(504)));

    FakeTx tx3;
    VolumeControl ctl3(tx3, Ms(1200), Ms(2000), {},
                       [] { return fake_now; });
    CHECK(ctl3.startup(false, true));
    CHECK(ctl3.press(Direction::DOWN, at(10), at(10)));
    tx3.on_send = [&](uint8_t byte) {
        if (byte == 0x09) at(200); // A press queued while STOP is in progress.
    };
    CHECK(ctl3.release(Direction::DOWN, at(100)));
    CHECK(!ctl3.press(Direction::DOWN, at(150), at(201)));
    CHECK(ctl3.press(Direction::DOWN, at(202), at(202)));
}

static void test_mute_failure() {
    FakeTx tx;
    VolumeControl ctl(tx, Ms(1200), Ms(2000), {},
                      [] { return fake_now; });
    CHECK(ctl.startup(false, true));
    tx.outcomes = {true, false, true};
    CHECK(!ctl.mute(at(10)));
    CHECK(ctl.state() == State::FAULT_LATCHED);
    CHECK(tx.sent == (std::vector<uint8_t>{0x17, 0xD8, 0x07}));
}

static void test_startup_recovery_and_interruption() {
    FakeTx tx;
    VolumeControl ctl(tx, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    CHECK(ctl.startup(true, false));
    CHECK(ctl.state() == State::IDLE);
    CHECK(tx.sent == (std::vector<uint8_t>{0x17, 0x09, 0x07}));

    FakeTx tx2;
    VolumeControl ctl2(tx2, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    tx2.outcomes = {false, true, true};
    CHECK(!ctl2.startup(true, false));
    CHECK(ctl2.state() == State::FAULT_LATCHED);
    CHECK(tx2.sent == (std::vector<uint8_t>{0x17, 0x09, 0x07}));

    FakeTx tx3;
    VolumeControl ctl3(tx3, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    tx3.outcomes = {true, false, false, false};
    CHECK(!ctl3.startup(true, false));
    CHECK(ctl3.stop_owed());
    CHECK(tx3.sent == (std::vector<uint8_t>{0x17, 0x09, 0x09, 0x09}));

    bool stop_requested = false;
    FakeTx tx4;
    VolumeControl ctl4(tx4, Ms(1200), Ms(2000),
                       [&] { return stop_requested; },
                       [] { return fake_now; });
    CHECK(ctl4.startup(false, true));
    tx4.on_send = [&](uint8_t byte) { if (byte == 0x17) stop_requested = true; };
    CHECK(!ctl4.press(Direction::UP, at(10), at(10)));
    CHECK(tx4.sent == (std::vector<uint8_t>{0x17, 0x07}));
    CHECK(!ctl4.stop_owed());

    bool stop_after_start = false;
    FakeTx tx5;
    VolumeControl ctl5(tx5, Ms(1200), Ms(2000),
                       [&] { return stop_after_start; },
                       [] { return fake_now; });
    CHECK(ctl5.startup(false, true));
    tx5.on_send = [&](uint8_t byte) {
        if (byte == 0xE9) stop_after_start = true;
    };
    CHECK(!ctl5.press(Direction::DOWN, at(20), at(20)));
    CHECK(tx5.sent == (std::vector<uint8_t>{0x17, 0xE9, 0x09, 0x07}));
    CHECK(!ctl5.stop_owed());
}

int main() {
    test_boot_and_normal_paths();
    test_direction_change_and_shutdown();
    test_partial_start_and_failed_stop();
    test_exit_failure_and_context_entry_failure();
    test_watchdogs_and_stale_queue();
    test_mute_failure();
    test_startup_recovery_and_interruption();
    std::cout << "volume state machine tests passed\n";
}
