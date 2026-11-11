// ITCH 5.0 payload decoder and encoder. Offsets are the specification's (see messages.hpp).
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "enginex/itch/byte_reader.hpp"
#include "enginex/itch/decoder.hpp"

namespace enginex::itch {

namespace {

Header read_header(const ByteReader& r) {
    Header h;
    h.type = r.alpha(0);
    h.stock_locate = r.u16(1);
    h.tracking_number = r.u16(3);
    h.timestamp_ns = r.u48(5);
    return h;
}

// Decodes a payload whose length already matches message_length(type).
DecodeResult decode_known(const ByteReader& r) {
    const Header h = read_header(r);
    switch (h.type) {
        case 'S':
            return Message{SystemEvent{h, r.alpha(11)}};
        case 'R': {
            StockDirectory m;
            m.h = h;
            m.stock = r.alpha<8>(11);
            m.market_category = r.alpha(19);
            m.financial_status = r.alpha(20);
            m.round_lot_size = r.u32(21);
            m.round_lots_only = r.alpha(25);
            m.issue_classification = r.alpha(26);
            m.issue_subtype = r.alpha<2>(27);
            m.authenticity = r.alpha(29);
            m.short_sale_threshold = r.alpha(30);
            m.ipo_flag = r.alpha(31);
            m.luld_tier = r.alpha(32);
            m.etp_flag = r.alpha(33);
            m.etp_leverage_factor = r.u32(34);
            m.inverse_indicator = r.alpha(38);
            return Message{m};
        }
        case 'H':
            return Message{
                StockTradingAction{h, r.alpha<8>(11), r.alpha(19), r.alpha(20), r.alpha<4>(21)}};
        case 'Y':
            return Message{RegShoRestriction{h, r.alpha<8>(11), r.alpha(19)}};
        case 'L':
            return Message{MarketParticipantPosition{h, r.alpha<4>(11), r.alpha<8>(15),
                                                     r.alpha(23), r.alpha(24), r.alpha(25)}};
        case 'V':
            return Message{MwcbDeclineLevel{h, r.u64(11), r.u64(19), r.u64(27)}};
        case 'W':
            return Message{MwcbStatus{h, r.alpha(11)}};
        case 'K':
            return Message{IpoQuotingPeriod{h, r.alpha<8>(11), r.u32(19), r.alpha(23), r.u32(24)}};
        case 'J':
            return Message{
                LuldAuctionCollar{h, r.alpha<8>(11), r.u32(19), r.u32(23), r.u32(27), r.u32(31)}};
        case 'h':
            return Message{OperationalHalt{h, r.alpha<8>(11), r.alpha(19), r.alpha(20)}};
        case 'A':
        case 'F': {
            const char side = r.alpha(19);
            if (side != 'B' && side != 'S') {
                return DecodeError::InvalidField;
            }
            AddOrder m;
            m.h = h;
            m.order_ref = r.u64(11);
            m.side = side == 'B' ? Side::Buy : Side::Sell;
            m.shares = r.u32(20);
            m.stock = r.alpha<8>(24);
            m.price = r.u32(32);
            if (h.type == 'F') {
                m.attribution = r.alpha<4>(36);
            }
            return Message{m};
        }
        case 'E':
            return Message{OrderExecuted{h, r.u64(11), r.u32(19), r.u64(23)}};
        case 'C': {
            const char printable = r.alpha(31);
            if (printable != 'Y' && printable != 'N') {
                return DecodeError::InvalidField;
            }
            return Message{
                OrderExecutedWithPrice{h, r.u64(11), r.u32(19), r.u64(23), printable == 'Y',
                                       r.u32(32)}};
        }
        case 'X':
            return Message{OrderCancel{h, r.u64(11), r.u32(19)}};
        case 'D':
            return Message{OrderDelete{h, r.u64(11)}};
        case 'U':
            return Message{OrderReplace{h, r.u64(11), r.u64(19), r.u32(27), r.u32(31)}};
        case 'P':
            return Message{
                Trade{h, r.u64(11), r.alpha(19), r.u32(20), r.alpha<8>(24), r.u32(32), r.u64(36)}};
        case 'Q':
            return Message{CrossTrade{h, r.u64(11), r.alpha<8>(19), r.u32(27), r.u64(31),
                                      r.alpha(39)}};
        case 'B':
            return Message{BrokenTrade{h, r.u64(11)}};
        case 'I':
            return Message{Noii{h, r.u64(11), r.u64(19), r.alpha(27), r.alpha<8>(28), r.u32(36),
                                r.u32(40), r.u32(44), r.alpha(48), r.alpha(49)}};
        case 'N':
            return Message{RetailInterest{h, r.alpha<8>(11), r.alpha(19)}};
        case 'O':
            return Message{DlcrPriceDiscovery{h, r.alpha<8>(11), r.alpha(19), r.u32(20),
                                              r.u32(24), r.u32(28), r.u64(32), r.u32(40),
                                              r.u32(44)}};
        default:
            return DecodeError::UnknownType;
    }
}

// Fixed-size output buffer with offset-checked writes.
class Writer {
public:
    explicit Writer(std::size_t size) : out_(size, 0) {}
    void be(std::size_t off, std::uint64_t v, std::size_t width) {
        check(off, width);
        put_be(out_.data() + off, v, width);
    }
    void ch(std::size_t off, char c) {
        check(off, 1);
        out_[off] = static_cast<std::uint8_t>(c);
    }
    template <std::size_t N>
    void str(std::size_t off, const std::array<char, N>& a) {
        check(off, N);
        std::memcpy(out_.data() + off, a.data(), N);
    }
    void header(const Header& h, char type) {
        if (h.timestamp_ns >= (1ULL << 48U)) {
            throw std::out_of_range("ITCH timestamp does not fit in 6 bytes");
        }
        ch(0, type);
        be(1, h.stock_locate, 2);
        be(3, h.tracking_number, 2);
        be(5, h.timestamp_ns, 6);
    }
    std::vector<std::uint8_t> take() { return std::move(out_); }

private:
    void check(std::size_t off, std::size_t width) const {
        if (off + width > out_.size()) {
            throw std::out_of_range("ITCH encode past end of message");
        }
    }
    std::vector<std::uint8_t> out_;
};

template <typename T>
constexpr char type_char() {
    if constexpr (std::is_same_v<T, SystemEvent>) return 'S';
    else if constexpr (std::is_same_v<T, StockDirectory>) return 'R';
    else if constexpr (std::is_same_v<T, StockTradingAction>) return 'H';
    else if constexpr (std::is_same_v<T, RegShoRestriction>) return 'Y';
    else if constexpr (std::is_same_v<T, MarketParticipantPosition>) return 'L';
    else if constexpr (std::is_same_v<T, MwcbDeclineLevel>) return 'V';
    else if constexpr (std::is_same_v<T, MwcbStatus>) return 'W';
    else if constexpr (std::is_same_v<T, IpoQuotingPeriod>) return 'K';
    else if constexpr (std::is_same_v<T, LuldAuctionCollar>) return 'J';
    else if constexpr (std::is_same_v<T, OperationalHalt>) return 'h';
    else if constexpr (std::is_same_v<T, OrderExecuted>) return 'E';
    else if constexpr (std::is_same_v<T, OrderExecutedWithPrice>) return 'C';
    else if constexpr (std::is_same_v<T, OrderCancel>) return 'X';
    else if constexpr (std::is_same_v<T, OrderDelete>) return 'D';
    else if constexpr (std::is_same_v<T, OrderReplace>) return 'U';
    else if constexpr (std::is_same_v<T, Trade>) return 'P';
    else if constexpr (std::is_same_v<T, CrossTrade>) return 'Q';
    else if constexpr (std::is_same_v<T, BrokenTrade>) return 'B';
    else if constexpr (std::is_same_v<T, Noii>) return 'I';
    else if constexpr (std::is_same_v<T, RetailInterest>) return 'N';
    else if constexpr (std::is_same_v<T, DlcrPriceDiscovery>) return 'O';
    else return 0;  // AddOrder: 'A' or 'F' depending on attribution
}

}  // namespace

std::string_view to_string(DecodeError e) noexcept {
    switch (e) {
        case DecodeError::Empty: return "empty";
        case DecodeError::UnknownType: return "unknown_type";
        case DecodeError::LengthMismatch: return "length_mismatch";
        case DecodeError::InvalidField: return "invalid_field";
    }
    return "unknown";
}

std::string_view to_string(FrameError e) noexcept {
    switch (e) {
        case FrameError::None: return "none";
        case FrameError::ZeroLength: return "zero_length";
        case FrameError::TruncatedTail: return "truncated_tail";
    }
    return "unknown";
}

DecodeResult decode(std::span<const std::uint8_t> payload) noexcept {
    if (payload.empty()) {
        return DecodeError::Empty;
    }
    const char type = static_cast<char>(payload[0]);
    const auto expected = message_length(type);
    if (!expected) {
        return DecodeError::UnknownType;
    }
    if (payload.size() != *expected) {
        return DecodeError::LengthMismatch;
    }
    // Every read in decode_known is within *expected bytes, so ByteReader cannot throw here;
    // the checks are a second line of defense against a wrong offset table.
    try {
        return decode_known(ByteReader(payload));
    } catch (const std::out_of_range&) {
        return DecodeError::LengthMismatch;
    }
}

const Header& header_of(const Message& m) noexcept {
    return std::visit([](const auto& x) -> const Header& { return x.h; }, m);
}

std::string_view type_name(char type) noexcept {
    switch (type) {
        case 'S': return "system_event";
        case 'R': return "stock_directory";
        case 'H': return "stock_trading_action";
        case 'Y': return "reg_sho_restriction";
        case 'L': return "market_participant_position";
        case 'V': return "mwcb_decline_level";
        case 'W': return "mwcb_status";
        case 'K': return "ipo_quoting_period";
        case 'J': return "luld_auction_collar";
        case 'h': return "operational_halt";
        case 'A': return "add_order";
        case 'F': return "add_order_mpid";
        case 'E': return "order_executed";
        case 'C': return "order_executed_with_price";
        case 'X': return "order_cancel";
        case 'D': return "order_delete";
        case 'U': return "order_replace";
        case 'P': return "trade";
        case 'Q': return "cross_trade";
        case 'B': return "broken_trade";
        case 'I': return "noii";
        case 'N': return "retail_interest";
        case 'O': return "dlcr_price_discovery";
        default: return "unknown";
    }
}

std::vector<std::uint8_t> encode(const Message& message) {
    return std::visit(
        [](const auto& m) -> std::vector<std::uint8_t> {
            using T = std::decay_t<decltype(m)>;
            char type = type_char<T>();
            if constexpr (std::is_same_v<T, AddOrder>) {
                type = m.attribution ? 'F' : 'A';
            }
            Writer w(*message_length(type));
            w.header(m.h, type);
            if constexpr (std::is_same_v<T, SystemEvent>) {
                w.ch(11, m.event_code);
            } else if constexpr (std::is_same_v<T, StockDirectory>) {
                w.str(11, m.stock);
                w.ch(19, m.market_category);
                w.ch(20, m.financial_status);
                w.be(21, m.round_lot_size, 4);
                w.ch(25, m.round_lots_only);
                w.ch(26, m.issue_classification);
                w.str(27, m.issue_subtype);
                w.ch(29, m.authenticity);
                w.ch(30, m.short_sale_threshold);
                w.ch(31, m.ipo_flag);
                w.ch(32, m.luld_tier);
                w.ch(33, m.etp_flag);
                w.be(34, m.etp_leverage_factor, 4);
                w.ch(38, m.inverse_indicator);
            } else if constexpr (std::is_same_v<T, StockTradingAction>) {
                w.str(11, m.stock);
                w.ch(19, m.trading_state);
                w.ch(20, m.reserved);
                w.str(21, m.reason);
            } else if constexpr (std::is_same_v<T, RegShoRestriction>) {
                w.str(11, m.stock);
                w.ch(19, m.reg_sho_action);
            } else if constexpr (std::is_same_v<T, MarketParticipantPosition>) {
                w.str(11, m.mpid);
                w.str(15, m.stock);
                w.ch(23, m.primary_market_maker);
                w.ch(24, m.market_maker_mode);
                w.ch(25, m.participant_state);
            } else if constexpr (std::is_same_v<T, MwcbDeclineLevel>) {
                w.be(11, m.level1, 8);
                w.be(19, m.level2, 8);
                w.be(27, m.level3, 8);
            } else if constexpr (std::is_same_v<T, MwcbStatus>) {
                w.ch(11, m.breached_level);
            } else if constexpr (std::is_same_v<T, IpoQuotingPeriod>) {
                w.str(11, m.stock);
                w.be(19, m.release_time, 4);
                w.ch(23, m.release_qualifier);
                w.be(24, m.ipo_price, 4);
            } else if constexpr (std::is_same_v<T, LuldAuctionCollar>) {
                w.str(11, m.stock);
                w.be(19, m.reference_price, 4);
                w.be(23, m.upper_collar, 4);
                w.be(27, m.lower_collar, 4);
                w.be(31, m.extension, 4);
            } else if constexpr (std::is_same_v<T, OperationalHalt>) {
                w.str(11, m.stock);
                w.ch(19, m.market_code);
                w.ch(20, m.halt_action);
            } else if constexpr (std::is_same_v<T, AddOrder>) {
                w.be(11, m.order_ref, 8);
                w.ch(19, static_cast<char>(m.side));
                w.be(20, m.shares, 4);
                w.str(24, m.stock);
                w.be(32, m.price, 4);
                if (m.attribution) {
                    w.str(36, *m.attribution);
                }
            } else if constexpr (std::is_same_v<T, OrderExecuted>) {
                w.be(11, m.order_ref, 8);
                w.be(19, m.executed_shares, 4);
                w.be(23, m.match_number, 8);
            } else if constexpr (std::is_same_v<T, OrderExecutedWithPrice>) {
                w.be(11, m.order_ref, 8);
                w.be(19, m.executed_shares, 4);
                w.be(23, m.match_number, 8);
                w.ch(31, m.printable ? 'Y' : 'N');
                w.be(32, m.execution_price, 4);
            } else if constexpr (std::is_same_v<T, OrderCancel>) {
                w.be(11, m.order_ref, 8);
                w.be(19, m.canceled_shares, 4);
            } else if constexpr (std::is_same_v<T, OrderDelete>) {
                w.be(11, m.order_ref, 8);
            } else if constexpr (std::is_same_v<T, OrderReplace>) {
                w.be(11, m.original_ref, 8);
                w.be(19, m.new_ref, 8);
                w.be(27, m.shares, 4);
                w.be(31, m.price, 4);
            } else if constexpr (std::is_same_v<T, Trade>) {
                w.be(11, m.order_ref, 8);
                w.ch(19, m.side);
                w.be(20, m.shares, 4);
                w.str(24, m.stock);
                w.be(32, m.price, 4);
                w.be(36, m.match_number, 8);
            } else if constexpr (std::is_same_v<T, CrossTrade>) {
                w.be(11, m.shares, 8);
                w.str(19, m.stock);
                w.be(27, m.cross_price, 4);
                w.be(31, m.match_number, 8);
                w.ch(39, m.cross_type);
            } else if constexpr (std::is_same_v<T, BrokenTrade>) {
                w.be(11, m.match_number, 8);
            } else if constexpr (std::is_same_v<T, Noii>) {
                w.be(11, m.paired_shares, 8);
                w.be(19, m.imbalance_shares, 8);
                w.ch(27, m.imbalance_direction);
                w.str(28, m.stock);
                w.be(36, m.far_price, 4);
                w.be(40, m.near_price, 4);
                w.be(44, m.current_reference_price, 4);
                w.ch(48, m.cross_type);
                w.ch(49, m.price_variation_indicator);
            } else if constexpr (std::is_same_v<T, RetailInterest>) {
                w.str(11, m.stock);
                w.ch(19, m.interest_flag);
            } else if constexpr (std::is_same_v<T, DlcrPriceDiscovery>) {
                w.str(11, m.stock);
                w.ch(19, m.open_eligibility_status);
                w.be(20, m.minimum_allowable_price, 4);
                w.be(24, m.maximum_allowable_price, 4);
                w.be(28, m.near_execution_price, 4);
                w.be(32, m.near_execution_time, 8);
                w.be(40, m.lower_price_range_collar, 4);
                w.be(44, m.upper_price_range_collar, 4);
            }
            return w.take();
        },
        message);
}

void append_framed(std::vector<std::uint8_t>& out, const Message& m) {
    const std::vector<std::uint8_t> bytes = encode(m);
    out.push_back(static_cast<std::uint8_t>(bytes.size() >> 8U));
    out.push_back(static_cast<std::uint8_t>(bytes.size() & 0xFFU));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

}  // namespace enginex::itch
