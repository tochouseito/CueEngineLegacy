#include <Cue/Distribution/Windows/Installer.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <Windows.h>

#include <charconv>
#include <cstdlib>
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
    if (error != std::errc{} || end != a_value.data() + a_value.size())
    {
        return std::nullopt;
    }
    return result;
}

/// @brief Per-user既定Install RootをLOCALAPPDATAから解決する
[[nodiscard]] std::optional<std::string> default_install_root()
{
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0U);
    if (required == 0U)
    {
        return std::nullopt;
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
    if (written == 0U || written >= required)
    {
        return std::nullopt;
    }
    value.resize(written);
    return utf8_text((std::filesystem::path(value) / L"CueEngine").native());
}

/// @brief Install Probe専用Child Process要求を検証して実行する
[[nodiscard]] int run_probe(int a_argumentCount, wchar_t **a_arguments, cue::Logger &a_logger,
                            const cue::AssertContext &a_assertContext)
{
    auto leaseText = argument_value(a_argumentCount, a_arguments, L"--lease-handle");
    auto installRoot = argument_value(a_argumentCount, a_arguments, L"--install-root");
    auto versionDirectory = argument_value(a_argumentCount, a_arguments, L"--version-directory");
    auto operationId = argument_value(a_argumentCount, a_arguments, L"--operation-id");
    auto manifestDigest = argument_value(a_argumentCount, a_arguments, L"--manifest-digest");
    auto leaseHandle = leaseText ? handle_value(*leaseText) : std::nullopt;
    if (!leaseHandle || !installRoot || !versionDirectory || !operationId || !manifestDigest)
    {
        static_cast<void>(a_logger.log(cue::LogLevel::Error, "Install Probe arguments are invalid"));
        return 2;
    }
    cue::distribution::WindowsInstallProbeRequest request;
    request.leaseHandle = *leaseHandle;
    request.installRoot = std::move(*installRoot);
    request.versionDirectory = std::move(*versionDirectory);
    request.operationId = std::move(*operationId);
    request.manifestDigest = std::move(*manifestDigest);
    auto probed = cue::distribution::run_windows_install_probe(request, a_assertContext);
    if (!probed)
    {
        static_cast<void>(
            a_logger.log(cue::LogLevel::Error, "Installed Version Probe failed", std::move(*probed.try_error())));
        return 1;
    }
    return 0;
}

/// @brief Local Developer Source SDKのInstallまたはUpdate CLIを実行する
[[nodiscard]] int run_install(int a_argumentCount, wchar_t **a_arguments, cue::Logger &a_logger,
                              const cue::AssertContext &a_assertContext)
{
    const std::wstring_view command = a_argumentCount > 1 ? std::wstring_view(a_arguments[1]) : std::wstring_view{};
    auto bundleRoot = argument_value(a_argumentCount, a_arguments, L"--bundle-root");
    auto installRoot = argument_value(a_argumentCount, a_arguments, L"--install-root");
    if (!installRoot)
    {
        installRoot = default_install_root();
    }
    if ((command != L"install" && command != L"update") || !bundleRoot || !installRoot)
    {
        static_cast<void>(a_logger.log(cue::LogLevel::Error,
                                       "Usage: CueEngineInstallerTool <install|update> --bundle-root <absolute> "
                                       "[--install-root <absolute>] [--allow-unsigned-local]"));
        return 2;
    }
    cue::distribution::WindowsInstallRequest request;
    request.bundleRoot = std::move(*bundleRoot);
    request.installRoot = std::move(*installRoot);
    request.isUpdate = command == L"update";
    request.allowUnsignedLocal = has_argument(a_argumentCount, a_arguments, L"--allow-unsigned-local");
    auto installed = cue::distribution::install_windows_source_sdk(request, a_assertContext);
    if (!installed)
    {
        static_cast<void>(a_logger.log(cue::LogLevel::Error, "Developer Source SDK installation failed",
                                       std::move(*installed.try_error())));
        return 1;
    }
    static_cast<void>(a_logger.log(cue::LogLevel::Info, installed.try_value()->wasAlreadyInstalled
                                                            ? "Developer Source SDK is already installed"
                                                            : "Developer Source SDK installation completed"));
    return 0;
}

/// @brief Install済みVersionのRollback選択または外部WorkerへのUninstall委譲を実行する
[[nodiscard]] int run_version_operation(int a_argumentCount, wchar_t **a_arguments, cue::Logger &a_logger,
                                        const cue::AssertContext &a_assertContext)
{
    const std::wstring_view command = a_argumentCount > 1 ? std::wstring_view(a_arguments[1]) : std::wstring_view{};
    auto installRoot = argument_value(a_argumentCount, a_arguments, L"--install-root");
    auto versionDirectory = argument_value(a_argumentCount, a_arguments, L"--version-directory");
    if (!installRoot)
    {
        installRoot = default_install_root();
    }
    if ((command != L"rollback" && command != L"uninstall") || !installRoot || !versionDirectory)
    {
        static_cast<void>(a_logger.log(cue::LogLevel::Error,
                                       "Usage: CueEngineInstallerTool <rollback|uninstall> --version-directory <name> "
                                       "[--install-root <absolute>]"));
        return 2;
    }

    cue::distribution::WindowsInstalledVersionRequest request;
    request.installRoot = std::move(*installRoot);
    request.versionDirectory = std::move(*versionDirectory);
    if (command == L"rollback")
    {
        auto rolledBack = cue::distribution::rollback_windows_installed_version(request, a_assertContext);
        if (!rolledBack)
        {
            static_cast<void>(a_logger.log(cue::LogLevel::Error, "Installed Version rollback failed",
                                           std::move(*rolledBack.try_error())));
            return 1;
        }
        static_cast<void>(a_logger.log(cue::LogLevel::Info, rolledBack.try_value()->wasAlreadySelected
                                                                ? "Installed Version is already selected"
                                                                : "Installed Version rollback completed"));
        return 0;
    }

    auto uninstalled = cue::distribution::uninstall_windows_installed_version(request, a_assertContext);
    if (!uninstalled)
    {
        static_cast<void>(a_logger.log(cue::LogLevel::Error, "Installed Version uninstall failed",
                                       std::move(*uninstalled.try_error())));
        return 1;
    }
    static_cast<void>(a_logger.log(cue::LogLevel::Info, "Installed Version uninstall was delegated to worker process " +
                                                            std::to_string(uninstalled.try_value()->workerProcessId)));
    return 0;
}

/// @brief Installer CLIのModeを選択して実行する
[[nodiscard]] int run(int a_argumentCount, wchar_t **a_arguments, cue::Logger &a_logger,
                      const cue::AssertContext &a_assertContext)
{
    if (has_argument(a_argumentCount, a_arguments, L"--install-probe"))
    {
        return run_probe(a_argumentCount, a_arguments, a_logger, a_assertContext);
    }
    if (a_argumentCount > 1 &&
        (std::wstring_view(a_arguments[1]) == L"rollback" || std::wstring_view(a_arguments[1]) == L"uninstall"))
    {
        return run_version_operation(a_argumentCount, a_arguments, a_logger, a_assertContext);
    }
    return run_install(a_argumentCount, a_arguments, a_logger, a_assertContext);
}
} // namespace

/// @brief Developer Source SDK Installerの診断寿命を所有してCLI結果を返す
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
        fatalHandler.terminate("Distribution Installer Tool allocation failed");
    }
}
