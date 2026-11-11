// `enginex itch ...`: historical ITCH 5.0 market-data tools. Independent of the synthetic
// exchange's commands: these read *outbound market data*, they do not place orders.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>

#include "cli.hpp"
#include "enginex/itch/book.hpp"
#include "enginex/itch/byte_reader.hpp"
#include "enginex/itch/mold_sim.hpp"
#include "enginex/itch/mold_udp.hpp"
#include "enginex/itch/replay.hpp"
#include "enginex/itch/synthetic.hpp"
#include "enginex/json_codec.hpp"
#include "enginex/metrics.hpp"

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

// Alpha fields come straight off the wire and are not guaranteed to be ASCII; JSON output
// must stay valid UTF-8 (nlohmann::json::dump throws otherwise). Printable ASCII is kept,
// every other byte (and '\\') becomes the text \xNN, so no information is lost.
std::string printable(std::string_view raw) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size());
    for (const char c : raw) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u <= 0x7E && c != '\\') {
            out += c;
        } else {
            out += "\\x";
            out += kHex[u >> 4U];
            out += kHex[u & 0xFU];
        }
    }
    return out;
}
std::string printable(char c) { return printable(std::string_view(&c, 1)); }

std::string sym(const Symbol& s) { return printable(trim_alpha(s)); }

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
                           {"levels_created", s.levels_created},
                           {"levels_removed", s.levels_removed},
                           {"index_pages", s.index_pages},
                           {"peak_index_pages", s.peak_index_pages},
                           {"non_displayed_trade_shares", s.non_displayed_trade_shares},
                           {"cross_shares", s.cross_shares},
                           {"broken_trades", s.broken_trades},
                           {"live_orders", s.live_orders},
                           {"peak_live_orders", s.peak_live_orders},
                           {"timestamp_regressions", s.timestamp_regressions},
                           {"last_timestamp_ns", s.last_timestamp_ns},
                           {"last_system_event", s.last_system_event == 0
                                                     ? std::string()
                                                     : printable(s.last_system_event)}};
    // Bytes the book's structures hold at the end, by structure (capacities, not RSS).
    const MemoryUsage mem = b.memory_usage();
    j["book_memory_bytes"] = Json{{"node_pool", mem.node_pool},
                                  {"free_slots", mem.free_slots},
                                  {"index_pages", mem.index_pages},
                                  {"index_directory", mem.index_directory},
                                  {"index_fallback", mem.index_fallback},
                                  {"levels", mem.levels},
                                  {"level_vectors", mem.level_vectors},
                                  {"level_maps_estimated", mem.level_maps_est},
                                  {"books", mem.books},
                                  {"total", mem.total()}};
    j["state_hash"] = b.state_hash();
    Json books = Json::array();
    // Crossed/locked books by the symbol's last trading state ('T' trading, 'H' halted,
    // 'P' paused, 'Q' quotation only, '-' never stated): on real data, crossed books occur in
    // halted or paused symbols, which keep accepting orders without matching them.
    Json crossed_by_state = Json::object();
    Json locked_by_state = Json::object();
    for (const std::uint16_t locate : b.locates()) {
        const TopOfBook t = b.top(locate);
        const std::string state = printable(b.trading_state(locate).value_or('-'));
        if (t.crossed()) {
            crossed_by_state[state] = crossed_by_state.value(state, 0) + 1;
        }
        if (t.locked()) {
            locked_by_state[state] = locked_by_state.value(state, 0) + 1;
        }
    }
    j["crossed_books_by_trading_state"] = crossed_by_state;
    j["locked_books_by_trading_state"] = locked_by_state;
    for (const std::uint16_t locate : b.locates()) {
        const auto symbol = b.symbol(locate);
        if (only_symbol && symbol != only_symbol) {
            continue;
        }
        const TopOfBook t = b.top(locate);
        Json bk;
        bk["locate"] = locate;
        bk["symbol"] = symbol ? Json(printable(*symbol)) : Json(nullptr);
        bk["best_bid"] = t.bid ? level_json(*t.bid) : Json(nullptr);
        bk["best_ask"] = t.ask ? level_json(*t.ask) : Json(nullptr);
        bk["locked"] = t.locked();
        bk["crossed"] = t.crossed();
        const auto state = b.trading_state(locate);
        bk["trading_state"] = state ? Json(printable(*state)) : Json(nullptr);
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
    const Args a(argv,
                 {"input", "depth", "symbol", "metrics-out", "expected-orders", "level-vector-limit",
                  "checkpoint-every", "checkpoint-out"},
                 {"strict", "json"});
    const std::string input = a.require("input");
    const std::size_t depth = a.get_u64("depth", 5);
    const auto only = a.get("symbol");
    ReplayOptions opts;
    opts.strict = a.flag("strict");
    // Capacity hint: size it near the expected peak of live orders (docs/benchmarks.md shows
    // both too small (rehash spikes) and far too large (cache misses) cost time).
    // --level-vector-limit (testing aid): a small value forces the deep-level path (D32); the
    // books and the state hash must not change.
    const std::uint64_t limit = a.get_u64("level-vector-limit", BookBuilder::kDefaultLevelVectorLimit);
    if (limit < 2 || limit > (std::uint64_t{1} << 30U)) {
        throw UsageError("--level-vector-limit must be in [2, 2^30]");
    }
    const std::uint64_t expected = a.get_u64("expected-orders", 0);
    if (expected > BookBuilder::kMaxOrders) {
        throw UsageError("--expected-orders must be at most " + std::to_string(BookBuilder::kMaxOrders));
    }
    BookBuilder book(static_cast<std::size_t>(expected), static_cast<std::size_t>(limit));
    // --checkpoint-every N --checkpoint-out FILE: after every N decoded messages, one JSON line
    // with the live-order count and every non-empty book's top of book, so an independent
    // model can be compared mid-stream, not only at the end (tools/itch_pybook.py).
    const std::uint64_t every = a.get_u64("checkpoint-every", 0);
    std::ofstream checkpoints;
    std::uint64_t decoded = 0;
    if (every == 0 && a.get("checkpoint-out")) {
        throw UsageError("--checkpoint-out needs --checkpoint-every N (N >= 1)");
    }
    if (every != 0) {
        const auto path = a.get("checkpoint-out");
        if (!path) {
            throw UsageError("--checkpoint-every needs --checkpoint-out FILE");
        }
        checkpoints.open(*path);
        if (!checkpoints) {
            throw std::runtime_error("cannot write " + *path);
        }
        opts.on_message = [&](const Message&, const ApplyOutcome&) {
            if (++decoded % every != 0) {
                return;
            }
            Json tops = Json::array();
            for (const std::uint16_t l : book.locates()) {
                const TopOfBook t = book.top(l);
                if (t.bid || t.ask) {
                    tops.push_back(Json::array({l, t.bid ? t.bid->price : 0, t.bid ? t.bid->shares : 0,
                                                t.ask ? t.ask->price : 0, t.ask ? t.ask->shares : 0}));
                }
            }
            checkpoints << Json{{"message", decoded},
                                {"live_orders", book.stats().live_orders},
                                {"tops", std::move(tops)}}
                               .dump()
                        << "\n";
        };
    }
    auto in = open_input(input);
    const auto t0 = std::chrono::steady_clock::now();
    const ReplayResult r = replay(*in, book, opts);
    const auto t1 = std::chrono::steady_clock::now();
    if (every != 0 && !checkpoints.flush()) {
        throw std::runtime_error("writing --checkpoint-out failed");
    }
    if (only && !book.find_locate(*only)) {
        throw UsageError("--symbol " + *only + ": no such symbol in " + input);
    }
    Json j = replay_json(input, r, book, depth, only);
    if (const auto out = a.get("metrics-out")) {
        Json m = j;
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        // Wall time of the whole replay including file reads; not a latency measurement.
        // The input is streamed in 1 MiB chunks, so peak RSS is dominated by the book itself.
        const std::uint64_t rss_kib = metrics::peak_rss_kib();
        const std::uint64_t peak_live = book.stats().peak_live_orders;
        m["timing"] = Json{{"wall_seconds", secs},
                           {"frames_per_second", secs > 0 ? static_cast<double>(r.frames) / secs : 0.0},
                           {"peak_rss_kib", rss_kib},
                           {"peak_rss_bytes_per_peak_live_order",
                            peak_live == 0 ? 0.0
                                           : static_cast<double>(rss_kib) * 1024.0 /
                                                 static_cast<double>(peak_live)},
                           {"note", "single run, includes I/O; RSS is the whole process (book, "
                                    "tables, JSON); see docs/benchmarks.md for methodology"}};
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
    j["type"] = printable(h.type);
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
                j["event_code"] = printable(x.event_code);
            } else if constexpr (std::is_same_v<T, StockDirectory>) {
                j["market_category"] = printable(x.market_category);
                j["round_lot_size"] = x.round_lot_size;
            } else if constexpr (std::is_same_v<T, StockTradingAction>) {
                j["trading_state"] = printable(x.trading_state);
            } else if constexpr (std::is_same_v<T, AddOrder>) {
                j["order_ref"] = x.order_ref;
                j["side"] = std::string(1, static_cast<char>(x.side));
                j["shares"] = x.shares;
                j["price"] = x.price;
                if (x.attribution) {
                    j["attribution"] = printable(std::string_view(x.attribution->data(), 4));
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
                j["cross_type"] = printable(x.cross_type);
            } else if constexpr (std::is_same_v<T, BrokenTrade>) {
                j["match_number"] = x.match_number;
            }
        },
        m);
    return j;
}

