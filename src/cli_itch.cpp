// `enginex itch ...`: historical ITCH 5.0 market-data tools. Independent of the synthetic
// exchange's commands: these read *outbound market data*, they do not place orders.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <type_traits>

#include "cli.hpp"
#include "enginex/itch/book.hpp"
#include "enginex/itch/byte_reader.hpp"
#include "enginex/itch/mold_sim.hpp"
#include "enginex/itch/replay.hpp"
#include "enginex/itch/synthetic.hpp"
#include "enginex/json_codec.hpp"

namespace enginex::cli {

namespace {

using codec::Json;
using namespace enginex::itch;
using Side = enginex::itch::Side;  // market-data side, not the synthetic exchange's enginex::Side

constexpr int kExitStopped = 1;

std::string px(Price4 p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%04u", p / 10000, p % 10000);
    return buf;
}

std::string sym(const Symbol& s) { return std::string(trim_alpha(s)); }

Json level_json(const LevelView& l) {
    return Json{{"price", l.price}, {"shares", l.shares}, {"orders", l.orders}};
}

std::unique_ptr<std::ifstream> open_input(const std::string& path) {
    auto in = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*in) {
        throw std::runtime_error("cannot open " + path);
    }
    return in;
}

int cmd_generate(const std::vector<std::string>& argv) {
    const Args a(argv, {"out", "seed", "messages", "symbols", "anomalies-per-million"}, {});
    SyntheticOptions o;
    o.seed = a.get_u64("seed", 1);
    o.messages = a.get_u64("messages", 100'000);
    const std::uint64_t symbols = a.get_u64("symbols", 8);
    if (symbols == 0 || symbols > 65535) {
        throw UsageError("--symbols must be in [1, 65535]");
    }
    o.symbols = static_cast<std::uint16_t>(symbols);
    const std::string out = a.require("out");
    const std::uint64_t anomalies = a.get_u64("anomalies-per-million", 0);
    if (anomalies > 1'000'000) {
        throw UsageError("--anomalies-per-million must be in [0, 1000000]");
    }
    o.anomalies_per_million = static_cast<std::uint32_t>(anomalies);
    const std::vector<std::uint8_t> bytes = generate_file(o);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.close();
    if (!f) {
        throw std::runtime_error("cannot write " + out);
    }
    std::cout << Json{{"type", "itch_generate"},
                      {"out", out},
                      {"seed", o.seed},
                      {"lifecycle_messages", o.messages},
                      {"symbols", o.symbols},
                      {"anomalies_per_million", o.anomalies_per_million},
                      {"bytes", bytes.size()},
                      {"note", "synthetic ITCH 5.0 fixture; not historical market data"}}
                     .dump()
              << "\n";
    return 0;
}

Json replay_json(const std::string& input, const ReplayResult& r, const BookBuilder& b,
                 std::size_t depth, const std::optional<std::string>& only_symbol) {
    const BookStats& s = b.stats();
    Json j;
    j["type"] = "itch_replay";
    j["input"] = input;
    j["bytes_read"] = r.bytes_read;
    j["bytes_processed"] = r.bytes_processed;
    j["frames"] = r.frames;
    j["decoded"] = r.decoded;
    Json errs = Json::object();
    for (const DecodeError e : {DecodeError::Empty, DecodeError::UnknownType,
                                DecodeError::LengthMismatch, DecodeError::InvalidField}) {
        errs[std::string(to_string(e))] = r.decode_errors[static_cast<std::size_t>(e)];
    }
    j["decode_errors"] = errs;
    Json unknown = Json::object();
    for (std::size_t t = 0; t < 256; ++t) {
        if (r.unknown_types[t] != 0) {
            unknown[std::to_string(t)] = r.unknown_types[t];
        }
    }
    j["unknown_type_bytes"] = unknown;
    j["frame_error"] = std::string(to_string(r.frame_error));
    if (r.frame_error != FrameError::None) {
        j["frame_error_offset"] = r.frame_error_offset;
    }
    j["stopped"] = r.stopped ? Json(*r.stopped) : Json(nullptr);
    j["stopped_at_message"] = r.stopped_at_message;
    Json by_type = Json::object();
    for (const char t : kKnownTypes) {
        if (const auto n = s.messages_by_type[static_cast<unsigned char>(t)]; n != 0) {
            by_type[std::string(1, t)] = n;
        }
    }
    j["messages_by_type"] = by_type;
    Json anomalies = Json::object();
    for (std::size_t k = 1; k < kAnomalyKinds; ++k) {
        anomalies[std::string(to_string(static_cast<Anomaly>(k)))] = s.anomalies[k];
    }
    j["anomalies"] = anomalies;
    j["book_stats"] = Json{{"applied", s.applied},
                           {"rejected", s.rejected},
                           {"adds", s.adds},
                           {"executions", s.executions},
                           {"executed_shares", s.executed_shares},
                           {"printable_executed_shares", s.printable_executed_shares},
                           {"cancels", s.cancels},
                           {"canceled_shares", s.canceled_shares},
                           {"deletes", s.deletes},
                           {"replaces", s.replaces},
                           {"non_displayed_trade_shares", s.non_displayed_trade_shares},
                           {"cross_shares", s.cross_shares},
                           {"broken_trades", s.broken_trades},
                           {"live_orders", s.live_orders},
                           {"peak_live_orders", s.peak_live_orders},
                           {"timestamp_regressions", s.timestamp_regressions},
                           {"last_timestamp_ns", s.last_timestamp_ns},
                           {"last_system_event", s.last_system_event == 0
                                                     ? std::string()
                                                     : std::string(1, s.last_system_event)}};
    j["state_hash"] = b.state_hash();
    Json books = Json::array();
    for (const std::uint16_t locate : b.locates()) {
        const auto symbol = b.symbol(locate);
        if (only_symbol && symbol != only_symbol) {
            continue;
        }
        const TopOfBook t = b.top(locate);
        Json bk;
        bk["locate"] = locate;
        bk["symbol"] = symbol ? Json(*symbol) : Json(nullptr);
        bk["best_bid"] = t.bid ? level_json(*t.bid) : Json(nullptr);
        bk["best_ask"] = t.ask ? level_json(*t.ask) : Json(nullptr);
        bk["locked"] = t.locked();
        bk["crossed"] = t.crossed();
        bk["bid_levels"] = b.level_count(locate, Side::Buy);
        bk["ask_levels"] = b.level_count(locate, Side::Sell);
        Json bids = Json::array();
        Json asks = Json::array();
        for (const auto& l : b.depth(locate, Side::Buy, depth)) bids.push_back(level_json(l));
        for (const auto& l : b.depth(locate, Side::Sell, depth)) asks.push_back(level_json(l));
        bk["bids"] = bids;
        bk["asks"] = asks;
        books.push_back(bk);
    }
    j["books"] = books;
    j["price_unit"] = "Price(4): integer, 4 implied decimals";
    return j;
}

void print_text(const Json& j) {
    std::cout << "ITCH 5.0 replay of " << j["input"].get<std::string>() << "\n"
              << "  bytes " << j["bytes_processed"] << ", frames " << j["frames"] << ", decoded "
              << j["decoded"] << "\n"
              << "  decode errors " << j["decode_errors"].dump() << "\n"
              << "  messages by type " << j["messages_by_type"].dump() << "\n"
              << "  anomalies " << j["anomalies"].dump() << "\n"
              << "  live orders " << j["book_stats"]["live_orders"] << " (peak "
              << j["book_stats"]["peak_live_orders"] << ")\n"
              << "  state hash " << j["state_hash"].get<std::string>() << "\n";
    if (!j["stopped"].is_null()) {
        std::cout << "  STOPPED at message " << j["stopped_at_message"] << ": "
                  << j["stopped"].get<std::string>() << "\n";
    }
    for (const auto& bk : j["books"]) {
        std::cout << "  [" << bk["locate"] << "] "
                  << (bk["symbol"].is_null() ? std::string("-") : bk["symbol"].get<std::string>())
                  << "  levels " << bk["bid_levels"] << "/" << bk["ask_levels"]
                  << (bk["crossed"].get<bool>() ? "  CROSSED" : "")
                  << (bk["locked"].get<bool>() ? "  LOCKED" : "") << "\n";
        const auto& bids = bk["bids"];
        const auto& asks = bk["asks"];
        const std::size_t rows = std::max(bids.size(), asks.size());
        for (std::size_t i = 0; i < rows; ++i) {
            char line[128];
            const auto cell = [](const Json& l) {
                char b[64];
                std::snprintf(b, sizeof b, "%8llu @ %-12s",
                              static_cast<unsigned long long>(l["shares"].get<std::uint64_t>()),
                              px(l["price"].get<Price4>()).c_str());
                return std::string(b);
            };
            std::snprintf(line, sizeof line, "      %-26s | %s",
                          i < bids.size() ? cell(bids[i]).c_str() : "",
                          i < asks.size() ? cell(asks[i]).c_str() : "");
            std::cout << line << "\n";
        }
    }
}

int cmd_replay_itch(const std::vector<std::string>& argv) {
    const Args a(argv, {"input", "depth", "symbol", "metrics-out", "expected-orders"},
                 {"strict", "json"});
    const std::string input = a.require("input");
    const std::size_t depth = a.get_u64("depth", 5);
    const auto only = a.get("symbol");
    ReplayOptions opts;
    opts.strict = a.flag("strict");
    // Capacity hint: size it near the expected peak of live orders (docs/benchmarks.md shows
    // both too small (rehash spikes) and far too large (cache misses) cost time).
    BookBuilder book(a.get_u64("expected-orders", 0));
    auto in = open_input(input);
    const auto t0 = std::chrono::steady_clock::now();
    const ReplayResult r = replay(*in, book, opts);
    const auto t1 = std::chrono::steady_clock::now();
    if (only && !book.find_locate(*only)) {
        throw UsageError("--symbol " + *only + ": no such symbol in " + input);
    }
    Json j = replay_json(input, r, book, depth, only);
    if (const auto out = a.get("metrics-out")) {
        Json m = j;
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        // Wall time of the whole replay including file reads; not a latency measurement.
        m["timing"] = Json{{"wall_seconds", secs},
                           {"frames_per_second", secs > 0 ? static_cast<double>(r.frames) / secs : 0.0},
                           {"note", "single run, includes I/O; see docs/benchmarks.md for methodology"}};
        std::ofstream f(*out, std::ios::trunc);
        f << m.dump(2) << "\n";
        if (!f) {
            throw std::runtime_error("cannot write " + *out);
        }
    }
    if (a.flag("json")) {
        std::cout << j.dump() << "\n";
    } else {
        print_text(j);
    }
    return r.ok() ? 0 : kExitStopped;
}

Json message_json(const Message& m) {
    const Header& h = header_of(m);
    Json j;
    j["type"] = std::string(1, h.type);
    j["name"] = std::string(type_name(h.type));
    j["locate"] = h.stock_locate;
    j["tracking"] = h.tracking_number;
    j["timestamp_ns"] = h.timestamp_ns;
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (requires { x.stock; }) {
                j["stock"] = sym(x.stock);
            }
            if constexpr (std::is_same_v<T, SystemEvent>) {
                j["event_code"] = std::string(1, x.event_code);
            } else if constexpr (std::is_same_v<T, StockDirectory>) {
                j["market_category"] = std::string(1, x.market_category);
                j["round_lot_size"] = x.round_lot_size;
            } else if constexpr (std::is_same_v<T, StockTradingAction>) {
                j["trading_state"] = std::string(1, x.trading_state);
            } else if constexpr (std::is_same_v<T, AddOrder>) {
                j["order_ref"] = x.order_ref;
                j["side"] = std::string(1, static_cast<char>(x.side));
                j["shares"] = x.shares;
                j["price"] = x.price;
                if (x.attribution) {
                    j["attribution"] = std::string(x.attribution->data(), 4);
                }
            } else if constexpr (std::is_same_v<T, OrderExecuted>) {
                j["order_ref"] = x.order_ref;
                j["executed_shares"] = x.executed_shares;
                j["match_number"] = x.match_number;
            } else if constexpr (std::is_same_v<T, OrderExecutedWithPrice>) {
                j["order_ref"] = x.order_ref;
                j["executed_shares"] = x.executed_shares;
                j["match_number"] = x.match_number;
                j["printable"] = x.printable;
                j["execution_price"] = x.execution_price;
            } else if constexpr (std::is_same_v<T, OrderCancel>) {
                j["order_ref"] = x.order_ref;
                j["canceled_shares"] = x.canceled_shares;
            } else if constexpr (std::is_same_v<T, OrderDelete>) {
                j["order_ref"] = x.order_ref;
            } else if constexpr (std::is_same_v<T, OrderReplace>) {
                j["original_ref"] = x.original_ref;
                j["new_ref"] = x.new_ref;
                j["shares"] = x.shares;
                j["price"] = x.price;
            } else if constexpr (std::is_same_v<T, Trade>) {
                j["shares"] = x.shares;
                j["price"] = x.price;
                j["match_number"] = x.match_number;
            } else if constexpr (std::is_same_v<T, CrossTrade>) {
                j["shares"] = x.shares;
                j["cross_price"] = x.cross_price;
                j["match_number"] = x.match_number;
                j["cross_type"] = std::string(1, x.cross_type);
            } else if constexpr (std::is_same_v<T, BrokenTrade>) {
                j["match_number"] = x.match_number;
            }
        },
        m);
    return j;
}

