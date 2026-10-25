// Seeded property tests (spec §11.2). Every generated command is applied to the real state
// machine and to a naive reference model; after every command the books, fills and invariants
// must agree. Seeds are fixed, so any failure is reproducible from its seed.
#include <gtest/gtest.h>

#include <map>
#include <set>

#include "enginex/rng.hpp"
#include "reference_model.hpp"
#include "test_util.hpp"

namespace enginex {
namespace {

using test::ReferenceBook;
using test::RefFill;

const std::vector<std::string> kSymbols{"AAPL", "MSFT"};

struct Harness {
    explicit Harness(std::uint64_t seed) : rng(seed) {}

    Rng rng;
    ExchangeStateMachine state;
    ReferenceBook ref;
    std::vector<Command> accepted_commands;
    std::vector<OrderId> issued_orders;
    std::set<ExecSeq> seen_exec_ids;
    std::map<OrderId, Quantity> original_qty;
    int next_request = 0;

    std::string fresh_request_id() { return "req-" + std::to_string(++next_request); }

    Command generate() {
        const auto roll = rng.below(100);
        if (roll < 55 || accepted_commands.empty()) {
            return NewLimitOrder{fresh_request_id(), kSymbols[rng.below(kSymbols.size())],
                                 rng.chance(50) ? Side::Buy : Side::Sell, rng.between(9990, 10010),
                                 rng.between(1, 20)};
        }
        if (roll < 75) {
            // Cancel: mostly a previously issued order (open or closed), sometimes unknown.
            OrderId target = rng.chance(85) && !issued_orders.empty()
                                 ? issued_orders[rng.below(issued_orders.size())]
                                 : static_cast<OrderId>(rng.between(1, 2000));
            return CancelOrder{fresh_request_id(), format_order_id(target)};
        }
        if (roll < 85) {
            // Invalid field values.
            NewLimitOrder bad{fresh_request_id(), "AAPL", Side::Buy, 10000, 5};
            switch (rng.below(5)) {
                case 0:
                    bad.symbol = "NOPE";
                    break;
                case 1:
                    bad.price = rng.chance(50) ? 0 : kMaxPrice + 1;
                    break;
                case 2:
                    bad.quantity = rng.chance(50) ? -1 : kMaxQuantity + 1;
                    break;
                case 3:
                    bad.side = static_cast<Side>(3);
                    break;
                default:
                    bad.request_id.clear();
            }
            return bad;
        }
        const Command& prior = accepted_commands[rng.below(accepted_commands.size())];
        if (roll < 95) {
            return prior;  // exact retry
        }
        // Same request id, different body.
        NewLimitOrder conflict{request_id_of(prior), "MSFT", Side::Sell, 1, 1};
        return conflict;
    }

