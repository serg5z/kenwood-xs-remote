#ifndef XS8_RESEARCH_H
#define XS8_RESEARCH_H

#include "xs8_monitor.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

bool xs8_parse_research_chain(const std::string& line,
                              std::vector<uint8_t>& bytes,
                              long& gap_ms, long& window_ms);

// Bounded, passive record of one research window. GPIO and polling stay in
// xs8_service.cpp; this class never transmits or attributes an RX origin.
class Xs8ResearchSession {
public:
    static constexpr size_t kMaxRecords = 256;
    Xs8ResearchSession(uint64_t id, uint64_t cursor, uint64_t start_ns,
                       uint64_t deadline_ns, bool sent, uint8_t byte = 0,
                       uint64_t tx_start_ns = 0, uint64_t tx_end_ns = 0);
    uint64_t id() const { return id_; }
    uint64_t cursor() const { return cursor_; }
    uint64_t *cursor_ptr() { return &cursor_; }
    uint64_t deadline_ns() const { return deadline_ns_; }
    bool active() const { return !done_; }
    bool integrity_ok() const { return !overflow_ && !monitor_failed_; }
    void observe(const xs8_monitor_record *records, size_t count);
    void overflow();
    void monitor_failed();
    void finish(uint64_t now_ns, const xs8_monitor_record *partial = nullptr);
    void cancel();
    void set_deadline(uint64_t deadline_ns);
    void add_tx_window(uint64_t start_ns, uint64_t end_ns);
    void set_chain() { chain_ = true; }
    std::string read(size_t index) const;
private:
    uint64_t id_, cursor_, start_ns_, deadline_ns_;
    bool sent_, done_ = false, overflow_ = false, monitor_failed_ = false;
    bool cancelled_ = false, chain_ = false;
    uint8_t byte_;
    std::vector<xs8_monitor_record> records_;
    std::vector<std::pair<uint64_t, uint64_t>> tx_windows_;
};

// A detached, bounded sequence. tick() is called only by the service's GPIO
// owner thread; its callbacks perform the existing safety check and one TX.
class Xs8ResearchChain {
public:
    struct Attempt {
        uint8_t byte;
        uint64_t start_ns, end_ns;
        bool gpio_completed;
        std::string waveform;
    };
    using Send = std::function<Attempt(uint8_t)>;
    using Ready = std::function<std::string()>;
    Xs8ResearchChain(uint64_t id, std::vector<uint8_t> bytes,
                     uint64_t gap_ns, uint64_t window_ns);
    void tick(uint64_t now_ns, const Ready& ready, const Send& send,
              Xs8ResearchSession& capture);
    void abort(uint64_t now_ns, const char *reason,
               Xs8ResearchSession& capture);
    bool active() const { return active_; }
    bool uncertain() const {
        return abort_reason_ != "NONE" && !attempts_.empty();
    }
    uint64_t id() const { return id_; }
    std::string read(size_t index) const;
private:
    uint64_t id_, gap_ns_, window_ns_, next_due_ns_ = 0;
    std::vector<uint8_t> bytes_;
    std::vector<Attempt> attempts_;
    bool active_ = true;
    std::string abort_reason_ = "NONE";
};

#endif
