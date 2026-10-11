#ifndef XS8_SERVICE_CORE_H
#define XS8_SERVICE_CORE_H

#include "volume_control.h"

#include <functional>
#include <string>

enum class WaveformEvidence { NOT_OBSERVED, MATCH, MISMATCH, INVALID, MISSING };
enum class ReceiverEvidence { UNVERIFIED };
struct ServiceTxEvidence {
    bool attempted = false;
    uint8_t byte = 0;
    bool gpio_completed = false;
    WaveformEvidence waveform = WaveformEvidence::NOT_OBSERVED;
    ReceiverEvidence receiver = ReceiverEvidence::UNVERIFIED;
};

class ServiceTransmitter : public VolumeTransmitter {
public:
    virtual bool monitor_healthy() const = 0;
    virtual ServiceTxEvidence last_evidence() const = 0;
    virtual ServiceTxEvidence last_stop_evidence() const = 0;
};

class Xs8ServiceCore {
public:
    using Clock = VolumeControl::Clock;
    using Time = VolumeControl::Time;
    Xs8ServiceCore(ServiceTransmitter& tx, VolumeControl::Ms max_hold,
                   bool operator_authorized, std::function<bool()> stopping = {},
                   std::function<Time()> now = Clock::now);
    void connected();
    std::string command(const std::string& line, Time received, Time now);
    // Empty result means one query frame was verified and RX collection may begin.
    std::string begin_query_status(Time now);
    void disconnected(Time now);
    void tick(Time now);
    bool shutdown(Time now);
    VolumeControl::State state() const { return volume_.state(); }
    bool stop_owed() const { return volume_.stop_owed(); }
private:
    std::string status() const;
    ServiceTransmitter& tx_;
    VolumeControl volume_;
    bool client_connected_ = false;
    bool monitor_fault_handled_ = false;
    VolumeControl::Direction owner_direction_ = VolumeControl::Direction::DOWN;
};

#endif
