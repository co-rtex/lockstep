// Seeded mutation driver for LLVMFuzzerTestOneInput where libFuzzer is unavailable (Apple
// clang). Not coverage-guided: it mutates corpus files (bit flips, byte sets, inserts,
// deletes, truncation, splices) with a portable PRNG, so a run is reproducible from its seed.
//   enginex_fuzz_itch_standalone ITERATIONS SEED CORPUS_FILE...
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "enginex/rng.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s ITERATIONS SEED CORPUS_FILE...\n", argv[0]);
        return 2;
    }
    const std::uint64_t iterations = std::stoull(argv[1]);
    enginex::Rng rng(std::stoull(argv[2]));
    std::vector<std::vector<std::uint8_t>> corpus;
    for (int i = 3; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        corpus.emplace_back(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::uint64_t bytes = 0;
    for (std::uint64_t it = 0; it < iterations; ++it) {
        std::vector<std::uint8_t> in(1, static_cast<std::uint8_t>(rng.below(3)));  // target
        const auto& base = corpus[rng.below(corpus.size())];
        in.insert(in.end(), base.begin(), base.end());
        const std::uint64_t edits = 1 + rng.below(8);
        for (std::uint64_t e = 0; e < edits && in.size() > 1; ++e) {
            const std::size_t at = 1 + rng.below(in.size() - 1);
            switch (rng.below(6)) {
                case 0: in[at] ^= static_cast<std::uint8_t>(1U << rng.below(8)); break;
                case 1: in[at] = static_cast<std::uint8_t>(rng.next()); break;
                case 2: in.insert(in.begin() + static_cast<std::ptrdiff_t>(at),
                                  static_cast<std::uint8_t>(rng.next())); break;
                case 3: in.erase(in.begin() + static_cast<std::ptrdiff_t>(at)); break;
                case 4: in.resize(at); break;
                default: {
                    const auto& other = corpus[rng.below(corpus.size())];
                    if (!other.empty()) {
                        const std::size_t from = rng.below(other.size());
                        const std::size_t n = std::min<std::size_t>(rng.below(64), other.size() - from);
                        in.insert(in.begin() + static_cast<std::ptrdiff_t>(at),
                                  other.begin() + static_cast<std::ptrdiff_t>(from),
                                  other.begin() + static_cast<std::ptrdiff_t>(from + n));
                    }
                }
            }
        }
        bytes += in.size();
        LLVMFuzzerTestOneInput(in.data(), in.size());
    }
    std::printf("standalone fuzz: %llu inputs, %llu bytes, no invariant violated\n",
                static_cast<unsigned long long>(iterations), static_cast<unsigned long long>(bytes));
    return 0;
}
