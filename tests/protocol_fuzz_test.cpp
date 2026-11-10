// Exchange command path under hostile input: random bytes, random JSON objects and mutated
// valid requests go through parse_client_message() and, when they parse, into a real
// ExchangeStateMachine. Nothing may throw, every error must serialize, the engine must stay
// consistent, and an identical input sequence must reproduce the identical state.
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "enginex/canonical.hpp"
#include "enginex/protocol.hpp"
#include "enginex/rng.hpp"
#include "enginex/state.hpp"

namespace enginex::net {
namespace {

using codec::Json;

Json random_value(Rng& rng, int depth = 0) {
    switch (rng.below(depth > 1 ? 7 : 9)) {
        case 0: return nullptr;
        case 1: return rng.chance(50);
        case 2: return static_cast<std::int64_t>(rng.between(-5, 20'000));
        case 3: return static_cast<std::int64_t>(rng.next());  // huge / negative
        case 4: return static_cast<double>(rng.below(100000)) / 7.0;
        case 5: return std::string(rng.below(3), 'x');
        case 6: {
            static const char* const kStrings[] = {"AAPL", "MSFT", "BUY", "SELL", "ord-1",
                                                   "ord-999999", "", "é漢", "\u0000",
                                                   "new_limit", "cancel", "subscribe"};
            return kStrings[rng.below(std::size(kStrings))];
        }
        case 7: {
            Json a = Json::array();
            for (std::uint64_t i = rng.below(3); i > 0; --i) a.push_back(random_value(rng, depth + 1));
            return a;
        }
        default: {
            Json o = Json::object();
            for (std::uint64_t i = rng.below(3); i > 0; --i) o["k"] = random_value(rng, depth + 1);
            return o;
        }
    }
}

std::string random_request(Rng& rng) {
    static const char* const kKeys[] = {"protocol_version", "type",   "request_id", "symbol",
                                        "side",             "price",  "quantity",   "order_id",
                                        "depth",            "extra"};
    static const char* const kTypes[] = {"new_limit", "cancel", "snapshot_request", "subscribe",
                                         "bogus"};
    const auto r = rng.below(10);
    if (r == 0) {  // raw bytes
        std::string s(rng.below(40), '\0');
        for (auto& c : s) c = static_cast<char>(rng.next());
        return s;
    }
    Json j = Json::object();
    if (r < 7) {  // mostly well-shaped, with random corruption
        const std::string type = kTypes[rng.below(std::size(kTypes))];
        j["protocol_version"] = 1;
        j["type"] = type;
        // Mostly unique ids; 5 % from a small pool to exercise duplicates and id conflicts.
        j["request_id"] = rng.chance(5) ? "dup" + std::to_string(rng.below(20))
                                        : "r" + std::to_string(rng.next());
        if (type == "new_limit") {
            j["symbol"] = rng.chance(80) ? "AAPL" : "MSFT";
            j["side"] = rng.chance(50) ? "BUY" : "SELL";
            j["price"] = static_cast<std::int64_t>(rng.between(9000, 11000));
            j["quantity"] = static_cast<std::int64_t>(rng.between(1, 100));
        } else if (type == "cancel") {
            j["order_id"] = "ord-" + std::to_string(rng.below(200));
        } else if (type == "snapshot_request") {
            j["depth"] = static_cast<std::int64_t>(rng.below(5));
        }
        if (rng.chance(30)) {  // corrupt: add, replace or remove a field
            const char* key = kKeys[rng.below(std::size(kKeys))];
            if (rng.chance(30)) {
                j.erase(key);
            } else {
                j[key] = random_value(rng);
            }
        }
    } else {  // arbitrary objects
        for (std::uint64_t i = rng.below(6); i > 0; --i) {
            j[kKeys[rng.below(std::size(kKeys))]] = random_value(rng);
        }
    }
    std::string text = j.dump();
    if (rng.chance(10) && !text.empty()) {  // byte-level damage of the JSON text
        text[rng.below(text.size())] = static_cast<char>(rng.next());
    }
    return text;
}

struct Outcome {
    std::uint64_t parsed = 0;
    std::uint64_t parse_errors = 0;
    std::uint64_t accepted = 0;
    std::string hash;
};

Outcome run(std::uint64_t seed, int requests) {
    Rng rng(seed);
    ExchangeStateMachine state(StateConfig{});
    CommandResult out;
    Outcome o;
    for (int i = 0; i < requests; ++i) {
        const std::string text = random_request(rng);
        codec::Parsed<ClientMessage> p;
        EXPECT_NO_THROW(p = parse_client_message(text));
        if (const auto* err = std::get_if<codec::ParseError>(&p)) {
            ++o.parse_errors;
            std::string reply;
            EXPECT_NO_THROW(reply = error_message(*err));
            EXPECT_NO_THROW((void)Json::parse(reply));  // a valid JSON frame body
            continue;
        }
        ++o.parsed;
        if (const auto* cmd = std::get_if<Command>(&std::get<ClientMessage>(p))) {
            EXPECT_NO_THROW(state.process(*cmd, out));
            o.accepted += out.accepted ? 1 : 0;
            EXPECT_NO_THROW((void)result_message(out, state));
        }
        if (i % 500 == 0) {
            const std::string bad = state.engine().check_invariants();
            EXPECT_TRUE(bad.empty()) << bad;
        }
    }
    const std::string bad = state.engine().check_invariants();
    EXPECT_TRUE(bad.empty()) << bad;
    o.hash = state_hash(state);
    return o;
}

TEST(ProtocolFuzz, HostileRequestsNeverBreakTheCommandPath) {
    std::uint64_t parsed = 0;
    std::uint64_t errors = 0;
    std::uint64_t accepted = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        const Outcome a = run(seed, 5000);
        const Outcome b = run(seed, 5000);  // identical input → identical state
        ASSERT_EQ(a.hash, b.hash) << "seed " << seed;
        ASSERT_EQ(a.accepted, b.accepted);
        parsed += a.parsed;
        errors += a.parse_errors;
        accepted += a.accepted;
    }
    // The generator must reach both the error paths and the engine.
    EXPECT_GT(errors, 10'000U);
    EXPECT_GT(accepted, 5'000U);
    std::printf("protocol fuzz: %llu parsed, %llu parse errors, %llu accepted commands\n",
                static_cast<unsigned long long>(parsed), static_cast<unsigned long long>(errors),
                static_cast<unsigned long long>(accepted));
}

}  // namespace
}  // namespace enginex::net
