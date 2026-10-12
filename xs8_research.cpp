#include "xs8_research.h"

#include <cstdio>

Xs8ResearchSession::Xs8ResearchSession(uint64_t id, uint64_t cursor,
    uint64_t start_ns, uint64_t deadline_ns, bool sent, uint8_t byte,
    uint64_t tx_start_ns, uint64_t tx_end_ns)
    : id_(id), cursor_(cursor), start_ns_(start_ns), deadline_ns_(deadline_ns),
      tx_start_ns_(tx_start_ns), tx_end_ns_(tx_end_ns), sent_(sent), byte_(byte)
    { records_.reserve(kMaxRecords); }

void Xs8ResearchSession::observe(const xs8_monitor_record *records, size_t count)
{
    if (done_) return;
    for (size_t i = 0; i < count; ++i) {
        if (records[i].decoded.time_ns > deadline_ns_) continue;
        if (records_.size() == kMaxRecords) { overflow_ = true; return; }
        xs8_monitor_record record = records[i];
        if (record.end_ns > deadline_ns_ &&
            record.decoded.kind == XS8_FRAME) {
            record.end_ns = deadline_ns_;
            record.decoded.valid = false;
            record.decoded.reason = "CAPTURE_WINDOW_ENDED";
        }
        records_.push_back(record);
    }
}

void Xs8ResearchSession::overflow() { overflow_ = true; }
void Xs8ResearchSession::monitor_failed() { monitor_failed_ = true; }

void Xs8ResearchSession::finish(uint64_t now_ns,
                                const xs8_monitor_record *partial)
{
    if (done_ || now_ns < deadline_ns_) return;
    if (partial) observe(partial, 1);
    done_ = true;
}

void Xs8ResearchSession::cancel() { cancelled_ = true; done_ = true; }

std::string Xs8ResearchSession::read(size_t index) const
{
    char line[320];
    if (index < records_.size()) {
        const auto& record = records_[index];
        const auto& decoded = record.decoded;
        const bool tx_window = sent_ && tx_start_ns_ &&
            decoded.time_ns >= tx_start_ns_ &&
            decoded.time_ns <= tx_end_ns_;
        std::snprintf(line, sizeof(line),
            "OK RESEARCH_RECORD id=%llu index=%zu kind=%s "
            "start_ns=%llu end_ns=%llu waveform_lsb=0x%02X "
            "legacy_msb_short1=0x%02X valid=%d bits=%u "
            "lost_events=%llu reason=%s relation=%s origin=UNATTRIBUTED "
            "receiver_execution=UNVERIFIED\n",
            (unsigned long long)id_, index,
            decoded.kind == XS8_EVENT_GAP ? "GAP" : "FRAME",
            (unsigned long long)decoded.time_ns,
            (unsigned long long)record.end_ns,
            decoded.waveform_lsb, decoded.legacy_msb_short1,
            decoded.valid ? 1 : 0, decoded.bits,
            (unsigned long long)decoded.lost_events,
            decoded.reason ? decoded.reason : "NONE",
            tx_window ? "TX_WINDOW" : "OTHER_TRAFFIC");
        return line;
    }
    if (sent_)
        std::snprintf(line, sizeof(line),
            "OK RESEARCH_%s id=%llu count=%zu start_ns=%llu "
            "deadline_ns=%llu kind=SEND requested=0x%02X overflow=%d "
            "monitor_failed=%d cancelled=%d\n",
            done_ ? "DONE" : "WAIT", (unsigned long long)id_,
            records_.size(), (unsigned long long)start_ns_,
            (unsigned long long)deadline_ns_, byte_, overflow_ ? 1 : 0,
            monitor_failed_ ? 1 : 0, cancelled_ ? 1 : 0);
    else
        std::snprintf(line, sizeof(line),
            "OK RESEARCH_%s id=%llu count=%zu start_ns=%llu "
            "deadline_ns=%llu kind=OBSERVE overflow=%d "
            "monitor_failed=%d cancelled=%d\n",
            done_ ? "DONE" : "WAIT", (unsigned long long)id_,
            records_.size(), (unsigned long long)start_ns_,
            (unsigned long long)deadline_ns_, overflow_ ? 1 : 0,
            monitor_failed_ ? 1 : 0, cancelled_ ? 1 : 0);
    return line;
}
