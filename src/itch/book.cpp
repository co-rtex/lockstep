// ITCH order-book reconstruction. See book.hpp for the message semantics.
//
// Data layout (baseline; see docs/itch.md for measurements):
//   * orders live in one pool (std::vector<Node>) with a free list, so steady-state adds and
//     removes do not allocate; nodes are linked into a per-level FIFO by 32-bit indices;
//   * an open-addressing hash index (order_index.hpp) maps order reference → pool slot; it is
//     never iterated, so it cannot affect output order;
//   * each locate's book keeps one std::map<price, Level> per side. Node::level points at its
//     map node, which std::map keeps stable until that level is erased; a level is erased only
//     after its last order is unlinked. Map nodes come from a per-builder free-list arena
//     (NodeArena), so level churn does not call malloc/free.
#include "enginex/itch/book.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <stdexcept>
#include <map>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include "enginex/itch/byte_reader.hpp"
#include "enginex/itch/order_index.hpp"
#include "enginex/itch/paged_index.hpp"
#include "enginex/sha256.hpp"

namespace enginex::itch {

namespace {

constexpr std::uint32_t kNil = UINT32_MAX;
constexpr std::size_t kLocates = 65536;

struct Level {
    std::uint64_t shares = 0;
    std::uint32_t count = 0;
    std::uint32_t head = kNil;
    std::uint32_t tail = kNil;
};
// Fixed-size block arena for std::map nodes: freed nodes go on a free list and are reused, so
// creating and removing price levels does not call malloc/free in steady state. The block
// size is fixed by the first allocation (the map's node type); any other size falls back to
// operator new. Memory is returned when the BookBuilder is destroyed.
class NodeArena {
public:
    NodeArena() = default;
    NodeArena(const NodeArena&) = delete;
    NodeArena& operator=(const NodeArena&) = delete;

    void* allocate(std::size_t size) {
        if (block_ == 0) {
            block_ = std::max(size, sizeof(void*));
        }
        if (size != block_) {
            return ::operator new(size);
        }
        if (free_ != nullptr) {
            void* p = free_;
            free_ = *static_cast<void**>(p);
            return p;
        }
        if (chunks_.empty() || used_ == kPerChunk) {
            chunks_.push_back(std::make_unique_for_overwrite<std::byte[]>(block_ * kPerChunk));
            used_ = 0;
        }
        return chunks_.back().get() + block_ * used_++;
    }
    void deallocate(void* p, std::size_t size) noexcept {
        if (size != block_) {
            ::operator delete(p);
            return;
        }
        *static_cast<void**>(p) = free_;
        free_ = p;
    }

private:
    static constexpr std::size_t kPerChunk = 4096;
    std::size_t block_ = 0;
    void* free_ = nullptr;
    std::size_t used_ = 0;
    std::vector<std::unique_ptr<std::byte[]>> chunks_;
};

template <typename T>
struct ArenaAllocator {
    using value_type = T;
    NodeArena* arena;
    explicit ArenaAllocator(NodeArena* a) noexcept : arena(a) {}
    template <typename U>
    ArenaAllocator(const ArenaAllocator<U>& o) noexcept : arena(o.arena) {}  // NOLINT
    T* allocate(std::size_t n) {
        if (n != 1) {
            return static_cast<T*>(::operator new(n * sizeof(T)));
        }
        return static_cast<T*>(arena->allocate(sizeof(T)));
    }
    void deallocate(T* p, std::size_t n) noexcept {
        if (n != 1) {
            ::operator delete(p);
            return;
        }
        arena->deallocate(p, sizeof(T));
    }
    template <typename U>
    bool operator==(const ArenaAllocator<U>& o) const noexcept {
        return arena == o.arena;
    }
};

// ---- price levels of one side ---------------------------------------------------------------
// Both implementations offer the same interface; Level objects never move while they exist
// (Node::level points at them), and a level is destroyed only after its last order is unlinked.

#ifdef ENGINEX_ITCH_MAP_LEVELS
// A/B baseline: one std::map per side, nodes from the builder's arena (or std::allocator).
#ifdef ENGINEX_ITCH_STD_LEVEL_ALLOC
using LevelMap = std::map<Price4, Level>;
#else
using LevelMap = std::map<Price4, Level, std::less<>,
                          ArenaAllocator<std::pair<const Price4, Level>>>;
#endif

class SideLevels {
public:
    struct Storage {
        NodeArena arena;
    };
#ifdef ENGINEX_ITCH_STD_LEVEL_ALLOC
    SideLevels(Side side, Storage*) : side_(side) {}
#else
    SideLevels(Side side, Storage* storage)
        : side_(side), map_(LevelMap::allocator_type(&storage->arena)) {}
#endif
    Level& get_or_create(Price4 price, bool& created) {
        const auto [it, inserted] = map_.try_emplace(price);
        created = inserted;
        return it->second;
    }
    void erase(Price4 price) { map_.erase(price); }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    template <typename F>
    void for_each_best_first(std::size_t max_levels, F&& f) const {
        std::size_t n = 0;
        auto visit = [&](auto first, auto last) {
            for (auto it = first; it != last && (max_levels == 0 || n < max_levels); ++it, ++n) {
                f(it->first, it->second);
            }
        };
        if (side_ == Side::Buy) {
            visit(map_.rbegin(), map_.rend());
        } else {
            visit(map_.begin(), map_.end());
        }
    }

private:
    Side side_;
    LevelMap map_;
};
#else
// Default: a contiguous vector of (rank, Level*) sorted by rank, where rank is the price for
// bids and UINT32_MAX - price for asks, so the BEST level of either side is at the back.
// Lookups are binary searches over contiguous memory; inserting or removing a level near the
// top of the book moves only the few entries behind it. Level objects live in a deque-based
// pool (stable addresses) with a free list. Chosen from real-data measurements (D27).
class SideLevels {
public:
    struct Storage {
        std::deque<Level> levels;
        std::vector<Level*> free;
    };
    SideLevels(Side side, Storage* storage) : side_(side), storage_(storage) {}

