#include <Cue/Distribution/Windows/SourceSdkPublisher.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }

    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

void require(bool a_condition, std::source_location a_location = std::source_location::current()) noexcept
{
    if (!a_condition)
    {
        std::fprintf(stderr, "Requirement failed at %s:%u\n", a_location.file_name(), a_location.line());
        std::abort();
    }
}
} // namespace

int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    cue::distribution::WindowsSourceSdkPublishRequest request;
    request.engineVersion = "1.0.0";
    request.bundleId = "12345678-1234-4abc-8def-1234567890ab";
    request.publisherBuildIdentity.configuration = "Release";
    const auto rejected = cue::distribution::publish_windows_source_sdk(request, assertContext);
    require(!rejected);
    require(rejected.try_error()->root_code().domain() == "Cue.Distribution");
    return 0;
}
