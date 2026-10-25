// enginex: command-line entry point. See README.md for the full command reference.
#include <charconv>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "cli.hpp"
#include "enginex/build_info.hpp"

namespace enginex::cli {

Args::Args(const std::vector<std::string>& argv, const std::set<std::string>& options,
           const std::set<std::string>& flags) {
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg.rfind("--", 0) != 0) {
            throw UsageError("unexpected argument: " + arg);
        }
        const std::string name = arg.substr(2);
        if (flags.contains(name)) {
            flags_.insert(name);
        } else if (options.contains(name)) {
            if (i + 1 >= argv.size()) {
                throw UsageError("missing value for --" + name);
            }
            values_[name] = argv[++i];
        } else {
            throw UsageError("unknown option: " + arg);
        }
    }
}

std::optional<std::string> Args::get(const std::string& name) const {
    const auto it = values_.find(name);
    if (it == values_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::string Args::get_or(const std::string& name, const std::string& fallback) const {
    return get(name).value_or(fallback);
}

std::string Args::require(const std::string& name) const {
    auto v = get(name);
    if (!v) {
        throw UsageError("missing required option --" + name);
    }
    return *v;
}

std::uint64_t Args::get_u64(const std::string& name, std::uint64_t fallback) const {
    const auto v = get(name);
    if (!v) {
        return fallback;
    }
    std::uint64_t out = 0;
    const auto [ptr, ec] = std::from_chars(v->data(), v->data() + v->size(), out);
    if (ec != std::errc{} || ptr != v->data() + v->size() || v->empty()) {
        throw UsageError("--" + name + " expects a nonnegative integer, got: " + *v);
    }
    return out;
}

std::vector<std::string> split_list(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = text.find(',', start);
        std::string part = text.substr(start, comma == std::string::npos ? comma : comma - start);
        if (part.empty()) {
            throw UsageError("empty element in list: \"" + text + "\"");
        }
        out.push_back(std::move(part));
        if (comma == std::string::npos) {
            return out;
        }
        start = comma + 1;
    }
}

}  // namespace enginex::cli

namespace {

void print_usage() {
    std::cout <<
        R"(enginex - deterministic C++20 limit-order matching engine (educational simulator)

Usage:
  enginex simulate  --input FILE|- [--output FILE|-] [--symbols AAPL,MSFT] [--compact]
  enginex benchmark --scenario NAME [--seed N] [--orders N] [--trials N] [--warmup N]
                    [--symbols N] [--out FILE.csv] [--no-latency]
                    [--mode core|durable|recovery] [--durability sync|fsync|none]
                    [--journal-dir DIR]
  enginex run       --journal FILE [--durability sync|fsync|none] [--input FILE|-]
                    [--output FILE|-] [--symbols AAPL,MSFT] [--refuse-tail]
  enginex replay    --journal FILE [--verify] [--repair-tail] [--dump] [--symbols ...]
  enginex version
  enginex help

Scenarios: no-cross, high-cross, same-price-burst, multi-level-sweep, cancel-heavy, mixed

run:    recovers state from the journal, then processes JSON-lines commands; each response
        line is written only after its journal record is synchronized (--durability sync).
        Exit 0 ok, 1 journal corrupt, 4 journal I/O failure (fail-stop).
replay: validates and replays a journal from an empty state and prints the state hash.
        Exit 0 clean, 1 corrupt/unreadable, 3 incomplete tail (use --repair-tail).
)";
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> all(argv + 1, argv + argc);
    if (all.empty() || all[0] == "help" || all[0] == "--help" || all[0] == "-h") {
        print_usage();
        return all.empty() ? 2 : 0;
    }
    const std::string command = all[0];
    const std::vector<std::string> rest(all.begin() + 1, all.end());
    try {
        if (command == "simulate") {
            return enginex::cli::cmd_simulate(rest);
        }
        if (command == "benchmark") {
            return enginex::cli::cmd_benchmark(rest);
        }
        if (command == "run") {
            return enginex::cli::cmd_run(rest);
        }
        if (command == "replay") {
            return enginex::cli::cmd_replay(rest);
        }
        if (command == "version") {
            namespace bi = enginex::build_info;
            std::cout << "enginex " << bi::kVersion << " (git " << bi::kGitSha << ", "
                      << bi::kCompilerId << " " << bi::kCompilerVersion << ", " << bi::kBuildType
                      << ", " << bi::kSystem << ")\n";
            return 0;
        }
        std::cerr << "enginex: unknown command \"" << command << "\"\n";
        print_usage();
        return 2;
    } catch (const enginex::cli::UsageError& e) {
        std::cerr << "enginex " << command << ": " << e.what() << "\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "enginex " << command << ": fatal: " << e.what() << "\n";
        return 1;
    }
}