    Level& get_or_create(Price4 price, bool& created) {
        const std::uint32_t r = rank(price);
        auto it = std::lower_bound(v_.begin(), v_.end(), r,
                                   [](const Entry& e, std::uint32_t key) { return e.rank < key; });
        if (it != v_.end() && it->rank == r) {
            created = false;
            return *it->level;
        }
        Level* level = nullptr;
        if (!storage_->free.empty()) {
            level = storage_->free.back();
            storage_->free.pop_back();
            *level = Level{};
        } else {
            level = &storage_->levels.emplace_back();
        }
        try {
            v_.insert(it, Entry{r, level});
        } catch (...) {
            storage_->free.push_back(level);  // not leaked if the vector cannot grow
            throw;
        }
        created = true;
        return *level;
    }
    void erase(Price4 price) {
        const std::uint32_t r = rank(price);
        auto it = std::lower_bound(v_.begin(), v_.end(), r,
                                   [](const Entry& e, std::uint32_t key) { return e.rank < key; });
        if (it == v_.end() || it->rank != r) {
            throw std::logic_error("ITCH book: erasing a missing price level");
        }
        storage_->free.push_back(it->level);
        v_.erase(it);
    }
    [[nodiscard]] std::size_t size() const noexcept { return v_.size(); }
    template <typename F>
    void for_each_best_first(std::size_t max_levels, F&& f) const {
        std::size_t n = 0;
        for (auto it = v_.rbegin(); it != v_.rend() && (max_levels == 0 || n < max_levels);
             ++it, ++n) {
            f(price_of(it->rank), *it->level);
        }
    }

private:
    struct Entry {
        std::uint32_t rank;
        Level* level;
    };
    [[nodiscard]] std::uint32_t rank(Price4 p) const noexcept {
        return side_ == Side::Buy ? p : UINT32_MAX - p;
    }
    [[nodiscard]] Price4 price_of(std::uint32_t r) const noexcept {
        return side_ == Side::Buy ? r : UINT32_MAX - r;
    }
    Side side_;
    Storage* storage_;
    std::vector<Entry> v_;
};
#endif

struct Book {
    explicit Book(SideLevels::Storage* storage)
        : bids(Side::Buy, storage), asks(Side::Sell, storage) {}
    SideLevels bids;
    SideLevels asks;
    SideLevels& side(Side s) noexcept { return s == Side::Buy ? bids : asks; }
    const SideLevels& side(Side s) const noexcept { return s == Side::Buy ? bids : asks; }
};

struct Node {
    OrderRef ref = 0;
    Level* level = nullptr;
    std::uint32_t prev = kNil;
    std::uint32_t next = kNil;
    std::uint32_t shares = 0;
    Price4 price = 0;
    std::uint16_t locate = 0;
    Side side = Side::Buy;
    std::optional<Mpid> attribution;
};

// A/B baseline: -DENGINEX_ITCH_STD_INDEX=ON swaps the open-addressing index for the original
// std::unordered_map (same interface) so both can be benchmarked from one commit.
#ifdef ENGINEX_ITCH_STD_INDEX
class StdOrderIndex {
public:
    static constexpr std::uint32_t kEmpty = OrderIndex::kEmpty;
    [[nodiscard]] std::uint32_t find(OrderRef k) const {
        const auto it = map_.find(k);
        return it == map_.end() ? kEmpty : it->second;
    }
    [[nodiscard]] bool contains(OrderRef k) const { return map_.contains(k); }
    bool insert(OrderRef k, std::uint32_t v) { return map_.emplace(k, v).second; }
    bool erase(OrderRef k) { return map_.erase(k) == 1; }
    void reserve(std::size_t n) { map_.reserve(n); }
    [[nodiscard]] std::size_t size() const { return map_.size(); }

private:
    std::unordered_map<OrderRef, std::uint32_t> map_;
};
using Index = StdOrderIndex;
#elif defined(ENGINEX_ITCH_HASH_INDEX)
using Index = OrderIndex;  // A/B: open-addressing hash table only
#else
using Index = PagedIndex;  // default (D27)
#endif

struct DirectoryEntry {
    std::optional<Symbol> symbol;
    char trading_state = 0;
};

}  // namespace

// Order nodes in fixed-size chunks. Growth allocates a new chunk and never moves existing
// nodes: a std::vector<Node> doubling at ~1 M live orders copied ~50 MB inside one message
// (a deterministic ~10 ms stall, measured on the real sample).
class NodePool {
public:
    static constexpr unsigned kChunkBits = 16;
    static constexpr std::uint32_t kChunk = std::uint32_t{1} << kChunkBits;

