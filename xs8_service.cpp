#include "xs8_service_core.h"
#include "xs8_gpio.h"
#include "xs8_monitor.h"
#include "xs8_query_status.h"
#include "xs8_receiver_state.h"
#include "xs8_research.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <optional>
#include <string>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Local protocol: one client at a time, one command and response per line.
 * The socket thread is the GPIO TX owner; RX edge collection stays passive. */
namespace {
constexpr unsigned int kFrameObservationMs = 300;
constexpr size_t kMaxLine = 128;
constexpr long kMaxResearchWindowMs = 5000;
volatile std::sig_atomic_t stopping = 0;

void on_signal(int) { stopping = 1; }

class GpioServiceTransmitter final : public ServiceTransmitter {
public:
    GpioServiceTransmitter(xs8_gpio& bus, xs8_monitor& monitor,
                           Xs8ReceiverState& receiver)
        : bus_(bus), monitor_(monitor), receiver_(receiver) {}
    bool monitor_healthy() const override {
        return xs8_monitor_healthy(const_cast<xs8_monitor*>(&monitor_));
    }
    ServiceTxEvidence last_evidence() const override { return last_; }
    ServiceTxEvidence last_stop_evidence() const override { return last_stop_; }
    uint64_t last_start_ns() const { return last_start_ns_; }
    uint64_t last_end_ns() const { return last_end_ns_; }
    bool send(uint8_t byte) override {
        last_ = {};
        last_.attempted = true;
        last_.byte = byte;
        uint64_t checkpoint = xs8_monitor_checkpoint(&monitor_);
        int64_t start = 0;
        last_.gpio_completed = xs8_gpio_send(&bus_, byte, &start) == 0;
        int64_t end = xs8_gpio_now_ns();
        last_start_ns_ = start > 0 ? (uint64_t)start : 0;
        last_end_ns_ = end > 0 ? (uint64_t)end : 0;
        receiver_.note_tx(last_start_ns_, last_end_ns_);
        if (last_.gpio_completed && start > 0 && end >= start) {
            enum xs8_observation observed = xs8_monitor_verify_tx(
                &monitor_, checkpoint, byte, (uint64_t)start,
                (uint64_t)end, kFrameObservationMs);
            switch (observed) {
            case XS8_OBS_MATCH: last_.waveform = WaveformEvidence::MATCH; break;
            case XS8_OBS_MISMATCH: last_.waveform = WaveformEvidence::MISMATCH; break;
            case XS8_OBS_INVALID: last_.waveform = WaveformEvidence::INVALID; break;
            case XS8_OBS_MISSING: last_.waveform = WaveformEvidence::MISSING; break;
            case XS8_OBS_UNHEALTHY:
                last_.waveform = WaveformEvidence::NOT_OBSERVED;
                break;
            }
        }
        if (byte == 0x09) {
            last_stop_ = last_;
            std::fprintf(stderr, "XS8 STOP evidence gpio_tx=%s waveform=%s "
                                 "receiver_execution=UNVERIFIED\n",
                last_.gpio_completed ? "TX_OK" : "TX_FAIL",
                last_.waveform == WaveformEvidence::MATCH ? "MATCH" :
                last_.waveform == WaveformEvidence::MISMATCH ? "MISMATCH" :
                last_.waveform == WaveformEvidence::INVALID ? "INVALID" :
                last_.waveform == WaveformEvidence::MISSING ? "MISSING" :
                "NOT_OBSERVED");
        }
        return last_.gpio_completed &&
               last_.waveform == WaveformEvidence::MATCH;
    }
private:
    xs8_gpio& bus_;
    xs8_monitor& monitor_;
    Xs8ReceiverState& receiver_;
    ServiceTxEvidence last_{};
    ServiceTxEvidence last_stop_{};
    uint64_t last_start_ns_ = 0;
    uint64_t last_end_ns_ = 0;
};

bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool reply(int fd, const std::string& message)
{
    ssize_t sent = send(fd, message.data(), message.size(), MSG_NOSIGNAL);
    return sent == static_cast<ssize_t>(message.size());
}

bool parse_number(const char *text, long& value)
{
    char *end = nullptr;
    errno = 0;
    value = std::strtol(text, &end, 10);
    return end != text && !*end && errno != ERANGE;
}

bool parse_research_send(const std::string& line, uint8_t& byte, long& window)
{
    constexpr const char *prefix = "RESEARCH_SEND ";
    if (line.compare(0, std::strlen(prefix), prefix) != 0) return false;
    const std::string args = line.substr(std::strlen(prefix));
    if (args.size() < 6 || args[0] != '0' || args[1] != 'x' ||
        !std::isxdigit(static_cast<unsigned char>(args[2])) ||
        !std::isxdigit(static_cast<unsigned char>(args[3])) || args[4] != ' ')
        return false;
    if (!parse_number(args.c_str() + 5, window) || window < 1 ||
        window > kMaxResearchWindowMs) return false;
    byte = static_cast<uint8_t>(std::strtoul(args.substr(2, 2).c_str(),
                                             nullptr, 16));
    return true;
}

bool parse_research_window(const std::string& line, long& window)
{
    constexpr const char *prefix = "RESEARCH_OBSERVE ";
    return line.compare(0, std::strlen(prefix), prefix) == 0 &&
        parse_number(line.c_str() + std::strlen(prefix), window) &&
        window >= 1 && window <= kMaxResearchWindowMs;
}

bool parse_research_index(const std::string& line, uint64_t& id, size_t& index)
{
    constexpr const char *prefix = "RESEARCH_READ ";
    if (line.compare(0, std::strlen(prefix), prefix) != 0) return false;
    const std::string args = line.substr(std::strlen(prefix));
    const size_t separator = args.find(' ');
    if (separator == std::string::npos || args.find(' ', separator + 1) !=
                                         std::string::npos) return false;
    long parsed_id, parsed_index;
    if (!parse_number(args.substr(0, separator).c_str(), parsed_id) ||
        !parse_number(args.substr(separator + 1).c_str(), parsed_index) ||
        parsed_id < 1 || parsed_index < 0 ||
        parsed_index > static_cast<long>(Xs8ResearchSession::kMaxRecords))
        return false;
    id = static_cast<uint64_t>(parsed_id);
    index = static_cast<size_t>(parsed_index);
    return true;
}

bool parse_research_cancel(const std::string& line, uint64_t& id)
{
    constexpr const char *prefix = "RESEARCH_CANCEL ";
    long parsed_id;
    if (line.compare(0, std::strlen(prefix), prefix) != 0 ||
        !parse_number(line.c_str() + std::strlen(prefix), parsed_id) ||
        parsed_id < 1) return false;
    id = static_cast<uint64_t>(parsed_id);
    return true;
}

int open_socket(const char *path)
{
    if (std::strlen(path) >= sizeof(sockaddr_un::sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path);
    mode_t old_mask = umask(0077);
    int bound = bind(fd, reinterpret_cast<sockaddr*>(&address),
                     sizeof(address));
    umask(old_mask);
    if (bound < 0 || listen(fd, 1) < 0 || !set_nonblocking(fd)) {
        int error = errno;
        if (bound == 0) unlink(path);
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}
}

int main(int argc, char **argv)
{
    const char *socket_path = "/tmp/kenwood-xs8.sock";
    bool authorized = false;
    bool research_tx_enabled = false;
    long hold_ms = 2000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--allow-unrecovered-start") == 0)
            authorized = true;
        else if (std::strcmp(argv[i], "--enable-research-tx") == 0)
            research_tx_enabled = true;
        else if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            socket_path = argv[++i];
        else if (std::strcmp(argv[i], "--max-hold-ms") == 0 && i + 1 < argc) {
            if (!parse_number(argv[++i], hold_ms) ||
                hold_ms < 100 || hold_ms > 3000) {
                std::fprintf(stderr, "Invalid max hold (100..3000 ms)\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "Usage: %s [--socket PATH] "
                "[--allow-unrecovered-start] [--enable-research-tx] "
                "[--max-hold-ms 100..3000]\n",
                argv[0]);
            return 2;
        }
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    xs8_gpio bus{};
    if (xs8_gpio_open(&bus, true) < 0) {
        std::perror("XS8 GPIO open");
        return 1;
    }
    xs8_monitor *monitor = nullptr;
    if (xs8_monitor_start_manual(&bus, &monitor) < 0) {
        std::perror("XS8 monitor start");
        xs8_gpio_close(&bus);
        return 1;
    }
    int server = open_socket(socket_path);
    if (server < 0) {
        std::perror("XS8 socket");
        xs8_monitor_stop(monitor);
        xs8_gpio_close(&bus);
        return 1;
    }

    Xs8ReceiverState receiver;
    GpioServiceTransmitter transmitter(bus, *monitor, receiver);
    Xs8ServiceCore core(transmitter, VolumeControl::Ms(hold_ms), authorized,
                        [] { return stopping != 0; });
    std::fprintf(stderr, "XS8 service listening on %s; state=%s "
                         "research_tx=%s\n",
        socket_path, authorized ? "IDLE (operator authorized)" : "BOOT_INHIBIT",
        research_tx_enabled ? "ENABLED" : "DISABLED");
    int client = -1;
    std::string input;
    std::optional<Xs8QueryStatus> query;
    std::optional<Xs8ResearchSession> research;
    uint64_t next_research_id = 1;
    bool research_tx_uncertain = false;
    uint64_t query_cursor = 0;
    uint64_t state_cursor = xs8_monitor_checkpoint(monitor);
    while (!stopping) {
        xs8_monitor_poll(monitor, 0);
        core.tick(VolumeControl::Clock::now());
        if (research && research->active()) {
            xs8_monitor_record records[16];
            size_t copied = 0;
            do {
                if (xs8_monitor_snapshot(monitor, research->cursor_ptr(),
                                         records, 16, &copied) < 0) {
                    research->overflow();
                    break;
                }
                research->observe(records, copied);
            } while (copied == 16);
            if (!xs8_monitor_healthy(monitor)) research->monitor_failed();
            int64_t now_ns = xs8_gpio_now_ns();
            if (now_ns <= 0) {
                research->monitor_failed();
                research->finish(research->deadline_ns());
            } else if ((uint64_t)now_ns >= research->deadline_ns()) {
                xs8_monitor_record partial{};
                bool has_partial = xs8_monitor_partial(
                    monitor, research->deadline_ns(), &partial);
                if (has_partial &&
                    partial.decoded.time_ns > research->deadline_ns())
                    has_partial = false;
                research->finish((uint64_t)now_ns,
                                 has_partial ? &partial : nullptr);
            }
        }
        xs8_monitor_record state_records[16];
        size_t state_count = 0;
        do {
            if (xs8_monitor_snapshot(monitor, &state_cursor, state_records, 16,
                                     &state_count) < 0) {
                receiver.history_overflow();
                state_cursor = xs8_monitor_checkpoint(monitor);
                break;
            }
            for (size_t i = 0; i < state_count; ++i)
                receiver.observe(state_records[i], query.has_value());
        } while (state_count == 16);
        if (!xs8_monitor_healthy(monitor)) receiver.monitor_failed();
        if (query) {
            pollfd pending_client = {client, POLLIN, 0};
            if (poll(&pending_client, 1, 0) > 0 &&
                (pending_client.revents &
                 (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
                query->cancel();
                query.reset();
                core.disconnected(VolumeControl::Clock::now());
                close(client);
                client = -1;
                input.clear();
                continue;
            }
            xs8_monitor_record records[8];
            size_t copied = 0;
            do {
                if (xs8_monitor_snapshot(monitor, &query_cursor, records, 8,
                                         &copied) < 0) {
                    query->overflow();
                    break;
                }
                query->observe(records, copied);
            } while (copied == 8 && !query->done(0));
            if (!xs8_monitor_healthy(monitor)) query->monitor_failed();
            int64_t now_ns = xs8_gpio_now_ns();
            if (now_ns < 0) query->monitor_failed();
            if (now_ns > 0 && (uint64_t)now_ns >= query->deadline_ns()) {
                xs8_monitor_record partial{};
                if (xs8_monitor_partial(monitor, (uint64_t)now_ns, &partial))
                    query->observe(&partial, 1);
            }
            if (query->done(now_ns > 0 ? (uint64_t)now_ns : 0)) {
                if (now_ns > 0)
                    receiver.apply_query(*query, (uint64_t)now_ns);
                bool sent = reply(client, query->response(
                    now_ns > 0 ? (uint64_t)now_ns : 0));
                query.reset();
                if (!sent) {
                    core.disconnected(VolumeControl::Clock::now());
                    close(client);
                    client = -1;
                    input.clear();
                }
            }
        }
        pollfd fds[2] = {{server, POLLIN, 0}, {client, POLLIN, 0}};
        int count = poll(fds, client >= 0 ? 2 : 1, 20);
        if (count < 0) {
            if (errno == EINTR) continue;
            std::perror("XS8 socket poll");
            break;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            std::fprintf(stderr, "XS8 listener failed\n");
            break;
        }
        if (fds[0].revents & POLLIN) {
            int accepted = accept(server, nullptr, nullptr);
            if (accepted >= 0) {
                bool nonblocking = set_nonblocking(accepted);
                if (client >= 0 || !nonblocking) {
                    if (nonblocking) reply(accepted, "ERR BUSY\n");
                    close(accepted);
                } else {
                    client = accepted;
                    input.clear();
                    core.connected();
                }
            }
        }
        if (client < 0) continue;
        if (!(fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
            continue;
        if (fds[1].revents & POLLNVAL) {
            if (query) query->cancel();
            core.disconnected(VolumeControl::Clock::now());
            close(client);
            client = -1;
            input.clear();
            continue;
        }
        char buffer[256];
        ssize_t size = read(client, buffer, sizeof(buffer));
        if (query) {
            if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            query->cancel();
            query.reset();
            core.disconnected(VolumeControl::Clock::now());
            close(client);
            client = -1;
            input.clear();
            continue;
        }
        if (size > 0) {
            input.append(buffer, static_cast<size_t>(size));
            size_t newline;
            bool failed = false;
            while ((newline = input.find('\n')) != std::string::npos) {
                if (newline > kMaxLine) { failed = true; break; }
                std::string line = input.substr(0, newline);
                input.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                auto received = VolumeControl::Clock::now();
                uint8_t research_byte = 0;
                long research_window = 0;
                uint64_t research_id = 0;
                size_t research_index = 0;
                if (parse_research_send(line, research_byte, research_window)) {
                    std::string result;
                    if (!research_tx_enabled)
                        result = "ERR RESEARCH reason=TX_DISABLED\n";
                    else if (research_tx_uncertain)
                        result = "ERR RESEARCH reason=TX_UNCERTAIN_LATCHED\n";
                    else if (research_byte == 0x17)
                        result = "ERR RESEARCH reason=CONTEXT_ENTRY_FORBIDDEN\n";
                    else if (research && research->active())
                        result = "ERR RESEARCH reason=SESSION_BUSY\n";
                    else if (stopping)
                        result = "ERR RESEARCH reason=SHUTDOWN\n";
                    else result = core.research_ready(received);
                    if (!result.empty()) {
                        if (!reply(client, result)) failed = true;
                        continue;
                    }
                    uint64_t checkpoint = xs8_monitor_checkpoint(monitor);
                    int64_t before = xs8_gpio_now_ns();
                    if (before <= 0) {
                        if (!reply(client,
                                   "ERR RESEARCH reason=CLOCK_FAILED\n"))
                            failed = true;
                        continue;
                    }
                    std::fprintf(stderr,
                        "XS8 RESEARCH_SEND requested=0x%02X window_ms=%ld "
                        "context=ROOT_ASSUMED\n", research_byte,
                        research_window);
                    result = core.research_send(research_byte, received);
                    std::fprintf(stderr, "XS8 RESEARCH_SEND outcome=%s\n",
                                 result.c_str());
                    int64_t after = xs8_gpio_now_ns();
                    if (after <= 0) after = before;
                    if (result.compare(0, 3, "ERR") == 0)
                        research_tx_uncertain = true;
                    research.emplace(next_research_id++, checkpoint,
                        (uint64_t)before,
                        (uint64_t)after +
                            (uint64_t)research_window * 1000000ULL,
                        true, research_byte, transmitter.last_start_ns(),
                        transmitter.last_end_ns());
                    char detail[180];
                    std::snprintf(detail, sizeof(detail),
                        " id=%llu tx_start_ns=%llu tx_end_ns=%llu "
                        "deadline_ns=%llu context=ROOT_ASSUMED\n",
                        (unsigned long long)research->id(),
                        (unsigned long long)transmitter.last_start_ns(),
                        (unsigned long long)transmitter.last_end_ns(),
                        (unsigned long long)research->deadline_ns());
                    if (!reply(client, result + detail)) failed = true;
                    continue;
                }
                if (parse_research_window(line, research_window)) {
                    if (research && research->active()) {
                        if (!reply(client,
                                   "ERR RESEARCH reason=SESSION_BUSY\n"))
                            failed = true;
                        continue;
                    }
                    if (!xs8_monitor_healthy(monitor)) {
                        if (!reply(client,
                                   "ERR RESEARCH reason=MONITOR_FAILED\n"))
                            failed = true;
                        continue;
                    }
                    const int64_t start = xs8_gpio_now_ns();
                    if (start <= 0) {
                        if (!reply(client,
                                   "ERR RESEARCH reason=CLOCK_FAILED\n"))
                            failed = true;
                        continue;
                    }
                    research.emplace(next_research_id++,
                        xs8_monitor_checkpoint(monitor), (uint64_t)start,
                        (uint64_t)start +
                            (uint64_t)research_window * 1000000ULL, false);
                    std::fprintf(stderr,
                        "XS8 RESEARCH_OBSERVE id=%llu window_ms=%ld\n",
                        (unsigned long long)research->id(), research_window);
                    char detail[160];
                    std::snprintf(detail, sizeof(detail),
                        "OK RESEARCH_OBSERVE id=%llu start_ns=%llu "
                        "deadline_ns=%llu\n",
                        (unsigned long long)research->id(),
                        (unsigned long long)start,
                        (unsigned long long)research->deadline_ns());
                    if (!reply(client, detail)) failed = true;
                    continue;
                }
                if (parse_research_index(line, research_id,
                                         research_index)) {
                    if (!research || research->id() != research_id) {
                        if (!reply(client,
                                   "ERR RESEARCH reason=UNKNOWN_SESSION\n"))
                            failed = true;
                    } else if (!reply(client, research->read(research_index)))
                        failed = true;
                    continue;
                }
                if (parse_research_cancel(line, research_id)) {
                    if (!research || research->id() != research_id) {
                        if (!reply(client,
                                   "ERR RESEARCH reason=UNKNOWN_SESSION\n"))
                            failed = true;
                    } else {
                        research->cancel();
                        std::fprintf(stderr, "XS8 RESEARCH_CANCEL id=%llu\n",
                            (unsigned long long)research_id);
                        char detail[80];
                        std::snprintf(detail, sizeof(detail),
                            "OK RESEARCH_CANCEL id=%llu\n",
                            (unsigned long long)research_id);
                        if (!reply(client, detail)) failed = true;
                    }
                    continue;
                }
                if (line.compare(0, 9, "RESEARCH_") == 0) {
                    if (!reply(client,
                               "ERR RESEARCH reason=INVALID_REQUEST\n"))
                        failed = true;
                    continue;
                }
                if (line == "QUERY_STATUS") {
                    if (!input.empty()) {
                        if (!reply(client, "ERR QUERY_STATUS reason=PIPELINED\n"))
                            failed = true;
                        continue;
                    }
                    query_cursor = xs8_monitor_checkpoint(monitor);
                    std::string result = core.begin_query_status(received);
                    if ((result.empty() || result.find("reason=TX_UNCERTAIN") !=
                           std::string::npos) && transmitter.last_end_ns())
                        receiver.suppress_until(transmitter.last_end_ns() +
                                                Xs8QueryStatus::kWindowNs);
                    if (result.empty()) {
                        if (!transmitter.last_start_ns() ||
                            transmitter.last_end_ns() <
                                transmitter.last_start_ns()) {
                            if (!reply(client,
                                "ERR QUERY_STATUS reason=TX_TIME_UNAVAILABLE\n"))
                                failed = true;
                            continue;
                        }
                        query.emplace(transmitter.last_start_ns(),
                                      transmitter.last_end_ns());
                        break;
                    }
                    if (!reply(client, result)) failed = true;
                    continue;
                }
                if (line == "STATUS")
                    core.set_receiver_status(receiver.status_fields());
                if (!reply(client, core.command(line, received, received))) {
                    failed = true;
                    break;
                }
                if (stopping) break;
            }
            if (input.size() > kMaxLine) failed = true;
            if (!failed) continue;
        } else if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        core.disconnected(VolumeControl::Clock::now());
        close(client);
        client = -1;
        input.clear();
    }
    if (client >= 0) {
        if (query) query->cancel();
        core.disconnected(VolumeControl::Clock::now());
        close(client);
    }
    bool cleaned = core.shutdown(VolumeControl::Clock::now());
    if (!cleaned || core.stop_owed())
        std::fprintf(stderr, "WARNING: XS8 STOP unresolved; check receiver\n");
    close(server);
    unlink(socket_path);
    xs8_monitor_stop(monitor);
    xs8_gpio_close(&bus);
    return cleaned ? 0 : 1;
}
