#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Project/Error.h>
#include <Cue/Project/Generator.h>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
#if !defined(CUE_TEST_CMAKE_COMMAND) || !defined(CUE_TEST_ENGINE_ROOT) || !defined(CUE_TEST_BUILD_CONFIGURATION)
#error Generator workspace tests require CMake command and Engine root definitions
#endif

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test 中の回復不能失敗を即座に終了 Code へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }

    /// @brief Message 付き回復不能失敗を即座に終了 Code へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

class TestDirectory final
{
  public:
    /// @brief Process と時刻から一意な実 Filesystem Test Root を作成する
    TestDirectory()
    {
        std::array<wchar_t, MAX_PATH> temporary{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (length == 0U || length >= temporary.size())
        {
            return;
        }
        m_path = temporary.data();
        m_path += L"CueGen-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        m_isCreated = CreateDirectoryW(m_path.c_str(), nullptr) != FALSE;
    }

    /// @brief Test Root の一意所有を保つため Copy 構築を禁止する
    TestDirectory(const TestDirectory &) = delete;
    /// @brief Test Root の一意所有を保つため Copy 代入を禁止する
    TestDirectory &operator=(const TestDirectory &) = delete;
    /// @brief Native Path の所有位置を固定するため Move 構築を禁止する
    TestDirectory(TestDirectory &&) = delete;
    /// @brief Native Path の所有位置を固定するため Move 代入を禁止する
    TestDirectory &operator=(TestDirectory &&) = delete;

    /// @brief Test 専用 Root だけを再帰 Cleanup する
    ~TestDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    /// @brief Root 作成に成功したか返す
    [[nodiscard]] bool is_created() const noexcept
    {
        return m_isCreated;
    }

    /// @brief Windows Filesystem Factory へ渡す UTF-8 Absolute Path を返す
    [[nodiscard]] std::string utf8_path() const
    {
        const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_path.c_str(),
                                              static_cast<int>(m_path.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<std::size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, m_path.c_str(), static_cast<int>(m_path.size()),
                            result.data(), count, nullptr, nullptr);
        return result;
    }

    /// @brief Root 配下の Native Path を存在確認へ返す
    [[nodiscard]] std::wstring child(std::wstring_view a_relative) const
    {
        return m_path + L"\\" + std::wstring(a_relative);
    }

  private:
    std::wstring m_path;
    bool m_isCreated = false;
};

/// @brief Native Directory が通常 Directory として存在するか判定する
[[nodiscard]] bool is_directory(const std::wstring &a_path) noexcept
{
    const DWORD attributes = GetFileAttributesW(a_path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

/// @brief Native File が通常 File として存在するか判定する
[[nodiscard]] bool is_file(const std::wstring &a_path) noexcept
{
    const DWORD attributes = GetFileAttributesW(a_path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

/// @brief Test設定のUTF-8 PathをWindows Process API用UTF-16へ変換する
[[nodiscard]] std::wstring utf8_to_wide(std::string_view a_text)
{
    const int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
    if (count <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_text.data(), static_cast<int>(a_text.size()),
                            result.data(), count) != count)
    {
        return {};
    }
    return result;
}

/// @brief Generated Projectへ渡すCMake引数を独立Processで実行する
[[nodiscard]] bool run_cmake(const std::wstring &a_projectRoot, std::wstring_view a_arguments)
{
    const std::wstring cmake = utf8_to_wide(CUE_TEST_CMAKE_COMMAND);
    if (cmake.empty())
    {
        return false;
    }
    std::wstring commandLine = L"\"" + cmake + L"\" " + std::wstring(a_arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(cmake.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0U, nullptr, a_projectRoot.c_str(),
                       &startup, &process) == FALSE)
    {
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 300000U);
    DWORD exitCode = 1U;
    const bool completed = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exitCode) != FALSE;
    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(process.hProcess, 1U);
        WaitForSingleObject(process.hProcess, 5000U);
    }
    CloseHandle(process.hProcess);
    return completed && exitCode == 0U;
}

/// @brief Generated Projectの指定PresetをConfigureする
[[nodiscard]] bool configure_preset(const std::wstring &a_projectRoot, std::wstring_view a_preset)
{
    return run_cmake(a_projectRoot, L"--preset " + std::wstring(a_preset));
}

/// @brief Generated Projectの指定PresetをBuildする
[[nodiscard]] bool build_preset(const std::wstring &a_projectRoot, std::wstring_view a_preset)
{
    return run_cmake(a_projectRoot, L"--build --preset " + std::wstring(a_preset) + L" --parallel");
}

/// @brief Generated Projectの指定Presetから一TargetだけをBuildする
[[nodiscard]] bool build_target_preset(const std::wstring &a_projectRoot, std::wstring_view a_preset,
                                       std::wstring_view a_target)
{
    return run_cmake(a_projectRoot, L"--build --preset " + std::wstring(a_preset) + L" --target " +
                                        std::wstring(a_target) + L" --parallel");
}

/// @brief Package外のRelease Monolithic Productが通常起動を拒否することを確認する
[[nodiscard]] bool reject_product_smoke_without_package(const std::wstring &a_projectRoot,
                                                        const std::wstring &a_product)
{
    const std::filesystem::path productPath(a_product);
    return !run_cmake(a_projectRoot, L"-E chdir \"" + productPath.parent_path().native() + L"\" \"" +
                                         productPath.native() + L"\" --package-smoke-test");
}

/// @brief Release Monolithic ProductのArtifact Probe Protocolと固定Markerを検証する
[[nodiscard]] bool run_product_artifact_probe(const std::wstring &a_projectRoot, const std::wstring &a_product)
{
    const std::filesystem::path productPath(a_product);
    const std::filesystem::path marker = productPath.parent_path() / L".probe-complete";
    DeleteFileW(marker.c_str());
    if (!run_cmake(a_projectRoot, L"-E chdir \"" + productPath.parent_path().native() + L"\" \"" +
                                      productPath.native() +
                                      L"\" --cue-artifact-probe Release "
                                      L"12345678-1234-4abc-8def-1234567890ab"))
    {
        return false;
    }
    std::ifstream input(marker, std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const bool valid = contents == "CueGameProductProbe:v1\n";
    input.close();
    return valid && DeleteFileW(marker.c_str()) != FALSE;
}

/// @brief 実 Windows IO で生成・再 Open・既存先拒否を一連の Process 契約として検証する
[[nodiscard]] bool test_windows_generation(const cue::AssertContext &a_assertContext)
{
    TestDirectory directory;
    if (!directory.is_created())
    {
        return false;
    }
    auto parent = cue::create_windows_filesystem_root(directory.utf8_path(), a_assertContext);
    auto projectId = cue::ProjectId::parse("12345678-1234-4abc-8def-1234567890ab", a_assertContext);
    cue::BlankProjectTemplate projectTemplate{cue::EngineCompatibility{cue::EngineVersion{0U, 1U, 0U}, std::nullopt}};
    if (!parent || !projectId)
    {
        return false;
    }
    auto generated =
        cue::generate_blank_project(**parent.try_value(), "SampleProject", "Sample Project", *projectId.try_value(),
                                    "00000000-0000-4000-8000-000000000099", projectTemplate, a_assertContext);
    if (!generated)
    {
        return false;
    }

    constexpr std::array paths = {std::wstring_view(L"SampleProject\\Assets\\Source"),
                                  std::wstring_view(L"SampleProject\\Assets\\Source\\Scenes"),
                                  std::wstring_view(L"SampleProject\\Assets\\Runtime"),
                                  std::wstring_view(L"SampleProject\\Generated"),
                                  std::wstring_view(L"SampleProject\\Saved"),
                                  std::wstring_view(L"SampleProject\\Source\\Game")};
    for (const std::wstring_view path : paths)
    {
        if (!is_directory(directory.child(path)))
        {
            return false;
        }
    }
    if (!is_file(directory.child(L"SampleProject\\CueProject.json")) ||
        !is_file(directory.child(L"SampleProject\\CMakeLists.txt")) ||
        !is_file(directory.child(L"SampleProject\\CMakePresets.json")) ||
        !is_file(directory.child(L"SampleProject\\Assets\\Source\\Scenes\\Default.cuescene")) ||
        !is_file(directory.child(L"SampleProject\\Source\\Game\\CMakeLists.txt")) ||
        !is_file(directory.child(L"SampleProject\\Source\\Game\\GameModule.cpp")))
    {
        return false;
    }

    const std::wstring engineRoot = utf8_to_wide(CUE_TEST_ENGINE_ROOT);
    const std::wstring projectRootPath = directory.child(L"SampleProject");
    if (engineRoot.empty() || SetEnvironmentVariableW(L"CUE_ENGINE_ROOT", engineRoot.c_str()) == FALSE ||
        !configure_preset(projectRootPath, L"windows-vs2026-debug") ||
        !configure_preset(projectRootPath, L"windows-vs2026-development") ||
        !configure_preset(projectRootPath, L"windows-vs2026-release") ||
        !build_preset(projectRootPath, L"windows-vs2026-debug") ||
        !build_preset(projectRootPath, L"windows-vs2026-development") ||
        !build_preset(projectRootPath, L"windows-vs2026-release") ||
        !build_target_preset(projectRootPath, L"windows-vs2026-debug", L"CueRuntimeHostForProject") ||
        !build_target_preset(projectRootPath, L"windows-vs2026-development", L"CueRuntimeHostForProject") ||
        !build_target_preset(projectRootPath, L"windows-vs2026-release", L"CueRuntimeHostForProject") ||
        !is_file(directory.child(
            L"SampleProject\\Generated\\Build\\windows-vs2026-x64-debug\\bin\\Debug\\CueGameModule.dll")) ||
        !is_file(directory.child(
            L"SampleProject\\Generated\\Build\\windows-vs2026-x64-debug\\bin\\Debug\\CueRuntimeHost.exe")) ||
        !is_file(directory.child(L"SampleProject\\Generated\\Build\\windows-vs2026-x64-development\\bin\\"
                                 L"Development\\CueGameModule.dll")) ||
        !is_file(directory.child(L"SampleProject\\Generated\\Build\\windows-vs2026-x64-development\\bin\\"
                                 L"Development\\CueRuntimeHost.exe")) ||
        !is_file(directory.child(
            L"SampleProject\\Generated\\Build\\windows-vs2026-x64-release\\bin\\Release\\CueGameModule.dll")) ||
        !is_file(directory.child(
            L"SampleProject\\Generated\\Build\\windows-vs2026-x64-release\\bin\\Release\\CueRuntimeHost.exe")))
    {
        return false;
    }

#if CUE_TEST_BUILD_CONFIGURATION == 3
    const std::wstring productPath = directory.child(
        L"SampleProject\\Generated\\Build\\windows-vs2026-x64-release\\bin\\Release\\CueGameProduct.exe");
    const std::wstring isolatedDirectory = directory.child(L"ProductSmoke");
    const std::wstring isolatedProduct = directory.child(L"ProductSmoke\\CueGameProduct.exe");
    if (!build_target_preset(projectRootPath, L"windows-vs2026-release", L"CueGameProduct") ||
        !is_file(directory.child(
            L"SampleProject\\Generated\\Build\\windows-vs2026-x64-release\\lib\\Release\\CueGameModule.Static.lib")) ||
        !is_file(productPath) || CreateDirectoryW(isolatedDirectory.c_str(), nullptr) == FALSE ||
        CopyFileW(productPath.c_str(), isolatedProduct.c_str(), TRUE) == FALSE ||
        !run_product_artifact_probe(projectRootPath, isolatedProduct) ||
        !reject_product_smoke_without_package(projectRootPath, isolatedProduct))
    {
        return false;
    }
#endif

    auto projectRoot = cue::create_windows_filesystem_root(directory.utf8_path() + "/SampleProject", a_assertContext);
    auto loaded =
        projectRoot ? cue::load_project_descriptor(**projectRoot.try_value(), a_assertContext)
                    : cue::Result<cue::ProjectDescriptor>::failure(cue::make_project_error(
                          a_assertContext, cue::ProjectError::IoFailure, "Generated project root could not be opened"));
    if (!loaded || !generated.try_value()->equivalent_to(*loaded.try_value()))
    {
        return false;
    }

    auto rootCMake = cue::RelativePath::parse("CMakeLists.txt", a_assertContext);
    auto presets = cue::RelativePath::parse("CMakePresets.json", a_assertContext);
    auto gameCMake = cue::RelativePath::parse("Source/Game/CMakeLists.txt", a_assertContext);
    auto gameModule = cue::RelativePath::parse("Source/Game/GameModule.cpp", a_assertContext);
    if (!rootCMake || !presets || !gameCMake || !gameModule)
    {
        return false;
    }
    const std::array workspacePaths = {rootCMake.try_value(), presets.try_value(), gameCMake.try_value(),
                                       gameModule.try_value()};
    for (const auto *path : workspacePaths)
    {
        if (!projectRoot.try_value()->get()->remove_file(*path))
        {
            return false;
        }
    }

    auto ensured =
        cue::ensure_project_game_workspace(**projectRoot.try_value(), *generated.try_value(), a_assertContext);
    auto ensuredAgain =
        ensured ? cue::ensure_project_game_workspace(**projectRoot.try_value(), *generated.try_value(), a_assertContext)
                : cue::Result<void>::failure(cue::make_project_error(a_assertContext, cue::ProjectError::IoFailure,
                                                                     "Workspace files were not recreated"));
    if (!ensured || !ensuredAgain || !is_file(directory.child(L"SampleProject\\CMakeLists.txt")) ||
        !is_file(directory.child(L"SampleProject\\CMakePresets.json")) ||
        !is_file(directory.child(L"SampleProject\\Source\\Game\\CMakeLists.txt")) ||
        !is_file(directory.child(L"SampleProject\\Source\\Game\\GameModule.cpp")))
    {
        return false;
    }

    constexpr std::string_view userSource = "// User-owned Game Module source\n";
    const std::span<const char> userCharacters(userSource.data(), userSource.size());
    if (!projectRoot.try_value()->get()->remove_file(*rootCMake.try_value()) ||
        !projectRoot.try_value()->get()->write_file_atomic(*gameModule.try_value(), std::as_bytes(userCharacters)))
    {
        return false;
    }
    auto conflict =
        cue::ensure_project_game_workspace(**projectRoot.try_value(), *generated.try_value(), a_assertContext);
    auto retainedSource = projectRoot.try_value()->get()->read_file(*gameModule.try_value(), 1024U);
    const bool retained =
        retainedSource && std::string_view(reinterpret_cast<const char *>(retainedSource.try_value()->data()),
                                           retainedSource.try_value()->size()) == userSource;
    if (conflict || is_file(directory.child(L"SampleProject\\CMakeLists.txt")) || !retained)
    {
        return false;
    }

    auto duplicateId = cue::ProjectId::parse("87654321-4321-4abc-8def-ba0987654321", a_assertContext);
    auto duplicate =
        cue::generate_blank_project(**parent.try_value(), "SampleProject", "Replacement", *duplicateId.try_value(),
                                    "00000000-0000-4000-8000-000000000099", projectTemplate, a_assertContext);
    auto reloaded = cue::load_project_descriptor(**projectRoot.try_value(), a_assertContext);
    return !duplicate && reloaded && generated.try_value()->equivalent_to(*reloaded.try_value());
}
} // namespace

/// @brief Windows 実 Filesystem 上の Atomic Blank Project 生成契約を終了 Code で検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_windows_generation(assertContext) ? 0 : 1;
}
