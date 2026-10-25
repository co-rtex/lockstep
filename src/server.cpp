#include "enginex/server.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "enginex/bounded_queue.hpp"
#include "enginex/protocol.hpp"
#include "enginex/socket.hpp"

namespace enginex::net {

namespace {

using Clock = std::chrono::steady_clock;
using Frame = std::shared_ptr<const std::string>;

constexpr std::chrono::milliseconds kReadTick{200};
constexpr std::chrono::milliseconds kPollTick{100};

Frame make_frame(const std::string& json) {
    return std::make_shared<const std::string>(encode_frame(json, kMaxServerFrame));
}

struct Counters {
    std::atomic<std::uint64_t> connections_accepted{0};
    std::atomic<std::uint64_t> connections_rejected{0};
    std::atomic<std::uint64_t> requests{0};
    std::atomic<std::uint64_t> protocol_errors{0};
    std::atomic<std::uint64_t> overloaded{0};
    std::atomic<std::uint64_t> slow_consumer_disconnects{0};
    std::atomic<std::uint64_t> send_timeout_disconnects{0};
    std::atomic<std::uint64_t> batches{0};
    std::atomic<std::uint64_t> commands_staged{0};
    std::atomic<std::uint64_t> events_published{0};
    std::atomic<std::uint64_t> max_batch_seen{0};
    std::atomic<std::uint64_t> subscribers{0};
    std::atomic<std::uint64_t> truncated_frames{0};
    std::atomic<std::uint64_t> internal_errors{0};
};

// One client connection. Shared by its reader thread, writer thread, queued requests and the
// subscriber list; the socket is closed when the last reference goes away.
class Session {
public:
    Session(std::uint64_t id, Socket socket, std::size_t capacity, std::size_t byte_limit)
        : id_(id), socket_(std::move(socket)), out_(capacity), byte_limit_(byte_limit) {}

    [[nodiscard]] int fd() const noexcept { return socket_.fd(); }
    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    BoundedQueue<Frame>& out() noexcept { return out_; }

    // Never blocks. On a full queue the connection is marked overflowed: one final control
    // frame is queued in the reserved slot and the queue is closed, so the writer flushes what
    // is queued and disconnects. Returns false when `frame` was not queued.
    bool enqueue(const Frame& frame, Counters& counters) {
        const std::size_t bytes = frame->size();
        if (queued_bytes_.fetch_add(bytes) + bytes <= byte_limit_ && out_.try_push(frame)) {
            return true;
        }
        queued_bytes_.fetch_sub(bytes);
        if (!overflowed_.exchange(true) && !out_.closed()) {
            const std::string notice =
                subscribed.load() ? resync_message("slow_consumer", last_event_queued.load())
                                  : error_message(ErrorCode::ServerOverloaded,
                                                  "outbound queue full; disconnecting slow reader");
            (void)out_.push_reserved(make_frame(notice));
            out_.close();
            counters.slow_consumer_disconnects.fetch_add(1);
        }
        return false;
    }

    // Called by the writer after a frame has been sent (or dropped).
    void sent(std::size_t bytes) noexcept { queued_bytes_.fetch_sub(bytes); }

