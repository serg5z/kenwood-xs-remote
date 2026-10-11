#ifndef CEC_CLIENT_CORE_H
#define CEC_CLIENT_CORE_H

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

enum class CecKey { UP, DOWN, MUTE, OTHER };
using CecClock = std::chrono::steady_clock;

struct CecEvent {
    CecKey key;
    unsigned int duration_ms;
    CecClock::time_point received;
};

class BoundedCecQueue {
public:
    static constexpr size_t kCapacity = 32;
    void push(CecEvent event);
    std::deque<CecEvent> drain(bool& overflowed);
private:
    std::mutex mutex_;
    std::deque<CecEvent> events_;
    bool overflowed_ = false;
};

class CecServiceTransport {
public:
    virtual ~CecServiceTransport() = default;
    virtual bool connect() = 0;
    virtual bool command(const std::string& request, std::string& reply) = 0;
    virtual bool alive() = 0;
    virtual void close() = 0;
};

class CecClientCore {
public:
    using Time = CecClock::time_point;
    using Ms = std::chrono::milliseconds;
    explicit CecClientCore(CecServiceTransport& transport,
        Ms inactivity = Ms(1200), Ms max_hold = Ms(2000));
    void event(CecEvent event, Time now);
    void tick(Time now);
    void overflow(Time now);
    void shutdown();
    bool holding() const { return holding_; }
    bool quarantined() const { return quarantined_; }
private:
    bool send(const std::string& request, const char* expected);
    void uncertain(const char* reason, Time now,
                   CecKey attempted = CecKey::OTHER);
    void release(const char* reason, Time now, bool await_release);
    CecServiceTransport& transport_;
    Ms inactivity_;
    Ms max_hold_;
    bool holding_ = false;
    bool quarantined_ = false;
    bool mute_latched_ = false;
    CecKey direction_ = CecKey::OTHER;
    bool blocked_up_ = false;
    bool blocked_down_ = false;
    Time started_{};
    Time last_press_{};
    Time stale_before_{};
};

#endif
