#include "xs8_receiver_state.h"

#include <cstdio>

namespace {
const char *source_name(uint8_t legacy, bool queried)
{
    if (queried) {
        switch (legacy) {
        case 0x11: return "VIDEO2";
        case 0x13: return "PHONO";
        case 0x19: return "TAPE1";
        case 0x1A: return "VIDEO1";
        case 0x1B: return "TUNER";
        case 0x1D: return "CD";
        }
    } else {
        switch (legacy) {
        case 0x71: return "VIDEO2";
        case 0x73: return "PHONO";
        case 0x79: return "TAPE1";
        case 0x7A: return "VIDEO1";
        case 0x7B: return "TUNER";
        case 0x7D: return "CD";
        }
    }
    return nullptr;
}

const char *monitor_name(uint8_t legacy)
{
    if (legacy == 0x18) return "OFF";
    if (legacy == 0x14) return "ON";
    return nullptr;
}
}

void Xs8ReceiverState::note_tx(uint64_t start_ns, uint64_t end_ns)
{
    if (!start_ns || end_ns < start_ns) return;
    if (tx_count_ == kTxWindows) {
        const TxWindow& oldest = tx_windows_[0];
        if (oldest.end_ns > evicted_through_ns_) evicted_through_ns_ = oldest.end_ns;
        for (size_t i = 1; i < kTxWindows; ++i) tx_windows_[i - 1] = tx_windows_[i];
        --tx_count_;
    }
    tx_windows_[tx_count_++] = {start_ns, end_ns};
}

void Xs8ReceiverState::suppress_until(uint64_t time_ns)
{
    if (time_ns > ignore_through_ns_) ignore_through_ns_ = time_ns;
}

void Xs8ReceiverState::update_source(uint8_t legacy, uint64_t time_ns,
                                     const char *evidence, bool queried)
{
    const char *name = source_name(legacy, queried);
    if (name && time_ns >= source_.time_ns && time_ns >= source_floor_ns_)
        source_ = {name, time_ns, evidence};
}

void Xs8ReceiverState::update_monitor(uint8_t legacy, uint64_t time_ns,
                                      const char *evidence)
{
    const char *name = monitor_name(legacy);
    if (name && time_ns >= monitor_.time_ns && time_ns >= monitor_floor_ns_)
        monitor_ = {name, time_ns, evidence};
}

void Xs8ReceiverState::observe(const xs8_monitor_record& record,
                               bool query_pending)
{
    if (record.decoded.kind == XS8_EVENT_GAP || record.decoded.global_gap_warnings) {
        integrity_ = false;
        return;
    }
    const uint64_t time_ns = record.decoded.time_ns;
    if (!integrity_ || query_pending || !record.decoded.valid ||
        time_ns <= ignore_through_ns_ || time_ns <= evicted_through_ns_)
        return;
    uint64_t previous_tx_end_ns = 0;
    for (size_t i = 0; i < tx_count_; ++i) {
        const TxWindow& tx = tx_windows_[i];
        if (time_ns >= tx.start_ns && time_ns <= tx.end_ns)
            return; // This frame's own local transmission.
        if (tx.end_ns < time_ns && tx.end_ns > previous_tx_end_ns)
            previous_tx_end_ns = tx.end_ns;
    }
    // A recent local command may cause observed traffic, but the observation
    // itself, rather than TX completion, supplies the state evidence.
    const char *evidence = previous_tx_end_ns &&
        time_ns - previous_tx_end_ns <= 500000000ULL
        ? "AFTER_LOCAL_TX" : "PASSIVE";
    update_source(record.decoded.legacy_msb_short1,
                  record.decoded.time_ns, evidence, false);
    update_monitor(record.decoded.legacy_msb_short1,
                   record.decoded.time_ns, evidence);
}

void Xs8ReceiverState::apply_query(const Xs8QueryStatus& query, uint64_t now_ns)
{
    if (!monitor_operational_ || !query.successful(now_ns)) return;
    const uint8_t source_byte = query.source_record().decoded.legacy_msb_short1;
    const uint8_t monitor_byte = query.monitor_record().decoded.legacy_msb_short1;
    update_source(query.source_record().decoded.legacy_msb_short1,
                  query.source_record().decoded.time_ns, "QUERY", true);
    update_monitor(query.monitor_record().decoded.legacy_msb_short1,
                   query.monitor_record().decoded.time_ns, "QUERY");
    if (source_name(source_byte, true) && monitor_name(monitor_byte))
        integrity_ = true;
}

void Xs8ReceiverState::forget_source(uint64_t floor_ns)
{
    if (source_.time_ns) last_source_ = source_;
    source_ = {};
    if (floor_ns > source_floor_ns_) source_floor_ns_ = floor_ns;
}

void Xs8ReceiverState::forget_monitor(uint64_t floor_ns)
{
    if (monitor_.time_ns) last_monitor_ = monitor_;
    monitor_ = {};
    if (floor_ns > monitor_floor_ns_) monitor_floor_ns_ = floor_ns;
}

void Xs8ReceiverState::history_overflow() { integrity_ = false; }
void Xs8ReceiverState::capture_warning() { integrity_ = false; }
void Xs8ReceiverState::monitor_failed()
{
    integrity_ = false;
    monitor_operational_ = false;
}

std::string Xs8ReceiverState::status_fields() const
{
    char result[320];
    std::snprintf(result, sizeof(result),
        " receiver_source=%s receiver_source_ns=%llu receiver_source_evidence=%s"
        " receiver_monitor=%s receiver_monitor_ns=%llu receiver_monitor_evidence=%s"
        " receiver_integrity=%s",
        source_.value, (unsigned long long)source_.time_ns, source_.evidence,
        monitor_.value, (unsigned long long)monitor_.time_ns, monitor_.evidence,
        integrity_ ? "HEALTHY" : "UNCERTAIN");
    return result;
}

std::string Xs8ReceiverState::last_known_fields() const
{
    char result[200];
    std::snprintf(result, sizeof(result),
        "last_source=%s last_source_ns=%llu last_source_evidence=%s "
        "last_monitor=%s last_monitor_ns=%llu last_monitor_evidence=%s",
        last_source_.value, (unsigned long long)last_source_.time_ns,
        last_source_.evidence, last_monitor_.value,
        (unsigned long long)last_monitor_.time_ns, last_monitor_.evidence);
    return result;
}
