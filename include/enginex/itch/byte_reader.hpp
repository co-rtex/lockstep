#pragma once

// Bounds-checked, endian-explicit reads of fixed-width fields from a byte buffer.
//
// ITCH 5.0 integers are unsigned big-endian (network byte order). Values are assembled with
// shifts from individual bytes, never by casting the buffer to a wider type: there is no
// alignment requirement, no strict-aliasing issue and the result does not depend on the host's
// endianness. Every read checks `offset + width <= size`; a failed check is a programming error
// in the caller (decoders validate the message length first), reported by an exception rather
// than undefined behavior.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

namespace enginex::itch {

class ByteReader {
public:
    explicit constexpr ByteReader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] constexpr std::size_t size() const noexcept { return bytes_.size(); }

    [[nodiscard]] constexpr std::uint8_t u8(std::size_t offset) const {
        check(offset, 1);
        return bytes_[offset];
    }
    [[nodiscard]] constexpr std::uint16_t u16(std::size_t offset) const {
        return static_cast<std::uint16_t>(be(offset, 2));
    }
    [[nodiscard]] constexpr std::uint32_t u32(std::size_t offset) const {
        return static_cast<std::uint32_t>(be(offset, 4));
    }
    // 6-byte timestamp (nanoseconds since midnight; at most 86'400e9 < 2^47).
    [[nodiscard]] constexpr std::uint64_t u48(std::size_t offset) const { return be(offset, 6); }
    [[nodiscard]] constexpr std::uint64_t u64(std::size_t offset) const { return be(offset, 8); }

    [[nodiscard]] constexpr char alpha(std::size_t offset) const {
        return static_cast<char>(u8(offset));
    }

    template <std::size_t N>
    [[nodiscard]] constexpr std::array<char, N> alpha(std::size_t offset) const {
        check(offset, N);
        std::array<char, N> out{};
        for (std::size_t i = 0; i < N; ++i) {
            out[i] = static_cast<char>(bytes_[offset + i]);
        }
        return out;
    }

private:
    constexpr void check(std::size_t offset, std::size_t width) const {
        if (offset > bytes_.size() || width > bytes_.size() - offset) {
            throw std::out_of_range("ITCH field read past end of message");
        }
    }
    [[nodiscard]] constexpr std::uint64_t be(std::size_t offset, std::size_t width) const {
        check(offset, width);
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < width; ++i) {
            v = (v << 8U) | bytes_[offset + i];
        }
        return v;
    }

    std::span<const std::uint8_t> bytes_;
};

// Big-endian writers used by the encoder (fixtures, synthetic generator). `out` must have room.
inline void put_be(std::uint8_t* out, std::uint64_t value, std::size_t width) noexcept {
    for (std::size_t i = 0; i < width; ++i) {
        out[width - 1 - i] = static_cast<std::uint8_t>(value & 0xFFU);
        value >>= 8U;
    }
}

// Alpha fields are left-justified ASCII, right-padded with spaces. Returns the unpadded view.
template <std::size_t N>
[[nodiscard]] std::string_view trim_alpha(const std::array<char, N>& field) noexcept {
    std::size_t n = N;
    while (n > 0 && field[n - 1] == ' ') {
        --n;
    }
    return {field.data(), n};
}

}  // namespace enginex::itch
