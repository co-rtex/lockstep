#pragma once

// Order-book reconstruction from decoded ITCH 5.0 messages.
//
// The reconstructor tracks every displayed order by its order reference number and maintains,
// per stock locate, price levels with an explicit FIFO of orders. Semantics follow the
// specification (§4.3–4.5):
//
//   A/F  add a displayed order (priority: back of its price level)
//   E/C  execute shares of a resting order; the order keeps its priority. C carries an execution
//        price that may differ from the display price; the order's display price is unchanged.
//        Effects are cumulative; an order whose shares reach zero is removed.
//   X    cancel part of an order (keeps priority); zero remaining → removed
//   D    delete the order
//   U    cancel-replace: the original order is removed and a new order (new reference, new
//        price/shares, same side/stock/attribution) joins the back of its level — priority is
//        lost even if price and shares are unchanged
//   P/Q/B  trades of non-displayed orders, crosses and trade breaks: recorded in statistics,
//        never change displayed order state
//   R    stock directory (locate → symbol); H trading state; everything else: statistics only
//
// The book may be locked or crossed: historical feeds can legitimately show that transiently,
// so it is reported, not treated as an error.
//
// Messages that contradict the reconstructed state (unknown reference, over-execution, a
// duplicate live reference, ...) are *anomalies*: they are counted, the message is not applied
// (except SymbolMismatch/DirectoryConflict, which are informational), and `strict` replay stops
// at the first one. A correct full-day feed processed from the start of day should produce
// none; a feed joined mid-day produces UnknownOrderRef for orders added before the join.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "enginex/itch/messages.hpp"

namespace enginex::itch {

enum class Anomaly : std::uint8_t {
    None,
    UnknownOrderRef,    // E/C/X/D/U for a reference that is not live
    DuplicateOrderRef,  // A/F/U introducing a reference that is already live
    OverExecution,      // executed or canceled shares exceed the remaining shares
    ZeroShares,         // add/replace with 0 shares, or E/C/X of 0 shares
    LocateMismatch,     // modify message's stock locate differs from the order's
    SymbolMismatch,     // add's stock differs from the directory symbol (applied anyway)
    DirectoryConflict,  // R remaps a locate to a different symbol (applied anyway)
};
inline constexpr std::size_t kAnomalyKinds = 8;
[[nodiscard]] std::string_view to_string(Anomaly a) noexcept;

enum class ApplyStatus : std::uint8_t {
    Applied,       // changed the displayed book
    NoBookEffect,  // valid message that does not affect displayed orders (trades, admin, ...)
    Rejected,      // anomaly; the book is unchanged
};

struct ApplyOutcome {
    ApplyStatus status = ApplyStatus::NoBookEffect;
    Anomaly anomaly = Anomaly::None;  // set for Rejected, and for informational anomalies
};

struct LevelView {
    Price4 price = 0;
    std::uint64_t shares = 0;
    std::uint32_t orders = 0;
    bool operator==(const LevelView&) const = default;
};

struct OrderView {
    OrderRef ref = 0;
    Side side = Side::Buy;
    Price4 price = 0;
    std::uint32_t shares = 0;
    bool operator==(const OrderView&) const = default;
};

struct TopOfBook {
    std::optional<LevelView> bid;
    std::optional<LevelView> ask;
    [[nodiscard]] bool crossed() const noexcept { return bid && ask && bid->price > ask->price; }
    [[nodiscard]] bool locked() const noexcept { return bid && ask && bid->price == ask->price; }
};

struct BookStats {
    std::array<std::uint64_t, 256> messages_by_type{};
    std::array<std::uint64_t, kAnomalyKinds> anomalies{};
    std::uint64_t messages = 0;
    std::uint64_t applied = 0;
    std::uint64_t rejected = 0;
    std::uint64_t adds = 0;
    std::uint64_t executions = 0;          // E + C applied
    std::uint64_t executed_shares = 0;     // E + C applied (printable or not)
    std::uint64_t printable_executed_shares = 0;  // E + printable C
    std::uint64_t non_displayed_trade_shares = 0;  // P
    std::uint64_t cross_shares = 0;                // Q
    std::uint64_t broken_trades = 0;               // B
    std::uint64_t cancels = 0;
    std::uint64_t canceled_shares = 0;
    std::uint64_t deletes = 0;
    std::uint64_t replaces = 0;
    std::uint64_t levels_created = 0;  // adds/replaces that opened a new price level
    std::uint64_t levels_removed = 0;
    std::uint64_t index_pages = 0;       // paged index only: pages currently allocated
    std::uint64_t peak_index_pages = 0;
    std::uint64_t live_orders = 0;
    std::uint64_t peak_live_orders = 0;
    std::uint64_t timestamp_regressions = 0;  // informational: timestamps are not sequence numbers
    std::uint64_t last_timestamp_ns = 0;
    char last_system_event = 0;
    [[nodiscard]] std::uint64_t anomaly_count() const noexcept;
};

// Which order-index implementation this build uses ("open_addressing" or "std_unordered_map").
extern const char* const kOrderIndexName;
// Price-level map node allocation ("node_arena" or "std_allocator").
extern const char* const kLevelAllocName;

class BookBuilder {
public:
    // `expected_orders` (optional) pre-sizes the order index and pool so that growth (a full
    // rehash and a pool reallocation, visible as a one-off latency spike) does not happen
    // while the book holds fewer live orders than that.
    // `level_vector_limit` bounds the contiguous part of each side's price levels (the best
    // levels); deeper levels go to an ordered map, so no level operation moves more than that
    // many entries (D32). The default is far above real book depths; tests use small values.
    static constexpr std::size_t kDefaultLevelVectorLimit = 8192;
    explicit BookBuilder(std::size_t expected_orders = 0,
                         std::size_t level_vector_limit = kDefaultLevelVectorLimit);
    ~BookBuilder();
    BookBuilder(const BookBuilder&) = delete;
    BookBuilder& operator=(const BookBuilder&) = delete;

