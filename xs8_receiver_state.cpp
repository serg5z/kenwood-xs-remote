#include "xs8_receiver_state.h"

#include <cstdio>

namespace {
const char *source_name(uint8_t legacy, bool queried)
{
    if (queried) {
        switch (legacy) {
        case 0x11: return "TV2";
        case 0x13: return "PHONO";
        case 0x19: return "TAPE1";
        case 0x1A: return "TV1";
        case 0x1B: return "TUNER";
        case 0x1D: return "CD";
        }
    } else {
        switch (legacy) {
        case 0x71: return "TV2";
        case 0x73: return "PHONO";
        case 0x79: return "TAPE1";
        case 0x7A: return "TV1";
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
    if (start_ns && end_ns >= start_ns && end_ns > ignore_through_ns_) {
        ignore_through_ns_ = end_ns;
        last_tx_end_ns_ = end_ns;
    }
}

void Xs8ReceiverState::suppress_until(uint64_t time_ns)
{
    if (time_ns > ignore_through_ns_) ignore_through_ns_ = time_ns;
}

void Xs8ReceiverState::update_source(uint8_t legacy, uint64_t time_ns,
                                     const char *evidence, bool queried)
{
    const char *name = source_name(legacy, queried);
    if (name && time_ns >= source_.time_ns)
        source_ = {name, time_ns, evidence};
}

void Xs8ReceiverState::update_monitor(uint8_t legacy, uint64_t time_ns,
                                      const char *evidence)
{
    const char *name = monitor_name(legacy);
    if (name && time_ns >= monitor_.time_ns)
        monitor_ = {name, time_ns, evidence};
}

void Xs8ReceiverState::observe(const xs8_monitor_record& record,
                               bool query_pending)
{
    if (record.decoded.kind == XS8_EVENT_GAP) {
        integrity_ = false;
        return;
    }
    if (!integrity_ || query_pending || !record.decoded.valid ||
        record.decoded.time_ns <= ignore_through_ns_)
        return;
    // A recent local command may cause observed traffic, but the observation
    // itself, rather than TX completion, supplies the state evidence.
    const char *evidence = last_tx_end_ns_ &&
        record.decoded.time_ns - last_tx_end_ns_ <= 500000000ULL
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

void Xs8ReceiverState::history_overflow() { integrity_ = false; }
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
