#include <Windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>

namespace
{
/// @brief 指定Command Line Optionの直後にある値を返す
[[nodiscard]] std::optional<std::wstring_view> argument_value(int a_argumentCount, wchar_t **a_arguments,
                                                              std::wstring_view a_name) noexcept
{
    for (int index = 1; index + 1 < a_argumentCount; ++index)
    {
        if (std::wstring_view(a_arguments[index]) == a_name)
        {
            return std::wstring_view(a_arguments[index + 1]);
        }
    }
    return std::nullopt;
}

/// @brief Fileが存在するかWin32属性で確認する
[[nodiscard]] bool path_exists(const std::filesystem::path &a_path) noexcept
{
    return GetFileAttributesW(a_path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
} // namespace

/// @brief Process Test用ProbeとしてReady通知後にRelease Gateまで待機する
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    if (a_argumentCount < 2 || std::wstring_view(a_arguments[1]) != L"--install-probe")
    {
        return 2;
    }
    auto installRoot = argument_value(a_argumentCount, a_arguments, L"--install-root");
    if (!installRoot)
    {
        return 2;
    }
    const std::filesystem::path operations = std::filesystem::path(*installRoot) / L"Operations";
    const std::filesystem::path enabled = operations / L"TestProbeGate.enabled";
    if (!path_exists(enabled))
    {
        return 0;
    }
    const std::filesystem::path ready = operations / L"TestProbeGate.ready";
    std::ofstream output(ready, std::ios::trunc);
    if (!output.good())
    {
        return 3;
    }
    output << GetCurrentProcessId() << '\n';
    output.close();
    const std::filesystem::path release = operations / L"TestProbeGate.release";
    constexpr DWORD timeoutMilliseconds = 60000U;
    constexpr DWORD intervalMilliseconds = 10U;
    for (DWORD elapsed = 0U; elapsed < timeoutMilliseconds; elapsed += intervalMilliseconds)
    {
        if (path_exists(release))
        {
            return 0;
        }
        Sleep(intervalMilliseconds);
    }
    return 4;
}
