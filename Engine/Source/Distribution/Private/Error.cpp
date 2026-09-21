#include <Cue/Distribution/Error.h>

#include <Cue/Foundation/Assert.h>

#include <utility>

namespace cue::distribution
{
Error make_distribution_error(const AssertContext &a_assertContext, DistributionError a_error,
                              std::string_view a_summary) noexcept
{
    ErrorCode code =
        ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Distribution", static_cast<std::int64_t>(a_error));
    return Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}
} // namespace cue::distribution
