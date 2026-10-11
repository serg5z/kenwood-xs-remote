#include "xs8_service_core.h"
#include "xs8_gpio.h"
#include "xs8_monitor.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
volatile std::sig_atomic_t stopping = 0;

void on_signal(int) { stopping = 1; }

class GpioServiceTransmitter final : public ServiceTransmitter {
public:
    GpioServiceTransmitter(xs8_gpio& bus, xs8_monitor& monitor)
        : bus_(bus), monitor_(monitor) {}
    bool monitor_healthy() const override {
        return xs8_monitor_healthy(const_cast<xs8_monitor*>(&monitor_));
    }
    ServiceTxEvidence last_evidence() const override { return last_; }
    ServiceTxEvidence last_stop_evidence() const override { return last_stop_; }
    bool send(uint8_t byte) override {
        last_ = {};
        last_.attempted = true;
        last_.byte = byte;
        uint64_t checkpoint = xs8_monitor_checkpoint(&monitor_);
        int64_t start = 0;
        last_.gpio_completed = xs8_gpio_send(&bus_, byte, &start) == 0;
        int64_t end = xs8_gpio_now_ns();
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
    ServiceTxEvidence last_{};
    ServiceTxEvidence last_stop_{};
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
    long hold_ms = 2000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--allow-unrecovered-start") == 0)
            authorized = true;
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
                "[--allow-unrecovered-start] [--max-hold-ms 100..3000]\n",
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

    GpioServiceTransmitter transmitter(bus, *monitor);
    Xs8ServiceCore core(transmitter, VolumeControl::Ms(hold_ms), authorized,
                        [] { return stopping != 0; });
    std::fprintf(stderr, "XS8 service listening on %s; state=%s\n",
        socket_path, authorized ? "IDLE (operator authorized)" : "BOOT_INHIBIT");
    int client = -1;
    std::string input;
    while (!stopping) {
        xs8_monitor_poll(monitor, 0);
        core.tick(VolumeControl::Clock::now());
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
            core.disconnected(VolumeControl::Clock::now());
            close(client);
            client = -1;
            input.clear();
            continue;
        }
        char buffer[256];
        ssize_t size = read(client, buffer, sizeof(buffer));
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