    std::atomic<bool> input_done{false};
    std::atomic<bool> subscribed{false};  // written by the sequencer only
    std::atomic<EventSeq> last_event_queued{0};
    std::atomic<std::int64_t> in_flight{0};  // requests queued to the sequencer, not yet answered
    std::atomic<bool> reader_done{false};
    std::atomic<bool> writer_done{false};
    std::thread reader;
    std::thread writer;

private:
    std::uint64_t id_;
    Socket socket_;
    BoundedQueue<Frame> out_;
    std::size_t byte_limit_;
    std::atomic<std::size_t> queued_bytes_{0};
    std::atomic<bool> overflowed_{false};
};

using SessionPtr = std::shared_ptr<Session>;

struct ReplyOnly {
    Frame frame;  // a pre-built reply (e.g. a parse error) delivered in request order
};
using Message = std::variant<Command, SnapshotRequest, SubscribeRequest, ReplyOnly>;

struct Request {
    SessionPtr session;
    Message message;
};

// Durable or in-memory exchange behind one interface.
class Backend {
public:
    Backend(const ServerOptions& options, RecoveryReport& report) {
        if (options.journal) {
            durable_ = DurableExchange::open(*options.journal, &report);
        } else {
            memory_ = std::make_unique<ExchangeStateMachine>(options.state);
        }
    }
    void stage(const Command& cmd, CommandResult& out) {
        if (durable_) {
            durable_->stage(cmd, out);
        } else {
            memory_->process(cmd, out);
        }
    }
    ErrorCode flush() { return durable_ ? durable_->flush() : ErrorCode::None; }
    [[nodiscard]] bool failed() const { return durable_ && durable_->failed(); }
    [[nodiscard]] std::string failure() const { return durable_ ? durable_->failure_reason() : ""; }
    [[nodiscard]] const ExchangeStateMachine& state() const {
        return durable_ ? durable_->state() : *memory_;
    }

private:
    std::unique_ptr<DurableExchange> durable_;
    std::unique_ptr<ExchangeStateMachine> memory_;
};

std::pair<std::string, std::string> correlation_of(const Message& m) {
    if (const auto* cmd = std::get_if<Command>(&m)) {
        return {request_id_of(*cmd), std::string(to_string(type_of(*cmd)))};
    }
    if (const auto* snap = std::get_if<SnapshotRequest>(&m)) {
        return {snap->request_id, "snapshot_request"};
    }
    if (const auto* sub = std::get_if<SubscribeRequest>(&m)) {
        return {sub->request_id, "subscribe"};
    }
    return {"", ""};
}

}  // namespace

class Server::Impl {
public:
    explicit Impl(ServerOptions options)
        : opts_(std::move(options)), backend_(opts_, recovery_), ingress_(opts_.ingress_capacity) {
        if (opts_.max_clients == 0 || opts_.ingress_capacity == 0 || opts_.outbound_capacity == 0 ||
            opts_.max_batch == 0 || opts_.outbound_byte_limit == 0) {
            throw std::invalid_argument("server limits must be positive");
        }
        const bool loopback = opts_.host == "localhost" || opts_.host.rfind("127.", 0) == 0;
        if (!loopback && !opts_.allow_remote) {
            throw std::invalid_argument(
                "refusing to bind " + opts_.host +
                ": the protocol has no authentication; use a loopback address or allow_remote");
        }
        if (opts_.max_frame_bytes < 1024 || opts_.max_frame_bytes > kMaxServerFrame) {
            throw std::invalid_argument("max_frame_bytes must be in [1024, 16 MiB]");
        }
        listener_ = listen_tcp(opts_.host, opts_.port);
        port_ = local_port(listener_.fd());
        // Non-blocking listener: a connection aborted between poll() and accept() must not
        // leave the acceptor (and therefore stop()) blocked inside accept().
        ::fcntl(listener_.fd(), F_SETFL, ::fcntl(listener_.fd(), F_GETFL, 0) | O_NONBLOCK);
        published_event_ = staged_event_ = backend_.state().last_command_sequence();
    }

    ~Impl() { stop(); }

    void start() {
        if (started_.exchange(true)) {
            return;
        }
        running_ = true;
        sequencer_ = std::thread([this] { sequencer_loop(); });
        acceptor_ = std::thread([this] { acceptor_loop(); });
    }

