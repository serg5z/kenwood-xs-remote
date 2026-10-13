#include "xs8_service_core.h"
#include "xs8_gpio.h"
#include "xs8_monitor.h"
#include "xs8_query_status.h"
#include "xs8_receiver_state.h"
#include "xs8_research.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <optional>
#include <string>
#include <sstream>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Local line protocol. One control thread owns all GPIO TX and services a
 * bounded set of socket clients; RX edge collection stays passive. */
namespace {
constexpr unsigned int kFrameObservationMs = 300;
constexpr size_t kMaxLine = 128;
constexpr size_t kMaxClients = 8;
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
    VolumeControl::Time last_tx_end_time() const override {
        return last_end_time_;
    }
    uint64_t last_start_ns() const { return last_start_ns_; }
    uint64_t last_end_ns() const { return last_end_ns_; }
    struct Frame { ServiceTxEvidence evidence; uint64_t start_ns, end_ns; };
    // Records every frame of one research CONTEXT command, including each 0x07
    // cleanup attempt, so captures label all local TX windows.
    void begin_record() { frames_.clear(); recording_ = true; }
    std::vector<Frame> end_record() { recording_ = false; return std::move(frames_); }
    bool send(uint8_t byte) override {
        const bool verified = send_frame(byte);
        if (recording_) frames_.push_back({last_, last_start_ns_, last_end_ns_});
        return verified;
    }
private:
    bool send_frame(uint8_t byte) {
        last_ = {};
        last_end_time_ = {};
        last_.attempted = true;
        last_.byte = byte;
        uint64_t checkpoint = xs8_monitor_checkpoint(&monitor_);
        int64_t start = 0;
        last_.gpio_completed = xs8_gpio_send(&bus_, byte, &start) == 0;
        last_end_time_ = VolumeControl::Clock::now();
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
    xs8_gpio& bus_;
    xs8_monitor& monitor_;
    Xs8ReceiverState& receiver_;
    std::vector<Frame> frames_;
    bool recording_ = false;
    ServiceTxEvidence last_{};
    ServiceTxEvidence last_stop_{};
    uint64_t last_start_ns_ = 0;
    uint64_t last_end_ns_ = 0;
    VolumeControl::Time last_end_time_{};
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

bool parse_research_index(const std::string& line, const char *prefix,
                          uint64_t& id, size_t& index)
{
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

bool parse_context_observe(const std::string& line, uint64_t& token,
                            long& window)
{
    std::istringstream input(line);
    std::string operation, token_text, window_text, extra;
    if (!(input >> operation >> token_text >> window_text) || (input >> extra) ||
        operation != "RESEARCH_CONTEXT_OBSERVE") return false;
    const auto parsed = std::from_chars(token_text.data(),
        token_text.data() + token_text.size(), token);
    return parsed.ec == std::errc{} &&
        parsed.ptr == token_text.data() + token_text.size() && token &&
        parse_number(window_text.c_str(), window) && window >= 1 && window <= 5000;
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
    bool research_chain_enabled = false;
    bool research_context_enabled = false;
    long hold_ms = 2000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--allow-unrecovered-start") == 0)
            authorized = true;
        else if (std::strcmp(argv[i], "--enable-research-tx") == 0)
            research_tx_enabled = true;
        else if (std::strcmp(argv[i], "--enable-research-chain") == 0)
            research_chain_enabled = true;
        else if (std::strcmp(argv[i], "--enable-research-context") == 0)
            research_context_enabled = true;
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
                "[--enable-research-chain] [--enable-research-context] "
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
    // Interactive CONTEXT research needs both default-off research flags.
    const bool context_research = research_tx_enabled && research_context_enabled;
    core.set_research_context_enabled(context_research);
    std::fprintf(stderr, "XS8 service listening on %s; state=%s "
                         "research_tx=%s research_chain=%s research_context=%s\n",
        socket_path, authorized ? "IDLE (operator authorized)" : "BOOT_INHIBIT",
        research_tx_enabled ? "ENABLED" : "DISABLED",
        research_chain_enabled ? "ENABLED" : "DISABLED",
        context_research ? "ENABLED" : "DISABLED");
    struct Client { int fd; uint64_t id; std::string input; };
    std::vector<Client> clients;
    uint64_t next_client_id = 1;
    size_t next_poll_client = 0;
    int query_client = -1;
    std::optional<Xs8QueryStatus> query;
    auto close_client = [&](size_t index) {
        if (index >= clients.size()) return;
        const int fd = clients[index].fd;
        const uint64_t id = clients[index].id;
        if (query_client == fd) {
            if (query) query->cancel();
            query.reset();
            query_client = -1;
        }
        core.disconnected(VolumeControl::Clock::now(), id);
        close(fd);
        clients.erase(clients.begin() + index);
    };
    std::optional<Xs8ResearchSession> research;
    std::optional<Xs8ResearchChain> chain;
    Xs8ContextPreparation context_preparation;
    uint64_t context_capture_id = 0;
    uint64_t next_research_id = 1;
    bool research_tx_uncertain = false;
    uint64_t query_cursor = 0;
    uint64_t state_cursor = xs8_monitor_checkpoint(monitor);
    while (!stopping) {
        xs8_monitor_poll(monitor, 0);
        const int64_t preparation_now = xs8_gpio_now_ns();
        const bool preparation_was_active = context_preparation.active();
        context_preparation.tick(preparation_now > 0 ? preparation_now : 0);
        if (!xs8_monitor_healthy(monitor)) context_preparation.monitor_failed();
        if (preparation_was_active && !context_preparation.active()) {
            if (research && research->id() == context_capture_id && research->active())
                research->cancel();
            std::fprintf(stderr, "XS8 CONTEXT_PREPARATION %s",
                         context_preparation.status().c_str());
        }
        core.tick(VolumeControl::Clock::now());
        if (research && research->active()) {
            xs8_monitor_record records[16];
            size_t copied = 0;
            do {
                if (xs8_monitor_snapshot(monitor, research->cursor_ptr(),
                                         records, 16, &copied) < 0) {
                    research->overflow();
                    if (research->id() == context_capture_id)
                        context_preparation.diagnostics_failed();
                    break;
                }
                research->observe(records, copied);
            } while (copied == 16);
            if (!xs8_monitor_healthy(monitor)) research->monitor_failed();
            int64_t now_ns = xs8_gpio_now_ns();
            if (chain && chain->active() && research->id() == chain->id()) {
                if (stopping || now_ns <= 0 ||
                    !xs8_monitor_healthy(monitor) ||
                    !research->integrity_ok()) {
                    chain->abort(now_ns > 0 ? (uint64_t)now_ns : 0,
                        stopping ? "SHUTDOWN" : "CAPTURE_OR_MONITOR_FAILED",
                        *research);
                } else {
                    chain->tick((uint64_t)now_ns,
                        [&] { return core.research_ready(
                            VolumeControl::Clock::now(), true); },
                        [&](uint8_t byte) {
                            std::fprintf(stderr,
                                "XS8 RESEARCH_CHAIN id=%llu requested=0x%02X\n",
                                (unsigned long long)chain->id(), byte);
                            std::string outcome = core.research_send(
                                byte, VolumeControl::Clock::now(), true);
                            const bool attempted =
                                outcome.find(" gpio_tx=") != std::string::npos;
                            const ServiceTxEvidence evidence =
                                transmitter.last_evidence();
                            const char *waveform =
                                !attempted ? "NOT_OBSERVED" :
                                evidence.waveform == WaveformEvidence::MATCH ? "MATCH" :
                                evidence.waveform == WaveformEvidence::MISMATCH ? "MISMATCH" :
                                evidence.waveform == WaveformEvidence::INVALID ? "INVALID" :
                                evidence.waveform == WaveformEvidence::MISSING ? "MISSING" :
                                "NOT_OBSERVED";
                            const bool completed = attempted &&
                                evidence.gpio_completed;
                            std::fprintf(stderr,
                                "XS8 RESEARCH_CHAIN id=%llu outcome=%s\n",
                                (unsigned long long)chain->id(), outcome.c_str());
                            return Xs8ResearchChain::Attempt{byte,
                                attempted ? transmitter.last_start_ns() : 0,
                                attempted ? transmitter.last_end_ns() : 0,
                                completed, waveform};
                        }, *research);
                    if (chain->uncertain()) research_tx_uncertain = true;
                }
                if (!chain->active())
                    std::fprintf(stderr, "XS8 RESEARCH_CHAIN %s",
                                 chain->read(4).c_str());
            }
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
        if (context_preparation.active() &&
            receiver.status_fields().find(" receiver_integrity=HEALTHY") ==
                std::string::npos)
            context_preparation.diagnostics_failed();
        if (!context_preparation.active() && research &&
            research->id() == context_capture_id && research->active())
            research->cancel();
        if (query) {
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
                const bool sent = reply(query_client, query->response(
                    now_ns > 0 ? (uint64_t)now_ns : 0));
                query.reset();
                const int completed_client = query_client;
                query_client = -1;
                if (!sent) {
                    for (size_t i = 0; i < clients.size(); ++i)
                        if (clients[i].fd == completed_client) {
                            close_client(i);
                            break;
                        }
                }
            }
        }
        std::array<pollfd, kMaxClients + 1> fds{};
        fds[0] = {server, POLLIN, 0};
        for (size_t i = 0; i < clients.size(); ++i)
            fds[i + 1] = {clients[i].fd, POLLIN, 0};
        const size_t polled_clients = clients.size();
        int count = poll(fds.data(), polled_clients + 1, 20);
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
                if (clients.size() >= kMaxClients || !nonblocking) {
                    if (nonblocking) reply(accepted, "ERR BUSY\n");
                    close(accepted);
                } else {
                    const uint64_t id = next_client_id++;
                    clients.push_back({accepted, id, {}});
                    core.connected(id);
                }
            }
        }
        size_t selected = clients.size();
        for (size_t offset = 0; offset < polled_clients; ++offset) {
            const size_t i = (next_poll_client + offset) % polled_clients;
            if (fds[i + 1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                selected = i;
                break;
            }
        }
        if (selected >= clients.size()) continue;
        next_poll_client = (selected + 1) % polled_clients;
        if (fds[selected + 1].revents & POLLNVAL) {
            close_client(selected);
            continue;
        }
        const int client = clients[selected].fd;
        const uint64_t client_id = clients[selected].id;
        std::string& input = clients[selected].input;
        char buffer[256];
        ssize_t size = read(client, buffer, sizeof(buffer));
        if (query && query_client == client) {
            if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            close_client(selected);
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
                if (query && line != "STATUS" && line != "RESEARCH_CONTEXT_STATUS" &&
                    line.compare(0, 15, "VOLUME_RELEASE ") != 0) {
                    if (!reply(client, "ERR QUERY_STATUS reason=SESSION_BUSY\n"))
                        failed = true;
                    continue;
                }
                if (line.compare(0, 17, "RESEARCH_CONTEXT_") == 0) {
                    const int64_t context_now = xs8_gpio_now_ns();
                    const uint64_t now_ns = context_now > 0 ? context_now : 0;
                    std::string result;
                    if (line.compare(0, 25, "RESEARCH_CONTEXT_PREPARE ") == 0) {
                        std::istringstream arguments(line);
                        std::string operation, profile, confirmation, extra;
                        if (!(arguments >> operation >> profile >> confirmation) ||
                            (arguments >> extra))
                            result = "ERR RESEARCH_CONTEXT reason=INVALID_REQUEST\n";
                        else if (core.state() != VolumeControl::State::IDLE ||
                                 core.stop_owed() || core.context_may_be_open() ||
                                 core.cd_fault() || core.cd_release_owed())
                            result = "ERR RESEARCH_CONTEXT reason=UNRESOLVED_CONTROL_STATE\n";
                        else if (!xs8_monitor_healthy(monitor) ||
                                 receiver.status_fields().find(" receiver_integrity=HEALTHY") ==
                                     std::string::npos)
                            result = "ERR RESEARCH_CONTEXT reason=MONITOR_OR_RECEIVER_INTEGRITY\n";
                        else if (query || (research && research->active()) ||
                                 (chain && chain->active()) || research_tx_uncertain || stopping)
                            result = "ERR RESEARCH_CONTEXT reason=SESSION_BUSY\n";
                        else result = context_preparation.prepare(
                            profile, confirmation == "CONFIRM_NO_TX", now_ns);
                        if (result.compare(0, 2, "OK") == 0)
                            std::fprintf(stderr, "XS8 CONTEXT_PREPARATION profile=MUTE "
                                                 "operator_confirmed=1 mode=NO_TX\n");
                    } else {
                        uint64_t token = 0;
                        long window = 0;
                        if (parse_context_observe(line, token, window)) {
                            result = context_preparation.authorize(token, now_ns);
                            if (result.empty() && research && research->active())
                                result = "ERR RESEARCH_CONTEXT reason=CAPTURE_BUSY\n";
                            if (result.empty()) {
                                const uint64_t deadline = std::min<uint64_t>(
                                    now_ns + uint64_t(window) * 1000000ULL,
                                    context_preparation.deadline_ns());
                                research.emplace(next_research_id++,
                                    xs8_monitor_checkpoint(monitor), now_ns,
                                    deadline, false);
                                context_capture_id = research->id();
                                result = "OK RESEARCH_OBSERVE id=" +
                                    std::to_string(context_capture_id) +
                                    " mode=CONTEXT_PREPARATION_PASSIVE tx_attempted=0\n";
                                std::fprintf(stderr, "XS8 CONTEXT_PREPARATION_OBSERVE "
                                                     "id=%llu mode=NO_TX\n",
                                    (unsigned long long)context_capture_id);
                            }
                        } else result = context_preparation.command(line, now_ns);
                    }
                    if (!context_preparation.active() && research &&
                        research->id() == context_capture_id && research->active())
                        research->cancel();
                    if (!reply(client, result)) failed = true;
                    continue;
                }
                if (context_preparation.active() && line != "STATUS" &&
                    line.compare(0, 14, "RESEARCH_READ ") != 0 &&
                    line.compare(0, 17, "RESEARCH_TX_READ ") != 0 &&
                    line != "VOLUME_RELEASE UP" && line != "VOLUME_RELEASE DOWN") {
                    if (!reply(client,
                        "ERR RESEARCH_CONTEXT reason=EXCLUSIVE_PREPARATION\n"))
                        failed = true;
                    continue;
                }
                uint8_t research_byte = 0;
                long research_window = 0;
                uint64_t research_id = 0;
                size_t research_index = 0;
                std::vector<uint8_t> chain_bytes;
                long chain_gap = 0;
                if (xs8_parse_research_chain(line, chain_bytes, chain_gap,
                                         research_window)) {
                    std::string denied;
                    if (!research_tx_enabled || !research_chain_enabled)
                        denied = "ERR RESEARCH reason=CHAIN_DISABLED\n";
                    else if (research_tx_uncertain)
                        denied = "ERR RESEARCH reason=TX_UNCERTAIN_LATCHED\n";
                    else if (research && research->active())
                        denied = "ERR RESEARCH reason=SESSION_BUSY\n";
                    else if (stopping)
                        denied = "ERR RESEARCH reason=SHUTDOWN\n";
                    else if (std::find(chain_bytes.begin(), chain_bytes.end(),
                                       0x17) != chain_bytes.end())
                        denied = "ERR RESEARCH reason=CONTEXT_ENTRY_FORBIDDEN\n";
                    else denied = core.research_ready(received);
                    if (!denied.empty()) {
                        if (!reply(client, denied)) failed = true;
                        continue;
                    }
                    const int64_t start = xs8_gpio_now_ns();
                    if (start <= 0) {
                        if (!reply(client,
                                   "ERR RESEARCH reason=CLOCK_FAILED\n"))
                            failed = true;
                        continue;
                    }
                    const uint64_t id = next_research_id++;
                    research.emplace(id, xs8_monitor_checkpoint(monitor),
                        (uint64_t)start, UINT64_MAX, false);
                    research->set_chain();
                    chain.emplace(id, chain_bytes,
                        (uint64_t)chain_gap * 1000000ULL,
                        (uint64_t)research_window * 1000000ULL);
                    std::fprintf(stderr,
                        "XS8 RESEARCH_CHAIN id=%llu count=%zu gap_ms=%ld "
                        "window_ms=%ld context=ROOT_ASSUMED\n",
                        (unsigned long long)id, chain_bytes.size(),
                        chain_gap, research_window);
                    char detail[160];
                    std::snprintf(detail, sizeof(detail),
                        "OK RESEARCH_CHAIN id=%llu count=%zu gap_ms=%ld "
                        "window_ms=%ld context=ROOT_ASSUMED\n",
                        (unsigned long long)id, chain_bytes.size(),
                        chain_gap, research_window);
                    if (!reply(client, detail)) failed = true;
                    continue;
                }
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
                if (parse_research_index(line, "RESEARCH_READ ", research_id,
                                         research_index)) {
                    if (!research || research->id() != research_id) {
                        if (!reply(client,
                                   "ERR RESEARCH reason=UNKNOWN_SESSION\n"))
                            failed = true;
                    } else if (chain && chain->active() &&
                               chain->id() == research_id) {
                        char detail[80];
                        std::snprintf(detail, sizeof(detail),
                            "OK RESEARCH_WAIT id=%llu count=0\n",
                            (unsigned long long)research_id);
                        if (!reply(client, detail)) failed = true;
                    } else if (!reply(client, research->read(research_index)))
                        failed = true;
                    continue;
                }
                if (parse_research_index(line, "RESEARCH_TX_READ ",
                                         research_id, research_index)) {
                    if (!chain || chain->id() != research_id) {
                        if (!reply(client,
                                   "ERR RESEARCH reason=UNKNOWN_SESSION\n"))
                            failed = true;
                    } else if (!reply(client, chain->read(research_index)))
                        failed = true;
                    continue;
                }
                if (parse_research_cancel(line, research_id)) {
                    if (!research || research->id() != research_id) {
                        if (!reply(client,
                                   "ERR RESEARCH reason=UNKNOWN_SESSION\n"))
                            failed = true;
                    } else {
                        if (chain && chain->id() == research_id &&
                            chain->active()) {
                            int64_t now_ns = xs8_gpio_now_ns();
                            chain->abort(now_ns > 0 ? (uint64_t)now_ns : 0,
                                         "CANCELLED", *research);
                            if (chain->uncertain())
                                research_tx_uncertain = true;
                        }
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
                if (line.compare(0, 16, "CONTEXT_SESSION_") == 0) {
                    // Optional trailing observation window for ENTER/SEND/EXIT.
                    long window = 1500;
                    std::string request = line;
                    const size_t space = line.rfind(' ');
                    const bool transmits = line.compare(0, 21, "CONTEXT_SESSION_ENTER") == 0 ||
                        line.compare(0, 20, "CONTEXT_SESSION_SEND") == 0 ||
                        line.compare(0, 20, "CONTEXT_SESSION_EXIT") == 0;
                    if (transmits && space != std::string::npos &&
                        std::count(line.begin(), line.end(), ' ') ==
                            (line.compare(0, 20, "CONTEXT_SESSION_SEND") == 0 ? 3 : 2)) {
                        if (!parse_number(line.c_str() + space + 1, window) ||
                            window < 1 || window > 5000) {
                            if (!reply(client, "ERR CONTEXT_SESSION reason=INVALID_REQUEST\n"))
                                failed = true;
                            continue;
                        }
                        request = line.substr(0, space);
                    }
                    std::string denied;
                    if (request.compare(0, 21, "CONTEXT_SESSION_ENTER") == 0) {
                        if (stopping) denied = "ERR CONTEXT_SESSION reason=SHUTDOWN\n";
                        else if (research_tx_uncertain)
                            denied = "ERR CONTEXT_SESSION reason=TX_UNCERTAIN_LATCHED\n";
                        else if (query || (research && research->active()) ||
                                 (chain && chain->active()) || context_preparation.active())
                            denied = "ERR CONTEXT_SESSION reason=SESSION_BUSY\n";
                        else if (receiver.status_fields().find(" receiver_integrity=HEALTHY") ==
                                 std::string::npos)
                            denied = "ERR CONTEXT_SESSION reason=RECEIVER_INTEGRITY\n";
                    } else if (request.compare(0, 20, "CONTEXT_SESSION_SEND") == 0 &&
                               research && research->active()) {
                        // Finish reading the previous capture first. EXIT is never held back.
                        denied = "ERR CONTEXT_SESSION reason=CAPTURE_BUSY\n";
                    }
                    if (!denied.empty()) {
                        if (!reply(client, denied)) failed = true;
                        continue;
                    }
                    const uint64_t checkpoint = xs8_monitor_checkpoint(monitor);
                    const int64_t before = xs8_gpio_now_ns();
                    transmitter.begin_record();
                    std::string result = core.command(request, received, received, client_id);
                    const auto frames = transmitter.end_record();
                    if (result.find("_UNCERTAIN ") != std::string::npos)
                        research_tx_uncertain = true;
                    if (!frames.empty() && result.back() == '\n') {
                        result.pop_back();
                        std::string detail = " capture_id=NONE";
                        if (!(research && research->active()) && before > 0) {
                            const uint64_t last_end = std::max<uint64_t>(
                                frames.back().end_ns, (uint64_t)before);
                            research.emplace(next_research_id++, checkpoint, (uint64_t)before,
                                last_end + (uint64_t)window * 1000000ULL, false);
                            for (const auto& frame : frames)
                                if (frame.start_ns && frame.end_ns >= frame.start_ns)
                                    research->add_tx_window(frame.start_ns, frame.end_ns);
                            detail = " capture_id=" + std::to_string(research->id());
                        }
                        detail += " frames=";
                        for (size_t i = 0; i < frames.size(); ++i) {
                            const auto& e = frames[i].evidence;
                            char item[96];
                            std::snprintf(item, sizeof(item), "%s0x%02X:%s:%s:%llu:%llu",
                                i ? "," : "", e.byte, e.gpio_completed ? "TX_OK" : "TX_FAIL",
                                e.waveform == WaveformEvidence::MATCH ? "MATCH" :
                                e.waveform == WaveformEvidence::MISMATCH ? "MISMATCH" :
                                e.waveform == WaveformEvidence::INVALID ? "INVALID" :
                                e.waveform == WaveformEvidence::MISSING ? "MISSING" :
                                "NOT_OBSERVED",
                                (unsigned long long)frames[i].start_ns,
                                (unsigned long long)frames[i].end_ns);
                            detail += item;
                        }
                        result += detail + "\n";
                    }
                    if (!reply(client, result)) failed = true;
                    continue;
                }
                if (line == "QUERY_STATUS") {
                    if (query) {
                        if (!reply(client,
                                   "ERR QUERY_STATUS reason=SESSION_BUSY\n"))
                            failed = true;
                        continue;
                    }
                    if (chain && chain->active()) {
                        if (!reply(client, "ERR RESEARCH reason=SESSION_BUSY\n"))
                            failed = true;
                        continue;
                    }
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
                        query_client = client;
                        break;
                    }
                    if (!reply(client, result)) failed = true;
                    continue;
                }
                if (line == "STATUS")
                    core.set_receiver_status(receiver.status_fields());
                else if (chain && chain->active() &&
                         line.compare(0, 15, "VOLUME_RELEASE ") != 0) {
                    if (!reply(client, "ERR RESEARCH reason=SESSION_BUSY\n"))
                        failed = true;
                    continue;
                }
                if (line == "CD_TRACK_NEXT" ||
                    line == "CD_TRACK_PREVIOUS" ||
                    line.compare(0, 9, "CD_PRESS ") == 0 ||
                    line == "VOLUME_STEP UP" || line == "VOLUME_STEP DOWN") {
                    if (stopping) {
                        if (!reply(client, "ERR SHUTDOWN\n")) failed = true;
                        continue;
                    }
                    if (research_tx_uncertain) {
                        if (!reply(client,
                                   "ERR RESEARCH reason=TX_UNCERTAIN_LATCHED\n"))
                            failed = true;
                        continue;
                    }
                }
                std::string result = core.command(line, received, received, client_id);
                if (line == "STATUS" && context_preparation.active() &&
                    result.compare(0, 14, "OK state=IDLE ") == 0)
                    result.replace(3, 10, "state=RESEARCH_PREPARATION");
                if (!reply(client, result)) {
                    failed = true;
                    break;
                }
                if (stopping) break;
            }
            if (input.size() > kMaxLine) failed = true;
            if (!failed) continue;
        } else if (size < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        close_client(selected);
    }
    context_preparation.shutdown();
    if (research && research->id() == context_capture_id && research->active())
        research->cancel();
    while (!clients.empty()) close_client(0);
    bool cleaned = core.shutdown(VolumeControl::Clock::now());
    if (core.stop_owed() ||
        core.state() == VolumeControl::State::FAULT_LATCHED)
        std::fprintf(stderr, "WARNING: XS8 STOP unresolved; check receiver\n");
    if (core.cd_fault() || core.cd_release_owed())
        std::fprintf(stderr, "WARNING: CD transport uncertain; physically "
                             "stop/inspect changer before authorized restart\n");
    close(server);
    unlink(socket_path);
    xs8_monitor_stop(monitor);
    xs8_gpio_close(&bus);
    return cleaned ? 0 : 1;
}
