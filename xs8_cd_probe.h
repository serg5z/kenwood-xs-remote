#ifndef XS8_CD_PROBE_H
#define XS8_CD_PROBE_H

#include "xs8_monitor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// Experimental CD changer presence probe: one ROOT 0xC3, then a bounded window.
// Pure observation classifier; the caller owns TX, GPIO, monitor and the clock.
// A valid 0x94 1-20 ms after TX completion, alone in a quiet window, is RESPONSE.
// A clean silent window is NO_RESPONSE, which never means the changer is off.
class Xs8CdProbe {
public:
    static constexpr uint8_t kRequest = 0xC3;
    static constexpr uint8_t kResponse = 0x94;
    // Provisional; from ~3-5 ms latencies in attended trials.
    static constexpr uint64_t kResponseMinNs = 1000000ULL;
    static constexpr uint64_t kResponseMaxNs = 20000000ULL;
    // Observed after the response window: a second or late frame is AMBIGUOUS.
    static constexpr uint64_t kQuietNs = 50000000ULL;
    // Preconditions: no local TX and no bus frame this recently, so a delayed
    // report from an earlier operation (0x94 ~320 ms after STOP) cannot land in
    // the window.
    static constexpr uint64_t kTxGuardNs = 1000000000ULL;
    static constexpr uint64_t kBusQuietNs = 500000000ULL;

    enum class Result { RESPONSE, NO_RESPONSE, AMBIGUOUS, FAILED };

    Xs8CdProbe(uint64_t tx_start_ns, uint64_t tx_end_ns);
    void observe(const xs8_monitor_record *records, size_t count);
    void overflow();
    void monitor_failed();
    void capture_warning();
    void cancel();
    bool done(uint64_t now_ns) const;
    Result result(uint64_t now_ns) const;
    uint64_t deadline_ns() const { return deadline_ns_; }
    uint64_t response_ns() const { return response_ns_; }
    std::string response(uint64_t now_ns) const;
private:
    enum class Fault {
        NONE, TX_MISSING, TX_INVALID, TX_MISMATCH, OVERLAP, INVALID_FRAME,
        EVENT_GAP, HISTORY_OVERFLOW, MONITOR_FAILED, CANCELLED, GLOBAL_GAP_WARNING
    };
    uint64_t tx_start_ns_;
    uint64_t tx_end_ns_;
    uint64_t deadline_ns_;
    bool echo_ = false;
    uint64_t last_end_ns_ = 0;
    uint64_t response_ns_ = 0;
    size_t frames_ = 0;  // After the echo.
    std::array<xs8_monitor_record, 4> shown_{};
    bool ambiguous_ = false;
    Fault fault_ = Fault::NONE;
};

// Last probe outcome for STATUS. Never touches receiver_source. Only RESPONSE
// sets PRESENT and only NO_RESPONSE sets NOT_DETECTED; AMBIGUOUS and FAILED
// keep the previous observation. ABSENT is never produced.
class Xs8CdPresence {
public:
    void apply(Xs8CdProbe::Result result, uint64_t now_ns, uint64_t response_ns);
    std::string status_fields() const;
private:
    const char *value_ = "UNKNOWN";
    uint64_t time_ns_ = 0;
    const char *last_ = "NONE";
};

#endif
