#include <gtest/gtest.h>

#include <string>

#include "enginex/sha256.hpp"

namespace enginex {
namespace {

// Known-answer vectors from FIPS 180-4 / NIST CAVP examples.
TEST(Sha256, KnownAnswers) {
    EXPECT_EQ(Sha256::hex_digest(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(Sha256::hex_digest("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(Sha256::hex_digest("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    EXPECT_EQ(Sha256::hex_digest(std::string(1'000'000, 'a')),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, StreamingMatchesOneShot) {
    std::string data;
    for (int i = 0; i < 1000; ++i) {
        data += std::to_string(i * 7919);
    }
    for (std::size_t chunk : {1U, 3U, 55U, 56U, 63U, 64U, 65U, 127U, 4096U}) {
        Sha256 h;
        for (std::size_t off = 0; off < data.size(); off += chunk) {
            h.update(std::string_view(data).substr(off, chunk));
        }
        EXPECT_EQ(Sha256::hex(h.finish()), Sha256::hex_digest(data)) << "chunk " << chunk;
    }
}

}  // namespace
}  // namespace enginex
