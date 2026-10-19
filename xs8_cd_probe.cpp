#include "xs8_cd_probe.h"

#include <cstdio>
#include <cstring>

namespace {
bool gap_reason(const xs8_monitor_record& record)
{
    return record.decoded.reason && std::strcmp(record.decoded.reason, "EVENT_GAP") == 0;
}

const char *result_name(Xs8CdProbe::Result result)
{
    switch (result) {
    case Xs8CdProbe::Result::RESPONSE: return "RESPONSE";
    case Xs8CdProbe::Result::NO_RESPONSE: return "NO_RESPONSE";
    case Xs8CdProbe::Result::AMBIGUOUS: return "AMBIGUOUS";
    case Xs8CdProbe::Result::FAILED: return "FAILED";
    }
    return "FAILED";
}
}

Xs8CdProbe::Xs8CdProbe(uint64_t tx_start_ns, uint64_t tx_end_ns)
    : tx_start_ns_(tx_start_ns), tx_end_ns_(tx_end_ns),
      deadline_ns_(tx_end_ns + kResponseMaxNs + kQuietNs) {}

void Xs8CdProbe::observe(const xs8_monitor_record *records, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const xs8_monitor_record& record = records[i];
        if (record.decoded.global_gap_warnings) {
            fault_ = Fault::GLOBAL_GAP_WARNING;
            continue;
        }
        if (record.decoded.kind == XS8_EVENT_GAP) {
            fault_ = Fault::EVENT_GAP;
            continue;
        }
        if (fault_ != Fault::NONE) continue;
        if (!echo_) {
            // Our own frame is the first one inside the TX window; history
            // before it is ignored unless it runs into the TX.
            if (record.decoded.time_ns < tx_start_ns_) {
                if (record.end_ns > tx_start_ns_) fault_ = Fault::OVERLAP;
                continue;
            }
            if (record.decoded.time_ns > tx_end_ns_) {
                fault_ = Fault::TX_MISSING;
                continue;
            }
            echo_ = true;
            last_end_ns_ = record.end_ns;
            if (!record.decoded.valid)
                fault_ = gap_reason(record) ? Fault::EVENT_GAP : Fault::TX_INVALID;
            else if (record.decoded.waveform_lsb != kRequest)
                fault_ = Fault::TX_MISMATCH;
            continue;
        }
        if (record.decoded.time_ns > deadline_ns_) continue;
        if (frames_ < shown_.size()) shown_[frames_] = record;
        ++frames_;
        if (record.decoded.time_ns < last_end_ns_) {
            fault_ = Fault::OVERLAP;
            continue;
        }
        last_end_ns_ = record.end_ns;
        if (!record.decoded.valid) {
            fault_ = gap_reason(record) ? Fault::EVENT_GAP : Fault::INVALID_FRAME;
            continue;
        }
        const uint64_t latency = record.decoded.time_ns >= tx_end_ns_
            ? record.decoded.time_ns - tx_end_ns_ : 0;
        if (frames_ == 1 && record.decoded.waveform_lsb == kResponse &&
            latency >= kResponseMinNs && latency <= kResponseMaxNs)
            response_ns_ = record.decoded.time_ns;
        else
            ambiguous_ = true;  // Early, late, unexpected or a second frame.
    }
}

void Xs8CdProbe::capture_warning()
{ if (fault_ == Fault::NONE) fault_ = Fault::GLOBAL_GAP_WARNING; }

void Xs8CdProbe::overflow() { fault_ = Fault::HISTORY_OVERFLOW; }
void Xs8CdProbe::monitor_failed()
{
    if (fault_ == Fault::NONE) fault_ = Fault::MONITOR_FAILED;
}
void Xs8CdProbe::cancel() { fault_ = Fault::CANCELLED; }

bool Xs8CdProbe::done(uint64_t now_ns) const
{
    return fault_ != Fault::NONE || now_ns >= deadline_ns_;
}

Xs8CdProbe::Result Xs8CdProbe::result(uint64_t now_ns) const
{
    if (fault_ != Fault::NONE || now_ns < deadline_ns_ || !echo_) return Result::FAILED;
    if (ambiguous_) return Result::AMBIGUOUS;
    return response_ns_ ? Result::RESPONSE : Result::NO_RESPONSE;
}

std::string Xs8CdProbe::response(uint64_t now_ns) const
{
    const Result outcome = result(now_ns);
    std::string rx;
    for (size_t i = 0; i < frames_ && i < shown_.size(); ++i) {
        const auto& decoded = shown_[i].decoded;
        char item[48];
        const long long offset_us = ((long long)decoded.time_ns - (long long)tx_end_ns_) / 1000;
        if (decoded.valid)
            std::snprintf(item, sizeof(item), "%s0x%02X@%+lldus", i ? "," : "",
                          decoded.waveform_lsb, offset_us);
        else
            std::snprintf(item, sizeof(item), "%sINVALID@%+lldus", i ? "," : "", offset_us);
        rx += item;
    }
    if (rx.empty()) rx = "NONE";
    if (outcome != Result::FAILED) {
        char latency[32] = "NONE";
        if (outcome == Result::RESPONSE)
            std::snprintf(latency, sizeof(latency), "%lluus",
                          (unsigned long long)((response_ns_ - tx_end_ns_) / 1000));
        char frames[24];
        std::snprintf(frames, sizeof(frames), "%zu", frames_);
        return std::string("OK CD_PROBE result=") + result_name(outcome) +
            " latency=" + latency + " frames=" + frames + " rx=" + rx +
            " gpio_tx=TX_OK waveform=MATCH receiver_execution=UNVERIFIED\n";
    }
    Fault fault = fault_;
    if (fault == Fault::NONE) fault = Fault::TX_MISSING;
    const char *reason = "TX_MISSING";
    switch (fault) {
    case Fault::NONE:
    case Fault::TX_MISSING: reason = "TX_MISSING"; break;
    case Fault::TX_INVALID: reason = "TX_INVALID"; break;
    case Fault::TX_MISMATCH: reason = "TX_MISMATCH"; break;
    case Fault::OVERLAP: reason = "OVERLAP"; break;
    case Fault::INVALID_FRAME: reason = "INVALID_FRAME"; break;
    case Fault::EVENT_GAP: reason = "EVENT_GAP"; break;
    case Fault::HISTORY_OVERFLOW: reason = "HISTORY_OVERFLOW"; break;
    case Fault::MONITOR_FAILED: reason = "MONITOR_FAILED"; break;
    case Fault::CANCELLED: reason = "CANCELLED"; break;
    case Fault::GLOBAL_GAP_WARNING: reason = "GLOBAL_GAP_WARNING"; break;
    }
    return std::string("ERR CD_PROBE reason=") + reason + " rx=" + rx +
        " receiver_execution=UNVERIFIED\n";
}

void Xs8CdPresence::apply(Xs8CdProbe::Result result, uint64_t now_ns, uint64_t response_ns)
{
    last_ = result_name(result);
    if (result == Xs8CdProbe::Result::RESPONSE) {
        value_ = "PRESENT";
        time_ns_ = response_ns;
    } else if (result == Xs8CdProbe::Result::NO_RESPONSE) {
        value_ = "NOT_DETECTED";
        time_ns_ = now_ns;
    }
}

std::string Xs8CdPresence::status_fields() const
{
    char result[160];
    std::snprintf(result, sizeof(result),
        " cd_changer=%s cd_changer_ns=%llu cd_changer_last_probe=%s",
        value_, (unsigned long long)time_ns_, last_);
    return result;
}
