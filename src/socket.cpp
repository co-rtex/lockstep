#include "enginex/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <stdexcept>

namespace enginex::net {

namespace {

[[noreturn]] void fail(const std::string& what, int err) {
    throw std::runtime_error(what + ": " + std::strerror(err));
}

sockaddr_in make_address(const std::string& host, std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const std::string resolved = host == "localhost" ? "127.0.0.1" : host;
    if (::inet_pton(AF_INET, resolved.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("invalid IPv4 address: " + host);
    }
    return addr;
}

timeval to_timeval(std::chrono::milliseconds ms) {
    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(ms.count() / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((ms.count() % 1000) * 1000);
    return tv;
}

}  // namespace

void Socket::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

Socket listen_tcp(const std::string& host, std::uint16_t port, int backlog) {
    Socket s(::socket(AF_INET, SOCK_STREAM, 0));
    if (!s.valid()) {
        fail("socket", errno);
    }
    ::fcntl(s.fd(), F_SETFD, FD_CLOEXEC);
    const int one = 1;
    ::setsockopt(s.fd(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    const sockaddr_in addr = make_address(host, port);
    if (::bind(s.fd(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        fail("bind " + host + ":" + std::to_string(port), errno);
    }
    if (::listen(s.fd(), backlog) != 0) {
        fail("listen", errno);
    }
    return s;
}

std::uint16_t local_port(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        fail("getsockname", errno);
    }
    return ntohs(addr.sin_port);
}

Socket connect_tcp(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout) {
    Socket s(::socket(AF_INET, SOCK_STREAM, 0));
    if (!s.valid()) {
        fail("socket", errno);
    }
    ::fcntl(s.fd(), F_SETFD, FD_CLOEXEC);
    const sockaddr_in addr = make_address(host, port);
    const int flags = ::fcntl(s.fd(), F_GETFL, 0);
    ::fcntl(s.fd(), F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(s.fd(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    if (rc != 0 && errno != EINPROGRESS) {
        fail("connect " + host + ":" + std::to_string(port), errno);
    }
    if (rc != 0) {
        pollfd pfd{s.fd(), POLLOUT, 0};
        rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        if (rc == 0) {
            throw std::runtime_error("connect " + host + ":" + std::to_string(port) +
                                     ": timed out");
        }
        int err = 0;
        socklen_t len = sizeof err;
        ::getsockopt(s.fd(), SOL_SOCKET, SO_ERROR, &err, &len);
        if (rc < 0 || err != 0) {
            fail("connect " + host + ":" + std::to_string(port), rc < 0 ? errno : err);
        }
    }
    ::fcntl(s.fd(), F_SETFL, flags);
    return s;
}

void configure_stream(int fd, std::chrono::milliseconds recv_timeout,
                      std::chrono::milliseconds send_timeout) {
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#if defined(SO_NOSIGPIPE)
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (recv_timeout.count() > 0) {
        const timeval tv = to_timeval(recv_timeout);
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    if (send_timeout.count() > 0) {
        const timeval tv = to_timeval(send_timeout);
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
}

int send_all(int fd, std::string_view bytes) {
#if defined(MSG_NOSIGNAL)
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    const char* p = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        const ssize_t n = ::send(fd, p, left, kFlags);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return 0;
}

void ignore_sigpipe() {
    std::signal(SIGPIPE, SIG_IGN);
}

}  // namespace enginex::net