    void stop() {
        if (!started_.load() || stopped_.exchange(true)) {
            return;
        }
        // 1. Stop accepting.
        stopping_ = true;
        if (acceptor_.joinable()) {
            acceptor_.join();
        }
        listener_.reset();
        // 2. Stop reading: no new requests enter the ingress queue.
        // The acceptor (the only other user of sessions_) has exited, so take the list.
        std::vector<SessionPtr> sessions;
        {
            std::lock_guard<std::mutex> lock(sessions_mu_);
            sessions.swap(sessions_);
        }
        for (const auto& s : sessions) {
            ::shutdown(s->fd(), SHUT_RD);
        }
        for (const auto& s : sessions) {
            if (s->reader.joinable()) {
                s->reader.join();
            }
        }
        // 3. The sequencer processes everything already queued, then makes the final flush.
        ingress_.close();
        if (sequencer_.joinable()) {
            sequencer_.join();
        }
        // 4. Writers deliver what is queued (bounded by the send timeout), then close.
        for (const auto& s : sessions) {
            s->out().close();
        }
        for (const auto& s : sessions) {
            if (s->writer.joinable()) {
                s->writer.join();
            }
        }
        running_ = false;
    }

    ServerOptions opts_;
    RecoveryReport recovery_;
    Backend backend_;
    Socket listener_;
    std::uint16_t port_ = 0;
    BoundedQueue<Request> ingress_;
    Counters counters_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> running_{false};

private:
    // ---- acceptor ------------------------------------------------------------------------

