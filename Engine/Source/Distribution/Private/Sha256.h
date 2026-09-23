#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cue::distribution_private
{
using Sha256Digest = std::array<std::uint8_t, 32U>;
using Sha256CancellationCallback = bool (*)(const void *) noexcept;

/// @brief 任意Byte列のSHA-256 DigestをAllocationなしで計算する
[[nodiscard]] Sha256Digest compute_sha256(std::span<const std::byte> a_bytes) noexcept;
/// @brief 1 MiBごとに取消を観測しながらSHA-256 Digestを計算する
[[nodiscard]] std::optional<Sha256Digest> compute_sha256_cancellable(
    std::span<const std::byte> a_bytes, Sha256CancellationCallback a_cancellation,
    const void *a_cancellationContext) noexcept;
/// @brief TextのSHA-256 Digestをlowercase hexadecimalへ変換する
[[nodiscard]] std::string sha256_text(std::string_view a_text);
} // namespace cue::distribution_private
