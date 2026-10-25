// Table-driven fixtures (spec §11.3). `testdata/tradegrid/` holds translations of the public
// TradeGrid matching specification examples and fixture files (attributed in each file);
// `testdata/enginex/` holds EngineX-specific cases such as cancellation, which the TradeGrid
// Sprint 2 matching spec leaves out of scope. Each fixture lists ordered steps (orders keyed by
// alias, cancels, expected rejections), the full ordered execution list with per-execution
// remaining quantities, and the complete final book with FIFO order.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

#include "enginex/canonical.hpp"
#include "enginex/json_codec.hpp"

namespace enginex {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

std::vector<fs::path> fixture_files() {
    std::vector<fs::path> files;
    for (const char* dir : {"tradegrid", "enginex"}) {
        for (const auto& entry : fs::directory_iterator(fs::path(ENGINEX_TESTDATA_DIR) / dir)) {
            if (entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string fixture_name(const ::testing::TestParamInfo<fs::path>& info) {
    std::string name =
        info.param.parent_path().filename().string() + "_" + info.param.stem().string();
    std::replace_if(name.begin(), name.end(), [](char c) { return !std::isalnum(c); }, '_');
    return name;
}

struct RunOutput {
    std::vector<Execution> executions;
    std::map<std::string, OrderId> ids;  // alias -> order id
    std::string hash;
};

RunOutput run_fixture(const Json& doc, ExchangeStateMachine& state) {
    RunOutput out;
    int n = 0;
    for (const Json& step : doc.at("steps")) {
        const std::string rid = "fx-" + std::to_string(++n);
        Command cmd;
        std::string label;
        if (step.contains("cancel")) {
            label = "cancel " + step.at("cancel").get<std::string>();
            const auto it = out.ids.find(step.at("cancel").get<std::string>());
            const OrderId target = it == out.ids.end() ? 0 : it->second;
            cmd = CancelOrder{rid, format_order_id(target)};
        } else {
            label = step.at("alias").get<std::string>();
            const auto side = parse_side(step.at("side").get<std::string>());
            EXPECT_TRUE(side.has_value()) << label;
            cmd = NewLimitOrder{rid, step.at("symbol").get<std::string>(), side.value_or(Side::Buy),
                                step.at("price").get<PriceCents>(),
                                step.at("quantity").get<Quantity>()};
        }
        const CommandResult r = state.process(cmd);
        if (step.contains("expect_error")) {
            EXPECT_FALSE(r.accepted) << label;
            EXPECT_EQ(to_string(r.error), step.at("expect_error").get<std::string>()) << label;
        } else {
            EXPECT_TRUE(r.accepted) << label << ": " << to_string(r.error) << " " << r.message;
            if (r.type == CommandType::NewLimit && r.accepted) {
                out.ids[label] = r.order_id;
            }
        }
        out.executions.insert(out.executions.end(), r.executions.begin(), r.executions.end());
        EXPECT_TRUE(state.engine().check_invariants().empty()) << state.engine().check_invariants();
    }
    out.hash = state_hash(state);
    return out;
}

class FixtureTest : public ::testing::TestWithParam<fs::path> {};

TEST_P(FixtureTest, MatchesExpectedOutcome) {
    std::ifstream in(GetParam());
    ASSERT_TRUE(in) << GetParam();
    const Json doc = Json::parse(in);
    SCOPED_TRACE(doc.at("name").get<std::string>());

    ExchangeStateMachine state;
    const RunOutput run = run_fixture(doc, state);
    const Json& expect = doc.at("expect");

    auto alias_of = [&run](OrderId id) {
        for (const auto& [alias, oid] : run.ids) {
            if (oid == id) {
                return alias;
            }
        }
        return std::string("?ord-") + std::to_string(id);
    };

    // Executions: identities, order, price, quantity, and remaining quantities after each one.
    const Json& execs = expect.at("executions");
    ASSERT_EQ(run.executions.size(), execs.size());
    std::map<std::string, Quantity> remaining;
    for (const Json& step : doc.at("steps")) {
        if (step.contains("alias") && !step.contains("expect_error")) {
            remaining[step.at("alias").get<std::string>()] = step.at("quantity").get<Quantity>();
        }
    }
    for (std::size_t i = 0; i < execs.size(); ++i) {
        const Execution& e = run.executions[i];
        const Json& want = execs[i];
        SCOPED_TRACE("execution " + std::to_string(i + 1));
        const std::string incoming = alias_of(e.incoming_order_id);
        const std::string resting = alias_of(e.resting_order_id);
        EXPECT_EQ(incoming, want.at("incoming").get<std::string>());
        EXPECT_EQ(resting, want.at("resting").get<std::string>());
        EXPECT_EQ(e.price, want.at("price").get<PriceCents>());
        EXPECT_EQ(e.quantity, want.at("quantity").get<Quantity>());
        EXPECT_EQ(e.execution_sequence, i + 1);
        remaining[incoming] -= e.quantity;
        remaining[resting] -= e.quantity;
        EXPECT_EQ(remaining[incoming], want.at("incoming_remaining").get<Quantity>());
        EXPECT_EQ(remaining[resting], want.at("resting_remaining").get<Quantity>());
    }

    // Complete final book, in priority order with FIFO identities.
    for (const auto& [symbol, sides] : expect.at("book").items()) {
        const auto sym = state.find_symbol(symbol);
        ASSERT_TRUE(sym.has_value()) << symbol;
        for (const Side side : {Side::Buy, Side::Sell}) {
            const Json& want = sides.at(side == Side::Buy ? "bids" : "asks");
            std::vector<std::tuple<std::string, Quantity, PriceCents>> actual;
            state.engine().book(*sym).for_each_level(side, [&](const PriceLevel& level) {
                for (const auto& o : level.orders) {
                    actual.emplace_back(alias_of(o.id), o.remaining, level.price);
                }
                return true;
            });
            ASSERT_EQ(actual.size(), want.size()) << symbol << " " << to_string(side);
            for (std::size_t i = 0; i < want.size(); ++i) {
                EXPECT_EQ(std::get<0>(actual[i]), want[i].at("alias").get<std::string>());
                EXPECT_EQ(std::get<1>(actual[i]), want[i].at("remaining").get<Quantity>());
                EXPECT_EQ(std::get<2>(actual[i]), want[i].at("price").get<PriceCents>());
            }
        }
    }

    if (expect.contains("remaining")) {
        for (const auto& [alias, qty] : expect.at("remaining").items()) {
            EXPECT_EQ(remaining.at(alias), qty.get<Quantity>()) << alias;
        }
    }
    if (expect.contains("order_ids")) {
        for (const auto& [alias, id] : expect.at("order_ids").items()) {
            EXPECT_EQ(format_order_id(run.ids.at(alias)), id.get<std::string>()) << alias;
        }
    }
    if (expect.contains("execution_ids")) {
        const Json& ids = expect.at("execution_ids");
        ASSERT_EQ(ids.size(), run.executions.size());
        for (std::size_t i = 0; i < ids.size(); ++i) {
            EXPECT_EQ(format_execution_id(run.executions[i].execution_sequence),
                      ids[i].at("execution_id").get<std::string>());
            EXPECT_EQ(run.executions[i].execution_sequence,
                      ids[i].at("execution_sequence").get<ExecSeq>());
        }
    }

    // Determinism: a second run from an empty state yields the identical canonical state.
    ExchangeStateMachine again;
    EXPECT_EQ(run_fixture(doc, again).hash, run.hash);
}

INSTANTIATE_TEST_SUITE_P(Fixtures, FixtureTest, ::testing::ValuesIn(fixture_files()), fixture_name);

TEST(Fixtures, AllTradeGridMatchingExamplesPresent) {
    std::size_t tradegrid = 0;
    for (const auto& f : fixture_files()) {
        tradegrid += f.parent_path().filename() == "tradegrid" ? 1U : 0U;
    }
    EXPECT_EQ(tradegrid, 10U);  // 7 worked examples + fixtures 01-03
}

}  // namespace
}  // namespace enginex
