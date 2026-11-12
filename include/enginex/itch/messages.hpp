#pragma once

// Nasdaq TotalView-ITCH 5.0 message definitions.
//
// Source: "Nasdaq TotalView-ITCH 5.0" interface specification, nasdaqtrader.com
// (NQTVITCHspecification.pdf, revision log through 2023-04-28; offsets also cross-checked against
// the 2015-03-06 NQTVITCHSpecification_5.0.pdf). Every layout below lists the specification's
// byte offsets. Field names follow the specification. See docs/itch.md for the supported-message
// matrix and what the book reconstructor does with each type.
//
// ITCH is an outbound market-data protocol. Nothing here is an order-entry message, and these
// types are deliberately unrelated to the synthetic exchange's Command types.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

namespace enginex::itch {

// Price(4): integer with 4 implied decimals (1 = $0.0001). Price(8): 8 implied decimals.
using Price4 = std::uint32_t;
using Price8 = std::uint64_t;
using OrderRef = std::uint64_t;
using Symbol = std::array<char, 8>;  // right-padded with spaces
using Mpid = std::array<char, 4>;

// Fields common to every message: type @0, stock locate @1 (u16), tracking number @3 (u16),
// timestamp @5 (u48, nanoseconds since midnight).
struct Header {
    char type = 0;
    std::uint16_t stock_locate = 0;
    std::uint16_t tracking_number = 0;
    std::uint64_t timestamp_ns = 0;
};
inline constexpr std::size_t kHeaderSize = 11;

enum class Side : char { Buy = 'B', Sell = 'S' };

// ---- administrative / system --------------------------------------------------------------

struct SystemEvent {  // 'S', 12 bytes
    Header h;
    char event_code = 0;  // @11 'O','S','Q','M','E','C'
};

struct StockDirectory {  // 'R', 39 bytes
    Header h;
    Symbol stock{};                         // @11
    char market_category = 0;               // @19
    char financial_status = 0;              // @20
    std::uint32_t round_lot_size = 0;       // @21
    char round_lots_only = 0;               // @25
    char issue_classification = 0;          // @26
    std::array<char, 2> issue_subtype{};    // @27
    char authenticity = 0;                  // @29
    char short_sale_threshold = 0;          // @30
    char ipo_flag = 0;                      // @31
    char luld_tier = 0;                     // @32
    char etp_flag = 0;                      // @33
    std::uint32_t etp_leverage_factor = 0;  // @34
    char inverse_indicator = 0;             // @38
};

struct StockTradingAction {  // 'H', 25 bytes
    Header h;
    Symbol stock{};                // @11
    char trading_state = 0;        // @19 'H','P','Q','T'
    char reserved = 0;             // @20
    std::array<char, 4> reason{};  // @21
};

struct RegShoRestriction {  // 'Y', 20 bytes
    Header h;
    Symbol stock{};           // @11
    char reg_sho_action = 0;  // @19
};

struct MarketParticipantPosition {  // 'L', 26 bytes
    Header h;
    Mpid mpid{};                    // @11
    Symbol stock{};                 // @15
    char primary_market_maker = 0;  // @23
    char market_maker_mode = 0;     // @24
    char participant_state = 0;     // @25
};

struct MwcbDeclineLevel {  // 'V', 35 bytes
    Header h;
    Price8 level1 = 0;  // @11
    Price8 level2 = 0;  // @19
    Price8 level3 = 0;  // @27
};

struct MwcbStatus {  // 'W', 12 bytes
    Header h;
    char breached_level = 0;  // @11
};

struct IpoQuotingPeriod {  // 'K', 28 bytes
    Header h;
    Symbol stock{};                  // @11
    std::uint32_t release_time = 0;  // @19 seconds since midnight
    char release_qualifier = 0;      // @23
    Price4 ipo_price = 0;            // @24
};

struct LuldAuctionCollar {  // 'J', 35 bytes
    Header h;
    Symbol stock{};               // @11
    Price4 reference_price = 0;   // @19
    Price4 upper_collar = 0;      // @23
    Price4 lower_collar = 0;      // @27
    std::uint32_t extension = 0;  // @31
};

struct OperationalHalt {  // 'h', 21 bytes
    Header h;
    Symbol stock{};        // @11
    char market_code = 0;  // @19
    char halt_action = 0;  // @20
};

// ---- order book messages ------------------------------------------------------------------

struct AddOrder {  // 'A', 36 bytes; 'F' (with attribution) 40 bytes
    Header h;
    OrderRef order_ref = 0;           // @11
    Side side = Side::Buy;            // @19 'B'/'S'
    std::uint32_t shares = 0;         // @20
    Symbol stock{};                   // @24
    Price4 price = 0;                 // @32
    std::optional<Mpid> attribution;  // @36, 'F' only
};

struct OrderExecuted {  // 'E', 31 bytes
    Header h;
    OrderRef order_ref = 0;             // @11
    std::uint32_t executed_shares = 0;  // @19
    std::uint64_t match_number = 0;     // @23
};

struct OrderExecutedWithPrice {  // 'C', 36 bytes
    Header h;
    OrderRef order_ref = 0;             // @11
    std::uint32_t executed_shares = 0;  // @19
    std::uint64_t match_number = 0;     // @23
    bool printable = false;             // @31 'Y'/'N'
    Price4 execution_price = 0;         // @32
};

struct OrderCancel {  // 'X', 23 bytes
    Header h;
    OrderRef order_ref = 0;             // @11
    std::uint32_t canceled_shares = 0;  // @19
};

struct OrderDelete {  // 'D', 19 bytes
    Header h;
    OrderRef order_ref = 0;  // @11
};

struct OrderReplace {  // 'U', 35 bytes
    Header h;
    OrderRef original_ref = 0;  // @11
    OrderRef new_ref = 0;       // @19
    std::uint32_t shares = 0;   // @27
    Price4 price = 0;           // @31
};

// ---- trades (never change displayed order state) ------------------------------------------

struct Trade {  // 'P', 44 bytes: execution of a non-displayed order
    Header h;
    OrderRef order_ref = 0;          // @11 (zero-filled since 2010)
    char side = 0;                   // @19 (always 'B' since 2014-07-14)
    std::uint32_t shares = 0;        // @20
    Symbol stock{};                  // @24
    Price4 price = 0;                // @32
    std::uint64_t match_number = 0;  // @36
};

struct CrossTrade {  // 'Q', 40 bytes
    Header h;
    std::uint64_t shares = 0;        // @11 (8 bytes in this message)
    Symbol stock{};                  // @19
    Price4 cross_price = 0;          // @27
    std::uint64_t match_number = 0;  // @31
    char cross_type = 0;             // @39
};

struct BrokenTrade {  // 'B', 19 bytes
    Header h;
    std::uint64_t match_number = 0;  // @11
};

// ---- auction / retail information ---------------------------------------------------------

struct Noii {  // 'I', 50 bytes
    Header h;
    std::uint64_t paired_shares = 0;     // @11
    std::uint64_t imbalance_shares = 0;  // @19
    char imbalance_direction = 0;        // @27
    Symbol stock{};                      // @28
    Price4 far_price = 0;                // @36
    Price4 near_price = 0;               // @40
    Price4 current_reference_price = 0;  // @44
    char cross_type = 0;                 // @48
    char price_variation_indicator = 0;  // @49
};

struct RetailInterest {  // 'N', 20 bytes (RPII)
    Header h;
    Symbol stock{};          // @11
    char interest_flag = 0;  // @19
};

struct DlcrPriceDiscovery {  // 'O', 48 bytes (added 2023-04-28)
    Header h;
    Symbol stock{};                         // @11
    char open_eligibility_status = 0;       // @19
    Price4 minimum_allowable_price = 0;     // @20
    Price4 maximum_allowable_price = 0;     // @24
    Price4 near_execution_price = 0;        // @28
    std::uint64_t near_execution_time = 0;  // @32
    Price4 lower_price_range_collar = 0;    // @40
    Price4 upper_price_range_collar = 0;    // @44
};

using Message =
    std::variant<SystemEvent, StockDirectory, StockTradingAction, RegShoRestriction,
                 MarketParticipantPosition, MwcbDeclineLevel, MwcbStatus, IpoQuotingPeriod,
                 LuldAuctionCollar, OperationalHalt, AddOrder, OrderExecuted,
                 OrderExecutedWithPrice, OrderCancel, OrderDelete, OrderReplace, Trade, CrossTrade,
                 BrokenTrade, Noii, RetailInterest, DlcrPriceDiscovery>;

// Specified total length (including the type byte) of each known message type; nullopt for a
// type this decoder does not know.
[[nodiscard]] constexpr std::optional<std::size_t> message_length(char type) noexcept {
    switch (type) {
        case 'S':
            return 12;
        case 'R':
            return 39;
        case 'H':
            return 25;
        case 'Y':
            return 20;
        case 'L':
            return 26;
        case 'V':
            return 35;
        case 'W':
            return 12;
        case 'K':
            return 28;
        case 'J':
            return 35;
        case 'h':
            return 21;
        case 'A':
            return 36;
        case 'F':
            return 40;
        case 'E':
            return 31;
        case 'C':
            return 36;
        case 'X':
            return 23;
        case 'D':
            return 19;
        case 'U':
            return 35;
        case 'P':
            return 44;
        case 'Q':
            return 40;
        case 'B':
            return 19;
        case 'I':
            return 50;
        case 'N':
            return 20;
        case 'O':
            return 48;
        default:
            return std::nullopt;
    }
}

inline constexpr std::string_view kKnownTypes = "SRHYLVWKJhAFECXDUPQBINO";

[[nodiscard]] const Header& header_of(const Message& m) noexcept;
[[nodiscard]] std::string_view type_name(char type) noexcept;

}  // namespace enginex::itch