// Every field of every message, under its struct member name; alpha fields full width (not
// trimmed) through printable(). Used by tools/itch_pydecode.py for an independent field-level
// differential check.
template <std::size_t N>
std::string alpha(const std::array<char, N>& a) {
    return printable(std::string_view(a.data(), N));
}

Json full_message_json(const Message& m) {
    const Header& h = header_of(m);
    Json j;
    j["type"] = printable(h.type);
    j["stock_locate"] = h.stock_locate;
    j["tracking_number"] = h.tracking_number;
    j["timestamp_ns"] = h.timestamp_ns;
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, SystemEvent>) {
                j["event_code"] = printable(x.event_code);
            } else if constexpr (std::is_same_v<T, StockDirectory>) {
                j["stock"] = alpha(x.stock);
                j["market_category"] = printable(x.market_category);
                j["financial_status"] = printable(x.financial_status);
                j["round_lot_size"] = x.round_lot_size;
                j["round_lots_only"] = printable(x.round_lots_only);
                j["issue_classification"] = printable(x.issue_classification);
                j["issue_subtype"] = alpha(x.issue_subtype);
                j["authenticity"] = printable(x.authenticity);
                j["short_sale_threshold"] = printable(x.short_sale_threshold);
                j["ipo_flag"] = printable(x.ipo_flag);
                j["luld_tier"] = printable(x.luld_tier);
                j["etp_flag"] = printable(x.etp_flag);
                j["etp_leverage_factor"] = x.etp_leverage_factor;
                j["inverse_indicator"] = printable(x.inverse_indicator);
            } else if constexpr (std::is_same_v<T, StockTradingAction>) {
                j["stock"] = alpha(x.stock);
                j["trading_state"] = printable(x.trading_state);
                j["reserved"] = printable(x.reserved);
                j["reason"] = alpha(x.reason);
            } else if constexpr (std::is_same_v<T, RegShoRestriction>) {
                j["stock"] = alpha(x.stock);
                j["reg_sho_action"] = printable(x.reg_sho_action);
            } else if constexpr (std::is_same_v<T, MarketParticipantPosition>) {
                j["mpid"] = alpha(x.mpid);
                j["stock"] = alpha(x.stock);
                j["primary_market_maker"] = printable(x.primary_market_maker);
                j["market_maker_mode"] = printable(x.market_maker_mode);
                j["participant_state"] = printable(x.participant_state);
            } else if constexpr (std::is_same_v<T, MwcbDeclineLevel>) {
                j["level1"] = x.level1;
                j["level2"] = x.level2;
                j["level3"] = x.level3;
            } else if constexpr (std::is_same_v<T, MwcbStatus>) {
                j["breached_level"] = printable(x.breached_level);
            } else if constexpr (std::is_same_v<T, IpoQuotingPeriod>) {
                j["stock"] = alpha(x.stock);
                j["release_time"] = x.release_time;
                j["release_qualifier"] = printable(x.release_qualifier);
                j["ipo_price"] = x.ipo_price;
            } else if constexpr (std::is_same_v<T, LuldAuctionCollar>) {
                j["stock"] = alpha(x.stock);
                j["reference_price"] = x.reference_price;
                j["upper_collar"] = x.upper_collar;
                j["lower_collar"] = x.lower_collar;
                j["extension"] = x.extension;
            } else if constexpr (std::is_same_v<T, OperationalHalt>) {
                j["stock"] = alpha(x.stock);
                j["market_code"] = printable(x.market_code);
                j["halt_action"] = printable(x.halt_action);
            } else if constexpr (std::is_same_v<T, AddOrder>) {
                j["order_ref"] = x.order_ref;
                j["side"] = printable(static_cast<char>(x.side));
                j["shares"] = x.shares;
                j["stock"] = alpha(x.stock);
                j["price"] = x.price;
                if (x.attribution) {
                    j["attribution"] = alpha(*x.attribution);
                }
            } else if constexpr (std::is_same_v<T, OrderExecuted>) {
                j["order_ref"] = x.order_ref;
                j["executed_shares"] = x.executed_shares;
                j["match_number"] = x.match_number;
            } else if constexpr (std::is_same_v<T, OrderExecutedWithPrice>) {
                j["order_ref"] = x.order_ref;
                j["executed_shares"] = x.executed_shares;
                j["match_number"] = x.match_number;
                j["printable"] = x.printable ? "Y" : "N";
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
                j["order_ref"] = x.order_ref;
                j["side"] = printable(x.side);
                j["shares"] = x.shares;
                j["stock"] = alpha(x.stock);
                j["price"] = x.price;
                j["match_number"] = x.match_number;
            } else if constexpr (std::is_same_v<T, CrossTrade>) {
                j["shares"] = x.shares;
                j["stock"] = alpha(x.stock);
                j["cross_price"] = x.cross_price;
                j["match_number"] = x.match_number;
                j["cross_type"] = printable(x.cross_type);
            } else if constexpr (std::is_same_v<T, BrokenTrade>) {
                j["match_number"] = x.match_number;
            } else if constexpr (std::is_same_v<T, Noii>) {
                j["paired_shares"] = x.paired_shares;
                j["imbalance_shares"] = x.imbalance_shares;
                j["imbalance_direction"] = printable(x.imbalance_direction);
                j["stock"] = alpha(x.stock);
                j["far_price"] = x.far_price;
                j["near_price"] = x.near_price;
                j["current_reference_price"] = x.current_reference_price;
                j["cross_type"] = printable(x.cross_type);
                j["price_variation_indicator"] = printable(x.price_variation_indicator);
            } else if constexpr (std::is_same_v<T, RetailInterest>) {
                j["stock"] = alpha(x.stock);
                j["interest_flag"] = printable(x.interest_flag);
            } else if constexpr (std::is_same_v<T, DlcrPriceDiscovery>) {
                j["stock"] = alpha(x.stock);
                j["open_eligibility_status"] = printable(x.open_eligibility_status);
                j["minimum_allowable_price"] = x.minimum_allowable_price;
                j["maximum_allowable_price"] = x.maximum_allowable_price;
                j["near_execution_price"] = x.near_execution_price;
                j["near_execution_time"] = x.near_execution_time;
                j["lower_price_range_collar"] = x.lower_price_range_collar;
                j["upper_price_range_collar"] = x.upper_price_range_collar;
            } else {
                static_assert(sizeof(T) == 0, "every message type must be listed");
            }
        },
        m);
    return j;
}

