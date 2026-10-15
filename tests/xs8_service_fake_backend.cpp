#include "xs8_gpio.h"
#include "xs8_monitor.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

/* Test-only backend: no device access, GPIO requests, or physical bus traffic. */
struct xs8_monitor {};

namespace {
// Optional simulated RX. When XS8_FAKE_RX_SCRIPT names a file, each TX reads
// it again and appends the records of the first line for that byte, timed from
// the TX start, for example:
//   0xC3 echo +3000:0x94 +40000:INVALID +50000:GAP
// A record is visible to snapshots only once its end time has passed.
std::vector<xs8_monitor_record> history;

void add_frame(uint64_t time, uint64_t end, uint8_t byte, bool valid, bool gap)
{
    xs8_monitor_record record{};
    record.serial = history.size() + 1;
    record.end_ns = end;
    record.decoded.kind = gap ? XS8_EVENT_GAP : XS8_FRAME;
    record.decoded.time_ns = time;
    record.decoded.waveform_lsb = byte;
    record.decoded.valid = valid;
    record.decoded.bits = valid ? 8 : 3;
    record.decoded.lost_events = gap ? 1 : 0;
    record.decoded.reason = gap ? "EVENT_GAP" : valid ? nullptr : "BAD_BITS";
    history.push_back(record);
}

void script_rx(uint8_t sent, uint64_t start)
{
    const char *path = std::getenv("XS8_FAKE_RX_SCRIPT");
    if (!path) return;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream words(line);
        std::string byte;
        if (!(words >> byte) || std::strtoul(byte.c_str(), nullptr, 16) != sent) continue;
        std::string item;
        while (words >> item) {
            if (item == "echo") {
                add_frame(start, start + 1000, sent, true, false);
                continue;
            }
            const size_t colon = item.find(':');
            const uint64_t time = start + std::strtoull(item.c_str() + 1, nullptr, 10) * 1000;
            const std::string what = item.substr(colon + 1);
            if (what == "GAP") add_frame(time, time, 0, false, true);
            else if (what == "INVALID") add_frame(time, time + 1000000, 0, false, false);
            else add_frame(time, time + 1000000,
                           (uint8_t)std::strtoul(what.c_str(), nullptr, 16), true, false);
        }
        return;
    }
}
}

extern "C" {
int xs8_gpio_open(xs8_gpio *, bool) { return 0; }
int xs8_gpio_release(xs8_gpio *) { return 0; }
void xs8_gpio_close(xs8_gpio *) {}
int64_t xs8_gpio_now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
int xs8_gpio_send(xs8_gpio *, uint8_t byte, int64_t *start)
{
    const int64_t now = xs8_gpio_now_ns();
    if (start) *start = now;
    std::fprintf(stderr, "FAKE_TX 0x%02X\n", byte);
    script_rx(byte, (uint64_t)now);
    return 0;
}
int xs8_monitor_start_manual(xs8_gpio *, xs8_monitor **monitor)
{
    *monitor = new xs8_monitor;
    return 0;
}
int xs8_monitor_poll(xs8_monitor *, int) { return 0; }
void xs8_monitor_stop(xs8_monitor *monitor) { delete monitor; }
bool xs8_monitor_healthy(xs8_monitor *) { return true; }
uint64_t xs8_monitor_checkpoint(xs8_monitor *) { return history.size(); }
int xs8_monitor_snapshot(xs8_monitor *, uint64_t *cursor, xs8_monitor_record *records,
                         size_t capacity, size_t *count)
{
    const uint64_t now = (uint64_t)xs8_gpio_now_ns();
    *count = 0;
    while (*count < capacity && *cursor < history.size() &&
           history[*cursor].end_ns <= now)
        records[(*count)++] = history[(*cursor)++];
    return 0;
}
bool xs8_monitor_partial(xs8_monitor *, uint64_t, xs8_monitor_record *)
{
    return false;
}
xs8_observation xs8_monitor_verify_tx(xs8_monitor *, uint64_t, uint8_t,
                                     uint64_t, uint64_t, unsigned int)
{
    return XS8_OBS_MATCH;
}
}
