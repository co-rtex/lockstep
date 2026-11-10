// ITCH 5.0 decoder golden tests. Expected bytes are written by hand from the specification's
// offset tables using a test-local writer, independent of the library's encoder, so an offset
// mistake in the codec cannot be mirrored by the test.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "enginex/itch/byte_reader.hpp"
#include "enginex/itch/decoder.hpp"
#include "enginex/rng.hpp"

namespace enginex::itch {
namespace {

// Independent big-endian writer over a zero-filled buffer of the specified size.
struct Bytes {
    explicit Bytes(std::size_t n) : v(n, 0) {}
    Bytes& at(std::size_t off, std::uint64_t value, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            v.at(off + i) = static_cast<std::uint8_t>(value >> (8 * (width - 1 - i)));
        }
        return *this;
    }
    Bytes& ch(std::size_t off, char c) {
        v.at(off) = static_cast<std::uint8_t>(c);
        return *this;
    }
    Bytes& str(std::size_t off, const std::string& s, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            v.at(off + i) = static_cast<std::uint8_t>(i < s.size() ? s[i] : ' ');
        }
        return *this;
    }
    Bytes& header(char type, std::uint16_t locate, std::uint16_t tracking, std::uint64_t ts) {
        return ch(0, type).at(1, locate, 2).at(3, tracking, 2).at(5, ts, 6);
    }
    std::vector<std::uint8_t> v;
};

template <typename T>
T decode_as(const std::vector<std::uint8_t>& bytes) {
    DecodeResult r = decode(bytes);
    if (const auto* err = std::get_if<DecodeError>(&r)) {
        ADD_FAILURE() << "decode failed: " << to_string(*err);
        return T{};
    }
    const Message& m = std::get<Message>(r);
    EXPECT_TRUE(std::holds_alternative<T>(m)) << "wrong message type";
    if (!std::holds_alternative<T>(m)) {
        return T{};
    }
    // Every golden message also round-trips byte-exactly through the encoder.
    EXPECT_EQ(encode(m), bytes);
    return std::get<T>(m);
}

std::string sym(const Symbol& s) { return std::string(trim_alpha(s)); }

constexpr std::uint64_t kTs = 0x0102030405A6ULL;  // exercises all 6 timestamp bytes

void expect_header(const Header& h, char type, std::uint16_t locate, std::uint16_t tracking) {
    EXPECT_EQ(h.type, type);
    EXPECT_EQ(h.stock_locate, locate);
    EXPECT_EQ(h.tracking_number, tracking);
    EXPECT_EQ(h.timestamp_ns, kTs);
}

TEST(ItchGolden, AddOrderLiteralBytes) {
    // Fully literal message: 'A', locate 1, tracking 2, ts 0x0102030405A6, ref
    // 0x1122334455667788, 'B', 100 shares, "AAPL    ", price 1'500'000 ($150.0000).
    const std::vector<std::uint8_t> bytes = {
        0x41, 0x00, 0x01, 0x00, 0x02, 0x01, 0x02, 0x03, 0x04, 0x05, 0xA6, 0x11,
        0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x42, 0x00, 0x00, 0x00, 0x64,
        0x41, 0x41, 0x50, 0x4C, 0x20, 0x20, 0x20, 0x20, 0x00, 0x16, 0xE3, 0x60};
    ASSERT_EQ(bytes.size(), 36U);
    const auto m = decode_as<AddOrder>(bytes);
    expect_header(m.h, 'A', 1, 2);
    EXPECT_EQ(m.order_ref, 0x1122334455667788ULL);
    EXPECT_EQ(m.side, Side::Buy);
    EXPECT_EQ(m.shares, 100U);
    EXPECT_EQ(sym(m.stock), "AAPL");
    EXPECT_EQ(m.price, 1'500'000U);
    EXPECT_FALSE(m.attribution);
}

TEST(ItchGolden, AddOrderWithMpid) {
    Bytes b(40);
    b.header('F', 7, 9, kTs).at(11, 42, 8).ch(19, 'S').at(20, 250, 4).str(24, "MSFT", 8);
    b.at(32, 3'201'500, 4).str(36, "GSCO", 4);
    const auto m = decode_as<AddOrder>(b.v);
    expect_header(m.h, 'F', 7, 9);
    EXPECT_EQ(m.order_ref, 42U);
    EXPECT_EQ(m.side, Side::Sell);
    EXPECT_EQ(m.shares, 250U);
    EXPECT_EQ(sym(m.stock), "MSFT");
    EXPECT_EQ(m.price, 3'201'500U);
    ASSERT_TRUE(m.attribution);
    EXPECT_EQ(std::string(m.attribution->data(), 4), "GSCO");
}

