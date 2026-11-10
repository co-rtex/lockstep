#include "enginex/mirror.hpp"

#include <thread>
#include <vector>

#include "enginex/client.hpp"

namespace enginex::net {

namespace {

using codec::Json;

std::string type_of(const Json& m) { return m.value("type", std::string()); }

// Levels of one side, best first, at most `depth` (0 = all).
template <typename Map>
std::vector<std::pair<std::int64_t, std::pair<std::int64_t, std::int64_t>>> side_levels(
    const Map& book, char side, std::size_t depth) {
    std::vector<std::pair<std::int64_t, std::pair<std::int64_t, std::int64_t>>> out;
    auto take = [&](auto first, auto last) {
        for (auto it = first; it != last; ++it) {
            if (it->first.first != side) {
                continue;
            }
            if (depth != 0 && out.size() >= depth) {
                break;
            }
            out.emplace_back(it->first.second, it->second);
        }
    };
    if (side == 'B') {
        take(book.rbegin(), book.rend());  // keys ('B', price) ascending → reverse = best bid
    } else {
        take(book.begin(), book.end());
    }
    return out;
}

}  // namespace

std::string_view to_string(FeedMirror::State s) noexcept {
    switch (s) {
        case FeedMirror::State::AwaitingBaseline: return "awaiting_baseline";
        case FeedMirror::State::Live: return "live";
        case FeedMirror::State::Stale: return "stale";
    }
    return "unknown";
}

std::string_view to_string(FeedMirror::Verdict v) noexcept {
    switch (v) {
        case FeedMirror::Verdict::Baseline: return "baseline";
        case FeedMirror::Verdict::Applied: return "applied";
        case FeedMirror::Verdict::Rejected: return "rejected";
        case FeedMirror::Verdict::Gap: return "gap";
        case FeedMirror::Verdict::ResyncRequired: return "resync_required";
        case FeedMirror::Verdict::SubscribeFailed: return "subscribe_failed";
        case FeedMirror::Verdict::Verified: return "verified";
        case FeedMirror::Verdict::Mismatch: return "mismatch";
        case FeedMirror::Verdict::Ignored: return "ignored";
    }
    return "unknown";
}

FeedMirror::Book FeedMirror::load_book(const Json& b) {
    Book book;
    for (const auto& [field, side] : {std::pair{"bids", 'B'}, std::pair{"asks", 'S'}}) {
        for (const auto& l : b.at(field)) {
            book[{side, l.at("price").get<std::int64_t>()}] = {
                l.at("quantity").get<std::int64_t>(), l.at("order_count").get<std::int64_t>()};
        }
    }
    return book;
}

FeedMirror::Verdict FeedMirror::stale(Verdict v, std::string why) {
    state_ = State::Stale;
    problem_ = std::move(why);
    return v;
}

FeedMirror::Verdict FeedMirror::on_message(const Json& m) {
    const std::string type = type_of(m);
    if (type == "snapshot_response") {
        if (m.value("subscribed", false)) {
            if (state_ == State::Live) {
                return stale(Verdict::Mismatch, "second subscribe snapshot on a live feed");
            }
            std::map<std::string, Book> books;
            for (const auto& b : m.at("books")) {
                books[b.at("symbol").get<std::string>()] = load_book(b);
            }
            books_ = std::move(books);
            last_event_ = m.at("event_watermark").get<std::uint64_t>();
            state_ = State::Live;
            ++stats_.baselines;
            return Verdict::Baseline;
        }
        return state_ == State::Live ? verify(m) : Verdict::Ignored;
    }
    if (type == "book_update") {
        const auto seq = m.at("event_sequence").get<std::uint64_t>();
        if (state_ != State::Live) {
            ++stats_.rejected;
            return Verdict::Rejected;
        }
        if (seq != last_event_ + 1) {
            ++stats_.gaps;
            return stale(Verdict::Gap, "event " + std::to_string(seq) + " after " +
                                           std::to_string(last_event_));
        }
        Book& book = books_[m.at("symbol").get<std::string>()];
        for (const auto& l : m.at("levels")) {
            const char side = l.at("side").get<std::string>() == "BUY" ? 'B' : 'S';
            const Key key{side, l.at("price").get<std::int64_t>()};
            const auto qty = l.at("quantity").get<std::int64_t>();
            if (qty == 0) {
                book.erase(key);
            } else {
                book[key] = {qty, l.at("order_count").get<std::int64_t>()};
            }
        }
        last_event_ = seq;
        ++stats_.applied;
        return Verdict::Applied;
    }
    if (type == "resync_required") {
        ++stats_.resyncs;
        return stale(Verdict::ResyncRequired,
                     "resync_required: " + m.value("reason", std::string("?")));
    }
    if (type == "error" && m.value("command_type", std::string()) == "subscribe") {
        ++stats_.subscribe_failures;
        problem_ = "subscribe failed: " + m.value("code", std::string("?")) + " " +
                   m.value("message", std::string());
        return Verdict::SubscribeFailed;
    }
    return Verdict::Ignored;
}

FeedMirror::Verdict FeedMirror::verify(const Json& snap) {
    const auto watermark = snap.at("event_watermark").get<std::uint64_t>();
    if (watermark != last_event_) {
        ++stats_.mismatches;
        return stale(Verdict::Mismatch, "in-band snapshot at watermark " +
                                            std::to_string(watermark) + " but mirror at " +
                                            std::to_string(last_event_));
    }
    const auto depth = snap.value("depth", std::size_t{0});
    for (const auto& b : snap.at("books")) {
        const std::string symbol = b.at("symbol").get<std::string>();
        const Book expected = load_book(b);
        const auto it = books_.find(symbol);
        const Book empty;
        const Book& have = it == books_.end() ? empty : it->second;
        for (const char side : {'B', 'S'}) {
            if (side_levels(have, side, depth) != side_levels(expected, side, 0)) {
                ++stats_.mismatches;
                return stale(Verdict::Mismatch, "book " + symbol + " side " +
                                                    std::string(1, side) + " differs at watermark " +
                                                    std::to_string(watermark));
            }
        }
    }
    ++stats_.verified;
    return Verdict::Verified;
}

void FeedMirror::on_disconnect() {
    ++stats_.disconnects;
    if (state_ == State::Live) {
        (void)stale(Verdict::Ignored, "disconnected");
    }
}

void FeedMirror::reset_for_resubscribe() {
    state_ = State::AwaitingBaseline;
    books_.clear();
    last_event_ = 0;
}

std::string FeedMirror::canonical() const {
    std::string out;
    for (const auto& [symbol, book] : books_) {
        for (const auto& [key, level] : book) {
            out += symbol + " " + key.first + " " + std::to_string(key.second) + " " +
                   std::to_string(level.first) + " " + std::to_string(level.second) + "\n";
        }
    }
    return out;
}

MirrorReport run_mirror(const MirrorOptions& o, FeedMirror& mirror) {
    using Clock = std::chrono::steady_clock;
    using std::chrono::milliseconds;
    MirrorReport report;
    const auto end = Clock::now() + o.duration;
    std::uint64_t verify_no = 0;
    bool stalled = false;
    std::size_t resubscribes = 0;
    while (true) {
        mirror.reset_for_resubscribe();
        Client c = Client::connect(o.host, o.port);
        ++report.connections;
        if (!c.send(Json{{"type", "subscribe"}, {"request_id", "mirror-subscribe"}})) {
            mirror.on_disconnect();
        }
        auto next_verify = Clock::now() + o.verify_every;
        bool final_sent = false;
        std::string final_id;
        // Read until the session ends or the mirror needs a new connection.
        while (true) {
            const auto now = Clock::now();
            if (!final_sent && now >= end && mirror.state() == FeedMirror::State::Live) {
                final_id = "mirror-final";
                final_sent = c.send(Json{{"type", "snapshot_request"},
                                         {"request_id", final_id},
                                         {"depth", 0}});
            } else if (!final_sent && now >= next_verify &&
                       mirror.state() == FeedMirror::State::Live) {
                (void)c.send(Json{{"type", "snapshot_request"},
                                  {"request_id", "mirror-verify-" + std::to_string(++verify_no)},
                                  {"depth", 0}});
                next_verify = now + o.verify_every;
            }
            if (now >= end + milliseconds(2000)) {
                break;  // final verification never arrived
            }
            const auto msg = c.receive(milliseconds(50));
            if (!msg) {
                if (c.closed()) {
                    mirror.on_disconnect();
                    break;
                }
                if (mirror.state() == FeedMirror::State::AwaitingBaseline && now >= end) {
                    break;  // never got a baseline
                }
                continue;
            }
            const FeedMirror::Verdict v = mirror.on_message(*msg);
            if (v == FeedMirror::Verdict::Baseline && !stalled &&
                o.stall_after_first_baseline.count() > 0) {
                stalled = true;
                std::this_thread::sleep_for(o.stall_after_first_baseline);
            }
            if (final_sent && msg->value("request_id", std::string()) == final_id &&
                msg->value("type", std::string()) == "snapshot_response") {
                report.final_verified = v == FeedMirror::Verdict::Verified;
                break;
            }
            if (mirror.state() == FeedMirror::State::Stale ||
                v == FeedMirror::Verdict::SubscribeFailed) {
                break;  // needs a fresh connection and snapshot
            }
        }
        c.close();
        const bool done = final_sent || Clock::now() >= end;
        if (done || resubscribes >= o.max_resubscribes) {
            break;
        }
        ++resubscribes;
    }
    report.stats = mirror.stats();
    report.last_event = mirror.last_event();
    report.state = std::string(to_string(mirror.state()));
    report.last_problem = mirror.last_problem();
    return report;
}

}  // namespace enginex::net
