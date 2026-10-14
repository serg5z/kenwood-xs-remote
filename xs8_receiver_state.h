#ifndef XS8_RECEIVER_STATE_H
#define XS8_RECEIVER_STATE_H

#include "xs8_monitor.h"
#include "xs8_query_status.h"

#include <cstddef>
#include <cstdint>
#include <string>

// Owner-thread only. Names are interpretations of validated KR-V6070 bytes.
class Xs8ReceiverState {
public:
    void note_tx(uint64_t start_ns, uint64_t end_ns);
    void suppress_until(uint64_t time_ns);
    void observe(const xs8_monitor_record& record, bool query_pending);
    void apply_query(const Xs8QueryStatus& query, uint64_t now_ns);
    void history_overflow();
    void monitor_failed();
    std::string status_fields() const;
private:
    struct Field {
        const char *value = "UNKNOWN";
        uint64_t time_ns = 0;
        const char *evidence = "NONE";
    };
    Field source_;
    Field monitor_;
    bool integrity_ = true;
    bool monitor_operational_ = true;
    // Local TX frames, each excluded only within its own [start, end] window, so
    // a receiver frame between the frames of one multi-frame command (for
    // example a source broadcast between 0x78 and the 0x07 exit) is still
    // observed. Frames older than an evicted window are excluded wholesale.
    struct TxWindow { uint64_t start_ns, end_ns; };
    static constexpr size_t kTxWindows = 16;
    TxWindow tx_windows_[kTxWindows] = {};
    size_t tx_count_ = 0;
    uint64_t evicted_through_ns_ = 0;
    uint64_t ignore_through_ns_ = 0; // suppress_until(): pending query windows.
    void update_source(uint8_t legacy, uint64_t time_ns, const char *evidence,
                       bool queried);
    void update_monitor(uint8_t legacy, uint64_t time_ns, const char *evidence);
};

#endif