TEST(ItchGolden, OrderExecuted) {
    Bytes b(31);
    b.header('E', 3, 4, kTs).at(11, 0xFFFFFFFFFFFFFFFEULL, 8).at(19, 0x80000001U, 4);
    b.at(23, 0x8000000000000001ULL, 8);
    const auto m = decode_as<OrderExecuted>(b.v);
    expect_header(m.h, 'E', 3, 4);
    EXPECT_EQ(m.order_ref, 0xFFFFFFFFFFFFFFFEULL);  // high bit set: unsigned, no sign issues
    EXPECT_EQ(m.executed_shares, 0x80000001U);
    EXPECT_EQ(m.match_number, 0x8000000000000001ULL);
}

TEST(ItchGolden, OrderExecutedWithPrice) {
    Bytes b(36);
    b.header('C', 3, 0, kTs).at(11, 77, 8).at(19, 10, 4).at(23, 5, 8).ch(31, 'N').at(32, 99, 4);
    const auto m = decode_as<OrderExecutedWithPrice>(b.v);
    expect_header(m.h, 'C', 3, 0);
    EXPECT_EQ(m.order_ref, 77U);
    EXPECT_EQ(m.executed_shares, 10U);
    EXPECT_EQ(m.match_number, 5U);
    EXPECT_FALSE(m.printable);
    EXPECT_EQ(m.execution_price, 99U);
    b.ch(31, 'Y');
    EXPECT_TRUE(decode_as<OrderExecutedWithPrice>(b.v).printable);
}

TEST(ItchGolden, OrderCancelDeleteReplace) {
    Bytes x(23);
    x.header('X', 1, 1, kTs).at(11, 5, 8).at(19, 30, 4);
    const auto c = decode_as<OrderCancel>(x.v);
    expect_header(c.h, 'X', 1, 1);
    EXPECT_EQ(c.order_ref, 5U);
    EXPECT_EQ(c.canceled_shares, 30U);

    Bytes d(19);
    d.header('D', 1, 1, kTs).at(11, 6, 8);
    EXPECT_EQ(decode_as<OrderDelete>(d.v).order_ref, 6U);

    Bytes u(35);
    u.header('U', 2, 1, kTs).at(11, 6, 8).at(19, 7, 8).at(27, 400, 4).at(31, 1'000'100, 4);
    const auto r = decode_as<OrderReplace>(u.v);
    expect_header(r.h, 'U', 2, 1);
    EXPECT_EQ(r.original_ref, 6U);
    EXPECT_EQ(r.new_ref, 7U);
    EXPECT_EQ(r.shares, 400U);
    EXPECT_EQ(r.price, 1'000'100U);
}

TEST(ItchGolden, TradeCrossBroken) {
    Bytes p(44);
    p.header('P', 4, 0, kTs).at(11, 0, 8).ch(19, 'B').at(20, 300, 4).str(24, "QQQ", 8);
    p.at(32, 4'000'000, 4).at(36, 123456789, 8);
    const auto t = decode_as<Trade>(p.v);
    EXPECT_EQ(t.order_ref, 0U);
    EXPECT_EQ(t.side, 'B');
    EXPECT_EQ(t.shares, 300U);
    EXPECT_EQ(sym(t.stock), "QQQ");
    EXPECT_EQ(t.price, 4'000'000U);
    EXPECT_EQ(t.match_number, 123456789U);

    Bytes q(40);
    q.header('Q', 4, 0, kTs).at(11, 0x0000000100000000ULL, 8).str(19, "QQQ", 8);
    q.at(27, 4'000'100, 4).at(31, 99, 8).ch(39, 'O');
    const auto x = decode_as<CrossTrade>(q.v);
    EXPECT_EQ(x.shares, 0x0000000100000000ULL);  // 8-byte shares field in this message
    EXPECT_EQ(sym(x.stock), "QQQ");
    EXPECT_EQ(x.cross_price, 4'000'100U);
    EXPECT_EQ(x.match_number, 99U);
    EXPECT_EQ(x.cross_type, 'O');

    Bytes b(19);
    b.header('B', 4, 0, kTs).at(11, 123456789, 8);
    EXPECT_EQ(decode_as<BrokenTrade>(b.v).match_number, 123456789U);
}