    Node& operator[](std::uint32_t i) noexcept { return chunks_[i >> kChunkBits][i & (kChunk - 1)]; }
    const Node& operator[](std::uint32_t i) const noexcept {
        return chunks_[i >> kChunkBits][i & (kChunk - 1)];
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    // Appends a default node and returns its index.
    std::uint32_t emplace_back() {
        if (size_ == chunks_.size() * std::size_t{kChunk}) {
            if (size_ >= std::size_t{UINT32_MAX} - kChunk) {
                throw std::length_error("ITCH book: too many live orders");
            }
            chunks_.push_back(std::make_unique<Node[]>(kChunk));
        }
        return static_cast<std::uint32_t>(size_++);
    }
    void reserve(std::size_t n) {
        while (chunks_.size() * std::size_t{kChunk} < n) {
            chunks_.push_back(std::make_unique<Node[]>(kChunk));
        }
    }

private:
    std::vector<std::unique_ptr<Node[]>> chunks_;
    std::size_t size_ = 0;
};

struct BookBuilder::Impl {
    SideLevels::Storage level_storage;  // declared first: destroyed after the books using it
    std::uint64_t levels_created = 0;
    std::uint64_t levels_removed = 0;
    NodePool pool;
    std::vector<std::uint32_t> free_slots;
    Index index;
    std::vector<std::unique_ptr<Book>> books = std::vector<std::unique_ptr<Book>>(kLocates);
    std::vector<DirectoryEntry> directory = std::vector<DirectoryEntry>(kLocates);

    Book& book(std::uint16_t locate) {
        auto& b = books[locate];
        if (!b) {
            b = std::make_unique<Book>(&level_storage);
        }
        return *b;
    }

    std::optional<std::uint32_t> lookup(OrderRef ref) const {
        const std::uint32_t slot = index.find(ref);
        if (slot == Index::kEmpty) {
            return std::nullopt;
        }
        return slot;
    }

