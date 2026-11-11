#include "enginex/itch/mold_udp.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace enginex::itch::mold::udp {

namespace {

using Clock = std::chrono::steady_clock;

sockaddr_in to_addr(const Endpoint& e) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(e.port);
    if (::inet_pton(AF_INET, e.host.c_str(), &a.sin_addr) != 1) {
        throw std::runtime_error("not an IPv4 address: " + e.host);
    }
    return a;
}

net::Socket open_udp(const Endpoint& bind_to, bool reuse_port = false) {
    net::Socket s(::socket(AF_INET, SOCK_DGRAM, 0));
    if (!s.valid()) {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }
    if (reuse_port) {
        const int on = 1;
        (void)::setsockopt(s.fd(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        (void)::setsockopt(s.fd(), SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
    }
    const sockaddr_in a = to_addr(bind_to);
    if (::bind(s.fd(), reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        throw std::runtime_error("bind " + bind_to.host + ":" + std::to_string(bind_to.port) +
                                 ": " + std::strerror(errno));
    }
    return s;
}

void clear_error(int fd) {
    int err = 0;
    socklen_t len = sizeof err;
    (void)::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);  // reading SO_ERROR clears it
}

// The request socket must allow SO_REUSEPORT (the per-peer answer sockets share its port),
// which would also let a second sender bind the same port silently. A fixed port is therefore
// first bound once without reuse: if that fails, the port is taken.
net::Socket bind_request_port(const Endpoint& e) {
    if (e.port != 0) {
        (void)open_udp(e, false);  // throws "bind ...: Address already in use" if taken
    }
    return open_udp(e, true);
}

bool same_peer(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

std::uint16_t bound_port(int fd) {
    sockaddr_in a{};
    socklen_t len = sizeof a;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        throw std::runtime_error(std::string("getsockname: ") + std::strerror(errno));
    }
    return ntohs(a.sin_port);
}

int ms_until(Clock::time_point t) {
    const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(t - Clock::now());
    return static_cast<int>(std::max<std::int64_t>(0, d.count()));
}

const char* state_name(Sequencer::State s) {
    switch (s) {
        case Sequencer::State::Running: return "running";
        case Sequencer::State::Gap: return "gap";
        case Sequencer::State::Ended: return "ended";
        case Sequencer::State::Failed: return "failed";
    }
    return "unknown";
}

}  // namespace

bool is_loopback(const std::string& host) {
    in_addr a{};
    return ::inet_pton(AF_INET, host.c_str(), &a) == 1 && (ntohl(a.s_addr) >> 24U) == 127U;
}

// --------------------------------------------------------------------------------- Sender

Sender::Sender(const PacketStore& store, SenderOptions options)
    : store_(store), o_(std::move(options)), sock_(bind_request_port(o_.bind)), rng_(o_.seed) {
    Endpoint any = o_.bind;
    any.port = 0;
    feed_ = open_udp(any);
    const sockaddr_in dest = to_addr(o_.dest);
    if (::connect(feed_.fd(), reinterpret_cast<const sockaddr*>(&dest), sizeof dest) != 0) {
        throw std::runtime_error(std::string("connect: ") + std::strerror(errno));
    }
    receiver_ip_ = dest.sin_addr;
    answer_tokens_ = static_cast<double>(o_.max_answer_packets_per_second) / 10;  // 100 ms burst
    tokens_at_ = Clock::now();
}

std::uint16_t Sender::port() const { return bound_port(sock_.fd()); }

// Answers go out on a socket connected to the requester and bound to the request port
// (SO_REUSEPORT), so the requester sees them come from the address it asked. A connected
// send() avoids the per-call cost of sendto(). Later requests from that peer arrive on the
// connected socket (the more specific match). Falls back to sendto() if that fails.
Sender::Peer* Sender::peer_for(const sockaddr_in& from) {
    for (Peer& p : peers_) {
        if (same_peer(p.addr, from)) {
            return &p;
        }
    }
    if (peers_.size() >= kMaxPeers) {
        return nullptr;
    }
    try {
        Endpoint local = o_.bind;
        local.port = bound_port(sock_.fd());
        net::Socket s = open_udp(local, true);
        if (::connect(s.fd(), reinterpret_cast<const sockaddr*>(&from), sizeof from) != 0) {
            return nullptr;
        }
        peers_.push_back(Peer{from, std::move(s)});
        return &peers_.back();
    } catch (const std::runtime_error&) {
        return nullptr;
    }
}

void Sender::answer(std::span<const std::uint8_t> datagram, const sockaddr_in& from, int via_fd,
                    SenderReport& r) {
    if (!o_.answer_any_source && from.sin_addr.s_addr != receiver_ip_.s_addr) {
        ++r.foreign_requests;  // never reflect answers toward an unexpected (maybe spoofed) source
        return;
    }
    const auto req = parse_request(datagram);
    const auto first = req ? store_.find(req->sequence) : std::nullopt;
    if (!req || req->session != store_.session() || !first) {
        ++r.bad_requests;
        return;
    }
    ++r.requests;
    last_request_ = Clock::now();
    if (via_fd < 0) {
        const Peer* p = peer_for(from);
        via_fd = p != nullptr ? p->sock.fd() : -1;
    }
    const std::uint64_t count = std::min<std::uint64_t>(req->count, o_.max_request_messages);
    const std::uint64_t end = req->sequence + count;  // exclusive
    for (std::size_t i = *first; i < store_.size() && store_.first(i) < end; ++i) {
        const auto now = Clock::now();
        const double rate = static_cast<double>(o_.max_answer_packets_per_second);
        answer_tokens_ = std::min(std::max(1.0, rate / 10), answer_tokens_ + rate * std::chrono::duration<double>(now - tokens_at_).count());
        tokens_at_ = now;
        if (answer_tokens_ < 1) {
            r.answers_rate_limited += 1;  // the receiver will ask again after retry_after
            continue;
        }
        answer_tokens_ -= 1;
        if (rng_.below(100) < o_.retransmit_drop_percent) {
            ++r.retransmit_dropped;
            continue;
        }
        ++r.retransmitted;
        const auto& pkt = store_.packet(i);
        const ssize_t n =
            via_fd >= 0 ? ::send(via_fd, pkt.data(), pkt.size(), 0)
                        : ::sendto(sock_.fd(), pkt.data(), pkt.size(), 0,
                                   reinterpret_cast<const sockaddr*>(&from), sizeof from);
        if (n != static_cast<ssize_t>(pkt.size())) {
            ++r.send_errors;  // UDP: a failed send is a lost packet; recovery handles it
        }
    }
}

void Sender::serve_requests(int timeout_ms, SenderReport& r) {
    std::uint8_t buf[256];
    for (int budget = 64; budget > 0; --budget) {
        std::vector<pollfd> fds;
        fds.push_back({sock_.fd(), POLLIN, 0});
        for (const Peer& p : peers_) fds.push_back({p.sock.fd(), POLLIN, 0});
        if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), std::max(0, timeout_ms)) <= 0) {
            return;
        }
        timeout_ms = 0;  // wait only for the first datagram
        for (std::size_t k = 0; k < fds.size(); ++k) {
            if ((fds[k].revents & POLLIN) == 0) {
                if ((fds[k].revents & POLLERR) != 0) {
                    clear_error(fds[k].fd);  // e.g. the receiver exited: else poll() spins
                }
                continue;
            }
            sockaddr_in from{};
            socklen_t from_len = sizeof from;
            const ssize_t n = ::recvfrom(fds[k].fd, buf, sizeof buf, MSG_DONTWAIT,
                                         reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n < 0) {
                continue;
            }
            // On a peer socket the sender is that peer (answer on it); on the shared socket,
            // find or create the peer's connected socket.
            answer(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)),
                   k == 0 ? from : peers_[k - 1].addr, k == 0 ? -1 : peers_[k - 1].sock.fd(), r);
        }
    }
}