TEST(ItchGolden, SystemAndDirectory) {
    Bytes s(12);
    s.header('S', 0, 0, kTs).ch(11, 'O');
    const auto e = decode_as<SystemEvent>(s.v);
    expect_header(e.h, 'S', 0, 0);
    EXPECT_EQ(e.event_code, 'O');

    Bytes r(39);
    r.header('R', 13, 0, kTs).str(11, "ZVZZT", 8).ch(19, 'Q').ch(20, 'N').at(21, 100, 4);
    r.ch(25, 'N').ch(26, 'C').str(27, "Z ", 2).ch(29, 'T').ch(30, 'N').ch(31, 'N').ch(32, '2');
    r.ch(33, 'N').at(34, 3, 4).ch(38, 'Y');
    const auto d = decode_as<StockDirectory>(r.v);
    expect_header(d.h, 'R', 13, 0);
    EXPECT_EQ(sym(d.stock), "ZVZZT");
    EXPECT_EQ(d.market_category, 'Q');
    EXPECT_EQ(d.financial_status, 'N');
    EXPECT_EQ(d.round_lot_size, 100U);
    EXPECT_EQ(d.round_lots_only, 'N');
    EXPECT_EQ(d.issue_classification, 'C');
    EXPECT_EQ(d.issue_subtype[0], 'Z');
    EXPECT_EQ(d.authenticity, 'T');
    EXPECT_EQ(d.short_sale_threshold, 'N');
    EXPECT_EQ(d.ipo_flag, 'N');
    EXPECT_EQ(d.luld_tier, '2');
    EXPECT_EQ(d.etp_flag, 'N');
    EXPECT_EQ(d.etp_leverage_factor, 3U);
    EXPECT_EQ(d.inverse_indicator, 'Y');
}

TEST(ItchGolden, StockStatusMessages) {
    Bytes h(25);
    h.header('H', 5, 0, kTs).str(11, "AAPL", 8).ch(19, 'T').ch(20, ' ').str(21, "LUDP", 4);
    const auto ta = decode_as<StockTradingAction>(h.v);
    EXPECT_EQ(sym(ta.stock), "AAPL");
    EXPECT_EQ(ta.trading_state, 'T');
    EXPECT_EQ(std::string(ta.reason.data(), 4), "LUDP");

    Bytes y(20);
    y.header('Y', 5, 0, kTs).str(11, "AAPL", 8).ch(19, '1');
    EXPECT_EQ(decode_as<RegShoRestriction>(y.v).reg_sho_action, '1');

    Bytes l(26);
    l.header('L', 5, 0, kTs).str(11, "NITE", 4).str(15, "AAPL", 8).ch(23, 'Y').ch(24, 'N');
    l.ch(25, 'A');
    const auto mp = decode_as<MarketParticipantPosition>(l.v);
    EXPECT_EQ(std::string(mp.mpid.data(), 4), "NITE");
    EXPECT_EQ(sym(mp.stock), "AAPL");
    EXPECT_EQ(mp.primary_market_maker, 'Y');
    EXPECT_EQ(mp.market_maker_mode, 'N');
    EXPECT_EQ(mp.participant_state, 'A');

    Bytes v(35);
    v.header('V', 0, 0, kTs).at(11, 1, 8).at(19, 2, 8).at(27, 0xFFFFFFFFFFFFFFFFULL, 8);
    const auto mw = decode_as<MwcbDeclineLevel>(v.v);
    EXPECT_EQ(mw.level1, 1U);
    EXPECT_EQ(mw.level2, 2U);
    EXPECT_EQ(mw.level3, 0xFFFFFFFFFFFFFFFFULL);

    Bytes w(12);
    w.header('W', 0, 0, kTs).ch(11, '2');
    EXPECT_EQ(decode_as<MwcbStatus>(w.v).breached_level, '2');

    Bytes k(28);
    k.header('K', 5, 0, kTs).str(11, "NEWCO", 8).at(19, 36000, 4).ch(23, 'A').at(24, 200000, 4);
    const auto ipo = decode_as<IpoQuotingPeriod>(k.v);
    EXPECT_EQ(sym(ipo.stock), "NEWCO");
    EXPECT_EQ(ipo.release_time, 36000U);
    EXPECT_EQ(ipo.release_qualifier, 'A');
    EXPECT_EQ(ipo.ipo_price, 200000U);

    Bytes j(35);
    j.header('J', 5, 0, kTs).str(11, "AAPL", 8).at(19, 10, 4).at(23, 11, 4).at(27, 9, 4);
    j.at(31, 2, 4);
    const auto col = decode_as<LuldAuctionCollar>(j.v);
    EXPECT_EQ(col.reference_price, 10U);
    EXPECT_EQ(col.upper_collar, 11U);
    EXPECT_EQ(col.lower_collar, 9U);
    EXPECT_EQ(col.extension, 2U);

    Bytes oh(21);
    oh.header('h', 5, 0, kTs).str(11, "AAPL", 8).ch(19, 'Q').ch(20, 'H');
    const auto halt = decode_as<OperationalHalt>(oh.v);
    EXPECT_EQ(halt.market_code, 'Q');
    EXPECT_EQ(halt.halt_action, 'H');
}

