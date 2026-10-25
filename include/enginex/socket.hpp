#pragma once

// Thin POSIX TCP helpers (IPv4). No exceptions escape send/recv paths; they return errno.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace enginex::net {

// Owns a file descriptor; closes it on destruction.
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) noexcept : fd_(fd) {}
    ~Socket() { reset(); }
    Socket(Socket&& other) noexcept : fd_(other.release()) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

// Binds and listens on host:port (port 0 picks an ephemeral port). Throws std::runtime_error.
[[nodiscard]] Socket listen_tcp(const std::string& host, std::uint16_t port, int backlog = 128);
[[nodiscard]] std::uint16_t local_port(int fd);

// Connects with a timeout. Throws std::runtime_error.
[[nodiscard]] Socket connect_tcp(const std::string& host, std::uint16_t port,
                                 std::chrono::milliseconds timeout);

// Per-connection options: TCP_NODELAY, no SIGPIPE, receive/send timeouts (0 = none).
void configure_stream(int fd, std::chrono::milliseconds recv_timeout,
                      std::chrono::milliseconds send_timeout);

// Sends every byte (loops over short writes and EINTR, never raises SIGPIPE).
// Returns 0 or an errno value (EAGAIN/EWOULDBLOCK means the socket send timeout expired).
// With a nonzero `deadline`, gives up with ETIMEDOUT once the whole send has taken longer
// (a peer that drains a few bytes at a time would otherwise reset SO_SNDTIMEO forever).
[[nodiscard]] int send_all(int fd, std::string_view bytes,
                           std::chrono::milliseconds deadline = std::chrono::milliseconds(0));

// Ignores SIGPIPE process-wide (writes to closed sockets then fail with EPIPE instead).
void ignore_sigpipe();

}  // namespace enginex::net
