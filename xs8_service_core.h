#ifndef XS8_SERVICE_CORE_H
#define XS8_SERVICE_CORE_H

#include "volume_control.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

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
    virtual VolumeControl::Time last_tx_end_time() const = 0;
};

class Xs8ServiceCore {
public:
    using Clock = VolumeControl::Clock;
    using Time = VolumeControl::Time;
    Xs8ServiceCore(ServiceTransmitter& tx, VolumeControl::Ms max_hold,
                   bool operator_authorized, std::function<bool()> stopping = {},
                   std::function<Time()> now = Clock::now,
                   std::function<void(Time)> wait_until = {},
                   VolumeControl::Ms cd_lease = VolumeControl::Ms(2000),
                   VolumeControl::Ms cd_max_hold = kCdMaxHold);
    void connected(uint64_t client_id = 0);
    std::string command(const std::string& line, Time received, Time now,
                        uint64_t client_id = 0);
    // Empty result means one query frame was verified and RX collection may begin.
    std::string begin_query_status(Time now);
    // Empty when a single ROOT research TX may be attempted. Caller also
    // enforces the separate default-off research authorization flag.
    std::string research_ready(Time now, bool accepted_session = false);
    std::string research_send(uint8_t byte, Time now,
                              bool accepted_session = false);
    void disconnected(Time now, uint64_t client_id = 0);
    void tick(Time now);
    bool shutdown(Time now);
    VolumeControl::State state() const { return volume_.state(); }
    bool stop_owed() const { return volume_.stop_owed(); }
    bool context_may_be_open() const { return volume_.context_may_be_open(); }
    bool cd_release_owed() const { return cd_release_owed_; }
    bool cd_fault() const { return cd_fault_; }
    void set_receiver_status(const std::string& fields) { receiver_status_ = fields; }
    // Attended CONTEXT research (default off). The caller also enforces its
    // startup flags and service-level capture/research exclusivity.
    void set_research_context_enabled(bool enabled) { context_enabled_ = enabled; }
    bool research_context_open() const { return context_open_; }
    static constexpr VolumeControl::Ms kContextLease{3000};
    static constexpr VolumeControl::Ms kContextMaxLifetime{300000};
    // Absolute limit on one explicit CD hold, measured from the successful
    // CD_PRESS request; heartbeats renew only the lease, never this limit.
    static constexpr VolumeControl::Ms kCdMaxHold{10000};
private:
    std::string context_command(const std::string& line, Time now,
                                uint64_t client_id);
    std::string context_close(Time now, const char *reason, bool fault_after);
    void context_sync();
    std::string status() const;
    std::string cd_track(uint8_t press_byte, const char *action);
    std::string cd_press(uint8_t press_byte, const char *action, Time now);
    std::string cd_release(const char *reason);
    std::string cd_preflight() const;
    bool attempt_cd_release();
    ServiceTransmitter& tx_;
    VolumeControl volume_;
    std::function<void(Time)> wait_until_;
    VolumeControl::Ms cd_lease_;
    VolumeControl::Ms cd_max_hold_;
    std::unordered_set<uint64_t> clients_;
    uint64_t volume_owner_client_ = 0;
    bool volume_owner_set_ = false;
    bool monitor_fault_handled_ = false;
    bool cd_active_ = false;
    bool cd_hold_ = false;
    Time cd_hold_started_{};
    Time cd_lease_renewed_{};
    uint64_t cd_hold_token_ = 0;
    uint64_t cd_last_token_ = 0;
    bool cd_lease_expired_ = false;
    std::string cd_action_ = "NONE";
    bool cd_release_owed_ = false;
    bool cd_fault_ = false;
    bool cd_shutdown_requested_ = false;
    bool cd_shutdown_release_attempted_ = false;
    std::string cd_last_result_ = "NONE";
    ServiceTxEvidence cd_release_evidence_{};
    VolumeControl::Direction owner_direction_ = VolumeControl::Direction::DOWN;
    bool context_enabled_ = false;
    bool context_open_ = false;
    uint64_t context_token_ = 0;
    uint64_t context_last_token_ = 0;
    uint64_t context_owner_ = 0;
    Time context_started_{};
    Time context_renewed_{};
    unsigned int context_sends_ = 0;
    std::string context_last_ = "NONE";
    std::string receiver_status_ =
        " receiver_source=UNKNOWN receiver_source_ns=0 receiver_source_evidence=NONE"
        " receiver_monitor=UNKNOWN receiver_monitor_ns=0 receiver_monitor_evidence=NONE"
        " receiver_integrity=HEALTHY";
};

#endif
