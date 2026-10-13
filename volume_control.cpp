#include "volume_control.h"
#include <cstdio>
#include <utility>

namespace {
constexpr unsigned int kCleanupAttempts = 3;

long long monotonic_us(VolumeControl::Time time)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        time.time_since_epoch()).count();
}
}

VolumeControl::VolumeControl(VolumeTransmitter& tx, Ms inactivity, Ms max_hold,
                             std::function<bool()> stopping,
                             std::function<Time()> now)
    : tx_(tx), inactivity_(inactivity), max_hold_(max_hold),
      stopping_(std::move(stopping)), now_(std::move(now)) {}

void VolumeControl::fault()
{
    if (state_ != State::FAULT_LATCHED)
        std::fprintf(stderr, "mono_us=%lld XS8 state=FAULT_LATCHED "
                             "stop_owed=%d context_may_be_open=%d\n",
                     monotonic_us(now_()), stop_owed_, context_may_be_open_);
    state_ = State::FAULT_LATCHED;
}

bool VolumeControl::send_retry(uint8_t byte, bool& any_failure,
                               unsigned int& retries)
{
    for (unsigned int i = 0; i < kCleanupAttempts; ++i) {
        if (tx_.send(byte)) {
            retries = i;
            return true;
        }
        any_failure = true;
    }
    retries = kCleanupAttempts - 1;
    return false;
}

void VolumeControl::log_tx_outcome(uint8_t byte, StopReason reason,
                                   bool success, unsigned int retries,
                                   Time completed)
{
    if (byte == 0x09)
        std::fprintf(stderr, "mono_us=%lld XS8 STOP reason=%s result=%s "
                             "retries=%u\n", monotonic_us(completed),
                     stop_reason_name(reason), success ? "TX_OK" : "TX_FAIL",
                     retries);
    else
        std::fprintf(stderr, "mono_us=%lld XS8 ROOT result=%s retries=%u\n",
                     monotonic_us(completed), success ? "TX_OK" : "TX_FAIL",
                     retries);
}

const char* VolumeControl::stop_reason_name(StopReason reason)
{
    switch (reason) {
    case StopReason::CEC_RELEASE: return "CEC_RELEASE";
    case StopReason::CLIENT_DISCONNECT: return "CLIENT_DISCONNECT";
    case StopReason::INACTIVITY: return "INACTIVITY";
    case StopReason::MAX_HOLD: return "MAX_HOLD";
    case StopReason::DIRECTION_CHANGE: return "DIRECTION_CHANGE";
    case StopReason::SHUTDOWN: return "SHUTDOWN";
    case StopReason::CANCELLED: return "CANCELLED";
    case StopReason::TX_ERROR: return "TX_ERROR";
    case StopReason::STARTUP_RECOVERY: return "STARTUP_RECOVERY";
    case StopReason::RESEARCH_EXIT: return "RESEARCH_EXIT";
    case StopReason::STEP: return "STEP";
    case StopReason::NONE: return "NONE";
    }
    return "UNKNOWN";
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
    unsigned int stop_retries = 0;
    const bool stop_ok = send_retry(0x09, any_failure, stop_retries);
    const Time stop_completed = now_();
    if (!stop_ok) {
        log_tx_outcome(0x09, StopReason::STARTUP_RECOVERY, false,
                       stop_retries, stop_completed);
        fault();
        return false;
    }
    stop_owed_ = false;
    state_ = State::EXIT_PENDING;
    unsigned int root_retries = 0;
    const bool root_ok = send_retry(0x07, any_failure, root_retries);
    const Time root_completed = now_();
    log_tx_outcome(0x09, StopReason::STARTUP_RECOVERY, true,
                   stop_retries, stop_completed);
    log_tx_outcome(0x07, StopReason::NONE, root_ok,
                   root_retries, root_completed);
    if (!root_ok) { fault(); return false; }
    context_may_be_open_ = false;
    if (any_failure) { fault(); return false; }
    state_ = State::IDLE;
    return true;
}

