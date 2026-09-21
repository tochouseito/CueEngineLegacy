#include <Cue/EngineAssets/Error.h>

#include <Cue/Foundation/Assert.h>

#include <utility>

namespace cue::engine_assets
{
/// @brief EngineAssets固有Domainで呼び出し側が分類可能なErrorを構築する
Error make_engine_assets_error(const AssertContext &a_assertContext, EngineAssetsError a_code,
                               std::string_view a_summary) noexcept
{
    ErrorCode code =
        ErrorCode::create(a_assertContext.fatal_handler(), "Cue.EngineAssets", static_cast<std::int64_t>(a_code));
    return Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}
} // namespace cue::engine_assets