int cmd_dump(const std::vector<std::string>& argv) {
    const Args a(argv, {"input", "limit"}, {"all-fields"});
    const std::uint64_t limit = a.get_u64("limit", 0);
    const bool all_fields = a.flag("all-fields");
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
                    Json j = all_fields ? full_message_json(std::get<Message>(d))
                                        : message_json(std::get<Message>(d));
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
                      {"failure", r.failure},
                      {"next_expected", r.next_expected},
                      {"book_matches_delivered_prefix", prefix_ok},
                      {"book_matches_full_replay", r.book_hash == r.full_hash},
                      {"book_hash", r.book_hash}}
                     .dump()
              << "\n";
    const bool ok = r.in_order && prefix_ok && (!o.recovery || complete);
    return ok ? 0 : kExitStopped;
}

std::uint16_t port_arg(const Args& a, const std::string& key, std::uint64_t fallback) {
    const std::uint64_t v = a.get_u64(key, fallback);
    if (v > 65535) {
        throw UsageError("--" + key + " must be a port number");
    }
    return static_cast<std::uint16_t>(v);
}

std::string host_arg(const Args& a, const std::string& key) {
    std::string host = a.get_or(key, "127.0.0.1");
    if (!mold::udp::is_loopback(host) && !a.flag("allow-remote")) {
        throw UsageError("--" + key + " " + host +
                         " is not a loopback address (pass --allow-remote to use it)");
    }
    return host;
}

