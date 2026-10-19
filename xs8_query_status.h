#ifndef XS8_QUERY_STATUS_H
#define XS8_QUERY_STATUS_H

#include "xs8_monitor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Pure observation classifier. The caller owns TX, GPIO, and the deadline.
class Xs8QueryStatus {
public:
    static constexpr uint64_t kWindowNs = 500000000ULL;
    // QUERY_STATUS SETTLE (provisional): transmit only once there has been no
    // local TX for kSettleTxNs and no bus frame for kSettleBusNs, waiting at most
    // kSettleMaxNs. 600 ms clears the changer's 0x94 report ~320 ms after STOP.
    static constexpr uint64_t kSettleTxNs = 600000000ULL;
    static constexpr uint64_t kSettleBusNs = 200000000ULL;
    static constexpr uint64_t kSettleMaxNs = 1500000000ULL;
    Xs8QueryStatus(uint64_t tx_start_ns, uint64_t tx_end_ns);
    void observe(const xs8_monitor_record *records, size_t count);
    void overflow();
    void monitor_failed();
    void capture_warning();
    void cancel();
    bool done(uint64_t now_ns) const;
    bool successful(uint64_t now_ns) const;
    const xs8_monitor_record& source_record() const { return observed_[1]; }
    const xs8_monitor_record& monitor_record() const { return observed_[2]; }
    uint64_t deadline_ns() const { return deadline_ns_; }
    std::string response(uint64_t now_ns) const;
private:
    enum class Fault {
        NONE, TX_MISSING, NO_REPLY, PARTIAL, TX_INVALID, TX_MISMATCH, EVENT_GAP,
        HISTORY_OVERFLOW, MONITOR_FAILED, CANCELLED, OVERLAP, INCOMPLETE,
        EXTRA, OUT_OF_ORDER, GLOBAL_GAP_WARNING
    };
    uint64_t tx_start_ns_;
    uint64_t tx_end_ns_;
    uint64_t deadline_ns_;
    std::array<xs8_monitor_record, 4> observed_{};
    size_t count_ = 0;
    uint64_t gap_events_ = 0;
    uint64_t gap_time_ns_ = 0;
    const char *gap_reason_ = nullptr;
    Fault fault_ = Fault::NONE;
};

#endif