    void add(std::uint16_t locate, OrderRef ref, Side side, Price4 price, std::uint32_t shares,
             const std::optional<Mpid>& attribution) {
        std::uint32_t slot = 0;
        if (!free_slots.empty()) {
            slot = free_slots.back();
            free_slots.pop_back();
        } else {
            slot = pool.emplace_back();
        }
        // Everything that can throw (index page, level vector) happens before the node is
        // linked, and is undone on failure, so a bad_alloc never leaves a half-added order.
        try {
            (void)index.insert(ref, slot);
        } catch (...) {
            free_slots.push_back(slot);
            throw;
        }
        bool created = false;
        Level* level_ptr = nullptr;
        try {
            level_ptr = &book(locate).side(side).get_or_create(price, created);
        } catch (...) {
            (void)index.erase(ref);
            free_slots.push_back(slot);
            throw;
        }
        Level& level = *level_ptr;
        levels_created += created ? 1U : 0U;
        Node& n = pool[slot];
        n.ref = ref;
        n.level = &level;
        n.prev = level.tail;
        n.next = kNil;
        n.shares = shares;
        n.price = price;
        n.locate = locate;
        n.side = side;
        n.attribution = attribution;
        if (level.tail != kNil) {
            pool[level.tail].next = slot;
        } else {
            level.head = slot;
        }
        level.tail = slot;
        level.shares += shares;
        ++level.count;
    }

    void remove(std::uint32_t slot) {
        Node& n = pool[slot];
        Level& level = *n.level;
        if (n.prev != kNil) {
            pool[n.prev].next = n.next;
        } else {
            level.head = n.next;
        }
        if (n.next != kNil) {
            pool[n.next].prev = n.prev;
        } else {
            level.tail = n.prev;
        }
        level.shares -= n.shares;
        --level.count;
        if (level.count == 0) {
            books[n.locate]->side(n.side).erase(n.price);  // invalidates `level`
            ++levels_removed;
        }
        index.erase(n.ref);
        n.level = nullptr;
        n.attribution.reset();
        free_slots.push_back(slot);
    }

