#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace enginex {

// CRC-32C (Castagnoli, reflected polynomial 0x82F63B78), table-driven software implementation.
// Detects accidental corruption of journal records; it is not cryptographic authentication.
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t size,
                                   std::uint32_t crc = 0) noexcept;

[[nodiscard]] inline std::uint32_t crc32c(std::string_view bytes) noexcept {
    return crc32c(bytes.data(), bytes.size());
}

}  // namespace enginex
