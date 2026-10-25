// `enginex run` (journaled JSON-lines session) and `enginex replay` (offline verification).
#include <cstdlib>
#include <fstream>
#include <iostream>

#include "cli.hpp"
#include "enginex/canonical.hpp"
#include "enginex/durable.hpp"
#include "enginex/json_codec.hpp"
#include "enginex/metrics.hpp"

namespace enginex::cli {

namespace {

using codec::Json;

constexpr int kExitCorrupt = 1;
constexpr int kExitIncompleteTail = 3;
constexpr int kExitFailStop = 4;

std::string tail_name(journal::TailState t) {
    return t == journal::TailState::Clean ? "clean" : "incomplete";
}

journal::FaultPlan faults_from_env() {
    const char* spec = std::getenv("ENGINEX_FAULT");
    if (spec == nullptr || *spec == '\0') {
        return {};
    }
    std::cerr << "enginex: WARNING: fault injection active (ENGINEX_FAULT=" << spec
              << "); for tests and demos only\n";
    return journal::FaultPlan::parse(spec);
}

Json recovery_json(const RecoveryReport& rep, const DurableExchange& ex) {
    Json j;
    j["type"] = "recovery";
    j["journal"] = ex.path();
    j["durability"] = std::string(journal::to_string(ex.sync_mode()));
    j["created"] = rep.created;
    j["records_replayed"] = rep.records_replayed;
    j["file_bytes"] = rep.file_bytes;
    j["valid_bytes"] = rep.valid_bytes;
    j["tail"] = tail_name(rep.tail);
    if (rep.tail == journal::TailState::Incomplete) {
        j["tail_reason"] = rep.tail_reason;
        j["tail_repaired"] = rep.tail_repaired;
        j["preserved_tail_path"] = rep.preserved_tail_path;
    }
    j["last_command_sequence"] = rep.last_command_sequence;
    j["state_hash"] = state_hash(ex.state());
    j["elapsed_us"] = rep.elapsed_ns / 1000;
    return j;
}

Json journal_error_json(const journal::JournalError& e) {
    Json j;
    j["type"] = "journal_error";
    j["error"] = Json{{"code", to_string(e.code())}, {"message", e.what()}};
    return j;
}

StateConfig state_config(const Args& args) {
    StateConfig config;
    if (const auto symbols = args.get("symbols")) {
        config.symbols = split_list(*symbols);
    }
    return config;
}

journal::SyncMode sync_mode(const Args& args) {
    const std::string text = args.get_or("durability", "sync");
    const auto mode = journal::parse_sync_mode(text);
    if (!mode) {
        throw UsageError("--durability must be sync, fsync or none");
    }
    return *mode;
}

// Read-only query against a durable exchange (snapshot / executions).
Json run_query(const DurableExchange& ex, const codec::Input& input) {
    const ExchangeStateMachine& state = ex.state();
    if (const auto* snap = std::get_if<codec::SnapshotQuery>(&input)) {
        const auto symbol = state.find_symbol(snap->symbol);
        if (!symbol) {
            return codec::to_json(
                codec::ParseError{ErrorCode::InvalidSymbol, "unknown symbol", {}, "snapshot"});
        }
        Json j;
        j["type"] = "snapshot";
        j["status"] = "ok";
        j.update(codec::snapshot_to_json(state, *symbol, snap->depth));
        return j;
    }
    const auto& q = std::get<codec::ExecutionsQuery>(input);
    std::optional<SymbolId> symbol;
    if (q.symbol) {
        symbol = state.find_symbol(*q.symbol);
        if (!symbol) {
            return codec::to_json(
                codec::ParseError{ErrorCode::InvalidSymbol, "unknown symbol", {}, "executions"});
        }
    }
    Json j;
    j["type"] = "executions";
    j["status"] = "ok";
    j["as_of_command_sequence"] = state.last_command_sequence();
    j["executions"] = codec::executions_to_json(state, symbol);
    return j;
}

}  // namespace

int cmd_run(const std::vector<std::string>& argv) {
    const Args args(argv, {"journal", "durability", "input", "output", "symbols"}, {"refuse-tail"});
    DurableOptions options;
    options.journal_path = args.require("journal");
    options.sync = sync_mode(args);
    options.state = state_config(args);
    options.tail_policy = args.flag("refuse-tail") ? TailPolicy::Refuse : TailPolicy::Repair;
    options.faults = faults_from_env();

    std::ofstream out_file;
    std::ostream* out = &std::cout;
    const std::string output = args.get_or("output", "-");
    if (output != "-") {
        out_file.open(output, std::ios::trunc);
        if (!out_file) {
            throw UsageError("cannot open output file: " + output);
        }
        out = &out_file;
    }
    std::ifstream in_file;
    std::istream* in = &std::cin;
    const std::string input = args.get_or("input", "-");
    if (input != "-") {
        in_file.open(input);
        if (!in_file) {
            throw UsageError("cannot open input file: " + input);
        }
        in = &in_file;
    }

    std::unique_ptr<DurableExchange> ex;
    RecoveryReport rep;
    try {
        ex = DurableExchange::open(options, &rep);
    } catch (const journal::JournalError& e) {
        *out << journal_error_json(e).dump() << std::endl;
        std::cerr << "enginex run: " << e.what() << "\n";
        return e.code() == ErrorCode::JournalCorrupt ? kExitCorrupt : kExitFailStop;
    }
    *out << recovery_json(rep, *ex).dump() << std::endl;

    std::string line;
    std::uint64_t line_number = 0;
    CommandResult result;
    while (std::getline(*in, line)) {
        ++line_number;
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        Json response;
        response["line"] = line_number;
        auto parsed = codec::parse_input(line);
        if (auto* err = std::get_if<codec::ParseError>(&parsed)) {
            response.update(codec::to_json(*err));
        } else if (auto* cmd = std::get_if<Command>(&std::get<codec::Input>(parsed))) {
            ex->process(*cmd, result);  // returns only after the record is synced
            response.update(codec::to_json(result, ex->state()));
        } else if (ex->failed()) {
            codec::ParseError e{ErrorCode::JournalIoError,
                                "journal failed; state not readable until restart",
                                {},
                                "query"};
            response.update(codec::to_json(e));
        } else {
            response.update(run_query(*ex, std::get<codec::Input>(parsed)));
        }
        // The response is released only now, after the durable flush inside process().
        *out << response.dump() << std::endl;
    }

    Json summary;
    summary["type"] = "summary";
    if (ex->failed()) {
        summary["status"] = "fail_stop";
        summary["reason"] = ex->failure_reason();
    } else {
        summary["status"] = "ok";
        summary.update(codec::summary_to_json(ex->state()));
        summary["journal_bytes"] = ex->durable_bytes();
    }
    *out << summary.dump() << std::endl;
    return ex->failed() ? kExitFailStop : 0;
}

int cmd_replay(const std::vector<std::string>& argv) {
    const Args args(argv, {"journal", "symbols"}, {"verify", "repair-tail", "dump", "quiet"});
    const std::string path = args.require("journal");
    const StateConfig config = state_config(args);

    Json report;
    report["type"] = "replay";
    report["journal"] = path;
    const auto start = metrics::Clock::now();
    try {
        if (args.flag("repair-tail")) {
            DurableOptions options;
            options.journal_path = path;
            options.state = config;
            options.create_if_missing = false;
            options.tail_policy = TailPolicy::Repair;
            RecoveryReport rep;
            auto ex = DurableExchange::open(options, &rep);
            report["records"] = rep.records_replayed;
            report["file_bytes"] = rep.file_bytes;
            report["valid_bytes"] = rep.valid_bytes;
            report["tail"] = tail_name(rep.tail);
            if (rep.tail_repaired) {
                report["tail_reason"] = rep.tail_reason;
                report["tail_repaired"] = true;
                report["preserved_tail_path"] = rep.preserved_tail_path;
            }
            report.update(codec::summary_to_json(ex->state()));
            report["status"] = "ok";
        } else {
            ExchangeStateMachine state(config);
            const std::string bytes = journal::read_file(path);
            if (args.flag("dump")) {
                (void)journal::scan_bytes(bytes, [](const journal::Record& rec, std::uint64_t off) {
                    Json j;
                    j["offset"] = off;
                    j["command_sequence"] = rec.sequence;
                    j["request_id"] = request_id_of(rec.command);
                    j["type"] = to_string(type_of(rec.command));
                    if (const auto* o = std::get_if<NewLimitOrder>(&rec.command)) {
                        j["symbol"] = o->symbol;
                        j["side"] = to_string(o->side);
                        j["price"] = o->price;
                        j["quantity"] = o->quantity;
                    } else {
                        j["order_id"] = std::get<CancelOrder>(rec.command).order_id;
                    }
                    std::cout << j.dump() << "\n";
                });
            }
            const journal::ScanResult scan = replay_into(bytes, state);
            report["records"] = scan.records;
            report["file_bytes"] = scan.file_bytes;
            report["valid_bytes"] = scan.valid_bytes;
            report["tail"] = tail_name(scan.tail);
            if (scan.tail == journal::TailState::Incomplete) {
                report["tail_reason"] = scan.tail_reason;
            }
            report.update(codec::summary_to_json(state));
            report["status"] = scan.tail == journal::TailState::Clean ? "ok" : "incomplete_tail";
        }
    } catch (const journal::JournalError& e) {
        report["status"] = e.code() == ErrorCode::JournalCorrupt ? "corrupt" : "io_error";
        report["error"] = Json{{"code", to_string(e.code())}, {"message", e.what()}};
        report["elapsed_us"] = metrics::elapsed_ns(start, metrics::Clock::now()) / 1000;
        std::cout << report.dump(2) << "\n";
        return kExitCorrupt;
    }
    report["elapsed_us"] = metrics::elapsed_ns(start, metrics::Clock::now()) / 1000;
    if (!args.flag("quiet")) {
        std::cout << report.dump(2) << "\n";
    }
    if (report["status"] == "incomplete_tail") {
        std::cerr << "enginex replay: journal ends with an incomplete record; run with "
                     "--repair-tail to preserve and truncate it\n";
        return kExitIncompleteTail;
    }
    return 0;
}

}  // namespace enginex::cli
