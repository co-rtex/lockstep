#include "enginex/crc32c.hpp"

#include <array>

namespace enginex {

namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1U) != 0 ? 0x82F63B78U ^ (c >> 1U) : c >> 1U;
        }
        table[i] = c;
    }
    return table;
}

constexpr auto kTable = make_table();

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t size, std::uint32_t crc) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc = kTable[(crc ^ p[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

}  // namespace enginex
