#include "cec_client_core.h"

#include <cstdio>
#include <utility>

namespace {
const char* name(CecKey key)
{
    return key == CecKey::UP ? "UP" : key == CecKey::DOWN ? "DOWN" :
           key == CecKey::MUTE ? "MUTE" : "OTHER";
}
long long mono_us(CecClock::time_point time)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        time.time_since_epoch()).count();
}
}

void BoundedCecQueue::push(CecEvent event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.size() >= kCapacity) {
        events_.clear();
        overflowed_ = true;
    }
    if (!overflowed_) events_.push_back(event);
}

std::deque<CecEvent> BoundedCecQueue::drain(bool& overflowed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    overflowed = std::exchange(overflowed_, false);
    std::deque<CecEvent> result;
    result.swap(events_);
    return result;
}

CecClientCore::CecClientCore(CecServiceTransport& transport, Ms inactivity,
                             Ms max_hold)
    : transport_(transport), inactivity_(inactivity), max_hold_(max_hold) {}

bool CecClientCore::send(const std::string& request, const char* expected)
{
    std::string reply;
    if (!transport_.command(request, reply)) return false;
    if (reply != expected) {
        std::fprintf(stderr, "mono_us=%lld CEC service request=%s reply=%s\n",
                     mono_us(CecClock::now()), request.c_str(), reply.c_str());
        return false;
    }
    return true;
}

void CecClientCore::uncertain(const char* reason, Time now, CecKey attempted)
{
    std::fprintf(stderr, "mono_us=%lld CEC service uncertain=%s; "
                         "waiting for release before another press\n",
                 mono_us(CecClock::now()), reason);
    if (holding_) {
        if (direction_ == CecKey::UP) blocked_up_ = true;
        else blocked_down_ = true;
    }
    if (attempted == CecKey::UP) blocked_up_ = true;
    else if (attempted == CecKey::DOWN) blocked_down_ = true;
    quarantined_ = blocked_up_ || blocked_down_;
    holding_ = false;
    stale_before_ = now;
    transport_.close(); // Service disconnect cleanup is the fallback.
}

void CecClientCore::release(const char* reason, Time now, bool await_release)
{
    if (!holding_) return;
    const CecKey released = direction_;
    const bool ok = send(std::string("VOLUME_RELEASE ") + name(released),
                         "OK VOLUME_RELEASE_TX");
    transport_.close();
    holding_ = false;
    if (await_release || !ok) {
        if (released == CecKey::UP) blocked_up_ = true;
        else blocked_down_ = true;
    }
    quarantined_ = blocked_up_ || blocked_down_;
    stale_before_ = now;
    std::fprintf(stderr, "mono_us=%lld CEC release=%s direction=%s result=%s\n",
                 mono_us(stale_before_), reason, name(released),
                 ok ? "SERVICE_OK" : "UNCERTAIN");
}

void CecClientCore::event(CecEvent event, Time now)
{
    const bool released = event.duration_ms > 0;
    if (!released) tick(now);
    const auto age = std::chrono::duration_cast<Ms>(now - event.received);
    std::fprintf(stderr, "mono_us=%lld CEC key=%s event=%s duration_ms=%u age_ms=%lld\n",
                 mono_us(now), name(event.key), released ? "RELEASE" : "PRESS",
                 event.duration_ms, static_cast<long long>(age.count()));
    if (event.key == CecKey::OTHER || event.received > now) return;
    if (event.key == CecKey::MUTE) {
        if (released) { mute_latched_ = false; return; }
        if (mute_latched_ || age >= inactivity_ || event.received <= stale_before_)
            return;
        mute_latched_ = true;
        if (quarantined_) return;
        if (!holding_ && !transport_.connect()) {
            uncertain("MUTE_CONNECT", now);
            return;
        }
        const bool ok = send("MUTE", "OK MUTE_TX");
        if (!ok) { uncertain("MUTE_REPLY", now); return; }
        if (holding_) {
            if (direction_ == CecKey::UP) blocked_up_ = true;
            else blocked_down_ = true;
            holding_ = false;
            quarantined_ = true;
            stale_before_ = now;
        }
        transport_.close();
        return;
    }
    if (released) {
        if (quarantined_ && event.received > stale_before_ && age < inactivity_) {
            if (event.key == CecKey::UP) blocked_up_ = false;
            else blocked_down_ = false;
            quarantined_ = blocked_up_ || blocked_down_;
            stale_before_ = now;
        }
        if (holding_ && event.key == direction_ && event.received > started_)
            release("CEC_RELEASE", now, false);
        return;
    }
    if (age >= inactivity_ || event.received <= stale_before_ || quarantined_)
        return;
    if (!holding_) {
        direction_ = event.key;
        if (!transport_.connect()) { uncertain("PRESS_CONNECT", now, event.key); return; }
    }
    const bool ok = send(std::string("VOLUME_PRESS ") + name(event.key),
                         "OK VOLUME_PRESS_TX");
    if (!ok) { uncertain("PRESS_REPLY", now, event.key); return; }
    if (!holding_ || event.key != direction_) started_ = now;
    direction_ = event.key;
    last_press_ = event.received;
    holding_ = true;
}

void CecClientCore::tick(Time now)
{
    if (!holding_) return;
    if (!transport_.alive()) { uncertain("SERVICE_DISCONNECT", now); return; }
    if (now - started_ >= max_hold_) release("MAX_HOLD", now, true);
    else if (now - last_press_ >= inactivity_) release("INACTIVITY", now, true);
}

void CecClientCore::overflow(Time now)
{
    if (holding_) release("QUEUE_OVERFLOW", now, true);
    else transport_.close();
}

void CecClientCore::shutdown()
{
    if (holding_) release("SHUTDOWN", CecClock::now(), true);
    transport_.close();
}