bool VolumeControl::finish(Time now, StopReason reason)
{
    bool any_failure = false;
    bool sent_stop = false;
    unsigned int stop_retries = 0;
    Time stop_completed{};
    if (stop_owed_) {
        state_ = State::STOP_PENDING;
        const bool stop_ok = send_retry(0x09, any_failure, stop_retries);
        stop_completed = now_();
        if (!stop_ok) {
            log_tx_outcome(0x09, reason, false, stop_retries, stop_completed);
            fault();
            return false;
        }
        sent_stop = true;
        stop_owed_ = false;
    }
    bool sent_root = false;
    unsigned int root_retries = 0;
    Time root_completed{};
    Time completed{};
    if (context_may_be_open_) {
        state_ = State::EXIT_PENDING;
        const bool root_ok = send_retry(0x07, any_failure, root_retries);
        last_root_attempts_ = root_retries + 1;
        root_completed = now_();
        completed = root_completed;
        if (sent_stop)
            log_tx_outcome(0x09, reason, true, stop_retries, stop_completed);
        log_tx_outcome(0x07, StopReason::NONE, root_ok,
                       root_retries, root_completed);
        if (!root_ok) { fault(); return false; }
        sent_root = true;
        context_may_be_open_ = false;
    }
    if (!sent_root) completed = now_();
    if (sent_stop && !sent_root)
        log_tx_outcome(0x09, reason, true, stop_retries, stop_completed);
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
        finish(now, StopReason::TX_ERROR);
        fault();
        return false;
    }
    if (cancelled()) {
        finish(now, StopReason::CANCELLED);
        return false;
    }

    // Set before TX: a frame can reach the receiver before a GPIO failure.
    state_ = State::START_UNCERTAIN;
    stop_owed_ = true;
    const bool started = tx_.send(direction == Direction::UP ? 0xF9 : 0xE9);
    std::fprintf(stderr, "mono_us=%lld XS8 START direction=%s result=%s\n",
                 monotonic_us(now_()), direction == Direction::UP ? "UP" : "DOWN",
                 started ? "TX_OK" : "TX_FAIL");
    if (!started) {
        finish(now, StopReason::TX_ERROR);
        fault();
        return false;
    }
    if (cancelled()) {
        finish(now, StopReason::CANCELLED);
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
        if (!finish(now, StopReason::DIRECTION_CHANGE)) return false;
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
        return finish(now, StopReason::CEC_RELEASE);
    return state_ == State::IDLE;
}

bool VolumeControl::disconnect(Direction direction, Time now)
{
    if (rearm_blocked_ && direction == blocked_direction_)
        rearm_blocked_ = false;
    if ((state_ == State::HOLDING_UP || state_ == State::HOLDING_DOWN) &&
        direction == direction_)
        return finish(now, StopReason::CLIENT_DISCONNECT);
    return state_ == State::IDLE;
}

bool VolumeControl::mute(Time now)
{
    if (state_ == State::BOOT_INHIBIT || state_ == State::FAULT_LATCHED ||
        cancelled()) return false;
    if (state_ == State::HOLDING_UP || state_ == State::HOLDING_DOWN) {
        if (!finish(now, StopReason::CANCELLED)) return false;
    }
    if (state_ != State::IDLE) return false;
    state_ = State::ENTERING;
    context_may_be_open_ = true;
    if (!tx_.send(0x17)) {
        finish(now, StopReason::TX_ERROR);
        fault();
        return false;
    }
    if (cancelled()) return finish(now, StopReason::CANCELLED);
    if (!tx_.send(0xD8)) {
        finish(now, StopReason::TX_ERROR);
        fault();
        return false;
    }
    if (!finish(now, StopReason::NONE)) return false;
    return true;
}

VolumeControl::StepResult VolumeControl::step(Direction direction, Time now)
{
    if (state_ != State::IDLE || stop_owed_ || context_may_be_open_ ||
        cancelled()) return StepResult::REFUSED;
    if (!start(direction, now, now))
        // start() already ran finish(); a failed frame or cleanup latched FAULT.
        return state_ == State::FAULT_LATCHED ? StepResult::UNCERTAIN
                                              : StepResult::CANCELLED;
    // STOP immediately: its own transmission time is the movement duration.
    return finish(now, StopReason::STEP) ? StepResult::VERIFIED
                                         : StepResult::UNCERTAIN;
}

bool VolumeControl::research_context_enter(Time now)
{
    if (state_ != State::IDLE || stop_owed_ || context_may_be_open_ ||
        cancelled()) return false;
    state_ = State::ENTERING;
    context_may_be_open_ = true; // Before TX: the frame may reach the receiver.
    last_root_attempts_ = 0;
    if (!tx_.send(0x17)) {
        finish(now, StopReason::TX_ERROR);
        fault();
        return false;
    }
    if (cancelled()) {
        finish(now, StopReason::CANCELLED);
        return false;
    }
    state_ = State::RESEARCH_CONTEXT;
    return true;
}

bool VolumeControl::research_context_send(uint8_t byte)
{
    if (state_ != State::RESEARCH_CONTEXT) return false;
    return tx_.send(byte);
}

bool VolumeControl::research_context_exit(Time now, bool fault_after)
{
    if (state_ != State::RESEARCH_CONTEXT && !context_may_be_open_) return false;
    const bool was_fault = state_ == State::FAULT_LATCHED;
    last_root_attempts_ = 0;
    const bool exited = finish(now, StopReason::RESEARCH_EXIT);
    if (fault_after || was_fault) fault();
    return exited && !fault_after && !was_fault;
}

void VolumeControl::tick(Time now)
{
    if (state_ != State::HOLDING_UP && state_ != State::HOLDING_DOWN) return;
    if (now - last_volume_event_ >= inactivity_ ||
        now - hold_started_ >= max_hold_) {
        rearm_blocked_ = true;
        blocked_direction_ = direction_;
        finish(now, now - last_volume_event_ >= inactivity_
                        ? StopReason::INACTIVITY : StopReason::MAX_HOLD);
    }
}

bool VolumeControl::shutdown(Time now)
{
    if (stop_owed_ || context_may_be_open_) {
        bool was_fault = state_ == State::FAULT_LATCHED;
        bool cleaned = finish(now, StopReason::SHUTDOWN);
        if (was_fault) fault();
        return cleaned && !was_fault;
    }
    return state_ != State::FAULT_LATCHED;
}
