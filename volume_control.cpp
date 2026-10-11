#include "volume_control.h"
#include <utility>

namespace {
constexpr unsigned int kCleanupAttempts = 3;
}

VolumeControl::VolumeControl(VolumeTransmitter& tx, Ms inactivity, Ms max_hold,
                             std::function<bool()> stopping,
                             std::function<Time()> now)
    : tx_(tx), inactivity_(inactivity), max_hold_(max_hold),
      stopping_(std::move(stopping)), now_(std::move(now)) {}

void VolumeControl::fault()
{
    state_ = State::FAULT_LATCHED;
}

bool VolumeControl::send_retry(uint8_t byte, bool& any_failure)
{
    for (unsigned int i = 0; i < kCleanupAttempts; ++i) {
        if (tx_.send(byte)) return true;
        any_failure = true;
    }
    return false;
}

bool VolumeControl::startup(bool recovery_enabled, bool operator_authorized)
{
    if (state_ != State::BOOT_INHIBIT) return false;
    if (!recovery_enabled) {
        if (operator_authorized) state_ = State::IDLE;
        return operator_authorized;
    }

    // Experimental recovery. No CEC event is accepted until it finishes.
    // An interrupted context-entry frame does not remove the STOP obligation.
    state_ = State::ENTERING;
    context_may_be_open_ = true;
    stop_owed_ = true;
    bool any_failure = !tx_.send(0x17);
    state_ = State::STOP_PENDING;
    if (!send_retry(0x09, any_failure)) { fault(); return false; }
    stop_owed_ = false;
    state_ = State::EXIT_PENDING;
    if (!send_retry(0x07, any_failure)) { fault(); return false; }
    context_may_be_open_ = false;
    if (any_failure) { fault(); return false; }
    state_ = State::IDLE;
    return true;
}

bool VolumeControl::finish(Time now)
{
    bool any_failure = false;
    if (stop_owed_) {
        state_ = State::STOP_PENDING;
        if (!send_retry(0x09, any_failure)) { fault(); return false; }
        stop_owed_ = false;
    }
    if (context_may_be_open_) {
        state_ = State::EXIT_PENDING;
        if (!send_retry(0x07, any_failure)) { fault(); return false; }
        context_may_be_open_ = false;
    }
    Time completed = now_();
    stale_before_ = completed > now ? completed : now;
    if (any_failure) { fault(); return false; }
    state_ = State::IDLE;
    return true;
}

bool VolumeControl::start(Direction direction, Time received, Time now)
{
    state_ = State::ENTERING;
    context_may_be_open_ = true;
    if (!tx_.send(0x17)) {
        finish(now);
        fault();
        return false;
    }
    if (cancelled()) {
        finish(now);
        return false;
    }

    // Set before TX: a frame can reach the receiver before a GPIO failure.
    state_ = State::START_UNCERTAIN;
    stop_owed_ = true;
    if (!tx_.send(direction == Direction::UP ? 0xF9 : 0xE9)) {
        finish(now);
        fault();
        return false;
    }
    if (cancelled()) {
        finish(now);
        return false;
    }
    direction_ = direction;
    last_volume_event_ = received;
    hold_started_ = now;
    state_ = direction == Direction::UP ? State::HOLDING_UP
                                        : State::HOLDING_DOWN;
    return true;
}

bool VolumeControl::press(Direction direction, Time received, Time now)
{
    tick(now);
    if (state_ == State::BOOT_INHIBIT || state_ == State::FAULT_LATCHED ||
        rearm_blocked_ || cancelled() || received <= stale_before_ ||
        received > now || now - received >= inactivity_)
        return false;
    if (state_ == State::FAULT_LATCHED || rearm_blocked_) return false;

    if (state_ == State::HOLDING_UP || state_ == State::HOLDING_DOWN) {
        if (direction == direction_) {
            if (received > last_volume_event_) last_volume_event_ = received;
            return true;
        }
        if (!finish(now)) return false;
        if (cancelled()) return false;
    }
    if (state_ != State::IDLE) return false;
    return start(direction, received, now);
}

bool VolumeControl::release(Direction direction, Time now)
{
    if (rearm_blocked_ && direction == blocked_direction_)
        rearm_blocked_ = false;
    if ((state_ == State::HOLDING_UP || state_ == State::HOLDING_DOWN) &&
        direction == direction_)
        return finish(now);
    return state_ == State::IDLE;
}

bool VolumeControl::mute(Time now)
{
    if (state_ == State::BOOT_INHIBIT || state_ == State::FAULT_LATCHED ||
        cancelled()) return false;
    if (state_ == State::HOLDING_UP || state_ == State::HOLDING_DOWN) {
        if (!finish(now)) return false;
    }
    if (state_ != State::IDLE) return false;
    state_ = State::ENTERING;
    context_may_be_open_ = true;
    if (!tx_.send(0x17)) {
        finish(now);
        fault();
        return false;
    }
    if (cancelled()) return finish(now);
    if (!tx_.send(0xD8)) {
        finish(now);
        fault();
        return false;
    }
    if (!finish(now)) return false;
    return true;
}

void VolumeControl::tick(Time now)
{
    if (state_ != State::HOLDING_UP && state_ != State::HOLDING_DOWN) return;
    if (now - last_volume_event_ >= inactivity_ ||
        now - hold_started_ >= max_hold_) {
        rearm_blocked_ = true;
        blocked_direction_ = direction_;
        finish(now);
    }
}

bool VolumeControl::shutdown(Time now)
{
    if (stop_owed_ || context_may_be_open_) {
        bool was_fault = state_ == State::FAULT_LATCHED;
        bool cleaned = finish(now);
        if (was_fault) fault();
        return cleaned && !was_fault;
    }
    return state_ != State::FAULT_LATCHED;
}
