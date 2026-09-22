#include <Cue/Distribution/Windows/Installer.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
/// @brief Windows Command LineのUnicode文字列をUTF-8へ変換する
[[nodiscard]] std::string utf8_text(std::wstring_view a_value)
{
    const std::u8string value = std::filesystem::path(a_value).u8string();
    return std::string(reinterpret_cast<const char *>(value.data()), value.size());
}

/// @brief 指定Optionの直後にある値を返す
[[nodiscard]] std::optional<std::string> argument_value(int a_argumentCount, wchar_t **a_arguments,
                                                        std::wstring_view a_name)
{
    for (int index = 1; index + 1 < a_argumentCount; ++index)
    {
        if (std::wstring_view(a_arguments[index]) == a_name)
        {
            return utf8_text(a_arguments[index + 1]);
        }
    }
    return std::nullopt;
}

/// @brief Command Lineに指定Flagが含まれるか判定する
[[nodiscard]] bool has_argument(int a_argumentCount, wchar_t **a_arguments, std::wstring_view a_name) noexcept
{
    for (int index = 1; index < a_argumentCount; ++index)
    {
        if (std::wstring_view(a_arguments[index]) == a_name)
        {
            return true;
        }
    }
    return false;
}

/// @brief 十進数文字列をNative Handle値へ厳密に変換する
[[nodiscard]] std::optional<std::uintptr_t> handle_value(std::string_view a_value) noexcept
{
    std::uintptr_t result = 0U;
    const auto [end, error] = std::from_chars(a_value.data(), a_value.data() + a_value.size(), result);
    if (error != std::errc{} || end != a_value.data() + a_value.size() || result == 0U)
    {
        return std::nullopt;
    }
    return result;
}

/// @brief Uninstall Worker専用Command Lineを検証して回復可能Transactionを再開する
[[nodiscard]] int run(int a_argumentCount, wchar_t **a_arguments, cue::Logger &a_logger,
                      const cue::AssertContext &a_assertContext)
{
    auto gateText = argument_value(a_argumentCount, a_arguments, L"--gate-handle");
    auto workerDirectoryText = argument_value(a_argumentCount, a_arguments, L"--worker-directory-handle");
    auto workerExecutableText = argument_value(a_argumentCount, a_arguments, L"--worker-executable-handle");
    auto workerMarkerText = argument_value(a_argumentCount, a_arguments, L"--worker-marker-handle");
    auto sourceProcessText = argument_value(a_argumentCount, a_arguments, L"--source-process-handle");
    auto installRoot = argument_value(a_argumentCount, a_arguments, L"--install-root");
    auto operationId = argument_value(a_argumentCount, a_arguments, L"--operation-id");
    auto workerId = argument_value(a_argumentCount, a_arguments, L"--worker-id");
    auto gateHandle = gateText ? handle_value(*gateText) : std::nullopt;
    auto workerDirectoryHandle = workerDirectoryText ? handle_value(*workerDirectoryText) : std::nullopt;
    auto workerExecutableHandle = workerExecutableText ? handle_value(*workerExecutableText) : std::nullopt;
    auto workerMarkerHandle = workerMarkerText ? handle_value(*workerMarkerText) : std::nullopt;
    auto sourceProcessHandle = sourceProcessText ? handle_value(*sourceProcessText) : std::nullopt;
    if (!has_argument(a_argumentCount, a_arguments, L"--uninstall-worker") || !gateHandle || !workerDirectoryHandle ||
        !workerExecutableHandle || !workerMarkerHandle || (sourceProcessText && !sourceProcessHandle) || !installRoot ||
        !operationId || !workerId)
    {
        static_cast<void>(a_logger.log(cue::LogLevel::Error, "Install Worker arguments are invalid"));
        return 2;
    }

    cue::distribution::WindowsUninstallWorkerRequest request;
    request.gateHandle = *gateHandle;
    request.workerDirectoryHandle = *workerDirectoryHandle;
    request.workerExecutableHandle = *workerExecutableHandle;
    request.workerMarkerHandle = *workerMarkerHandle;
    request.sourceProcessHandle = sourceProcessHandle.value_or(0U);
    request.installRoot = std::move(*installRoot);
    request.operationId = std::move(*operationId);
    request.workerId = std::move(*workerId);
    auto uninstalled = cue::distribution::run_windows_uninstall_worker(request, a_assertContext);
    if (!uninstalled)
    {
        static_cast<void>(a_logger.log(cue::LogLevel::Error, "Installed Version uninstall failed",
                                       std::move(*uninstalled.try_error())));
        return 1;
    }
    return 0;
}
} // namespace

/// @brief Version外Uninstall Workerの診断寿命を所有してTransaction結果を返す
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    cue::AbortFatalHandler fatalHandler;
    try
    {
        std::vector<std::unique_ptr<cue::LogSink>> sinks;
        sinks.push_back(std::make_unique<cue::ConsoleLogSink>());
        cue::Logger logger(fatalHandler, std::move(sinks));
        cue::AssertContext assertContext(logger, fatalHandler);
        return run(a_argumentCount, a_arguments, logger, assertContext);
    }
    catch (...)
    {
        fatalHandler.terminate("Distribution Install Worker allocation failed");
    }
}
