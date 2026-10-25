#include <fstream>
#include <iostream>

#include "cli.hpp"
#include "enginex/simulate.hpp"

namespace enginex::cli {

int cmd_simulate(const std::vector<std::string>& argv) {
    const Args args(argv, {"input", "output", "symbols"}, {"compact"});
    const std::string input_path = args.require("input");
    const std::string output_path = args.get_or("output", "-");

    StateConfig config;
    if (const auto symbols = args.get("symbols")) {
        config.symbols = split_list(*symbols);
    }

    codec::Json doc;
    if (input_path == "-") {
        doc = run_simulation(std::cin, config);
    } else {
        std::ifstream in(input_path);
        if (!in) {
            throw UsageError("cannot open input file: " + input_path);
        }
        doc = run_simulation(in, config);
    }

    const int indent = args.flag("compact") ? -1 : 2;
    const std::string text = doc.dump(indent) + "\n";
    if (output_path == "-") {
        std::cout << text;
        std::cout.flush();
        return std::cout ? 0 : 1;
    }
    std::ofstream out(output_path, std::ios::trunc);
    if (!out) {
        throw UsageError("cannot open output file: " + output_path);
    }
    out << text;
    out.close();
    if (!out) {
        std::cerr << "enginex simulate: failed writing " << output_path << "\n";
        return 1;
    }
    std::cerr << "enginex simulate: wrote " << doc["responses"].size() << " responses to "
              << output_path << " (state_hash "
              << doc["final_state"]["state_hash"].get<std::string>() << ")\n";
    return 0;
}

}  // namespace enginex::cli