int cmd_dump(const std::vector<std::string>& argv) {
    const Args a(argv, {"input", "limit"}, {});
    const std::uint64_t limit = a.get_u64("limit", 0);
    auto in = open_input(a.require("input"));
    // Decode only (no book): every frame becomes one JSON line, errors included.
    StreamFramer framer;
    std::uint64_t n = 0;
    std::vector<char> buf(1U << 16U);
    bool done = false;
    while (!done && *in) {
        in->read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto got = static_cast<std::size_t>(in->gcount());
        if (got == 0) {
            break;
        }
        const FrameError e = framer.feed(
            std::span(reinterpret_cast<const std::uint8_t*>(buf.data()), got),
            [&](std::span<const std::uint8_t> p) {
                if (done) {
                    return;
                }
                ++n;
                const DecodeResult d = decode(p);
                if (const auto* err = std::get_if<DecodeError>(&d)) {
                    std::cout << Json{{"frame", n},
                                      {"decode_error", std::string(to_string(*err))},
                                      {"length", p.size()},
                                      {"type_byte", p.empty() ? 0 : p[0]}}
                                     .dump()
                              << "\n";
                } else {
                    Json j = message_json(std::get<Message>(d));
                    j["frame"] = n;
                    std::cout << j.dump() << "\n";
                }
                done = limit != 0 && n >= limit;
            });
        if (e != FrameError::None) {
            break;
        }
    }
    if (!done) {
        if (const FrameError e = framer.finish(); e != FrameError::None) {
            std::cout << Json{{"frame_error", std::string(to_string(e))},
                              {"offset", framer.error_offset()}}
                             .dump()
                      << "\n";
            return kExitStopped;
        }
    }
    return 0;
}

