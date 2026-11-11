// Per-datagram cost of the UDP system calls the MoldUDP64 transport uses, on loopback:
// unconnected sendto(), send() on a connected socket, recv() and recvfrom(). This is the
// environment's floor for mold-send / mold-recv: a single receiver thread cannot take more
// datagrams per second than 1 / (recv cost).
//
//   enginex_udp_syscall_bench [--datagrams N] [--trials T] [--bytes B]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include "enginex/build_info.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double per_op_us(Clock::time_point a, Clock::time_point b, int n) {
    return std::chrono::duration<double, std::micro>(b - a).count() / std::max(n, 1);
}

}  // namespace

// A positive int in [1, max], or -1.
int positive_arg(const char* text, int max) {
    int v = 0;
    const char* end = text + std::strlen(text);
    const auto [ptr, ec] = std::from_chars(text, end, v);
    return ec == std::errc{} && ptr == end && v >= 1 && v <= max ? v : -1;
}

int main(int argc, char** argv) {
    int n = 4000;  // fits an 8 MiB receive buffer at 1400 bytes
    int trials = 5;
    int bytes = 1400;  // datagram payload; the per-call cost vs size says what batching buys
    for (int i = 1; i < argc; i += 2) {
        const bool known = std::strcmp(argv[i], "--datagrams") == 0 ||
                           std::strcmp(argv[i], "--trials") == 0 ||
                           std::strcmp(argv[i], "--bytes") == 0;
        if (!known || i + 1 >= argc) {
            std::fprintf(stderr,
                         "usage: enginex_udp_syscall_bench [--datagrams N] [--trials T] "
                         "[--bytes B]\n");
            return 2;
        }
        const bool is_bytes = std::strcmp(argv[i], "--bytes") == 0;
        int& target = std::strcmp(argv[i], "--datagrams") == 0 ? n : is_bytes ? bytes : trials;
        target = positive_arg(argv[i + 1], is_bytes ? 65'507 : 1'000'000);
        if (target < 0) {
            std::fprintf(stderr, "enginex_udp_syscall_bench: %s must be in [1, %d]\n", argv[i],
                         is_bytes ? 65'507 : 1'000'000);
            return 2;
        }
    }
    // Every call below is checked: a failing socket would otherwise "send" at the cost of an
    // EBADF return and produce a meaningless, flattering number.
    const int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (rx < 0) {
        std::perror("socket");
        return 1;
    }
    const int big = 8 << 20;
    (void)::setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (::bind(rx, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
        ::getsockname(rx, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        std::perror("bind");
        return 1;
    }
    std::vector<char> payload(static_cast<std::size_t>(bytes), 'x');
    std::vector<char> buf(static_cast<std::size_t>(bytes) + 1024);
    auto drain = [&](bool with_addr) {
        int got = 0;
        sockaddr_in from{};
        while (got < n) {
            socklen_t fl = sizeof from;
            const ssize_t r = with_addr ? ::recvfrom(rx, buf.data(), buf.size(), MSG_DONTWAIT,
                                                     reinterpret_cast<sockaddr*>(&from), &fl)
                                        : ::recv(rx, buf.data(), buf.size(), MSG_DONTWAIT);
            if (r <= 0)
                break;
            ++got;
        }
        return got;
    };
    namespace bi = enginex::build_info;
    std::printf("enginex_udp_syscall_bench: git %s, %s, %d datagrams of %d bytes, %d trials\n",
                bi::kGitSha, bi::kSystem, n, bytes, trials);
    std::vector<double> sendto_us, recv_us, send_us, recvfrom_us;
    int lost = 0;
    for (int t = 0; t < trials; ++t) {
        const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) {
            std::perror("socket");
            return 1;
        }
        int failed = 0;
        auto t0 = Clock::now();
        for (int i = 0; i < n; ++i) {
            failed += ::sendto(s, payload.data(), payload.size(), 0,
                               reinterpret_cast<sockaddr*>(&a), sizeof a) < 0
                          ? 1
                          : 0;
        }
        auto t1 = Clock::now();
        const int g1 = drain(false);
        auto t2 = Clock::now();
        if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            std::perror("connect");
            return 1;
        }
        auto t3 = Clock::now();
        for (int i = 0; i < n; ++i) {
            failed += ::send(s, payload.data(), payload.size(), 0) < 0 ? 1 : 0;
        }
        auto t4 = Clock::now();
        const int g2 = drain(true);
        auto t5 = Clock::now();
        ::close(s);
        if (failed != 0) {
            std::fprintf(stderr,
                         "enginex_udp_syscall_bench: %d sends failed in trial %d; no result\n",
                         failed, t + 1);
            return 1;
        }
        lost += (n - g1) + (n - g2);
        sendto_us.push_back(per_op_us(t0, t1, n));
        recv_us.push_back(per_op_us(t1, t2, g1));
        send_us.push_back(per_op_us(t3, t4, n));
        recvfrom_us.push_back(per_op_us(t4, t5, g2));
    }
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::printf("  sendto (unconnected)  median %6.2f us/datagram\n", median(sendto_us));
    std::printf("  send (connected)      median %6.2f us/datagram\n", median(send_us));
    std::printf("  recv                  median %6.2f us/datagram\n", median(recv_us));
    std::printf("  recvfrom              median %6.2f us/datagram\n", median(recvfrom_us));
    std::printf("  datagrams not received (buffer overflow): %d\n", lost);
    ::close(rx);
    return 0;
}
