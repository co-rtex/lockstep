#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace enginex {

// Minimal SHA-256 (FIPS 180-4) used to identify canonical states in tests and recovery checks.
// It is not used for authentication or any security property.
class Sha256 {
public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    void update(std::string_view text) noexcept { update(text.data(), text.size()); }
    [[nodiscard]] Digest finish() noexcept;

    [[nodiscard]] static Digest digest(std::string_view text) noexcept;
    [[nodiscard]] static std::string hex(const Digest& digest);
    [[nodiscard]] static std::string hex_digest(std::string_view text) { return hex(digest(text)); }

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

}  // namespace enginex