    // Applies one message. Throws only on allocation failure (std::bad_alloc /
    // std::length_error); the builder is then in an unspecified state and must be discarded
    // (replay() propagates the exception and the CLI exits with an error).
    ApplyOutcome apply(const Message& m);

    [[nodiscard]] const BookStats& stats() const noexcept { return stats_; }

    // Locates with a directory entry or at least one add, ascending.
    [[nodiscard]] std::vector<std::uint16_t> locates() const;
    [[nodiscard]] std::optional<std::string> symbol(std::uint16_t locate) const;
    [[nodiscard]] std::optional<std::uint16_t> find_locate(std::string_view symbol) const;
    [[nodiscard]] std::optional<char> trading_state(std::uint16_t locate) const;

    [[nodiscard]] TopOfBook top(std::uint16_t locate) const;
    // Best `max_levels` levels of one side, best first (0 = all levels).
    [[nodiscard]] std::vector<LevelView> depth(std::uint16_t locate, Side side,
                                               std::size_t max_levels) const;
    // Every order of one side in priority order: best price first, FIFO within a level.
    [[nodiscard]] std::vector<OrderView> orders(std::uint16_t locate, Side side) const;
    [[nodiscard]] std::optional<OrderView> find_order(OrderRef ref) const;
    // MPID of a live order added with 'F' (kept across replaces); nullopt otherwise.
    [[nodiscard]] std::optional<Mpid> attribution(OrderRef ref) const;
    [[nodiscard]] std::size_t level_count(std::uint16_t locate, Side side) const;

    // Deterministic text form of every book (levels and, optionally, every order in priority
    // order) and its SHA-256. Equal inputs give byte-identical output.
    [[nodiscard]] std::string canonical_state(bool include_orders) const;
    [[nodiscard]] std::string state_hash() const;

    // Internal consistency check (level totals vs orders, index vs pool); returns the first
    // violation or nullopt. Linear time; for tests.
    [[nodiscard]] std::optional<std::string> check_invariants() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    BookStats stats_;
};

}  // namespace enginex::itch
