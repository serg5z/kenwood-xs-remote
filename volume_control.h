#ifndef VOLUME_CONTROL_H
#define VOLUME_CONTROL_H

#include <chrono>
#include <cstdint>
#include <functional>

class VolumeTransmitter {
public:
    virtual ~VolumeTransmitter() = default;
    // True means the frame routine completed, not that the receiver accepted it.
    virtual bool send(uint8_t byte) = 0;
};

class VolumeControl {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    using Ms = std::chrono::milliseconds;

    enum class State {
        BOOT_INHIBIT, IDLE, ENTERING, START_UNCERTAIN,
        HOLDING_UP, HOLDING_DOWN, STOP_PENDING, EXIT_PENDING, FAULT_LATCHED
    };
    enum class Direction { UP, DOWN };

    VolumeControl(VolumeTransmitter& tx, Ms inactivity, Ms max_hold,
                  std::function<bool()> stopping = {},
                  std::function<Time()> now = Clock::now);

    // With recovery disabled, explicit operator authorization is needed to arm.
    bool startup(bool recovery_enabled, bool operator_authorized);
    bool press(Direction direction, Time received, Time now);
    bool release(Direction direction, Time now);
    bool disconnect(Direction direction, Time now);
    bool mute(Time now);
    void tick(Time now);
    bool shutdown(Time now);

    State state() const { return state_; }
    bool stop_owed() const { return stop_owed_; }
    bool context_may_be_open() const { return context_may_be_open_; }
    bool rearm_blocked() const { return rearm_blocked_; }

private:
    enum class StopReason {
        CEC_RELEASE, CLIENT_DISCONNECT, INACTIVITY, MAX_HOLD, DIRECTION_CHANGE,
        SHUTDOWN, CANCELLED, TX_ERROR, STARTUP_RECOVERY, NONE
    };
    bool start(Direction direction, Time received, Time now);
    bool finish(Time now, StopReason reason);
    bool send_retry(uint8_t byte, bool& any_failure, unsigned int& retries);
    static void log_tx_outcome(uint8_t byte, StopReason reason, bool success,
                               unsigned int retries, Time completed);
    static const char* stop_reason_name(StopReason reason);
    void fault();
    bool cancelled() const { return stopping_ && stopping_(); }

    VolumeTransmitter& tx_;
    Ms inactivity_;
    Ms max_hold_;
    std::function<bool()> stopping_;
    std::function<Time()> now_;
    State state_ = State::BOOT_INHIBIT;
    Direction direction_ = Direction::DOWN;
    Direction blocked_direction_ = Direction::DOWN;
    bool context_may_be_open_ = false;
    bool stop_owed_ = false;
    bool rearm_blocked_ = false;
    Time last_volume_event_{};
    Time hold_started_{};
    Time stale_before_ = Time::min();
};

#endif