    void reap_finished() {
        std::lock_guard<std::mutex> lock(sessions_mu_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            const SessionPtr& s = *it;
            if (s->reader_done.load() && s->writer_done.load()) {
                s->reader.join();
                s->writer.join();
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void acceptor_loop() {
        while (!stopping_.load()) {
            pollfd pfd{listener_.fd(), POLLIN, 0};
            const int rc = ::poll(&pfd, 1, static_cast<int>(kPollTick.count()));
            reap_finished();
            if (rc <= 0 || (pfd.revents & POLLIN) == 0) {
                continue;
            }
            Socket sock(::accept(listener_.fd(), nullptr, nullptr));
            if (!sock.valid()) {
                if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                    // Out of descriptors/memory: back off instead of spinning on a readable
                    // listener; reaping finished sessions frees descriptors.
                    std::this_thread::sleep_for(kPollTick);
                }
                continue;
            }
            // Accepted sockets inherit O_NONBLOCK on BSD/macOS; the per-connection threads use
            // blocking I/O with timeouts.
            ::fcntl(sock.fd(), F_SETFL, ::fcntl(sock.fd(), F_GETFL, 0) & ~O_NONBLOCK);
            configure_stream(sock.fd(), kReadTick, opts_.send_timeout);
            if (opts_.socket_send_buffer > 0) {
                ::setsockopt(sock.fd(), SOL_SOCKET, SO_SNDBUF, &opts_.socket_send_buffer,
                             sizeof opts_.socket_send_buffer);
            }
            std::lock_guard<std::mutex> lock(sessions_mu_);
            if (sessions_.size() >= opts_.max_clients) {
                counters_.connections_rejected.fetch_add(1);
                const std::string msg = encode_frame(error_message(
                    ErrorCode::ServerOverloaded,
                    "connection limit of " + std::to_string(opts_.max_clients) + " reached"));
                (void)send_all(sock.fd(), msg);
                continue;  // closes the socket
            }
            counters_.connections_accepted.fetch_add(1);
            auto s = std::make_shared<Session>(++next_session_, std::move(sock),
                                               opts_.outbound_capacity, opts_.outbound_byte_limit);
            s->reader = std::thread([this, s] { reader_loop(s); });
            s->writer = std::thread([this, s] { writer_loop(s); });
            sessions_.push_back(std::move(s));
        }
    }

    // ---- per-connection reader / writer --------------------------------------------------

    void submit(const SessionPtr& s, Message message) {
        s->in_flight.fetch_add(1);
        auto [rid, type] = correlation_of(message);
        if (ingress_.try_push(Request{s, std::move(message)})) {
            return;
        }
        s->in_flight.fetch_sub(1);
        counters_.overloaded.fetch_add(1);
        (void)s->enqueue(
            make_frame(error_message(
                ErrorCode::ServerOverloaded,
                "server ingress queue full; request not processed, safe to retry", rid, type)),
            counters_);
    }

    void reader_loop(const SessionPtr& s) {
        FrameDecoder decoder(kMaxClientFrame);
        std::vector<char> buf(64 * 1024);
        std::string payload;
        std::optional<Clock::time_point> partial_since;
        while (!stopping_.load() && !s->out().closed()) {
            const ssize_t n = ::recv(s->fd(), buf.data(), buf.size(), 0);
            if (n > 0) {
                decoder.feed(buf.data(), static_cast<std::size_t>(n));
                bool fatal = false;
                while (true) {
                    const auto status = decoder.next(payload);
                    if (status == FrameDecoder::Status::NeedMore) {
                        break;
                    }
                    if (status == FrameDecoder::Status::Error) {
                        // The stream cannot be resynchronized after a bad length: reply, close.
                        counters_.protocol_errors.fetch_add(1);
                        submit(s, ReplyOnly{make_frame(error_message(
                                      ErrorCode::InvalidRequest,
                                      "protocol error: " + decoder.error() + "; closing"))});
                        fatal = true;
                        break;
                    }
                    counters_.requests.fetch_add(1);
                    auto parsed = parse_client_message(payload);
                    if (auto* err = std::get_if<codec::ParseError>(&parsed)) {
                        submit(s, ReplyOnly{make_frame(error_message(*err))});
                    } else {
                        std::visit([&](auto&& m) { submit(s, Message{std::move(m)}); },
                                   std::get<ClientMessage>(std::move(parsed)));
                    }
                }
                if (fatal) {
                    break;
                }
                if (!decoder.has_partial()) {
                    partial_since.reset();
                } else if (!partial_since) {
                    partial_since = Clock::now();
                } else if (Clock::now() - *partial_since > opts_.partial_frame_timeout) {
                    // A frame trickled in slower than the deadline (slowloris): disconnect.
                    counters_.protocol_errors.fetch_add(1);
                    submit(s, ReplyOnly{make_frame(error_message(
                                  ErrorCode::InvalidRequest,
                                  "protocol error: incomplete frame timed out; closing"))});
                    break;
                }
                continue;
            }
            if (n == 0) {
                break;  // peer closed its sending side
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (partial_since && Clock::now() - *partial_since > opts_.partial_frame_timeout) {
                    counters_.protocol_errors.fetch_add(1);
                    submit(s, ReplyOnly{make_frame(error_message(
                                  ErrorCode::InvalidRequest,
                                  "protocol error: incomplete frame timed out; closing"))});
                    break;
                }
                continue;
            }
            break;  // connection reset or other error
        }
        s->input_done = true;
        s->reader_done = true;
    }

    void writer_loop(const SessionPtr& s) {
        std::vector<Frame> items;
        bool healthy = true;
        while (healthy) {
            items.clear();
            (void)s->out().pop_batch(items, 64, kPollTick);
            for (const auto& f : items) {
                if (healthy) {
                    // Overall per-frame budget: the send timeout plus 1 s per MiB.
                    const auto budget =
                        opts_.send_timeout +
                        std::chrono::milliseconds(static_cast<std::int64_t>(f->size() >> 10U));
                    const int err = send_all(s->fd(), *f, budget);
                    if (err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT) {
                        counters_.send_timeout_disconnects.fetch_add(1);  // slow reader
                    }
                    healthy = err == 0;
                }
                s->sent(f->size());
            }
            if (!healthy || s->out().closed_and_empty()) {
                break;
            }
            // End of input (EOF or reset) ends the session once every reply is delivered. A
            // subscriber must keep its sending side open; otherwise a vanished client would
            // hold a connection slot until the next event write failed.
            if (s->input_done.load() && s->in_flight.load() == 0 && s->out().size() == 0) {
                break;
            }
        }
        s->out().close();
        s->out().clear();
        ::shutdown(s->fd(), SHUT_RDWR);
        s->writer_done = true;
    }

    // ---- sequencer (the only thread touching exchange state) -------------------------------

    struct Pending {
        SessionPtr session;
        Frame reply;
        bool state_dependent = false;  // command replies (invalidated by a failed flush)
        std::string request_id;
        std::string command_type;
        Frame event;             // book_update to publish after a successful flush
        bool has_event = false;  // true for accepted, non-duplicate commands
        EventSeq event_seq = 0;
    };

    void sequencer_loop() {
        std::vector<Request> batch;
        while (true) {
            batch.clear();
            (void)ingress_.pop_batch(batch, opts_.max_batch, kPollTick);
            prune_subscribers();
            if (batch.empty()) {
                if (ingress_.closed_and_empty()) {
                    break;
                }
                continue;
            }
            for (Request& req : batch) {
                try {
                    std::visit([&](auto& m) { handle(req.session, m); }, req.message);
                } catch (const std::exception& e) {
                    internal_failure(req.session, e.what());
                }
            }
            safe_commit();
        }
        safe_commit();
    }

    // Unexpected exceptions must not terminate the process (std::thread boundary). State may
    // be inconsistent afterwards, so the server stops accepting commands (fail-stop) and
    // answers INTERNAL_ERROR; operators restart it to recover from the journal.
    void internal_failure(const SessionPtr& s, const char* what) {
        counters_.internal_errors.fetch_add(1);
        std::fprintf(stderr, "enginex server: internal error, now fail-stop: %s\n", what);
        fatal_ = true;
        Pending p;
        p.session = s;
        p.reply = make_frame(
            error_message(ErrorCode::InternalError, "internal server error; server is fail-stop"));
        pending_.push_back(std::move(p));
    }

    void safe_commit() {
        try {
            commit_pending();
        } catch (const std::exception& e) {
            counters_.internal_errors.fetch_add(1);
            std::fprintf(stderr, "enginex server: internal error during commit: %s\n", e.what());
            fatal_ = true;
            for (Pending& p : pending_) {
                p.session->in_flight.fetch_sub(1);
            }
            pending_.clear();
        }
    }

    // Drops subscribers whose connection has ended, even when no events are flowing (an idle
    // market must not keep closed sockets alive).
    void prune_subscribers() {
        for (auto it = subscribers_.begin(); it != subscribers_.end();) {
            if ((*it)->out().closed() || (*it)->writer_done.load()) {
                (*it)->subscribed = false;
                it = subscribers_.erase(it);
            } else {
                ++it;
            }
        }
        counters_.subscribers = subscribers_.size();
    }

    // Frames within the configured limit, or nullptr when too large.
    Frame bounded_frame(const std::string& json) const {
        if (json.size() > opts_.max_frame_bytes) {
            return nullptr;
        }
        return std::make_shared<const std::string>(encode_frame(json, opts_.max_frame_bytes));
    }

    // Ack/event for a command whose execution list does not fit in one frame: the same
    // message without "executions", plus "executions_truncated": true and the count. Clients
    // can read the executions with a later snapshot/feed or from the journal.
    Frame truncated_frame(const std::string& json, std::size_t execution_count) {
        codec::Json j = codec::Json::parse(json);
        j["executions"] = codec::Json::array();
        j["executions_truncated"] = true;
        j["execution_count"] = execution_count;
        counters_.truncated_frames.fetch_add(1);
        return bounded_frame(j.dump());
    }

    void handle(const SessionPtr& s, Command& cmd) {
        Pending p;
        p.session = s;
        if (fatal_) {
            p.reply = make_frame(error_message(ErrorCode::InternalError,
                                               "server is fail-stop after an internal error",
                                               request_id_of(cmd), to_string(type_of(cmd))));
            pending_.push_back(std::move(p));
            return;
        }
        backend_.stage(cmd, scratch_);
        counters_.commands_staged.fetch_add(1);
        p.state_dependent = true;
        p.request_id = scratch_.request_id;
        p.command_type = std::string(to_string(scratch_.type));
        const std::string reply = result_message(scratch_, backend_.state());
        p.reply = bounded_frame(reply);
        if (!p.reply) {
            p.reply = truncated_frame(reply, scratch_.executions.size());
        }
        if (scratch_.accepted && !scratch_.duplicate) {
            p.event_seq = ++staged_event_;
            const std::string event = book_update_message(backend_.state(), scratch_, p.event_seq);
            p.event = bounded_frame(event);
            if (!p.event) {
                p.event = truncated_frame(event, scratch_.executions.size());
            }
            p.has_event = true;  // a null frame (levels alone too large) forces a resync
        }
        if (!p.reply) {
            // Even the truncated ack does not fit: answer without details.
            p.reply = make_frame(error_message(ErrorCode::InternalError,
                                               "reply exceeds the frame limit; command applied, "
                                               "retry the request_id to read its outcome",
                                               p.request_id, p.command_type));
        }
        pending_.push_back(std::move(p));
    }

    void handle(const SessionPtr& s, ReplyOnly& r) {
        Pending p;
        p.session = s;
        p.reply = std::move(r.frame);
        pending_.push_back(std::move(p));
    }

    // Reads see exactly the durable, published state: flush and publish the batch first.
    void handle(const SessionPtr& s, SnapshotRequest& r) {
        commit_pending();
        Frame f;
        if (backend_.failed()) {
            f = make_frame(error_message(
                ErrorCode::JournalIoError,
                "journal failed (" + backend_.failure() + "); state unavailable until restart",
                r.request_id, "snapshot_request"));
        } else {
            std::optional<SymbolId> symbol;
            if (r.symbol) {
                symbol = backend_.state().find_symbol(*r.symbol);
            }
            if (r.symbol && !symbol) {
                f = make_frame(error_message(ErrorCode::InvalidSymbol, "unknown symbol",
                                             r.request_id, "snapshot_request"));
            } else {
                f = snapshot_frame(r.request_id, symbol, r.depth, false);
            }
        }
        (void)s->enqueue(f, counters_);
        s->in_flight.fetch_sub(1);
    }

    void handle(const SessionPtr& s, SubscribeRequest& r) {
        commit_pending();
        Frame f;
        bool subscribe = false;
        if (backend_.failed()) {
            f = make_frame(error_message(ErrorCode::JournalIoError,
                                         "journal failed; cannot subscribe until restart",
                                         r.request_id, "subscribe"));
        } else if (fatal_) {
            f = make_frame(error_message(ErrorCode::InternalError,
                                         "server is fail-stop after an internal error",
                                         r.request_id, "subscribe"));
        } else if (s->subscribed.load()) {
            f = make_frame(error_message(ErrorCode::InvalidRequest, "already subscribed",
                                         r.request_id, "subscribe"));
        } else {
            f = snapshot_frame(r.request_id, std::nullopt, 0, true);
            subscribe = true;
        }
        // Snapshot and registration happen together on this thread: no event can fall between.
        s->last_event_queued = published_event_;
        if (s->enqueue(f, counters_) && subscribe) {
            s->subscribed = true;
            subscribers_.push_back(s);
        }
        s->in_flight.fetch_sub(1);
    }

    Frame snapshot_frame(const std::string& request_id, std::optional<SymbolId> symbol,
                         std::size_t depth, bool subscribed) {
        try {
            if (Frame f = bounded_frame(snapshot_message(backend_.state(), request_id, symbol,
                                                         depth, published_event_, subscribed))) {
                return f;
            }
        } catch (const std::length_error&) {
        }
        return make_frame(error_message(ErrorCode::InternalError,
                                        "snapshot exceeds the frame limit; request less depth",
                                        request_id, subscribed ? "subscribe" : "snapshot_request"));
    }

    void publish(EventSeq seq, const Frame& f) {
        counters_.events_published.fetch_add(1);
        if (!f) {
            // The event cannot be framed at all: every subscriber would see a gap, so end
            // their feeds explicitly; they resubscribe for a fresh snapshot.
            const Frame notice = make_frame(resync_message("event_too_large", seq - 1));
            for (const SessionPtr& s : subscribers_) {
                (void)s->enqueue(notice, counters_);
                s->out().close();
                s->subscribed = false;
            }
            subscribers_.clear();
            return;
        }
        for (auto it = subscribers_.begin(); it != subscribers_.end();) {
            const SessionPtr& s = *it;
            if (s->out().closed() || !s->enqueue(f, counters_)) {
                s->subscribed = false;
                it = subscribers_.erase(it);
                continue;
            }
            s->last_event_queued = seq;
            ++it;
        }
    }

    void commit_pending() {
        if (pending_.empty()) {
            return;
        }
        const ErrorCode err = backend_.flush();
        counters_.batches.fetch_add(1);
        if (err != ErrorCode::None && !failure_logged_) {
            // Operator detail stays local; clients get a generic, stable error (see below).
            std::fprintf(stderr, "enginex server: journal failure, now fail-stop: %s\n",
                         backend_.failure().c_str());
            failure_logged_ = true;
        }
        const auto size = static_cast<std::uint64_t>(pending_.size());
        if (size > counters_.max_batch_seen.load()) {
            counters_.max_batch_seen = size;
        }
        for (Pending& p : pending_) {
            Frame reply = p.reply;
            if (err != ErrorCode::None && p.state_dependent) {
                reply = make_frame(error_message(
                    ErrorCode::JournalIoError,
                    "journal failure; outcome unknown, retry with the same request_id after "
                    "restart",
                    p.request_id, p.command_type));
            }
            (void)p.session->enqueue(reply, counters_);
            p.session->in_flight.fetch_sub(1);
            if (err == ErrorCode::None && p.has_event) {
                published_event_ = p.event_seq;
                publish(p.event_seq, p.event);
            }
        }
        pending_.clear();
        if (err != ErrorCode::None) {
            staged_event_ = published_event_;
        }
    }

    std::thread acceptor_;
    std::thread sequencer_;
    std::mutex sessions_mu_;
    std::vector<SessionPtr> sessions_;
    std::uint64_t next_session_ = 0;
    // Sequencer-only state.
    std::vector<SessionPtr> subscribers_;
    std::vector<Pending> pending_;
    CommandResult scratch_;
    EventSeq published_event_ = 0;
    EventSeq staged_event_ = 0;
    bool failure_logged_ = false;
    bool fatal_ = false;
};

Server::Server(ServerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Server::~Server() = default;

void Server::start() {
    impl_->start();
}

void Server::stop() {
    impl_->stop();
}

std::uint16_t Server::port() const noexcept {
    return impl_->port_;
}

const RecoveryReport& Server::recovery() const noexcept {
    return impl_->recovery_;
}

bool Server::running() const noexcept {
    return impl_->running_.load();
}

bool Server::journal_failed() const {
    return impl_->backend_.failed();
}

const ExchangeStateMachine& Server::state() const {
    if (impl_->running_.load()) {
        throw std::logic_error("Server::state() is only available while stopped");
    }
    return impl_->backend_.state();
}

ServerStats Server::stats() const {
    const Counters& c = impl_->counters_;
    ServerStats s;
    s.connections_accepted = c.connections_accepted.load();
    s.connections_rejected = c.connections_rejected.load();
    s.requests = c.requests.load();
    s.protocol_errors = c.protocol_errors.load();
    s.overloaded = c.overloaded.load();
    s.slow_consumer_disconnects = c.slow_consumer_disconnects.load();
    s.send_timeout_disconnects = c.send_timeout_disconnects.load();
    s.batches = c.batches.load();
    s.commands_staged = c.commands_staged.load();
    s.events_published = c.events_published.load();
    s.max_batch_seen = c.max_batch_seen.load();
    s.subscribers = c.subscribers.load();
    s.truncated_frames = c.truncated_frames.load();
    s.internal_errors = c.internal_errors.load();
    return s;
}

}  // namespace enginex::net