TEST(ItchGolden, AuctionAndRetailMessages) {
    Bytes i(50);
    i.header('I', 5, 0, kTs).at(11, 1000, 8).at(19, 200, 8).ch(27, 'B').str(28, "AAPL", 8);
    i.at(36, 1, 4).at(40, 2, 4).at(44, 3, 4).ch(48, 'C').ch(49, 'L');
    const auto n = decode_as<Noii>(i.v);
    EXPECT_EQ(n.paired_shares, 1000U);
    EXPECT_EQ(n.imbalance_shares, 200U);
    EXPECT_EQ(n.imbalance_direction, 'B');
    EXPECT_EQ(sym(n.stock), "AAPL");
    EXPECT_EQ(n.far_price, 1U);
    EXPECT_EQ(n.near_price, 2U);
    EXPECT_EQ(n.current_reference_price, 3U);
    EXPECT_EQ(n.cross_type, 'C');
    EXPECT_EQ(n.price_variation_indicator, 'L');

    Bytes rp(20);
    rp.header('N', 5, 0, kTs).str(11, "AAPL", 8).ch(19, 'A');
    EXPECT_EQ(decode_as<RetailInterest>(rp.v).interest_flag, 'A');

    Bytes o(48);
    o.header('O', 5, 0, kTs).str(11, "DLCR", 8).ch(19, 'Y').at(20, 1, 4).at(24, 2, 4);
    o.at(28, 3, 4).at(32, 0x0102030405060708ULL, 8).at(40, 4, 4).at(44, 5, 4);
    const auto dl = decode_as<DlcrPriceDiscovery>(o.v);
    EXPECT_EQ(dl.open_eligibility_status, 'Y');
    EXPECT_EQ(dl.minimum_allowable_price, 1U);
    EXPECT_EQ(dl.maximum_allowable_price, 2U);
    EXPECT_EQ(dl.near_execution_price, 3U);
    EXPECT_EQ(dl.near_execution_time, 0x0102030405060708ULL);
    EXPECT_EQ(dl.lower_price_range_collar, 4U);
    EXPECT_EQ(dl.upper_price_range_collar, 5U);
}

TEST(ItchGolden, MaximumTimestampAndLocate) {
    Bytes d(19);
    d.header('D', 0xFFFF, 0xFFFF, 0xFFFFFFFFFFFFULL).at(11, 1, 8);
    const auto m = decode_as<OrderDelete>(d.v);
    EXPECT_EQ(m.h.stock_locate, 0xFFFFU);
    EXPECT_EQ(m.h.tracking_number, 0xFFFFU);
    EXPECT_EQ(m.h.timestamp_ns, 0xFFFFFFFFFFFFULL);
}

// ---- malformed payloads ---------------------------------------------------------------------

TEST(ItchMalformed, EmptyAndUnknownTypes) {
    EXPECT_EQ(std::get<DecodeError>(decode({})), DecodeError::Empty);
    for (int t = 0; t < 256; ++t) {
        const char c = static_cast<char>(t);
        if (message_length(c)) {
            continue;
        }
        std::vector<std::uint8_t> bytes(36, 0);
        bytes[0] = static_cast<std::uint8_t>(t);
        const DecodeResult r = decode(bytes);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(r)) << t;
        EXPECT_EQ(std::get<DecodeError>(r), DecodeError::UnknownType) << t;
    }
    EXPECT_EQ(kKnownTypes.size(), 23U);
}

TEST(ItchMalformed, EveryKnownTypeRejectsEveryWrongLength) {
    for (const char t : kKnownTypes) {
        const std::size_t len = *message_length(t);
        for (std::size_t n = 1; n <= 64; ++n) {
            if (n == len) {
                continue;
            }
            std::vector<std::uint8_t> bytes(n, 0);
            bytes[0] = static_cast<std::uint8_t>(t);
            const DecodeResult r = decode(bytes);
            ASSERT_TRUE(std::holds_alternative<DecodeError>(r)) << t << " len " << n;
            EXPECT_EQ(std::get<DecodeError>(r), DecodeError::LengthMismatch) << t << " " << n;
        }
    }
}

