#pragma once

#include <cstdint>
#include <optional>

namespace enginex {

// Overflow-checked arithmetic on signed 64-bit integers. Returns nullopt on overflow instead of
// invoking undefined behavior.
[[nodiscard]] inline std::optional<std::int64_t> checked_add(std::int64_t a,
                                                             std::int64_t b) noexcept {
    std::int64_t out = 0;
    if (__builtin_add_overflow(a, b, &out)) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] inline std::optional<std::int64_t> checked_sub(std::int64_t a,
                                                             std::int64_t b) noexcept {
    std::int64_t out = 0;
    if (__builtin_sub_overflow(a, b, &out)) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] inline std::optional<std::int64_t> checked_mul(std::int64_t a,
                                                             std::int64_t b) noexcept {
    std::int64_t out = 0;
    if (__builtin_mul_overflow(a, b, &out)) {
        return std::nullopt;
    }
    return out;
}

}  // namespace enginex