SenderReport Sender::run(const std::atomic<bool>* stop) {
    SenderReport r;
    r.packets = store_.size();
    const auto start = Clock::now();
    const auto stopped = [&] { return stop != nullptr && stop->load(); };
    for (std::size_t i = 0; i < store_.size() && !stopped(); ++i) {
        if (o_.packets_per_second != 0) {
            const auto due = start + std::chrono::nanoseconds(
                                         static_cast<std::int64_t>(i * 1'000'000'000ULL /
                                                                   o_.packets_per_second));
            while (Clock::now() < due) {
                serve_requests(0, r);
                if (due - Clock::now() > std::chrono::microseconds(200)) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
        }
        if (i % 16 == 0) {
            serve_requests(0, r);
        }
        if (rng_.below(100) < o_.drop_percent) {
            ++r.dropped;
            continue;
        }
        ++r.sent;
        const auto& pkt = store_.packet(i);
        if (::send(feed_.fd(), pkt.data(), pkt.size(), 0) != static_cast<ssize_t>(pkt.size())) {
            ++r.send_errors;  // e.g. ENOBUFS, or ECONNREFUSED after an ICMP port unreachable
        }
    }
    // End of session: repeat it (it may be lost too) and keep answering requests.
    const auto end_packet = build_control(store_.session(), store_.end_sequence(), kEndOfSession);
    last_request_ = Clock::now();
    while (!stopped() && Clock::now() - last_request_ < o_.linger) {
        ++r.end_packets;
        if (::send(feed_.fd(), end_packet.data(), end_packet.size(), 0) !=
            static_cast<ssize_t>(end_packet.size())) {
            ++r.send_errors;
        }
        const auto next_end = Clock::now() + o_.end_interval;
        while (!stopped() && Clock::now() < next_end) {
            serve_requests(ms_until(next_end), r);
        }
    }
    return r;
}

// ------------------------------------------------------------------------------- Receiver

std::vector<std::pair<std::uint64_t, std::uint16_t>> RequestPlanner::plan(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& missing,
    std::uint64_t next_expected, Clock::time_point now) {
    for (auto it = asked_.begin(); it != asked_.end() && it->first < next_expected;) {
        it = it->second.end <= next_expected ? asked_.erase(it) : std::next(it);  // answered
    }
    std::vector<std::pair<std::uint64_t, std::uint16_t>> out;
    for (const auto& [hole, count] : missing) {
        if (out.size() == max_ranges_) {
            break;
        }
        std::uint64_t first = hole;
        if (auto it = asked_.upper_bound(hole); it != asked_.begin()) {
            --it;  // the latest request starting at or before the hole
            if (it->second.end > hole && now - it->second.at < retry_after_) {
                first = it->second.end;  // covered up to there; its answer may be in flight
            }
        }
        std::uint64_t end = hole + count;
        // Stop at a later request that is still active (it covers what follows).
        for (auto it = asked_.upper_bound(first); it != asked_.end() && it->first < end; ++it) {
            if (now - it->second.at < retry_after_) {
                end = it->first;
                break;
            }
        }
        if (first >= end) {
            continue;
        }
        const auto n = static_cast<std::uint16_t>(
            std::min<std::uint64_t>({end - first, max_messages_, kEndOfSession - 1U}));
        asked_[first] = Asked{first + n, now};
        out.emplace_back(first, n);
    }
    return out;
}

Receiver::Receiver(ReceiverOptions options) : o_(std::move(options)), sock_(open_udp(o_.bind)) {
    const int size = o_.receive_buffer_bytes;
    (void)::setsockopt(sock_.fd(), SOL_SOCKET, SO_RCVBUF, &size, sizeof size);  // best effort
}

std::uint16_t Receiver::port() const { return bound_port(sock_.fd()); }

ReceiverReport Receiver::run(
    const std::function<void(std::uint64_t, std::span<const std::uint8_t>)>& deliver,
    const std::atomic<bool>* stop) {
    ReceiverReport r;
    Sequencer q(o_.sequencer);
    std::uint64_t expect = o_.sequencer.first_sequence;
    auto on_message = [&](std::uint64_t seq, std::span<const std::uint8_t> m) {
        r.in_order = r.in_order && seq == expect;
        ++expect;
        deliver(seq, m);
    };
    // Requests go out on their own socket, connected to the re-request server: a connected
    // send() is much cheaper than sendto() here, and the answers come back on it.
    net::Socket req;
    if (o_.recovery) {
        Endpoint local = o_.bind;
        local.port = 0;
        req = open_udp(local);
        const sockaddr_in to = to_addr(o_.request_to);
        if (::connect(req.fd(), reinterpret_cast<const sockaddr*>(&to), sizeof to) != 0) {
            throw std::runtime_error(std::string("connect: ") + std::strerror(errno));
        }
        const int size = o_.receive_buffer_bytes;
        (void)::setsockopt(req.fd(), SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    }
    RequestPlanner planner(o_.retry_after, o_.max_ranges_per_round, o_.max_request_messages);
    std::vector<std::uint8_t> buf(65536);
    const auto start = Clock::now();
    auto last_packet = start;
    auto last_progress = start;
    std::uint64_t accepted = 0;
    auto last_round = start - o_.request_interval;
    in_addr feed_ip{};
    const std::string feed_host = o_.feed_from.empty() ? o_.request_to.host : o_.feed_from;
    if (::inet_pton(AF_INET, feed_host.c_str(), &feed_ip) != 1) {
        throw std::runtime_error("not an IPv4 address: " + feed_host);
    }
    auto drain = [&](int fd) {
        bool got = false;
        for (int budget = 256; budget > 0; --budget) {
            sockaddr_in from{};
            socklen_t from_len = sizeof from;
            const ssize_t n = ::recvfrom(fd, buf.data(), buf.size(), MSG_DONTWAIT,
                                         reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n < 0) {
                break;
            }
            if (!o_.accept_any_source && from.sin_addr.s_addr != feed_ip.s_addr) {
                ++r.foreign_source;
                continue;
            }
            got = true;
            ++r.packets;
            r.bytes += static_cast<std::uint64_t>(n);
            q.on_packet(std::span<const std::uint8_t>(buf.data(), static_cast<std::size_t>(n)),
                        on_message);
        }
        return got;
    };
    while (stop == nullptr || !stop->load()) {
        pollfd fds[2] = {{sock_.fd(), POLLIN, 0}, {req.valid() ? req.fd() : -1, POLLIN, 0}};
        if (::poll(fds, 2, static_cast<int>(o_.request_interval.count())) > 0) {
            bool got = false;
            if ((fds[0].revents & POLLIN) != 0) got = drain(sock_.fd()) || got;
            if (req.valid() && (fds[1].revents & POLLIN) != 0) got = drain(req.fd()) || got;
            for (const pollfd& f : fds) {
                if (f.fd >= 0 && (f.revents & (POLLERR | POLLIN)) == POLLERR) {
                    clear_error(f.fd);  // e.g. ICMP port unreachable: else poll() spins
                }
            }
            if (got) {
                last_packet = Clock::now();
            }
        }
        const auto s = q.state();
        if (s == Sequencer::State::Ended || s == Sequencer::State::Failed) {
            break;
        }
        const auto now = Clock::now();
        // Progress = a message accepted (delivered or held for reordering). Heartbeats, the
        // sender's repeated end-of-session packets and duplicates are not progress: while a gap
        // is open they would otherwise keep the run alive forever if it can never be filled.
        if (const std::uint64_t a = q.stats().delivered + q.buffered(); a != accepted) {
            accepted = a;
            last_progress = now;
        }
        if (now - last_packet > o_.idle_timeout) {
            r.timed_out = true;
            break;
        }
        if (s == Sequencer::State::Gap && now - last_progress > o_.idle_timeout) {
            r.timed_out = true;
            r.gap_stalled = true;
            break;
        }
        if (!o_.recovery || s != Sequencer::State::Gap || now - last_round < o_.request_interval) {
            continue;
        }
        last_round = now;
        ++r.request_rounds;
        for (const auto& [first, n] : planner.plan(q.missing(), q.next_expected(), now)) {
            const auto bytes = build_request(*q.session(), first, n);
            if (::send(req.fd(), bytes.data(), bytes.size(), 0) ==
                static_cast<ssize_t>(bytes.size())) {
                ++r.requests_sent;
            }
        }
    }
    const Sequencer::Stats& st = q.stats();
    r.delivered = st.delivered;
    r.duplicates = st.duplicates;
    r.malformed = st.malformed_packets;
    r.foreign_session = st.foreign_session_packets;
    r.gaps_opened = st.gaps_opened;
    r.gaps_closed = st.gaps_closed;
    r.buffered_peak = st.buffered_peak;
    r.final_state = state_name(q.state());
    r.failure = std::string(to_string(q.fail_reason()));
    r.next_expected = q.next_expected();
    r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return r;
}

}  // namespace enginex::itch::mold::udp
