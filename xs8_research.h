#ifndef XS8_RESEARCH_H
#define XS8_RESEARCH_H

#include "xs8_monitor.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
    void observe(const xs8_monitor_record *records, size_t count);
    void overflow();
    void monitor_failed();
    void finish(uint64_t now_ns, const xs8_monitor_record *partial = nullptr);
    void cancel();
    std::string read(size_t index) const;
private:
    uint64_t id_, cursor_, start_ns_, deadline_ns_, tx_start_ns_, tx_end_ns_;
    bool sent_, done_ = false, overflow_ = false, monitor_failed_ = false;
    bool cancelled_ = false;
    uint8_t byte_;
    std::vector<xs8_monitor_record> records_;
};

#endif
