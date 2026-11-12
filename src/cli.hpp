#pragma once

// Small hand-written argument parser shared by the `enginex` subcommands. Options are
// `--name value` or `--flag`; anything unrecognized is an error so typos never silently change a
// benchmark or durability setting.

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace enginex::cli {

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Args {
public:
    // `flags` take no value; `options` take exactly one value.
    Args(const std::vector<std::string>& argv, const std::set<std::string>& options,
         const std::set<std::string>& flags);

    [[nodiscard]] bool flag(const std::string& name) const { return flags_.contains(name); }
    [[nodiscard]] std::optional<std::string> get(const std::string& name) const;
    [[nodiscard]] std::string get_or(const std::string& name, const std::string& fallback) const;
    [[nodiscard]] std::string require(const std::string& name) const;
    [[nodiscard]] std::uint64_t get_u64(const std::string& name, std::uint64_t fallback) const;
    // A duration in milliseconds that must lie in [min_ms, max_ms] (UsageError otherwise), so a
    // huge value can never wrap to a negative std::chrono::milliseconds.
    [[nodiscard]] std::chrono::milliseconds get_ms(const std::string& name, std::uint64_t fallback,
                                                   std::uint64_t min_ms,
                                                   std::uint64_t max_ms) const;

private:
    std::map<std::string, std::string> values_;
    std::set<std::string> flags_;
};

// Splits "AAPL,MSFT" into its parts (empty parts are an error).
std::vector<std::string> split_list(const std::string& text);

int cmd_simulate(const std::vector<std::string>& args);
int cmd_benchmark(const std::vector<std::string>& args);
int cmd_run(const std::vector<std::string>& args);
int cmd_replay(const std::vector<std::string>& args);
int cmd_serve(const std::vector<std::string>& args);
int cmd_client(const std::vector<std::string>& args);
int cmd_itch(const std::vector<std::string>& args);

}  // namespace enginex::cli
