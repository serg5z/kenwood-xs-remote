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

static void test_step() {
    using Step = VolumeControl::StepResult;
    using Bytes = std::vector<uint8_t>;
    for (const auto& trial : {std::pair<Direction, uint8_t>{Direction::UP, 0xF9},
                              {Direction::DOWN, 0xE9}}) {
        FakeTx tx;
        VolumeControl ctl(tx, Ms(1200), Ms(2000), {}, [] { return fake_now; });
        CHECK(ctl.startup(false, true));
        tx.on_send = [&](uint8_t byte) {
            State expected = byte == 0x17 ? State::ENTERING
                           : byte == trial.second ? State::START_UNCERTAIN
                           : byte == 0x09 ? State::STOP_PENDING
                                          : State::EXIT_PENDING;
            CHECK(ctl.state() == expected);
            // The obligations are recorded before each frame may reach the bus.
            CHECK(ctl.context_may_be_open());
            CHECK(ctl.stop_owed() == (byte == trial.second || byte == 0x09));
        };
        CHECK(ctl.step(trial.first, at(10)) == Step::VERIFIED);
        CHECK(tx.sent == (Bytes{0x17, trial.second, 0x09, 0x07}));
        CHECK(ctl.state() == State::IDLE && !ctl.stop_owed() &&
              !ctl.context_may_be_open() && !ctl.rearm_blocked());
        tx.on_send = {};
        // Ordinary CEC press/release after a step is unchanged.
        CHECK(ctl.press(trial.first, at(20), at(20)));
        CHECK(ctl.release(trial.first, at(30)));
        CHECK(tx.sent.size() == 8 && !ctl.rearm_blocked());
    }

    // Refusals transmit nothing and leave the existing state alone.
    FakeTx boot_tx;
    VolumeControl boot(boot_tx, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    CHECK(boot.step(Direction::UP, at(1)) == Step::REFUSED);
    CHECK(boot.state() == State::BOOT_INHIBIT && boot_tx.sent.empty());

    FakeTx hold_tx;
    VolumeControl hold(hold_tx, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    CHECK(hold.startup(false, true));
    CHECK(hold.press(Direction::DOWN, at(1), at(1)));
    CHECK(hold.step(Direction::DOWN, at(2)) == Step::REFUSED);
    CHECK(hold.step(Direction::UP, at(2)) == Step::REFUSED);
    CHECK(hold.state() == State::HOLDING_DOWN && hold.stop_owed());
    CHECK(hold_tx.sent == (Bytes{0x17, 0xE9}));
    CHECK(hold.press(Direction::DOWN, at(3), at(3)));
    CHECK(hold.release(Direction::DOWN, at(4)));
    CHECK(hold_tx.sent == (Bytes{0x17, 0xE9, 0x09, 0x07}));

    FakeTx context_tx;
    VolumeControl context(context_tx, Ms(1200), Ms(2000), {},
                          [] { return fake_now; });
    CHECK(context.startup(false, true));
    CHECK(context.research_context_enter(at(1)));
    CHECK(context.step(Direction::UP, at(2)) == Step::REFUSED);
    CHECK(context_tx.sent == (Bytes{0x17}));

    bool stopping = true;
    FakeTx stop_tx;
    VolumeControl stopped(stop_tx, Ms(1200), Ms(2000), [&] { return stopping; },
                          [] { return fake_now; });
    CHECK(stopped.startup(false, true));
    CHECK(stopped.step(Direction::UP, at(1)) == Step::REFUSED);
    CHECK(stop_tx.sent.empty());

    // Re-arm after a watchdog stop: a step neither sets nor clears it.
    FakeTx rearm_tx;
    VolumeControl rearm(rearm_tx, Ms(1200), Ms(2000), {}, [] { return fake_now; });
    CHECK(rearm.startup(false, true));
    CHECK(rearm.press(Direction::UP, at(10), at(10)));
    rearm.tick(at(1210));
    CHECK(rearm.rearm_blocked());
    CHECK(rearm.step(Direction::UP, at(1300)) == Step::VERIFIED);
    CHECK(rearm.rearm_blocked());
    CHECK(!rearm.press(Direction::UP, at(1400), at(1400)));
    CHECK(rearm.release(Direction::UP, at(1410)));
    CHECK(rearm.press(Direction::UP, at(1420), at(1420)));

    // Failure at every frame: cleanup through finish(), FAULT latched, and
    // never a second 0x17 or start frame.
    struct Failure { std::deque<bool> outcomes; Bytes sent; bool stop_owed, open; };
    const Failure failures[] = {
        {{false}, {0x17, 0x07}, false, false},                      // entry
        {{true, false}, {0x17, 0xF9, 0x09, 0x07}, false, false},    // start
        {{true, true, false}, {0x17, 0xF9, 0x09, 0x09, 0x07}, false, false},
        {{true, true, false, false, false}, {0x17, 0xF9, 0x09, 0x09, 0x09},
         true, true},                                               // STOP
        {{true, true, true, false}, {0x17, 0xF9, 0x09, 0x07, 0x07}, false, false},
        {{true, true, true, false, false, false},
         {0x17, 0xF9, 0x09, 0x07, 0x07, 0x07}, false, true},        // exit
    };
    for (const auto& failure : failures) {
        FakeTx tx;
        VolumeControl ctl(tx, Ms(1200), Ms(2000), {}, [] { return fake_now; });
        CHECK(ctl.startup(false, true));
        tx.outcomes = failure.outcomes;
        CHECK(ctl.step(Direction::UP, at(10)) == Step::UNCERTAIN);
        CHECK(tx.sent == failure.sent);
        CHECK(ctl.state() == State::FAULT_LATCHED);
        CHECK(ctl.stop_owed() == failure.stop_owed);
        CHECK(ctl.context_may_be_open() == failure.open);
        CHECK(!ctl.rearm_blocked());
        tx.outcomes.clear();
        CHECK(ctl.step(Direction::UP, at(20)) == Step::REFUSED);
        CHECK(!ctl.press(Direction::UP, at(21), at(21)));
        CHECK(!ctl.mute(at(22)));
        CHECK(tx.sent == failure.sent);
        // Shutdown still owes the unresolved cleanup, in order, and the fault
        // stays latched.
        const bool owed = failure.stop_owed || failure.open;
        CHECK(!ctl.shutdown(at(30)));
        Bytes expected = failure.sent;
        if (failure.stop_owed) expected.push_back(0x09);
        if (owed) expected.push_back(0x07);
        CHECK(tx.sent == expected);
        CHECK(ctl.state() == State::FAULT_LATCHED);
        CHECK(!ctl.stop_owed() && !ctl.context_may_be_open());
    }

    // Shutdown requested during each frame: the cleanup path still runs.
    const std::pair<uint8_t, Bytes> interruptions[] = {
        {0x17, {0x17, 0x07}},
        {0xF9, {0x17, 0xF9, 0x09, 0x07}},
        {0x09, {0x17, 0xF9, 0x09, 0x07}},
        {0x07, {0x17, 0xF9, 0x09, 0x07}},
    };
    for (const auto& interruption : interruptions) {
        bool signalled = false;
        FakeTx tx;
        VolumeControl ctl(tx, Ms(1200), Ms(2000), [&] { return signalled; },
                          [] { return fake_now; });
        CHECK(ctl.startup(false, true));
        tx.on_send = [&](uint8_t byte) {
            if (byte == interruption.first) signalled = true;
        };
        const Step result = ctl.step(Direction::UP, at(10));
        // STOP and exit are never cancelled once the start frame is sent.
        CHECK(result == (interruption.first == 0x17 || interruption.first == 0xF9
                         ? Step::CANCELLED : Step::VERIFIED));
        CHECK(tx.sent == interruption.second);
        CHECK(ctl.state() == State::IDLE && !ctl.stop_owed() &&
              !ctl.context_may_be_open());
        CHECK(ctl.shutdown(at(20)));
        CHECK(tx.sent == interruption.second);
    }
}

int main() {
    test_boot_and_normal_paths();
    test_direction_change_and_shutdown();
    test_partial_start_and_failed_stop();
    test_exit_failure_and_context_entry_failure();
    test_watchdogs_and_stale_queue();
    test_mute_failure();
    test_startup_recovery_and_interruption();
    test_step();
    std::cout << "volume state machine tests passed\n";
}