TEST(ItchMalformed, InvalidEnumeratedFields) {
    Bytes a(36);
    a.header('A', 1, 0, kTs).at(11, 1, 8).ch(19, 'X').at(20, 1, 4).str(24, "A", 8);
    EXPECT_EQ(std::get<DecodeError>(decode(a.v)), DecodeError::InvalidField);
    Bytes c(36);
    c.header('C', 1, 0, kTs).at(11, 1, 8).ch(31, ' ');
    EXPECT_EQ(std::get<DecodeError>(decode(c.v)), DecodeError::InvalidField);
}

TEST(ItchMalformed, RandomBytesNeverCrashAndAreClassified) {
    Rng rng(20261110);
    for (int i = 0; i < 200'000; ++i) {
        const std::size_t n = rng.below(70);
        std::vector<std::uint8_t> bytes(n);
        for (auto& b : bytes) {
            b = static_cast<std::uint8_t>(rng.next());
        }
        if (n > 0 && rng.chance(50)) {  // bias towards known types
            bytes[0] = static_cast<std::uint8_t>(kKnownTypes[rng.below(kKnownTypes.size())]);
        }
        const DecodeResult r = decode(bytes);
        if (const auto* m = std::get_if<Message>(&r)) {
            ASSERT_EQ(n, *message_length(static_cast<char>(bytes[0])));
            ASSERT_EQ(encode(*m), bytes);  // a successful decode is lossless
        }
    }
}

// The generic random test almost never produces a decodable A/F/C (length and enum byte must
// both match), so exercise their lossless round trip explicitly.
TEST(ItchMalformed, RandomValidOrderMessagesRoundTrip) {
    Rng rng(42);
    for (int i = 0; i < 50'000; ++i) {
        const char t = "AFC"[rng.below(3)];
        std::vector<std::uint8_t> bytes(*message_length(t));
        for (auto& b : bytes) b = static_cast<std::uint8_t>(rng.next());
        bytes[0] = static_cast<std::uint8_t>(t);
        if (t == 'C') {
            bytes[31] = rng.chance(50) ? 'Y' : 'N';
        } else {
            bytes[19] = rng.chance(50) ? 'B' : 'S';
        }
        const DecodeResult r = decode(bytes);
        ASSERT_TRUE(std::holds_alternative<Message>(r)) << t;
        ASSERT_EQ(encode(std::get<Message>(r)), bytes) << t;
    }
}

TEST(ItchEncode, RejectsTimestampsThatDoNotFitSixBytes) {
    OrderDelete d{Header{'D', 1, 0, 1ULL << 48}, 1};
    EXPECT_THROW((void)encode(d), std::out_of_range);
    d.h.timestamp_ns = (1ULL << 48) - 1;
    EXPECT_EQ(encode(d).size(), 19U);
}

TEST(ByteReader, ThrowsInsteadOfReadingPastEnd) {
    const std::vector<std::uint8_t> bytes = {1, 2, 3};
    const ByteReader r(bytes);
    EXPECT_EQ(r.u16(1), 0x0203U);
    EXPECT_THROW((void)r.u16(2), std::out_of_range);
    EXPECT_THROW((void)r.u32(0), std::out_of_range);
    EXPECT_THROW((void)r.u8(3), std::out_of_range);
    EXPECT_THROW((void)r.u8(SIZE_MAX), std::out_of_range);  // no offset+width overflow
}

// ---- framing --------------------------------------------------------------------------------

