#ifndef CEC_SERVICE_SOCKET_H
#define CEC_SERVICE_SOCKET_H

#include "cec_client_core.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

class UnixServiceTransport final : public CecServiceTransport {
public:
    explicit UnixServiceTransport(std::string path) : path_(std::move(path)) {}
    ~UnixServiceTransport() override { close(); }

    bool connect() override
    {
        if (fd_ >= 0) return true;
        if (path_.size() >= sizeof(sockaddr_un::sun_path)) return false;
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        int flags = fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(); return false;
        }
        fcntl(fd_, F_SETFD, FD_CLOEXEC);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path_.c_str(), path_.size() + 1);
        int result = ::connect(fd_, reinterpret_cast<sockaddr*>(&address),
                               sizeof(address));
        if (result == 0) return true;
        if (errno != EINPROGRESS && errno != EAGAIN) { close(); return false; }
        if (!wait(POLLOUT, 500)) { close(); return false; }
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error) {
            close(); return false;
        }
        return true;
    }

    bool command(const std::string& request, std::string& reply) override
    {
        if (fd_ < 0) return false;
        const std::string line = request + "\n";
        const auto deadline = CecClock::now() + std::chrono::seconds(3);
        size_t sent = 0;
        while (sent < line.size()) {
            if (!wait_until(POLLOUT, deadline)) return false;
            ssize_t n = send(fd_, line.data() + sent, line.size() - sent,
                             MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n > 0) sent += static_cast<size_t>(n);
            else if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            else return false;
        }
        reply.clear();
        while (reply.size() < 256) {
            if (!wait_until(POLLIN, deadline)) return false;
            char byte;
            ssize_t n = recv(fd_, &byte, 1, MSG_DONTWAIT);
            if (n == 1 && byte == '\n') return true;
            if (n == 1) reply.push_back(byte);
            else if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            else return false;
        }
        return false;
    }

    bool alive() override
    {
        if (fd_ < 0) return false;
        pollfd item{fd_, static_cast<short>(POLLIN | POLLHUP | POLLERR), 0};
        int result = poll(&item, 1, 0);
        if (result < 0) return errno == EINTR;
        if (result == 0) return true;
        if (item.revents & (POLLHUP | POLLERR | POLLNVAL)) return false;
        if (item.revents & POLLIN) {
            char byte;
            return recv(fd_, &byte, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
        }
        return true;
    }

    void close() override
    {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

private:
    bool wait(short condition, int milliseconds)
    {
        pollfd item{fd_, condition, 0};
        int result;
        do { result = poll(&item, 1, milliseconds); }
        while (result < 0 && errno == EINTR);
        // A peer may close immediately after writing its reply: consume
        // readable bytes even when POLLHUP is set in the same poll result.
        return result > 0 && (item.revents & condition);
    }

    bool wait_until(short condition, CecClock::time_point deadline)
    {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - CecClock::now()).count();
        return remaining > 0 && wait(condition, static_cast<int>(remaining));
    }

    std::string path_;
    int fd_ = -1;
};

#endif