std::string session_arg(const Args& a) {
    std::string s = a.get_or("session", "ENGINEX");
    if (s.empty() || s.size() > 10) {
        throw UsageError("--session must be 1 to 10 characters (the MoldUDP64 session field)");
    }
    return s;
}

std::uint32_t percent_arg(const Args& a, const std::string& key) {
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(a.get_u64(key, 0), 100));
}

// MoldUDP64 over real UDP: sends a file's messages and answers re-requests (mold_udp.hpp).
int cmd_mold_send(const std::vector<std::string>& argv) {
    const Args a(argv,
                 {"input", "port", "dest-host", "dest-port", "drop-percent",
                  "retransmit-drop-percent", "seed", "mtu", "rate-pps", "linger-ms", "session"},
                 {"allow-remote"});
    const std::string input = a.require("input");
    if (a.flag("allow-remote") && !a.get("session")) {
        throw UsageError("--allow-remote requires an explicit --session (the transport is unauthenticated)");
    }
    mold::udp::SenderOptions o;
    o.bind.host = "0.0.0.0";
    o.bind.port = port_arg(a, "port", 0);
    if (!a.flag("allow-remote")) {
        o.bind.host = "127.0.0.1";
    }
    o.dest.host = host_arg(a, "dest-host");
    o.dest.port = port_arg(a, "dest-port", 0);
    if (o.dest.port == 0) {
        throw UsageError("--dest-port is required");
    }
    o.drop_percent = percent_arg(a, "drop-percent");
    o.retransmit_drop_percent = percent_arg(a, "retransmit-drop-percent");
    o.seed = a.get_u64("seed", 1);
    o.packets_per_second = a.get_u64("rate-pps", 100'000);
    o.linger = a.get_ms("linger-ms", 1000, 0, 3'600'000);
    const std::size_t mtu = a.get_u64("mtu", 1400);
    if (mtu < 100 || mtu > 65507) {
        throw UsageError("--mtu must be in [100, 65507]");
    }
    // Frames are copied into packets as they are, without decoding (the sender is a transport).
    mold::PacketStore store(mold::make_session(session_arg(a)), mtu);
    StreamFramer framer;
    auto in = open_input(input);
    std::vector<char> buf(1 << 20);
    FrameError fe = FrameError::None;
    while (fe == FrameError::None && *in) {
        in->read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<std::size_t>(in->gcount());
        fe = framer.feed(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(buf.data()), n),
                         [&](std::span<const std::uint8_t> m) { store.append(m); });
    }
    if (fe == FrameError::None) {
        fe = framer.finish();
    }
    if (fe != FrameError::None && fe != FrameError::TruncatedTail) {
        throw std::runtime_error("input framing error: " + std::string(to_string(fe)));
    }
    store.finish();
    for (std::size_t i = 0; i < store.size(); ++i) {
        if (store.packet(i).size() > 65507) {  // the IPv4 UDP payload limit
            throw std::runtime_error("message " + std::to_string(store.first(i)) +
                                     " does not fit in one UDP datagram");
        }
    }
    mold::udp::Sender sender(store, o);
    std::cerr << "mold-send: " << store.size() << " packets, " << store.end_sequence() - 1
              << " messages, from port " << sender.port() << " to " << o.dest.host << ":"
              << o.dest.port << "\n";
    const mold::udp::SenderReport r = sender.run();
    std::cout << Json{{"type", "mold_udp_send"},
                      {"input", input},
                      {"messages", store.end_sequence() - 1},
                      {"input_truncated_tail", fe == FrameError::TruncatedTail},
                      {"packets", r.packets},
                      {"sent", r.sent},
                      {"dropped_deliberately", r.dropped},
                      {"requests_answered", r.requests},
                      {"bad_requests", r.bad_requests},
                      {"foreign_requests", r.foreign_requests},
                      {"answers_rate_limited", r.answers_rate_limited},
                      {"retransmitted_packets", r.retransmitted},
                      {"retransmit_dropped_deliberately", r.retransmit_dropped},
                      {"end_of_session_packets", r.end_packets},
                      {"send_errors", r.send_errors}}
                     .dump()
              << "\n";
    return 0;
}