    // Applies one command to both models and checks every property.
    void step(const Command& cmd) {
        const CommandSeq seq_before = state.last_command_sequence();
        const ExecSeq exec_before = state.last_execution_sequence();
        const CommandRecord* prior = state.find_request(request_id_of(cmd));
        const bool is_retry = prior != nullptr && prior->command == cmd;
        const bool is_conflict = prior != nullptr && !is_retry;

        const CommandResult r = state.process(cmd);
        ASSERT_TRUE(state.engine().check_invariants().empty()) << state.engine().check_invariants();

        if (is_retry) {
            ASSERT_TRUE(r.accepted);
            ASSERT_TRUE(r.duplicate);
            ASSERT_EQ(r.command_sequence, prior->sequence);
            ASSERT_EQ(state.last_command_sequence(), seq_before);
            ASSERT_EQ(state.last_execution_sequence(), exec_before);
            return;
        }
        if (is_conflict) {
            ASSERT_FALSE(r.accepted);
            ASSERT_EQ(r.error, ErrorCode::DuplicateRequestIdConflict);
            return;
        }

        if (const auto* o = std::get_if<NewLimitOrder>(&cmd)) {
            const bool valid = state.find_symbol(o->symbol).has_value() && is_valid_side(o->side) &&
                               o->price >= kMinPrice && o->price <= kMaxPrice &&
                               o->quantity >= kMinQuantity && o->quantity <= kMaxQuantity &&
                               !o->request_id.empty();
            ASSERT_EQ(r.accepted, valid) << to_string(r.error);
            if (!valid) {
                ASSERT_EQ(state.last_command_sequence(), seq_before);
                ASSERT_EQ(state.last_execution_sequence(), exec_before);
                return;
            }
            ASSERT_EQ(r.command_sequence, seq_before + 1);
            ASSERT_EQ(r.order_id, r.command_sequence);
            issued_orders.push_back(r.order_id);
            original_qty[r.order_id] = o->quantity;

            // Snapshot the reference's opposite side before matching to check per-fill rules.
            std::map<OrderId, std::pair<PriceCents, Quantity>> pre;
            for (const auto& ro : ref.side_in_priority(o->symbol, opposite(o->side))) {
                pre[ro.id] = {ro.price, ro.remaining};
            }
            const auto ref_fills =
                ref.submit(r.order_id, o->symbol, o->side, o->price, o->quantity);
            ASSERT_EQ(r.executions.size(), ref_fills.size());

            Quantity incoming_remaining = o->quantity;
            Quantity filled = 0;
            for (std::size_t i = 0; i < r.executions.size(); ++i) {
                const Execution& e = r.executions[i];
                ASSERT_EQ((RefFill{e.incoming_order_id, e.resting_order_id, e.price, e.quantity}),
                          ref_fills[i])
                    << "fill " << i;
                ASSERT_GT(e.quantity, 0);
                ASSERT_EQ(e.execution_sequence, exec_before + i + 1);
                ASSERT_TRUE(seen_exec_ids.insert(e.execution_sequence).second);
                ASSERT_EQ(e.command_sequence, r.command_sequence);
                const auto& [rest_price, rest_remaining] = pre.at(e.resting_order_id);
                ASSERT_EQ(e.price, rest_price) << "execution must use the resting price";
                ASSERT_EQ(e.quantity, std::min(incoming_remaining, rest_remaining));
                ASSERT_TRUE(o->side == Side::Buy ? e.price <= o->price : e.price >= o->price);
                incoming_remaining -= e.quantity;
                pre.at(e.resting_order_id).second -= e.quantity;
                filled += e.quantity;
            }
            ASSERT_EQ(r.remaining, o->quantity - filled);
            ASSERT_GE(r.remaining, 0);
            accepted_commands.push_back(cmd);
        } else {
            const auto& c = std::get<CancelOrder>(cmd);
            const auto target = parse_order_id(c.order_id);
            const bool open = target && ref.is_resting(*target);
            ASSERT_EQ(r.accepted, open) << to_string(r.error) << " " << c.order_id;
            if (open) {
                ASSERT_TRUE(ref.cancel(*target));
                ASSERT_EQ(r.command_sequence, seq_before + 1);
                accepted_commands.push_back(cmd);
            } else {
                ASSERT_TRUE(r.error == ErrorCode::UnknownOrder ||
                            r.error == ErrorCode::OrderNotOpen);
                ASSERT_EQ(state.last_command_sequence(), seq_before);
            }
        }

        // Books must be identical, order by order, in priority order.
        for (const auto& sym : kSymbols) {
            for (const Side side : {Side::Buy, Side::Sell}) {
                const auto real = test::resting(state, sym, side);
                const auto expected = ref.side_in_priority(sym, side);
                ASSERT_EQ(real.size(), expected.size()) << sym << " " << to_string(side);
                for (std::size_t i = 0; i < real.size(); ++i) {
                    ASSERT_EQ(real[i].id, expected[i].id);
                    ASSERT_EQ(real[i].price, expected[i].price);
                    ASSERT_EQ(real[i].remaining, expected[i].remaining);
                    ASSERT_LE(real[i].remaining, original_qty.at(real[i].id));
                }
            }
        }
        ASSERT_EQ(state.engine().open_order_count(), ref.size());
    }
};

class PropertyTest : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(PropertyTest, MatchesReferenceModelAfterEveryCommand) {
    Harness h(GetParam());
    for (int i = 0; i < 400; ++i) {
        const Command cmd = h.generate();
        ASSERT_NO_FATAL_FAILURE(h.step(cmd)) << "seed " << GetParam() << " step " << i;
    }
    // Lifecycle status agrees with the book for every issued order.
    for (const OrderId id : h.issued_orders) {
        const auto status = h.state.command(id).status;
        const bool open = status == OrderStatus::Open || status == OrderStatus::PartiallyFilled;
        ASSERT_EQ(open, h.state.engine().is_resting(id)) << "ord-" << id;
    }
}

TEST_P(PropertyTest, SameSeedIsDeterministic) {
    auto run = [](std::uint64_t seed) {
        Harness h(seed);
        for (int i = 0; i < 300; ++i) {
            (void)h.state.process(h.generate());
        }
        return state_hash(h.state);
    };
    EXPECT_EQ(run(GetParam()), run(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(Seeds, PropertyTest, ::testing::Range<std::uint64_t>(1, 101));

TEST(PropertyStress, LongRunKeepsInvariants) {
    Harness h(424242);
    for (int i = 0; i < 20000; ++i) {
        const Command cmd = h.generate();
        const CommandResult r = h.state.process(cmd);
        if (r.accepted && !r.duplicate) {
            h.accepted_commands.push_back(cmd);
            if (r.type == CommandType::NewLimit) {
                h.issued_orders.push_back(r.order_id);
            }
        }
        if (i % 500 == 0) {
            ASSERT_TRUE(h.state.engine().check_invariants().empty())
                << h.state.engine().check_invariants();
        }
    }
    ASSERT_TRUE(h.state.engine().check_invariants().empty());
}

TEST(RngPortability, KnownSequence) {
    // Pins the generator output so workloads stay identical across platforms and releases.
    // Values cross-checked against an independent Python implementation of the same algorithm.
    Rng rng(42);
    EXPECT_EQ(rng.next(), 1546998764402558742ULL);
    EXPECT_EQ(rng.next(), 6990951692964543102ULL);
    EXPECT_EQ(rng.below(1000), 9U);
    Rng r2(7);
    for (int i = 0; i < 1000; ++i) {
        const auto v = r2.between(-3, 3);
        ASSERT_GE(v, -3);
        ASSERT_LE(v, 3);
    }
}

}  // namespace
}  // namespace enginex