// SIMULATED MoldUDP64 delivery of a file's messages over a lossy channel (mold_sim.hpp).
// Exit 0 only if the rebuilt book equals a direct replay of exactly the delivered prefix (never
// silently wrong) and, with recovery, the session ended with every message delivered.
int cmd_mold_sim(const std::vector<std::string>& argv) {
    const Args a(argv, {"input", "seed", "loss-percent", "duplicate-percent", "reorder-window", "mtu"},
                 {"no-recovery"});
    const std::string input = a.require("input");
    mold::ChannelOptions o;
    o.seed = a.get_u64("seed", 1);
    o.loss_percent = static_cast<std::uint32_t>(std::min<std::uint64_t>(a.get_u64("loss-percent", 3), 100));
    o.duplicate_percent =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(a.get_u64("duplicate-percent", 5), 100));
    o.reorder_window = a.get_u64("reorder-window", 8);
    o.mtu = a.get_u64("mtu", 1400);
    o.recovery = !a.flag("no-recovery");
    if (o.mtu < 100 || o.mtu > 65535) {
        throw UsageError("--mtu must be in [100, 65535]");
    }
    // Decode the whole file first (the simulation re-encodes messages into packets).
    std::vector<Message> stream;
    BookBuilder unused;
    ReplayOptions ro;
    ro.strict = true;
    ro.on_message = [&](const Message& m, const ApplyOutcome&) { stream.push_back(m); };
    auto in = open_input(input);
    const ReplayResult rr = replay(*in, unused, ro);
    if (!rr.ok()) {
        throw std::runtime_error("input is not a clean ITCH file: " + *rr.stopped);
    }
    const mold::SimulationReport r = mold::simulate(stream, o);
    const bool prefix_ok = r.book_hash == r.reference_hash;
    const bool complete = r.final_state == "ended" && r.delivered == r.messages;
    std::cout << Json{{"type", "mold_simulation"},
                      {"note", "simulated channel; not network I/O"},
                      {"input", input},
                      {"seed", o.seed},
                      {"loss_percent", o.loss_percent},
                      {"duplicate_percent", o.duplicate_percent},
                      {"reorder_window", o.reorder_window},
                      {"recovery", o.recovery},
                      {"messages", r.messages},
                      {"packets", r.packets},
                      {"packets_lost", r.packets_lost},
                      {"packets_duplicated", r.packets_duplicated},
                      {"rerequest_rounds", r.rerequest_rounds},
                      {"retransmitted_packets", r.retransmitted_packets},
                      {"delivered", r.delivered},
                      {"duplicates_dropped", r.duplicates_dropped},
                      {"gaps_opened", r.gaps_opened},
                      {"gaps_closed", r.gaps_closed},
                      {"reorder_buffer_peak", r.buffered_peak},
                      {"in_order_exactly_once", r.in_order},
                      {"final_state", r.final_state},
                      {"next_expected", r.next_expected},
                      {"book_matches_delivered_prefix", prefix_ok},
                      {"book_matches_full_replay", r.book_hash == r.full_hash},
                      {"book_hash", r.book_hash}}
                     .dump()
              << "\n";
    const bool ok = r.in_order && prefix_ok && (!o.recovery || complete);
    return ok ? 0 : kExitStopped;
}

}  // namespace

int cmd_itch(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw UsageError("expected a subcommand: generate, replay, dump or mold-sim");
    }
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    if (args[0] == "generate") {
        return cmd_generate(rest);
    }
    if (args[0] == "replay") {
        return cmd_replay_itch(rest);
    }
    if (args[0] == "dump") {
        return cmd_dump(rest);
    }
    if (args[0] == "mold-sim") {
        return cmd_mold_sim(rest);
    }
    throw UsageError("unknown itch subcommand: " + args[0]);
}

}  // namespace enginex::cli