// Receives MoldUDP64 over UDP, re-requests gaps, rebuilds the books. Exit 0 only if the session
// ended with every message delivered in order, exactly once.
int cmd_mold_recv(const std::vector<std::string>& argv) {
    const Args a(argv,
                 {"port", "request-host", "request-port", "session", "idle-timeout-ms",
                  "request-interval-ms", "max-buffered-messages", "max-ahead"},
                 {"no-recovery", "allow-remote"});
    if (a.flag("allow-remote") && !a.get("session")) {
        throw UsageError("--allow-remote requires an explicit --session (the transport is unauthenticated)");
    }
    mold::udp::ReceiverOptions o;
    o.bind.host = a.flag("allow-remote") ? "0.0.0.0" : "127.0.0.1";
    o.bind.port = port_arg(a, "port", 0);
    if (o.bind.port == 0) {
        throw UsageError("--port is required (the sender needs it)");
    }
    o.request_to.host = host_arg(a, "request-host");
    o.request_to.port = port_arg(a, "request-port", 0);
    o.recovery = !a.flag("no-recovery");
    if (o.recovery && o.request_to.port == 0) {
        throw UsageError("--request-port is required unless --no-recovery");
    }
    o.idle_timeout = a.get_ms("idle-timeout-ms", 5000, 1, 3'600'000);
    o.request_interval = a.get_ms("request-interval-ms", 5, 1, 1000);
    // Reorder buffer: bounded, but large enough to ride out a few request rounds at full rate.
    o.sequencer.max_buffered_messages = a.get_u64("max-buffered-messages", 1U << 20U);
    // How far past the next expected sequence a packet may start before the gap is declared
    // unrecoverable. It is also the reorder window's length (12 bytes per slot), hence the cap.
    const std::uint64_t ahead = a.get_u64("max-ahead", 1'000'000);
    if (ahead < 1000 || ahead > 16'000'000) {
        throw UsageError("--max-ahead must be in [1000, 16000000]");
    }
    o.sequencer.max_ahead = ahead;
    o.sequencer.session = mold::make_session(session_arg(a));
    mold::udp::Receiver rx(o);
    std::cerr << "mold-recv: listening on " << o.bind.host << ":" << rx.port() << "\n";
    BookBuilder book;
    std::uint64_t decode_errors = 0;
    const mold::udp::ReceiverReport r = rx.run([&](std::uint64_t, std::span<const std::uint8_t> m) {
        const DecodeResult d = decode(m);
        if (const auto* msg = std::get_if<Message>(&d)) {
            (void)book.apply(*msg);
        } else {
            ++decode_errors;
        }
    });
    std::cout << Json{{"type", "mold_udp_receive"},
                      {"final_state", r.final_state},
                      {"failure", r.failure},
                      {"delivered", r.delivered},
                      {"next_expected", r.next_expected},
                      {"in_order_exactly_once", r.in_order},
                      {"datagrams", r.packets},
                      {"bytes", r.bytes},
                      {"duplicates_dropped", r.duplicates},
                      {"malformed_datagrams", r.malformed},
                      {"foreign_session_datagrams", r.foreign_session},
                      {"foreign_source_datagrams", r.foreign_source},
                      {"gaps_opened", r.gaps_opened},
                      {"gaps_closed", r.gaps_closed},
                      {"reorder_buffer_peak", r.buffered_peak},
                      {"request_rounds", r.request_rounds},
                      {"requests_sent", r.requests_sent},
                      {"decode_errors", decode_errors},
                      {"timed_out", r.timed_out},
                      {"gap_stalled", r.gap_stalled},
                      {"seconds", r.seconds},
                      {"book_hash", book.state_hash()}}
                     .dump()
              << "\n";
    return r.final_state == "ended" && r.in_order ? 0 : kExitStopped;
}

}  // namespace

int cmd_itch(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw UsageError("expected a subcommand: generate, replay, dump, mold-sim, mold-send or mold-recv");
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
    if (args[0] == "mold-send") {
        return cmd_mold_send(rest);
    }
    if (args[0] == "mold-recv") {
        return cmd_mold_recv(rest);
    }
    throw UsageError("unknown itch subcommand: " + args[0]);
}

}  // namespace enginex::cli
