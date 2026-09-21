#pragma once

#include <Cue/Foundation/Error.h>

#include <cstdint>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::engine_assets
{
/// @brief Built-in Asset Catalog境界の回復可能な失敗を分類するCode
enum class EngineAssetsError : std::int64_t
{
    InvalidAssetId = 1,
    UnknownAsset = 2,
    KindMismatch = 3,
    DuplicateAssetId = 4,
    InvalidRevision = 5,
    InvalidDescriptor = 6,
    PayloadUnavailable = 7
};

/// @brief EngineAssets Errorを診断Summaryと共に生成する
[[nodiscard]] Error make_engine_assets_error(const AssertContext &a_assertContext, EngineAssetsError a_code,
                                             std::string_view a_summary) noexcept;
} // namespace cue::engine_assets
