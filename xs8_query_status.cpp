#include "xs8_query_status.h"

#include <cstdio>
#include <cstring>

namespace {
const char *source_name(uint8_t legacy)
{
    switch (legacy) {
    case 0x11: return "VIDEO2";
    case 0x13: return "PHONO";
    case 0x19: return "TAPE1";
    case 0x1A: return "VIDEO1";
    case 0x1B: return "TUNER";
    case 0x1D: return "CD";
    default: return "UNKNOWN";
    }
}

const char *monitor_name(uint8_t legacy)
{
    switch (legacy) {
    case 0x18: return "OFF";
    case 0x14: return "ON";
    default: return "UNKNOWN";
    }
}

void append_record(std::string& out, const char *name,
                   const xs8_monitor_record& record)
{
    char field[144];
    std::snprintf(field, sizeof(field),
        " %s=0x%02X/0x%02X@%llu-%llu:%s", name,
        record.decoded.waveform_lsb, record.decoded.legacy_msb_short1,
        (unsigned long long)record.decoded.time_ns,
        (unsigned long long)record.end_ns,
        record.decoded.valid ? "VALID" : "INCOMPLETE");
    out += field;
    if (!record.decoded.valid && record.decoded.reason) {
        out += ":";
        out += record.decoded.reason;
        char bits[24];
        std::snprintf(bits, sizeof(bits), ":bits=%u", record.decoded.bits);
        out += bits;
    }
}
}

Xs8QueryStatus::Xs8QueryStatus(uint64_t tx_start_ns, uint64_t tx_end_ns)
    : tx_start_ns_(tx_start_ns), tx_end_ns_(tx_end_ns),
      deadline_ns_(tx_end_ns + kWindowNs) {}

void Xs8QueryStatus::observe(const xs8_monitor_record *records, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const xs8_monitor_record& record = records[i];
        if (record.decoded.kind == XS8_EVENT_GAP) {
            gap_events_ += record.decoded.lost_events;
            gap_time_ns_ = record.decoded.time_ns;
            gap_reason_ = record.decoded.reason;
            fault_ = Fault::EVENT_GAP;
            continue;
        }
        if (fault_ != Fault::NONE) continue;
        if (count_ == 0) {
            if (record.decoded.time_ns < tx_start_ns_) {
                if (record.end_ns > tx_start_ns_) fault_ = Fault::OVERLAP;
                continue;
            }
            if (record.decoded.time_ns > tx_end_ns_) {
                fault_ = Fault::TX_MISSING;
                break;
            }
            observed_[count_++] = record;
            if (!record.decoded.valid)
                fault_ = record.decoded.reason &&
                         std::strcmp(record.decoded.reason, "EVENT_GAP") == 0
                    ? Fault::EVENT_GAP : Fault::TX_INVALID;
            else if (record.decoded.waveform_lsb != 0x13)
                fault_ = Fault::TX_MISMATCH;
            continue;
        }
        if (record.decoded.time_ns > deadline_ns_) continue;
        if (record.decoded.time_ns < observed_[count_ - 1].end_ns) {
            if (count_ < observed_.size()) observed_[count_++] = record;
            fault_ = Fault::OVERLAP;
            break;
        }
        if (count_ >= observed_.size()) {
            fault_ = Fault::EXTRA;
            break;
        }
        observed_[count_++] = record;
        if (!record.decoded.valid)
            fault_ = record.decoded.reason &&
                     std::strcmp(record.decoded.reason, "EVENT_GAP") == 0
                ? Fault::EVENT_GAP : Fault::INCOMPLETE;
        else if (count_ == observed_.size()) fault_ = Fault::EXTRA;
        else if (count_ == 3 &&
                 std::strcmp(monitor_name(
                     observed_[1].decoded.legacy_msb_short1), "UNKNOWN") != 0 &&
                 std::strcmp(source_name(
                     observed_[2].decoded.legacy_msb_short1), "UNKNOWN") != 0)
            fault_ = Fault::OUT_OF_ORDER;
    }
}

void Xs8QueryStatus::overflow() { fault_ = Fault::HISTORY_OVERFLOW; }
void Xs8QueryStatus::monitor_failed()
{
    if (fault_ == Fault::NONE) fault_ = Fault::MONITOR_FAILED;
}
void Xs8QueryStatus::cancel() { fault_ = Fault::CANCELLED; }

bool Xs8QueryStatus::done(uint64_t now_ns) const
{
    return fault_ != Fault::NONE || now_ns >= deadline_ns_;
}

bool Xs8QueryStatus::successful(uint64_t now_ns) const
{
    return now_ns >= deadline_ns_ && fault_ == Fault::NONE && count_ == 3;
}

std::string Xs8QueryStatus::response(uint64_t now_ns) const
{
    Fault fault = fault_;
    if (fault == Fault::NONE && now_ns >= deadline_ns_ && count_ < 3)
        fault = count_ ? Fault::MISSING : Fault::TX_MISSING;
    const bool pair = count_ == 3;
    const bool success = fault == Fault::NONE && pair;
    const char *reason = "NONE";
    switch (fault) {
    case Fault::NONE: reason = pair ? "NONE" : "MISSING"; break;
    case Fault::TX_MISSING: reason = "TX_MISSING"; break;
    case Fault::MISSING: reason = "MISSING"; break;
    case Fault::TX_INVALID: reason = "TX_INVALID"; break;
    case Fault::TX_MISMATCH: reason = "TX_MISMATCH"; break;
    case Fault::EVENT_GAP: reason = "EVENT_GAP"; break;
    case Fault::HISTORY_OVERFLOW: reason = "OVERFLOW"; break;
    case Fault::MONITOR_FAILED: reason = "MONITOR_FAILED"; break;
    case Fault::CANCELLED: reason = "CANCELLED"; break;
    case Fault::OVERLAP: reason = "OVERLAP"; break;
    case Fault::INCOMPLETE: reason = "INCOMPLETE"; break;
    case Fault::EXTRA: reason = "EXTRA"; break;
    case Fault::OUT_OF_ORDER: reason = "OUT_OF_ORDER"; break;
    }
    std::string out = success ? "OK QUERY_STATUS" : "ERR QUERY_STATUS";
    out += " reason=";
    out += reason;
    out += " gpio_tx=TX_OK waveform=MATCH";
    out += " bytes=lsb/legacy@start_ns-end_ns:validity";
    if (count_ > 0) append_record(out, "tx", observed_[0]);
    if (count_ > 1) append_record(out, "rx1", observed_[1]);
    if (count_ > 2) append_record(out, "rx2", observed_[2]);
    if (count_ > 3) append_record(out, "rx3", observed_[3]);
    char gap[40];
    std::snprintf(gap, sizeof(gap), " gap_events=%llu",
                  (unsigned long long)gap_events_);
    out += gap;
    if (gap_reason_) {
        char gap_time[48];
        std::snprintf(gap_time, sizeof(gap_time), " gap_ns=%llu",
                      (unsigned long long)gap_time_ns_);
        out += gap_time;
        out += " gap_reason=";
        out += gap_reason_;
    }
    out += " source_inferred=";
    out += success ? source_name(observed_[1].decoded.legacy_msb_short1)
                   : "UNKNOWN";
    out += " monitor_inferred=";
    out += success ? monitor_name(observed_[2].decoded.legacy_msb_short1)
                   : "UNKNOWN";
    out += " attribution=TEMPORAL origin=UNATTRIBUTED"
           " receiver_execution=UNVERIFIED\n";
    return out;
}
