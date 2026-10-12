#include "xs8_research.h"

#include <cstdio>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <sstream>

bool xs8_parse_research_chain(const std::string& line,
                              std::vector<uint8_t>& bytes,
                              long& gap_ms, long& window_ms)
{
    constexpr const char *prefix = "RESEARCH_CHAIN ";
    if (line.compare(0, 15, prefix) != 0) return false;
    std::istringstream stream(line.substr(15));
    std::string count_text, gap_text, window_text, token, extra;
    auto parse_decimal = [](const std::string& value, long& output) {
        if (value.empty() || !std::all_of(value.begin(), value.end(),
            [](unsigned char c) { return std::isdigit(c); })) return false;
        char *end = nullptr;
        errno = 0;
        output = std::strtol(value.c_str(), &end, 10);
        return errno == 0 && *end == '\0';
    };
    long count = 0;
    if (!(stream >> count_text >> gap_text >> window_text) ||
        !parse_decimal(count_text, count) || count < 2 || count > 4 ||
        !parse_decimal(gap_text, gap_ms) || gap_ms < 50 || gap_ms > 1000 ||
        !parse_decimal(window_text, window_ms) ||
        window_ms < 1 || window_ms > 5000) return false;
    bytes.clear();
    for (long i = 0; i < count; ++i) {
        if (!(stream >> token) || token.size() != 4 || token[0] != '0' ||
            token[1] != 'x' ||
            !std::isxdigit(static_cast<unsigned char>(token[2])) ||
            !std::isxdigit(static_cast<unsigned char>(token[3]))) return false;
        bytes.push_back(static_cast<uint8_t>(
            std::strtoul(token.c_str() + 2, nullptr, 16)));
    }
    return !(stream >> extra);
}

Xs8ResearchSession::Xs8ResearchSession(uint64_t id, uint64_t cursor,
    uint64_t start_ns, uint64_t deadline_ns, bool sent, uint8_t byte,
    uint64_t tx_start_ns, uint64_t tx_end_ns)
    : id_(id), cursor_(cursor), start_ns_(start_ns), deadline_ns_(deadline_ns),
      sent_(sent), byte_(byte)
    {
        records_.reserve(kMaxRecords);
        if (sent && tx_start_ns && tx_end_ns >= tx_start_ns)
            tx_windows_.emplace_back(tx_start_ns, tx_end_ns);
    }

void Xs8ResearchSession::add_tx_window(uint64_t start_ns, uint64_t end_ns)
{
    if (start_ns && end_ns >= start_ns)
        tx_windows_.emplace_back(start_ns, end_ns);
}

void Xs8ResearchSession::set_deadline(uint64_t deadline_ns)
{
    deadline_ns_ = deadline_ns;
    records_.erase(std::remove_if(records_.begin(), records_.end(),
        [&](const xs8_monitor_record& record) {
            return record.decoded.time_ns > deadline_ns;
        }), records_.end());
    for (auto& record : records_) {
        if (record.end_ns > deadline_ns &&
            record.decoded.kind == XS8_FRAME) {
            record.end_ns = deadline_ns;
            record.decoded.valid = false;
            record.decoded.reason = "CAPTURE_WINDOW_ENDED";
        }
    }
}

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
        const bool tx_window = std::any_of(tx_windows_.begin(),
            tx_windows_.end(), [&](const auto& window) {
                return decoded.time_ns >= window.first &&
                       decoded.time_ns <= window.second;
            });
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
    if (chain_)
        std::snprintf(line, sizeof(line),
            "OK RESEARCH_%s id=%llu count=%zu start_ns=%llu "
            "deadline_ns=%llu kind=CHAIN overflow=%d "
            "monitor_failed=%d cancelled=%d\n",
            done_ ? "DONE" : "WAIT", (unsigned long long)id_,
            records_.size(), (unsigned long long)start_ns_,
            (unsigned long long)deadline_ns_, overflow_ ? 1 : 0,
            monitor_failed_ ? 1 : 0, cancelled_ ? 1 : 0);
    else if (sent_)
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

Xs8ResearchChain::Xs8ResearchChain(uint64_t id, std::vector<uint8_t> bytes,
    uint64_t gap_ns, uint64_t window_ns)
    : id_(id), gap_ns_(gap_ns), window_ns_(window_ns),
      bytes_(std::move(bytes)) { attempts_.reserve(bytes_.size()); }

void Xs8ResearchChain::abort(uint64_t now_ns, const char *reason,
                             Xs8ResearchSession& capture)
{
    if (!active_) return;
    active_ = false;
    abort_reason_ = reason;
    capture.set_deadline(now_ns + window_ns_);
}

void Xs8ResearchChain::tick(uint64_t now_ns, const Ready& ready,
    const Send& send, Xs8ResearchSession& capture)
{
    if (!active_ || now_ns < next_due_ns_) return;
    const std::string denied = ready();
    if (!denied.empty()) {
        abort(now_ns, "SAFETY_CHECK_FAILED", capture);
        return;
    }
    Attempt result = send(bytes_[attempts_.size()]);
    attempts_.push_back(result);
    capture.add_tx_window(result.start_ns, result.end_ns);
    const uint64_t end_ns = result.end_ns ? result.end_ns : now_ns;
    if (!result.gpio_completed || result.waveform != "MATCH") {
        abort(end_ns, "TX_UNCERTAIN", capture);
    } else if (attempts_.size() == bytes_.size()) {
        active_ = false;
        capture.set_deadline(end_ns + window_ns_);
    } else {
        next_due_ns_ = end_ns + gap_ns_;
    }
}

std::string Xs8ResearchChain::read(size_t index) const
{
    char line[320];
    if (index < attempts_.size()) {
        const Attempt& a = attempts_[index];
        std::snprintf(line, sizeof(line),
            "OK RESEARCH_TX_RECORD id=%llu index=%zu requested=0x%02X "
            "tx_start_ns=%llu tx_end_ns=%llu gpio_tx=%s waveform=%s "
            "receiver_execution=UNVERIFIED\n",
            (unsigned long long)id_, index, a.byte,
            (unsigned long long)a.start_ns, (unsigned long long)a.end_ns,
            a.gpio_completed ? "TX_OK" : "TX_FAIL", a.waveform.c_str());
    } else {
        std::snprintf(line, sizeof(line),
            "OK RESEARCH_TX_%s id=%llu count=%zu planned=%zu "
            "aborted=%d reason=%s\n", active_ ? "WAIT" : "DONE",
            (unsigned long long)id_, attempts_.size(), bytes_.size(),
            abort_reason_ != "NONE", abort_reason_.c_str());
    }
    return line;
}