std::vector<std::uint8_t> sample_stream(std::vector<std::vector<std::uint8_t>>* payloads) {
    std::vector<std::uint8_t> out;
    Rng rng(7);
    for (int i = 0; i < 40; ++i) {
        const char t = kKnownTypes[rng.below(kKnownTypes.size())];
        std::vector<std::uint8_t> p(*message_length(t), static_cast<std::uint8_t>(i));
        p[0] = static_cast<std::uint8_t>(t);
        payloads->push_back(p);
        out.push_back(static_cast<std::uint8_t>(p.size() >> 8));
        out.push_back(static_cast<std::uint8_t>(p.size()));
        out.insert(out.end(), p.begin(), p.end());
    }
    // One large unknown-type message: lengths are not limited to known sizes.
    std::vector<std::uint8_t> big(300, 0xAB);
    big[0] = 'z';
    payloads->push_back(big);
    out.push_back(static_cast<std::uint8_t>(big.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(big.size()));
    out.insert(out.end(), big.begin(), big.end());
    return out;
}

TEST(StreamFramer, AnyTwoChunkSplitYieldsTheSameMessages) {
    std::vector<std::vector<std::uint8_t>> expected;
    const auto stream = sample_stream(&expected);
    for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
        StreamFramer f;
        std::vector<std::vector<std::uint8_t>> got;
        auto cb = [&](std::span<const std::uint8_t> p) { got.emplace_back(p.begin(), p.end()); };
        const std::span<const std::uint8_t> all(stream);
        ASSERT_EQ(f.feed(all.first(cut), cb), FrameError::None);
        ASSERT_EQ(f.feed(all.subspan(cut), cb), FrameError::None);
        ASSERT_EQ(f.finish(), FrameError::None) << cut;
        ASSERT_EQ(got, expected) << "cut at " << cut;
        EXPECT_EQ(f.bytes_fed(), stream.size());
    }
}

TEST(StreamFramer, ByteAtATimeAndRandomChunks) {
    std::vector<std::vector<std::uint8_t>> expected;
    const auto stream = sample_stream(&expected);
    Rng rng(99);
    for (int trial = 0; trial < 200; ++trial) {
        StreamFramer f;
        std::vector<std::vector<std::uint8_t>> got;
        auto cb = [&](std::span<const std::uint8_t> p) { got.emplace_back(p.begin(), p.end()); };
        std::size_t pos = 0;
        while (pos < stream.size()) {
            const std::size_t n =
                trial == 0 ? 1 : std::min<std::size_t>(rng.below(97), stream.size() - pos);
            ASSERT_EQ(f.feed(std::span(stream).subspan(pos, n), cb), FrameError::None);
            pos += n;
        }
        ASSERT_EQ(f.finish(), FrameError::None);
        ASSERT_EQ(got, expected);
        EXPECT_EQ(f.frames(), expected.size());
    }
}

TEST(StreamFramer, TruncationAtEveryOffsetIsReported) {
    std::vector<std::vector<std::uint8_t>> expected;
    const auto stream = sample_stream(&expected);
    // Message boundaries (offsets of each length prefix).
    std::vector<std::size_t> starts;
    for (std::size_t pos = 0; pos < stream.size();) {
        starts.push_back(pos);
        pos += 2 + ((static_cast<std::size_t>(stream[pos]) << 8) | stream[pos + 1]);
    }
    for (std::size_t len = 0; len < stream.size(); ++len) {
        StreamFramer f;
        std::size_t complete = 0;
        ASSERT_EQ(f.feed(std::span(stream).first(len), [&](auto) { ++complete; }),
                  FrameError::None);
        const bool on_boundary = std::find(starts.begin(), starts.end(), len) != starts.end();
        const FrameError e = f.finish();
        if (on_boundary) {
            EXPECT_EQ(e, FrameError::None) << len;
        } else {
            ASSERT_EQ(e, FrameError::TruncatedTail) << len;
            // The error points at the prefix of the incomplete message.
            const auto it = std::upper_bound(starts.begin(), starts.end(), len);
            EXPECT_EQ(f.error_offset(), *(it - 1)) << len;
            EXPECT_EQ(complete, static_cast<std::size_t>(it - starts.begin() - 1));
        }
    }
}

TEST(StreamFramer, ZeroLengthPrefixIsFatalWithOffset) {
    std::vector<std::uint8_t> stream = {0x00, 0x0C, 'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'O'};
    stream.push_back(0x00);
    stream.push_back(0x00);  // zero length at offset 14
    stream.insert(stream.end(), {0x00, 0x13, 'D'});
    for (std::size_t cut = 0; cut <= stream.size(); ++cut) {
        StreamFramer f;
        int n = 0;
        auto cb = [&](auto) { ++n; };
        FrameError e = f.feed(std::span(stream).first(cut), cb);
        if (e == FrameError::None) {
            e = f.feed(std::span(stream).subspan(cut), cb);
        }
        ASSERT_EQ(e, FrameError::ZeroLength) << cut;
        EXPECT_EQ(f.error_offset(), 14U) << cut;
        EXPECT_EQ(n, 1);
        // The framer stays failed.
        EXPECT_EQ(f.feed(stream, cb), FrameError::ZeroLength);
        EXPECT_EQ(f.finish(), FrameError::ZeroLength);
    }
}

}  // namespace
}  // namespace enginex::itch
