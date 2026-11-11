// Prints the book state hash for a fixed set of seeded synthetic ITCH workloads. Built twice
// (default layout, and -DENGINEX_ITCH_MAP_LEVELS -DENGINEX_ITCH_HASH_INDEX) so ctest can check
// that every layout variant reconstructs byte-identical books (tests/CMakeLists.txt).
#include <cstdio>

#include "enginex/itch/book.hpp"
#include "enginex/itch/synthetic.hpp"

int main() {
    using namespace enginex::itch;
    struct Case {
        std::uint64_t seed;
        std::size_t messages;
        std::uint16_t symbols;
        std::uint32_t anomalies;
    };
    const Case cases[] = {{1, 50'000, 1, 0},
                          {2, 50'000, 8, 0},
                          {3, 30'000, 500, 0},
                          {4, 50'000, 8, 20'000},
                          {5, 20'000, 2, 100'000}};
    std::printf("layout %s %s\n", kOrderIndexName, kLevelAllocName);
    for (const Case& c : cases) {
        SyntheticOptions o;
        o.seed = c.seed;
        o.messages = c.messages;
        o.symbols = c.symbols;
        o.anomalies_per_million = c.anomalies;
        BookBuilder b;
        for (const Message& m : generate(o)) {
            (void)b.apply(m);
        }
        std::printf("seed %llu anomalies %llu live %llu hash %s\n",
                    static_cast<unsigned long long>(c.seed),
                    static_cast<unsigned long long>(b.stats().anomaly_count()),
                    static_cast<unsigned long long>(b.stats().live_orders), b.state_hash().c_str());
    }
    return 0;
}