    // Reduces an order by `shares` (≤ its remaining shares); removes it at zero.
    void reduce(std::uint32_t slot, std::uint32_t shares) {
        Node& n = pool[slot];
        n.shares -= shares;
        n.level->shares -= shares;
        if (n.shares == 0) {
            remove(slot);
        }
    }
};

#if defined(ENGINEX_ITCH_STD_INDEX)
const char* const kOrderIndexName = "std_unordered_map";
#elif defined(ENGINEX_ITCH_HASH_INDEX)
const char* const kOrderIndexName = "open_addressing";
#else
const char* const kOrderIndexName = "paged_direct";
#endif
#if !defined(ENGINEX_ITCH_MAP_LEVELS)
const char* const kLevelAllocName = "sorted_vector";
#elif defined(ENGINEX_ITCH_STD_LEVEL_ALLOC)
const char* const kLevelAllocName = "map_std_allocator";
#else
const char* const kLevelAllocName = "map_node_arena";
#endif

std::string_view to_string(Anomaly a) noexcept {
    switch (a) {
        case Anomaly::None: return "none";
        case Anomaly::UnknownOrderRef: return "unknown_order_ref";
        case Anomaly::DuplicateOrderRef: return "duplicate_order_ref";
        case Anomaly::OverExecution: return "over_execution";
        case Anomaly::ZeroShares: return "zero_shares";
        case Anomaly::LocateMismatch: return "locate_mismatch";
        case Anomaly::SymbolMismatch: return "symbol_mismatch";
        case Anomaly::DirectoryConflict: return "directory_conflict";
    }
    return "unknown";
}

std::uint64_t BookStats::anomaly_count() const noexcept {
    std::uint64_t n = 0;
    for (std::size_t i = 1; i < anomalies.size(); ++i) {
        n += anomalies[i];
    }
    return n;
}

BookBuilder::BookBuilder(std::size_t expected_orders) : impl_(std::make_unique<Impl>()) {
    if (expected_orders != 0) {
        impl_->index.reserve(expected_orders);
        impl_->pool.reserve(expected_orders);
    }
}
BookBuilder::~BookBuilder() = default;

ApplyOutcome BookBuilder::apply(const Message& m) {
    Impl& im = *impl_;
    const Header& h = header_of(m);
    ++stats_.messages;
    ++stats_.messages_by_type[static_cast<unsigned char>(h.type)];
    if (h.timestamp_ns < stats_.last_timestamp_ns) {
        ++stats_.timestamp_regressions;
    }
    stats_.last_timestamp_ns = h.timestamp_ns;

    auto reject = [&](Anomaly a) {
        ++stats_.rejected;
        ++stats_.anomalies[static_cast<std::size_t>(a)];
        return ApplyOutcome{ApplyStatus::Rejected, a};
    };
    auto applied = [&](Anomaly info = Anomaly::None) {
        ++stats_.applied;
        if (info != Anomaly::None) {
            ++stats_.anomalies[static_cast<std::size_t>(info)];
        }
        stats_.live_orders = im.index.size();
        stats_.levels_created = im.levels_created;
        stats_.levels_removed = im.levels_removed;
        stats_.peak_live_orders = std::max(stats_.peak_live_orders, stats_.live_orders);
        return ApplyOutcome{ApplyStatus::Applied, info};
    };
    // Validates a modify message's target: live, same locate.
    auto target = [&](OrderRef ref, std::uint16_t locate, Anomaly* why) -> std::optional<std::uint32_t> {
        const auto slot = im.lookup(ref);
        if (!slot) {
            *why = Anomaly::UnknownOrderRef;
            return std::nullopt;
        }
        if (im.pool[*slot].locate != locate) {
            *why = Anomaly::LocateMismatch;
            return std::nullopt;
        }
        return slot;
    };
    // Shared by E, C and X: reduce by `shares`, which must be in [1, remaining].
    auto reduce = [&](OrderRef ref, std::uint32_t shares) -> std::optional<ApplyOutcome> {
        Anomaly why = Anomaly::None;
        const auto slot = target(ref, h.stock_locate, &why);
        if (!slot) {
            return reject(why);
        }
        if (shares == 0) {
            return reject(Anomaly::ZeroShares);
        }
        if (shares > im.pool[*slot].shares) {
            return reject(Anomaly::OverExecution);
        }
        im.reduce(*slot, shares);
        return std::nullopt;
    };

    return std::visit(
        [&](const auto& msg) -> ApplyOutcome {
            using T = std::decay_t<decltype(msg)>;
            if constexpr (std::is_same_v<T, AddOrder>) {
                if (im.index.contains(msg.order_ref)) {
                    return reject(Anomaly::DuplicateOrderRef);
                }
                if (msg.shares == 0) {
                    return reject(Anomaly::ZeroShares);
                }
                Anomaly info = Anomaly::None;
                DirectoryEntry& d = im.directory[h.stock_locate];
                if (d.symbol && *d.symbol != msg.stock) {
                    info = Anomaly::SymbolMismatch;
                } else if (!d.symbol) {
                    d.symbol = msg.stock;  // no directory message seen: learn from the add
                }
                im.add(h.stock_locate, msg.order_ref, msg.side, msg.price, msg.shares,
                       msg.attribution);
                ++stats_.adds;
                return applied(info);
            } else if constexpr (std::is_same_v<T, OrderExecuted>) {
                if (auto r = reduce(msg.order_ref, msg.executed_shares)) {
                    return *r;
                }
                ++stats_.executions;
                stats_.executed_shares += msg.executed_shares;
                stats_.printable_executed_shares += msg.executed_shares;
                return applied();
            } else if constexpr (std::is_same_v<T, OrderExecutedWithPrice>) {
                if (auto r = reduce(msg.order_ref, msg.executed_shares)) {
                    return *r;
                }
                ++stats_.executions;
                stats_.executed_shares += msg.executed_shares;
                if (msg.printable) {
                    stats_.printable_executed_shares += msg.executed_shares;
                }
                return applied();
            } else if constexpr (std::is_same_v<T, OrderCancel>) {
                if (auto r = reduce(msg.order_ref, msg.canceled_shares)) {
                    return *r;
                }
                ++stats_.cancels;
                stats_.canceled_shares += msg.canceled_shares;
                return applied();
            } else if constexpr (std::is_same_v<T, OrderDelete>) {
                Anomaly why = Anomaly::None;
                const auto slot = target(msg.order_ref, h.stock_locate, &why);
                if (!slot) {
                    return reject(why);
                }
                im.remove(*slot);
                ++stats_.deletes;
                return applied();
            } else if constexpr (std::is_same_v<T, OrderReplace>) {
                Anomaly why = Anomaly::None;
                const auto slot = target(msg.original_ref, h.stock_locate, &why);
                if (!slot) {
                    return reject(why);
                }
                if (msg.new_ref != msg.original_ref && im.index.contains(msg.new_ref)) {
                    return reject(Anomaly::DuplicateOrderRef);
                }
                if (msg.shares == 0) {
                    return reject(Anomaly::ZeroShares);
                }
                const Side side = im.pool[*slot].side;
                const std::optional<Mpid> attribution = im.pool[*slot].attribution;
                im.remove(*slot);
                im.add(h.stock_locate, msg.new_ref, side, msg.price, msg.shares, attribution);
                ++stats_.replaces;
                return applied();
            } else {
                if constexpr (std::is_same_v<T, StockDirectory>) {
                    DirectoryEntry& d = im.directory[h.stock_locate];
                    const bool conflict = d.symbol && *d.symbol != msg.stock;
                    d.symbol = msg.stock;
                    if (conflict) {
                        ++stats_.anomalies[static_cast<std::size_t>(Anomaly::DirectoryConflict)];
                        return ApplyOutcome{ApplyStatus::NoBookEffect, Anomaly::DirectoryConflict};
                    }
                } else if constexpr (std::is_same_v<T, StockTradingAction>) {
                    im.directory[h.stock_locate].trading_state = msg.trading_state;
                } else if constexpr (std::is_same_v<T, SystemEvent>) {
                    stats_.last_system_event = msg.event_code;
                } else if constexpr (std::is_same_v<T, Trade>) {
                    stats_.non_displayed_trade_shares += msg.shares;
                } else if constexpr (std::is_same_v<T, CrossTrade>) {
                    stats_.cross_shares += msg.shares;
                } else if constexpr (std::is_same_v<T, BrokenTrade>) {
                    ++stats_.broken_trades;
                }
                return ApplyOutcome{ApplyStatus::NoBookEffect, Anomaly::None};
            }
        },
        m);
}

std::vector<std::uint16_t> BookBuilder::locates() const {
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < kLocates; ++i) {
        if (impl_->books[i] || impl_->directory[i].symbol) {
            out.push_back(static_cast<std::uint16_t>(i));
        }
    }
    return out;
}

std::optional<std::string> BookBuilder::symbol(std::uint16_t locate) const {
    const auto& d = impl_->directory[locate];
    if (!d.symbol) {
        return std::nullopt;
    }
    return std::string(trim_alpha(*d.symbol));
}

std::optional<std::uint16_t> BookBuilder::find_locate(std::string_view symbol) const {
    for (std::size_t i = 0; i < kLocates; ++i) {
        const auto& d = impl_->directory[i];
        if (d.symbol && trim_alpha(*d.symbol) == symbol) {
            return static_cast<std::uint16_t>(i);
        }
    }
    return std::nullopt;
}

std::optional<char> BookBuilder::trading_state(std::uint16_t locate) const {
    const char s = impl_->directory[locate].trading_state;
    if (s == 0) {
        return std::nullopt;
    }
    return s;
}

TopOfBook BookBuilder::top(std::uint16_t locate) const {
    TopOfBook t;
    const auto& b = impl_->books[locate];
    if (!b) {
        return t;
    }
    b->bids.for_each_best_first(1, [&](Price4 price, const Level& level) {
        t.bid = LevelView{price, level.shares, level.count};
    });
    b->asks.for_each_best_first(1, [&](Price4 price, const Level& level) {
        t.ask = LevelView{price, level.shares, level.count};
    });
    return t;
}

namespace {

// Visits levels of one side best-first.
template <typename F>
void for_each_level(const SideLevels& levels, Side, std::size_t max_levels, F&& f) {
    levels.for_each_best_first(max_levels, std::forward<F>(f));
}

}  // namespace

std::vector<LevelView> BookBuilder::depth(std::uint16_t locate, Side side,
                                          std::size_t max_levels) const {
    std::vector<LevelView> out;
    const auto& b = impl_->books[locate];
    if (b) {
        for_each_level(b->side(side), side, max_levels, [&](Price4 p, const Level& l) {
            out.push_back(LevelView{p, l.shares, l.count});
        });
    }
    return out;
}

std::vector<OrderView> BookBuilder::orders(std::uint16_t locate, Side side) const {
    std::vector<OrderView> out;
    const auto& b = impl_->books[locate];
    if (b) {
        for_each_level(b->side(side), side, 0, [&](Price4, const Level& l) {
            for (std::uint32_t s = l.head; s != kNil; s = impl_->pool[s].next) {
                const Node& n = impl_->pool[s];
                out.push_back(OrderView{n.ref, n.side, n.price, n.shares});
            }
        });
    }
    return out;
}

std::optional<OrderView> BookBuilder::find_order(OrderRef ref) const {
    const auto slot = impl_->lookup(ref);
    if (!slot) {
        return std::nullopt;
    }
    const Node& n = impl_->pool[*slot];
    return OrderView{n.ref, n.side, n.price, n.shares};
}

std::optional<Mpid> BookBuilder::attribution(OrderRef ref) const {
    const auto slot = impl_->lookup(ref);
    if (!slot) {
        return std::nullopt;
    }
    return impl_->pool[*slot].attribution;
}

std::size_t BookBuilder::level_count(std::uint16_t locate, Side side) const {
    const auto& b = impl_->books[locate];
    return b ? b->side(side).size() : 0;
}

std::string BookBuilder::canonical_state(bool include_orders) const {
    std::string out;
    char buf[96];
    for (const std::uint16_t locate : locates()) {
        std::snprintf(buf, sizeof buf, "locate %u ", static_cast<unsigned>(locate));
        out += buf;
        // All 8 symbol bytes, non-printables escaped: distinct symbols never collide (an
        // embedded NUL or different padding stays visible).
        if (const auto& sym = impl_->directory[locate].symbol) {
            for (const char c : *sym) {
                const auto u = static_cast<unsigned char>(c);
                if (u >= 0x21 && u <= 0x7E && c != '\\') {
                    out += c;
                } else {
                    std::snprintf(buf, sizeof buf, "\\x%02X", u);
                    out += buf;
                }
            }
        } else {
            out += '-';
        }
        out += '\n';
        if (!impl_->books[locate]) {
            continue;  // directory entry only: no orders seen
        }
        for (const Side side : {Side::Buy, Side::Sell}) {
            for_each_level(impl_->books[locate]->side(side), side, 0,
                           [&](Price4 p, const Level& l) {
                               std::snprintf(buf, sizeof buf, "%c %u %llu %u",
                                             static_cast<char>(side), p,
                                             static_cast<unsigned long long>(l.shares), l.count);
                               out += buf;
                               if (include_orders) {
                                   for (std::uint32_t s = l.head; s != kNil;
                                        s = impl_->pool[s].next) {
                                       std::snprintf(
                                           buf, sizeof buf, " %llu:%u",
                                           static_cast<unsigned long long>(impl_->pool[s].ref),
                                           impl_->pool[s].shares);
                                       out += buf;
                                   }
                               }
                               out += '\n';
                           });
        }
    }
    return out;
}

std::string BookBuilder::state_hash() const {
    return Sha256::hex_digest(canonical_state(true));
}

std::optional<std::string> BookBuilder::check_invariants() const {
    const Impl& im = *impl_;
    std::size_t linked = 0;
    for (const std::uint16_t locate : locates()) {
        if (!im.books[locate]) {
            continue;
        }
        for (const Side side : {Side::Buy, Side::Sell}) {
            std::optional<std::string> problem;
            std::optional<Price4> previous_price;
            im.books[locate]->side(side).for_each_best_first(0, [&](Price4 price,
                                                                    const Level& level) {
                if (problem) {
                    return;
                }
                // Best-first order must be strictly decreasing for bids, increasing for asks.
                if (previous_price && (side == Side::Buy ? price >= *previous_price
                                                         : price <= *previous_price)) {
                    problem = "levels out of order at price " + std::to_string(price);
                    return;
                }
                previous_price = price;
                if (level.count == 0) {
                    problem = "empty level kept at price " + std::to_string(price);
                    return;
                }
                std::uint64_t shares = 0;
                std::uint32_t count = 0;
                std::uint32_t prev = kNil;
                for (std::uint32_t s = level.head; s != kNil; s = im.pool[s].next) {
                    const Node& n = im.pool[s];
                    if (n.prev != prev || n.level != &level || n.price != price ||
                        n.side != side || n.locate != locate || n.shares == 0) {
                        problem = "corrupt order node for ref " + std::to_string(n.ref);
                        return;
                    }
                    if (im.index.find(n.ref) != s) {
                        problem = "index does not point at ref " + std::to_string(n.ref);
                        return;
                    }
                    shares += n.shares;
                    ++count;
                    prev = s;
                }
                if (prev != level.tail || shares != level.shares || count != level.count) {
                    problem = "level totals disagree at price " + std::to_string(price);
                    return;
                }
                linked += count;
            });
            if (problem) {
                return problem;
            }
        }
    }
    if (linked != im.index.size() || im.index.size() + im.free_slots.size() != im.pool.size()) {
        return "index/pool size mismatch";
    }
    return std::nullopt;
}

}  // namespace enginex::itch
