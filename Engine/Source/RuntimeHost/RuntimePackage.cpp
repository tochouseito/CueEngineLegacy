#include "RuntimePackage.h"
#if defined(CUE_RUNTIME_PACKAGE_DYNAMIC)
#include "RuntimeModuleIdentity.h"
#endif
#include "RuntimePath.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>
#include <Cue/GameModule/GameModuleAbi.h>
#include <Cue/IO/RelativePath.h>
#include <Cue/IO/Windows/WindowsFilesystem.h>
#include <Cue/Math/Transform.h>
#include <Cue/Package/Error.h>
#include <Cue/Package/Manifest.h>
#include <Cue/Package/RuntimeData.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/Renderer/RendererSchema.h>
#include <Cue/Runtime/Error.h>
#include <Cue/Runtime/RuntimeSchema.h>
#include <Cue/RuntimeHost/GameModuleQueryProvider.h>
#include <Cue/RuntimeHost/StaticRuntimePackage.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/SceneDocument.h>
#include <Cue/Schema/Registry.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef CUE_RUNTIME_BUILD_CONFIGURATION
#error CUE_RUNTIME_BUILD_CONFIGURATION must identify the RuntimeHost build configuration
#endif

#if !defined(CUE_RUNTIME_PACKAGE_DYNAMIC) && !defined(CUE_RUNTIME_PACKAGE_STATIC)
#error RuntimePackage.cpp requires a dynamic or static package loader selection
#endif

namespace
{
constexpr cue::EngineVersion k_engineVersion{1U, 0U, 0U};
#if defined(CUE_RUNTIME_PACKAGE_DYNAMIC)
constexpr std::size_t k_maximumGameModuleMetadataBytes = 64U * 1024U;
constexpr std::uint64_t k_maximumJsonInteger = 9007199254740991ULL;

/// @brief lowercase hexadecimal文字か判定する
[[nodiscard]] bool is_lower_hex(char a_value) noexcept
{
    return (a_value >= '0' && a_value <= '9') || (a_value >= 'a' && a_value <= 'f');
}

/// @brief lowercase canonical UUID Version 4文字列か判定する
[[nodiscard]] bool is_canonical_uuid_v4(std::string_view a_text) noexcept
{
    if (a_text.size() != 36U || a_text[8] != '-' || a_text[13] != '-' || a_text[18] != '-' || a_text[23] != '-' ||
        a_text[14] != '4' || (a_text[19] != '8' && a_text[19] != '9' && a_text[19] != 'a' && a_text[19] != 'b'))
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_text.size(); ++index)
    {
        if (index != 8U && index != 13U && index != 18U && index != 23U && !is_lower_hex(a_text[index]))
        {
            return false;
        }
    }
    return true;
}
#endif

/// @brief Filesystem PathをEngine内部契約のUTF-8表現へ変換する
[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &a_path)
{
    const std::u8string text = a_path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

/// @brief Absolute Windows PathをExtended-length形式へ変換する
#if defined(CUE_RUNTIME_PACKAGE_DYNAMIC)
[[nodiscard]] std::filesystem::path extended_windows_path(const std::filesystem::path &a_path)
{
    std::filesystem::path preferred = a_path;
    preferred.make_preferred();
    const std::wstring &native = preferred.native();
    if (native.starts_with(L"\\\\?\\"))
    {
        return preferred;
    }
    if (native.starts_with(L"\\\\"))
    {
        return std::filesystem::path(L"\\\\?\\UNC\\" + native.substr(2U));
    }
    return std::filesystem::path(L"\\\\?\\" + native);
}
#endif

/// @brief Runtime Package処理中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_package_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Unexpected exception escaped Runtime Package loading");
    std::abort();
}

/// @brief Runtime Package起動の失敗をPackage Domainへ分類する
[[nodiscard]] cue::Error package_error(const cue::AssertContext &a_assertContext, cue::package::PackageError a_code,
                                       std::string_view a_summary) noexcept
{
    return cue::package::make_package_error(a_assertContext, a_code, a_summary);
}

/// @brief Win32失敗をRuntime Package起動Errorへ変換する
[[nodiscard]] cue::Error windows_package_error(const cue::AssertContext &a_assertContext,
                                               cue::package::PackageError a_code, DWORD a_nativeCode,
                                               std::string_view a_summary) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Package", static_cast<std::int64_t>(a_code));
    cue::NativeError native = cue::NativeError::create(a_assertContext.fatal_handler(), "Win32", a_nativeCode);
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary, std::move(native));
}

/// @brief Win32 Handleを単一所有する
class UniqueHandle final
{
  public:
    /// @brief 無効Handleを構築する
    UniqueHandle() noexcept = default;
    /// @brief Native Handleの所有権を取得する
    explicit UniqueHandle(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    /// @brief Handleの複製を禁止する
    UniqueHandle(const UniqueHandle &) = delete;
    /// @brief Handleの複製代入を禁止する
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    /// @brief Handle所有権を移動する
    UniqueHandle(UniqueHandle &&a_other) noexcept : m_handle(std::exchange(a_other.m_handle, INVALID_HANDLE_VALUE))
    {
    }
    /// @brief 既存Handleを閉じて所有権を移動代入する
    UniqueHandle &operator=(UniqueHandle &&a_other) noexcept
    {
        if (this != &a_other)
        {
            reset();
            m_handle = std::exchange(a_other.m_handle, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    /// @brief 所有Handleを閉じる
    ~UniqueHandle() noexcept
    {
        reset();
    }
    /// @brief 有効Handleか返す
    [[nodiscard]] bool is_valid() const noexcept
    {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }
    /// @brief 借用Native Handleを返す
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }

  private:
    /// @brief 所有Handleがあれば閉じて無効化する
    void reset() noexcept
    {
        if (is_valid())
        {
            CloseHandle(m_handle);
        }
        m_handle = INVALID_HANDLE_VALUE;
    }

    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

/// @brief 事前Load済みDLLを依存元より後まで保持して逆順解放する
#if defined(CUE_RUNTIME_PACKAGE_DYNAMIC)
void unload_libraries(std::vector<HMODULE> &a_libraries) noexcept
{
    for (auto library = a_libraries.rbegin(); library != a_libraries.rend(); ++library)
    {
        if (*library != nullptr)
        {
            FreeLibrary(*library);
        }
    }
    a_libraries.clear();
}

/// @brief RuntimeHostが同時保持するGame Moduleと依存PEの宣言Sizeを全体読込前に制限する
[[nodiscard]] cue::Result<void> validate_runtime_pe_memory_contract(const cue::package::PackageManifest &a_manifest,
                                                                    const cue::AssertContext &a_assertContext) noexcept
{
    std::uint64_t totalBytes = 0U;
    for (const cue::package::PackageFileEntry &entry : a_manifest.files())
    {
        if (entry.role() != cue::package::PackageFileRole::GameModule &&
            entry.role() != cue::package::PackageFileRole::RuntimeDependency)
        {
            continue;
        }
        if (entry.byte_size() > cue::package::k_maximumRuntimePeImageBytes ||
            totalBytes > cue::package::k_maximumRuntimePeInventoryBytes - entry.byte_size())
        {
            return cue::Result<void>::failure(
                package_error(a_assertContext, cue::package::PackageError::PackageManifestResourceLimitExceeded,
                              "Runtime PE image inventory exceeds the RuntimeHost memory contract"));
        }
        totalBytes += entry.byte_size();
    }
    return cue::Result<void>::success();
}
#endif

/// @brief Canonical Runtime JSONを順序、重複、末尾Data込みでFail-closedに読むCursor
class JsonCursor final
{
  public:
    /// @brief Cursor寿命中だけ入力Byte列を借用する
    explicit JsonCursor(std::string_view a_input) noexcept : m_input(a_input)
    {
    }

    /// @brief 空白を除いた次Tokenが指定文字なら消費する
    [[nodiscard]] bool consume(char a_value) noexcept
    {
        skip_whitespace();
        if (m_offset >= m_input.size() || m_input[m_offset] != a_value)
        {
            return false;
        }
        ++m_offset;
        return true;
    }

    /// @brief 空白を除いた次Tokenが指定文字か返す
    [[nodiscard]] bool next_is(char a_value) noexcept
    {
        skip_whitespace();
        return m_offset < m_input.size() && m_input[m_offset] == a_value;
    }

    /// @brief Object Member名とColonを固定順で読む
    [[nodiscard]] bool member(std::string_view a_expected)
    {
        std::string name;
        return string(name) && name == a_expected && consume(':');
    }

    /// @brief JSON StringをASCII Escape検証付きで復号する
    [[nodiscard]] bool string(std::string &a_output)
    {
        skip_whitespace();
        if (m_offset >= m_input.size() || m_input[m_offset++] != '"')
        {
            return false;
        }
        a_output.clear();
        while (m_offset < m_input.size())
        {
            const unsigned char value = static_cast<unsigned char>(m_input[m_offset++]);
            if (value == '"')
            {
                return a_output.size() <= cue::package::k_maximumPackageManifestStringBytes;
            }
            if (value < 0x20U)
            {
                return false;
            }
            if (value != '\\')
            {
                a_output.push_back(static_cast<char>(value));
            }
            else
            {
                if (m_offset >= m_input.size())
                {
                    return false;
                }
                const char escaped = m_input[m_offset++];
                switch (escaped)
                {
                case '"':
                case '\\':
                case '/':
                    a_output.push_back(escaped);
                    break;
                case 'b':
                    a_output.push_back('\b');
                    break;
                case 'f':
                    a_output.push_back('\f');
                    break;
                case 'n':
                    a_output.push_back('\n');
                    break;
                case 'r':
                    a_output.push_back('\r');
                    break;
                case 't':
                    a_output.push_back('\t');
                    break;
                default:
                    return false;
                }
            }
            if (a_output.size() > cue::package::k_maximumPackageManifestStringBytes)
            {
                return false;
            }
        }
        return false;
    }

    /// @brief JSON unsigned整数を対象型へ完全変換する
    template <typename Value> [[nodiscard]] bool unsigned_number(Value &a_output) noexcept
    {
        skip_whitespace();
        const std::size_t begin = m_offset;
        while (m_offset < m_input.size() && m_input[m_offset] >= '0' && m_input[m_offset] <= '9')
        {
            ++m_offset;
        }
        if (begin == m_offset || (m_offset - begin > 1U && m_input[begin] == '0'))
        {
            return false;
        }
        const auto converted = std::from_chars(m_input.data() + begin, m_input.data() + m_offset, a_output);
        return converted.ec == std::errc{} && converted.ptr == m_input.data() + m_offset;
    }

    /// @brief JSON有限浮動小数を対象精度へ完全変換する
    template <typename Value> [[nodiscard]] bool floating(Value &a_output) noexcept
    {
        skip_whitespace();
        const std::size_t begin = m_offset;
        if (m_offset < m_input.size() && m_input[m_offset] == '-')
        {
            ++m_offset;
        }
        if (m_offset >= m_input.size())
        {
            return false;
        }
        if (m_input[m_offset] == '0')
        {
            ++m_offset;
            if (m_offset < m_input.size() && m_input[m_offset] >= '0' && m_input[m_offset] <= '9')
            {
                return false;
            }
        }
        else if (m_input[m_offset] >= '1' && m_input[m_offset] <= '9')
        {
            do
            {
                ++m_offset;
            } while (m_offset < m_input.size() && m_input[m_offset] >= '0' && m_input[m_offset] <= '9');
        }
        else
        {
            return false;
        }
        if (m_offset < m_input.size() && m_input[m_offset] == '.')
        {
            ++m_offset;
            const std::size_t fractionBegin = m_offset;
            while (m_offset < m_input.size() && m_input[m_offset] >= '0' && m_input[m_offset] <= '9')
            {
                ++m_offset;
            }
            if (fractionBegin == m_offset)
            {
                return false;
            }
        }
        if (m_offset < m_input.size() && (m_input[m_offset] == 'e' || m_input[m_offset] == 'E'))
        {
            ++m_offset;
            if (m_offset < m_input.size() && (m_input[m_offset] == '+' || m_input[m_offset] == '-'))
            {
                ++m_offset;
            }
            const std::size_t exponentBegin = m_offset;
            while (m_offset < m_input.size() && m_input[m_offset] >= '0' && m_input[m_offset] <= '9')
            {
                ++m_offset;
            }
            if (exponentBegin == m_offset)
            {
                return false;
            }
        }
        const auto converted =
            std::from_chars(m_input.data() + begin, m_input.data() + m_offset, a_output, std::chars_format::general);
        return converted.ec == std::errc{} && converted.ptr == m_input.data() + m_offset && std::isfinite(a_output);
    }

    /// @brief JSON Booleanを読む
    [[nodiscard]] bool boolean(bool &a_output) noexcept
    {
        skip_whitespace();
        if (m_input.substr(m_offset, 4U) == "true")
        {
            m_offset += 4U;
            a_output = true;
            return true;
        }
        if (m_input.substr(m_offset, 5U) == "false")
        {
            m_offset += 5U;
            a_output = false;
            return true;
        }
        return false;
    }

    /// @brief JSON nullを読む
    [[nodiscard]] bool null_value() noexcept
    {
        skip_whitespace();
        if (m_input.substr(m_offset, 4U) != "null")
        {
            return false;
        }
        m_offset += 4U;
        return true;
    }

    /// @brief 文書末尾まで空白以外がないか返す
    [[nodiscard]] bool finished() noexcept
    {
        skip_whitespace();
        return m_offset == m_input.size();
    }

  private:
    /// @brief JSONで許可されるASCII空白を読み飛ばす
    void skip_whitespace() noexcept
    {
        while (m_offset < m_input.size() && (m_input[m_offset] == ' ' || m_input[m_offset] == '\t' ||
                                             m_input[m_offset] == '\r' || m_input[m_offset] == '\n'))
        {
            ++m_offset;
        }
    }

    std::string_view m_input;
    std::size_t m_offset = 0U;
};

/// @brief canonical major.minor.patch文字列をEngine Versionへ変換する
[[nodiscard]] bool parse_engine_version(std::string_view a_text, cue::EngineVersion &a_output) noexcept
{
    const std::size_t first = a_text.find('.');
    const std::size_t second = first == std::string_view::npos ? first : a_text.find('.', first + 1U);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        a_text.find('.', second + 1U) != std::string_view::npos)
    {
        return false;
    }
    const auto parsePart = [](std::string_view a_part, std::uint32_t &a_value) noexcept
    {
        if (a_part.empty() || (a_part.size() > 1U && a_part.front() == '0'))
        {
            return false;
        }
        const auto converted = std::from_chars(a_part.data(), a_part.data() + a_part.size(), a_value);
        return converted.ec == std::errc{} && converted.ptr == a_part.data() + a_part.size();
    };
    return parsePart(a_text.substr(0U, first), a_output.major) &&
           parsePart(a_text.substr(first + 1U, second - first - 1U), a_output.minor) &&
           parsePart(a_text.substr(second + 1U), a_output.patch);
}

/// @brief RuntimeHost Build ConfigurationをManifest表現へ変換する
[[nodiscard]] constexpr cue::BuildConfiguration host_configuration() noexcept
{
#if CUE_RUNTIME_BUILD_CONFIGURATION == 1
    return cue::BuildConfiguration::Debug;
#elif CUE_RUNTIME_BUILD_CONFIGURATION == 2
    return cue::BuildConfiguration::Development;
#elif CUE_RUNTIME_BUILD_CONFIGURATION == 3
    return cue::BuildConfiguration::Release;
#else
#error Unsupported CUE_RUNTIME_BUILD_CONFIGURATION value
#endif
}

/// @brief Runtime Project Dataから起動に必要なIdentityとCompatibilityだけを所有する
struct RuntimeProjectInfo final
{
    std::string projectId;
    cue::EngineCompatibility compatibility;
    std::string startupSceneAssetId;
};

/// @brief Runtime Project Data v1をCanonical Member順とResource Limitへ検証する
[[nodiscard]] cue::Result<RuntimeProjectInfo> parse_runtime_project(std::string_view a_bytes,
                                                                    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        JsonCursor cursor(a_bytes);
        std::uint32_t schemaVersion = 0U;
        std::string minimumText;
        std::string maximumText;
        bool hasMaximum = false;
        RuntimeProjectInfo info{{}, {{}, std::nullopt}, {}};
        if (!cursor.consume('{') || !cursor.member("schemaVersion") || !cursor.unsigned_number(schemaVersion) ||
            schemaVersion != cue::package::k_runtimeProjectDataSchemaVersion || !cursor.consume(',') ||
            !cursor.member("projectId") || !cursor.string(info.projectId) || !cursor.consume(',') ||
            !cursor.member("engineCompatibility") || !cursor.consume('{') || !cursor.member("minimum") ||
            !cursor.string(minimumText) || !cursor.consume(',') || !cursor.member("maximumExclusive"))
        {
            return cue::Result<RuntimeProjectInfo>::failure(
                package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                              "Runtime Project Data members are invalid"));
        }
        if (cursor.next_is('"'))
        {
            hasMaximum = cursor.string(maximumText);
        }
        else if (!cursor.null_value())
        {
            return cue::Result<RuntimeProjectInfo>::failure(
                package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                              "Runtime Project maximum compatibility is invalid"));
        }
        if (!cursor.consume('}') || !cursor.consume(',') || !cursor.member("requiredCapabilities") ||
            !cursor.consume('[') || !cursor.consume(']') || !cursor.consume(',') ||
            !cursor.member("startupSceneAssetId") || !cursor.string(info.startupSceneAssetId) || !cursor.consume('}') ||
            !cursor.finished() || !parse_engine_version(minimumText, info.compatibility.minimum))
        {
            return cue::Result<RuntimeProjectInfo>::failure(
                package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                              "Runtime Project Data is not a supported canonical v1 document"));
        }
        if (hasMaximum)
        {
            cue::EngineVersion maximum{};
            if (!parse_engine_version(maximumText, maximum) || maximum <= info.compatibility.minimum)
            {
                return cue::Result<RuntimeProjectInfo>::failure(
                    package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                                  "Runtime Project compatibility range is invalid"));
            }
            info.compatibility.maximumExclusive = maximum;
        }
        if (k_engineVersion < info.compatibility.minimum || (info.compatibility.maximumExclusive.has_value() &&
                                                             k_engineVersion >= *info.compatibility.maximumExclusive))
        {
            return cue::Result<RuntimeProjectInfo>::failure(
                package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                              "Runtime Project is incompatible with this Engine version"));
        }
        return cue::Result<RuntimeProjectInfo>::success(std::move(info));
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}

/// @brief 固定要素数のfloat Arrayを読む
template <std::size_t Size>
[[nodiscard]] bool read_float_array(JsonCursor &a_cursor, std::array<float, Size> &a_values) noexcept
{
    if (!a_cursor.consume('['))
    {
        return false;
    }
    for (std::size_t index = 0U; index < Size; ++index)
    {
        if ((index > 0U && !a_cursor.consume(',')) || !a_cursor.floating(a_values[index]))
        {
            return false;
        }
    }
    return a_cursor.consume(']');
}

/// @brief Runtime Scene v2の一Componentを一時Registryで検証して所有Dataへ変換する
[[nodiscard]] cue::Result<cue::scene::SceneComponent> read_runtime_component(
    JsonCursor &a_cursor, const cue::schema::SchemaRegistry &a_registry,
    const cue::scene::ComponentValueSchemaRegistry &a_valueRegistry,
    const cue::renderer::RendererSchemaTypeIds &a_typeIds,
    const cue::AssertContext &a_assertContext) noexcept
{
    std::string instanceText;
    std::string typeText;
    std::uint32_t versionValue = 0U;
    if (!a_cursor.consume('{') || !a_cursor.member("instanceId") || !a_cursor.string(instanceText) ||
        !a_cursor.consume(',') || !a_cursor.member("typeId") || !a_cursor.string(typeText) ||
        !a_cursor.consume(',') || !a_cursor.member("schemaVersion") ||
        !a_cursor.unsigned_number(versionValue) || versionValue != 1U || !a_cursor.consume(',') ||
        !a_cursor.member("fields") || !a_cursor.consume('['))
    {
        return cue::Result<cue::scene::SceneComponent>::failure(package_error(
            a_assertContext, cue::package::PackageError::InvalidRuntimeData,
            "Runtime Scene v2 component identity or version is invalid"));
    }
    auto instanceId = cue::scene::ComponentInstanceId::parse(instanceText, a_assertContext);
    auto typeId = cue::schema::TypeId::parse(typeText, a_assertContext);
    auto version = cue::schema::SchemaVersion::create(versionValue, a_assertContext);
    if (!instanceId || !typeId || !version ||
        (*typeId.try_value() != a_typeIds.camera && *typeId.try_value() != a_typeIds.mesh))
    {
        return cue::Result<cue::scene::SceneComponent>::failure(package_error(
            a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
            "Runtime Scene v2 contains an unsupported component type"));
    }
    const bool isCamera = *typeId.try_value() == a_typeIds.camera;
    const std::size_t fieldCount = isCamera ? 4U : 1U;
    std::vector<cue::scene::KnownFieldData> fields;
    fields.reserve(fieldCount);
    for (std::size_t index = 0U; index < fieldCount; ++index)
    {
        std::uint32_t fieldNumber = 0U;
        if ((index > 0U && !a_cursor.consume(',')) || !a_cursor.consume('{') ||
            !a_cursor.member("fieldId") || !a_cursor.unsigned_number(fieldNumber) ||
            fieldNumber != index + 1U || !a_cursor.consume(',') || !a_cursor.member("value"))
        {
            return cue::Result<cue::scene::SceneComponent>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                "Runtime Scene v2 fields are missing or unordered"));
        }
        auto fieldId = cue::schema::FieldId::create(fieldNumber, a_assertContext);
        if (!fieldId)
        {
            return cue::Result<cue::scene::SceneComponent>::failure(std::move(*fieldId.try_error()));
        }
        std::optional<cue::scene::FieldValue> value;
        cue::scene::FieldValueKind kind = cue::scene::FieldValueKind::Boolean;
        if (isCamera && index == 0U)
        {
            bool parsed = false;
            if (a_cursor.boolean(parsed))
            {
                value.emplace(cue::scene::FieldValue::boolean(parsed));
            }
        }
        else if (isCamera)
        {
            double parsed = 0.0;
            kind = cue::scene::FieldValueKind::FloatingPoint;
            if (a_cursor.floating(parsed))
            {
                auto floating = cue::scene::FieldValue::floating_point(parsed, a_assertContext);
                if (floating)
                {
                    value.emplace(std::move(*floating.try_value()));
                }
            }
        }
        else
        {
            std::string parsed;
            kind = cue::scene::FieldValueKind::AssetReference;
            if (a_cursor.string(parsed))
            {
                auto asset = cue::scene::AssetReferenceValue::create(parsed, a_assertContext);
                if (asset)
                {
                    value.emplace(cue::scene::FieldValue::asset_reference(std::move(*asset.try_value())));
                }
            }
        }
        if (!value || !a_cursor.consume('}'))
        {
            return cue::Result<cue::scene::SceneComponent>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                "Runtime Scene v2 field value is invalid"));
        }
        auto field = cue::scene::create_known_field(*fieldId.try_value(), std::move(*value), kind,
                                                    a_assertContext);
        if (!field)
        {
            return cue::Result<cue::scene::SceneComponent>::failure(std::move(*field.try_error()));
        }
        fields.push_back(std::move(*field.try_value()));
    }
    if (!a_cursor.consume(']') || !a_cursor.consume('}'))
    {
        return cue::Result<cue::scene::SceneComponent>::failure(package_error(
            a_assertContext, cue::package::PackageError::InvalidRuntimeData,
            "Runtime Scene v2 component has unsupported fields or members"));
    }
    auto known = cue::scene::create_known_component(
        std::move(*instanceId.try_value()), std::move(*typeId.try_value()), std::move(*version.try_value()),
        std::move(fields), {}, a_registry, a_valueRegistry, a_assertContext);
    if (!known)
    {
        return cue::Result<cue::scene::SceneComponent>::failure(package_error(
            a_assertContext, cue::package::PackageError::InvalidRuntimeData,
            "Runtime Scene v2 component does not match its registered schema"));
    }
    cue::scene::SceneComponent component = cue::scene::SceneComponent::known(std::move(*known.try_value()));
    auto validated = cue::renderer::validate_runtime_scene_component(component, a_assertContext);
    if (!validated)
    {
        return cue::Result<cue::scene::SceneComponent>::failure(package_error(
            a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
            "Runtime Scene v2 component value is unsupported"));
    }
    return cue::Result<cue::scene::SceneComponent>::success(std::move(component));
}

/// @brief Runtime Scene v1／v2のCore ObjectをScene Snapshotへ復元する
[[nodiscard]] cue::Result<cue::scene::SceneSnapshot> parse_runtime_scene(
    std::string_view a_bytes, std::string_view a_expectedSceneId, const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        JsonCursor cursor(a_bytes);
        std::uint32_t schemaVersion = 0U;
        std::string sceneIdText;
        if (!cursor.consume('{') || !cursor.member("schemaVersion") || !cursor.unsigned_number(schemaVersion) ||
            (schemaVersion != cue::package::k_runtimeSceneDataSchemaVersion &&
             schemaVersion != cue::package::k_runtimeSceneDataWithRendererSchemaVersion) || !cursor.consume(',') ||
            !cursor.member("sceneAssetId") || !cursor.string(sceneIdText) || sceneIdText != a_expectedSceneId ||
            !cursor.consume(',') || !cursor.member("objects") || !cursor.consume('['))
        {
            return cue::Result<cue::scene::SceneSnapshot>::failure(
                package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                              "Runtime Scene identity or schema is invalid"));
        }
        const bool hasRendererData = schemaVersion == cue::package::k_runtimeSceneDataWithRendererSchemaVersion;
        cue::schema::SchemaRegistryIdentitySource identitySource;
        std::unique_ptr<cue::schema::SchemaRegistry> registry;
        std::optional<cue::scene::ComponentValueSchemaRegistry> valueRegistry;
        auto rendererIds = cue::renderer::make_renderer_schema_type_ids(a_assertContext);
        if (!rendererIds)
        {
            return cue::Result<cue::scene::SceneSnapshot>::failure(std::move(*rendererIds.try_error()));
        }
        if (hasRendererData)
        {
            cue::schema::SchemaRegistryBuilder builder(identitySource, a_assertContext);
            auto addedCore = cue::runtime::add_runtime_schema_types(builder, a_assertContext);
            if (!addedCore)
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(std::move(*addedCore.try_error()));
            }
            auto addedRenderer = cue::renderer::add_renderer_schema_types(builder, a_assertContext);
            if (!addedRenderer)
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(std::move(*addedRenderer.try_error()));
            }
            auto sealed = builder.seal();
            if (!sealed)
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(std::move(*sealed.try_error()));
            }
            registry = std::move(*sealed.try_value());
            auto schemas = cue::renderer::make_renderer_value_schemas(*registry, a_assertContext);
            if (!schemas)
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(std::move(*schemas.try_error()));
            }
            auto values = cue::scene::ComponentValueSchemaRegistry::create(
                std::move(*schemas.try_value()), *registry, a_assertContext);
            if (!values)
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(std::move(*values.try_error()));
            }
            valueRegistry.emplace(std::move(*values.try_value()));
        }
        std::vector<cue::scene::RuntimeSceneObjectData> objects;
        std::set<cue::scene::ComponentInstanceId> componentIds;
        while (!cursor.next_is(']'))
        {
            if (!objects.empty() && !cursor.consume(','))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(
                    package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                                  "Runtime Scene object separator is invalid"));
            }
            if (objects.size() >= (hasRendererData ? 4096U : cue::scene::k_maximumRuntimeSceneObjectCount))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(
                    package_error(a_assertContext, cue::package::PackageError::RuntimeDataResourceLimitExceeded,
                                  "Runtime Scene object count exceeds the supported limit"));
            }
            std::string objectIdText;
            std::string parentIdText;
            bool hasParent = false;
            bool isActive = false;
            std::array<float, 3U> translation{};
            std::array<float, 4U> rotation{};
            std::array<float, 3U> scale{};
            if (!cursor.consume('{') || !cursor.member("objectId") || !cursor.string(objectIdText) ||
                !cursor.consume(',') || !cursor.member("parentObjectId"))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(
                    package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                                  "Runtime Scene object identity is invalid"));
            }
            if (cursor.next_is('"'))
            {
                hasParent = cursor.string(parentIdText);
            }
            else if (!cursor.null_value())
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(
                    package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                                  "Runtime Scene parent identity is invalid"));
            }
            if (!cursor.consume(',') || !cursor.member("active") || !cursor.boolean(isActive) || !cursor.consume(',') ||
                !cursor.member("transform") || !cursor.consume('{') || !cursor.member("translation") ||
                !read_float_array(cursor, translation) || !cursor.consume(',') || !cursor.member("rotation") ||
                !read_float_array(cursor, rotation) || !cursor.consume(',') || !cursor.member("scale") ||
                !read_float_array(cursor, scale) || !cursor.consume('}') || !cursor.consume(',') ||
                !cursor.member("components") || !cursor.consume('['))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(
                    package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                                  "Runtime Scene object values are invalid"));
            }
            std::vector<cue::scene::SceneComponent> components;
            if (hasRendererData)
            {
                while (!cursor.next_is(']'))
                {
                    if (components.size() >= 2U || (!components.empty() && !cursor.consume(',')))
                    {
                        return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                            a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
                            "Runtime Scene v2 component count or separator is invalid"));
                    }
                    auto component = read_runtime_component(cursor, *registry, *valueRegistry, *rendererIds.try_value(),
                                                            a_assertContext);
                    if (!component)
                    {
                        return cue::Result<cue::scene::SceneSnapshot>::failure(std::move(*component.try_error()));
                    }
                    if (!componentIds.insert(component.try_value()->instance_id()).second)
                    {
                        return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                            a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                            "Runtime Scene v2 component identities must be unique across the Scene"));
                    }
                    if (!components.empty() &&
                        (!(components.back().instance_id() < component.try_value()->instance_id()) ||
                         components.back().try_known()->type_id() == component.try_value()->try_known()->type_id()))
                    {
                        return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                            a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                            "Runtime Scene v2 component order or type uniqueness is invalid"));
                    }
                    components.push_back(std::move(*component.try_value()));
                }
            }
            if (!cursor.consume(']'))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(
                    package_error(a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
                                  "Runtime Scene has unsupported Component Data"));
            }
            if (!cursor.consume('}'))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(
                    package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                                  "Runtime Scene object has unsupported members"));
            }
            auto objectId = cue::scene::ObjectId::parse(objectIdText, a_assertContext);
            auto parentId = hasParent ? cue::scene::ObjectId::parse(parentIdText, a_assertContext)
                                      : cue::Result<cue::scene::ObjectId>::failure(package_error(
                                            a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                                            "Runtime Scene object has no parent"));
            auto tolerance = cue::math::Tolerance::create(a_assertContext.fatal_handler(), 0.00001F, 0.00001F);
            const cue::math::Quaternion parsedRotation{rotation[0], rotation[1], rotation[2], rotation[3]};
            if (tolerance && !cue::math::is_unit_rotation(parsedRotation, *tolerance.try_value()))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(
                    package_error(a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
                                  "Runtime Scene rotation is not a supported unit Quaternion"));
            }
            auto transform = tolerance ? cue::math::Transform::create(a_assertContext.fatal_handler(),
                                                                      {translation[0], translation[1], translation[2]},
                                                                      parsedRotation, {scale[0], scale[1], scale[2]},
                                                                      *tolerance.try_value())
                                       : cue::Result<cue::math::Transform>::failure(std::move(*tolerance.try_error()));
            if (!objectId || (hasParent && !parentId) || !transform)
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(
                    package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                                  "Runtime Scene contains an invalid Object identity or Transform"));
            }
            if (!objects.empty() && !(objects.back().id < *objectId.try_value()))
            {
                return cue::Result<cue::scene::SceneSnapshot>::failure(
                    package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                                  "Runtime Scene object order is not canonical"));
            }
            std::optional<cue::scene::ObjectId> parsedParent;
            if (hasParent)
            {
                parsedParent.emplace(std::move(*parentId.try_value()));
            }
            objects.push_back({std::move(*objectId.try_value()), std::move(parsedParent), isActive,
                               std::move(*transform.try_value()), std::move(components)});
        }
        if (!cursor.consume(']') || !cursor.consume('}') || !cursor.finished())
        {
            return cue::Result<cue::scene::SceneSnapshot>::failure(
                package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                              "Runtime Scene has trailing or unsupported data"));
        }
        auto sceneId = cue::scene::SceneAssetId::parse(sceneIdText, a_assertContext);
        if (!sceneId)
        {
            return cue::Result<cue::scene::SceneSnapshot>::failure(package_error(
                a_assertContext, cue::package::PackageError::InvalidRuntimeData, "Runtime Scene identity is invalid"));
        }
        auto snapshot = cue::scene::create_runtime_scene_snapshot(std::move(*sceneId.try_value()), std::move(objects),
                                                                  a_assertContext);
        if (!snapshot)
        {
            return cue::Result<cue::scene::SceneSnapshot>::failure(
                package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                              "Runtime Scene object set or hierarchy is invalid"));
        }
        return snapshot;
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}

/// @brief Parse済みRuntime DataがPublisherのCanonical Byte列と完全一致するか検証する
[[nodiscard]] cue::Result<void> validate_canonical_runtime_data(const RuntimeProjectInfo &a_project,
                                                                const cue::scene::SceneSnapshot &a_scene,
                                                                std::string_view a_projectBytes,
                                                                std::string_view a_sceneBytes,
                                                                const cue::AssertContext &a_assertContext) noexcept
{
    auto projectId = cue::ProjectId::parse(a_project.projectId, a_assertContext);
    auto descriptor = projectId ? cue::create_blank_project_descriptor(*projectId.try_value(), "Runtime Package",
                                                                       a_project.compatibility,
                                                                       a_project.startupSceneAssetId, a_assertContext)
                                : cue::Result<cue::ProjectDescriptor>::failure(std::move(*projectId.try_error()));
    auto publication =
        descriptor
            ? cue::package::publish_minimal_runtime_data(*descriptor.try_value(), a_scene, a_assertContext)
            : cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(std::move(*descriptor.try_error()));
    if (!publication)
    {
        return cue::Result<void>::failure(
            package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                          "Runtime Data could not be reproduced by the canonical Publisher"));
    }
    if (publication.try_value()->project_data().bytes() != a_projectBytes)
    {
        return cue::Result<void>::failure(
            package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                          "Runtime Project Data is not the canonical Publisher representation"));
    }
    if (publication.try_value()->startup_scene_data().bytes() != a_sceneBytes)
    {
        return cue::Result<void>::failure(
            package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                          "Runtime Scene Data is not the canonical Publisher representation"));
    }
    return cue::Result<void>::success();
}

/// @brief Game Module MetadataのEngine互換Rangeを所有する
#if defined(CUE_RUNTIME_PACKAGE_DYNAMIC)
struct ModuleMetadataCompatibility final
{
    std::string minimum;
    std::string maximumExclusive;
    bool hasMaximum = false;
};

/// @brief Game Module MetadataのMSVC Toolset Identityを所有する
struct ModuleMetadataToolset final
{
    std::uint64_t compilerVersion = 0U;
    std::uint64_t fullVersion = 0U;
    std::uint64_t build = 0U;
};

/// @brief Game Module Metadata v1の全必須Memberを所有する
struct ModuleMetadataInfo final
{
    std::uint32_t schemaVersion = 0U;
    std::string artifactId;
    std::string projectId;
    ModuleMetadataCompatibility compatibility;
    std::uint32_t abiVersion = 0U;
    std::string configuration;
    std::string architecture;
    std::string compilerFamily;
    ModuleMetadataToolset toolset;
    std::string runtimeLibrary;
    std::uint32_t iteratorDebugLevel = 0U;
    std::string moduleFile;
    std::string entrySymbol;
};

/// @brief Metadata Engine Compatibility Objectを順序非依存かつ未知・重複拒否で読む
[[nodiscard]] bool read_metadata_compatibility(JsonCursor &a_cursor, ModuleMetadataCompatibility &a_compatibility)
{
    constexpr std::uint32_t k_minimum = 1U << 0U;
    constexpr std::uint32_t k_maximumExclusive = 1U << 1U;
    constexpr std::uint32_t k_required = k_minimum | k_maximumExclusive;
    if (!a_cursor.consume('{'))
    {
        return false;
    }
    std::uint32_t seen = 0U;
    bool first = true;
    while (!a_cursor.next_is('}'))
    {
        if ((!first && !a_cursor.consume(',')))
        {
            return false;
        }
        first = false;
        std::string name;
        if (!a_cursor.string(name) || !a_cursor.consume(':'))
        {
            return false;
        }
        if (name == "minimum")
        {
            if ((seen & k_minimum) != 0U || !a_cursor.string(a_compatibility.minimum))
            {
                return false;
            }
            seen |= k_minimum;
        }
        else if (name == "maximumExclusive")
        {
            if ((seen & k_maximumExclusive) != 0U)
            {
                return false;
            }
            if (a_cursor.next_is('"'))
            {
                if (!a_cursor.string(a_compatibility.maximumExclusive))
                {
                    return false;
                }
                a_compatibility.hasMaximum = true;
            }
            else if (!a_cursor.null_value())
            {
                return false;
            }
            seen |= k_maximumExclusive;
        }
        else
        {
            return false;
        }
    }
    return a_cursor.consume('}') && seen == k_required;
}

/// @brief Metadata MSVC Toolset Objectを順序非依存かつ未知・重複拒否で読む
[[nodiscard]] bool read_metadata_toolset(JsonCursor &a_cursor, ModuleMetadataToolset &a_toolset)
{
    constexpr std::uint32_t k_compilerVersion = 1U << 0U;
    constexpr std::uint32_t k_fullVersion = 1U << 1U;
    constexpr std::uint32_t k_build = 1U << 2U;
    constexpr std::uint32_t k_required = k_compilerVersion | k_fullVersion | k_build;
    if (!a_cursor.consume('{'))
    {
        return false;
    }
    std::uint32_t seen = 0U;
    bool first = true;
    while (!a_cursor.next_is('}'))
    {
        if (!first && !a_cursor.consume(','))
        {
            return false;
        }
        first = false;
        std::string name;
        if (!a_cursor.string(name) || !a_cursor.consume(':'))
        {
            return false;
        }
        if (name == "compilerVersion")
        {
            if ((seen & k_compilerVersion) != 0U || !a_cursor.unsigned_number(a_toolset.compilerVersion))
            {
                return false;
            }
            seen |= k_compilerVersion;
        }
        else if (name == "fullVersion")
        {
            if ((seen & k_fullVersion) != 0U || !a_cursor.unsigned_number(a_toolset.fullVersion))
            {
                return false;
            }
            seen |= k_fullVersion;
        }
        else if (name == "build")
        {
            if ((seen & k_build) != 0U || !a_cursor.unsigned_number(a_toolset.build))
            {
                return false;
            }
            seen |= k_build;
        }
        else
        {
            return false;
        }
    }
    return a_cursor.consume('}') && seen == k_required;
}

/// @brief Metadata v1 Top-level Objectを順序非依存かつ未知・重複拒否で読む
[[nodiscard]] bool read_module_metadata(JsonCursor &a_cursor, ModuleMetadataInfo &a_info)
{
    constexpr std::uint32_t k_schemaVersion = 1U << 0U;
    constexpr std::uint32_t k_artifactId = 1U << 1U;
    constexpr std::uint32_t k_projectId = 1U << 2U;
    constexpr std::uint32_t k_engineCompatibility = 1U << 3U;
    constexpr std::uint32_t k_abiVersion = 1U << 4U;
    constexpr std::uint32_t k_configuration = 1U << 5U;
    constexpr std::uint32_t k_architecture = 1U << 6U;
    constexpr std::uint32_t k_compilerFamily = 1U << 7U;
    constexpr std::uint32_t k_msvcToolset = 1U << 8U;
    constexpr std::uint32_t k_runtimeLibrary = 1U << 9U;
    constexpr std::uint32_t k_iteratorDebugLevel = 1U << 10U;
    constexpr std::uint32_t k_moduleFile = 1U << 11U;
    constexpr std::uint32_t k_entrySymbol = 1U << 12U;
    constexpr std::uint32_t k_required = (1U << 13U) - 1U;
    if (!a_cursor.consume('{'))
    {
        return false;
    }
    std::uint32_t seen = 0U;
    bool first = true;
    while (!a_cursor.next_is('}'))
    {
        if (!first && !a_cursor.consume(','))
        {
            return false;
        }
        first = false;
        std::string name;
        if (!a_cursor.string(name) || !a_cursor.consume(':'))
        {
            return false;
        }
        std::uint32_t member = 0U;
        bool read = false;
        if (name == "schemaVersion")
        {
            member = k_schemaVersion;
            read = a_cursor.unsigned_number(a_info.schemaVersion);
        }
        else if (name == "artifactId")
        {
            member = k_artifactId;
            read = a_cursor.string(a_info.artifactId);
        }
        else if (name == "projectId")
        {
            member = k_projectId;
            read = a_cursor.string(a_info.projectId);
        }
        else if (name == "engineCompatibility")
        {
            member = k_engineCompatibility;
            read = read_metadata_compatibility(a_cursor, a_info.compatibility);
        }
        else if (name == "abiVersion")
        {
            member = k_abiVersion;
            read = a_cursor.unsigned_number(a_info.abiVersion);
        }
        else if (name == "configuration")
        {
            member = k_configuration;
            read = a_cursor.string(a_info.configuration);
        }
        else if (name == "architecture")
        {
            member = k_architecture;
            read = a_cursor.string(a_info.architecture);
        }
        else if (name == "compilerFamily")
        {
            member = k_compilerFamily;
            read = a_cursor.string(a_info.compilerFamily);
        }
        else if (name == "msvcToolset")
        {
            member = k_msvcToolset;
            read = read_metadata_toolset(a_cursor, a_info.toolset);
        }
        else if (name == "runtimeLibrary")
        {
            member = k_runtimeLibrary;
            read = a_cursor.string(a_info.runtimeLibrary);
        }
        else if (name == "iteratorDebugLevel")
        {
            member = k_iteratorDebugLevel;
            read = a_cursor.unsigned_number(a_info.iteratorDebugLevel);
        }
        else if (name == "moduleFile")
        {
            member = k_moduleFile;
            read = a_cursor.string(a_info.moduleFile);
        }
        else if (name == "entrySymbol")
        {
            member = k_entrySymbol;
            read = a_cursor.string(a_info.entrySymbol);
        }
        if (member == 0U || (seen & member) != 0U || !read)
        {
            return false;
        }
        seen |= member;
    }
    return a_cursor.consume('}') && a_cursor.finished() && seen == k_required;
}

/// @brief Metadata v1のPackage起動互換性を検証する
[[nodiscard]] cue::Result<void> validate_module_metadata(std::string_view a_bytes,
                                                         const cue::package::PackageManifest &a_manifest,
                                                         const RuntimeProjectInfo &a_project,
                                                         const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        JsonCursor cursor(a_bytes);
        ModuleMetadataInfo info;
        if (!read_module_metadata(cursor, info) || info.toolset.compilerVersion > k_maximumJsonInteger ||
            info.toolset.fullVersion > k_maximumJsonInteger || info.toolset.build > k_maximumJsonInteger)
        {
            return cue::Result<void>::failure(package_error(a_assertContext,
                                                            cue::package::PackageError::InvalidRuntimeData,
                                                            "Game Module Metadata body is invalid"));
        }
        if (info.schemaVersion != 1U || !is_canonical_uuid_v4(info.artifactId) || !is_canonical_uuid_v4(info.projectId))
        {
            return cue::Result<void>::failure(package_error(a_assertContext,
                                                            cue::package::PackageError::InvalidRuntimeData,
                                                            "Game Module Metadata header is invalid"));
        }
        cue::EngineVersion minimum{};
        cue::EngineVersion maximum{};
        const bool compatibilityMatches =
            parse_engine_version(info.compatibility.minimum, minimum) && minimum == a_project.compatibility.minimum &&
            (info.compatibility.hasMaximum == a_project.compatibility.maximumExclusive.has_value()) &&
            (!info.compatibility.hasMaximum || (parse_engine_version(info.compatibility.maximumExclusive, maximum) &&
                                                maximum == *a_project.compatibility.maximumExclusive));
        const std::string_view expectedConfiguration =
            a_manifest.configuration() == cue::BuildConfiguration::Debug
                ? "Debug"
                : (a_manifest.configuration() == cue::BuildConfiguration::Development ? "Development" : "Release");
        const bool debug = a_manifest.configuration() == cue::BuildConfiguration::Debug;
        // _MSC_FULL_VERと_MSC_BUILDはProvenanceとして保持し、同一_MSC_VER内のServicing更新は許容する。
        if (info.projectId != a_manifest.project_id() || info.projectId != a_project.projectId ||
            !compatibilityMatches || info.abiVersion != CUE_GAME_MODULE_ABI_VERSION_1 ||
            info.configuration != expectedConfiguration || info.architecture != "x64" ||
            info.compilerFamily != "msvc" || info.toolset.compilerVersion != _MSC_VER ||
            info.runtimeLibrary != (debug ? "DebugDll" : "Dll") || info.iteratorDebugLevel != (debug ? 2U : 0U) ||
            info.moduleFile != "CueGameModule.dll" || info.entrySymbol != "cue_game_module_query")
        {
            return cue::Result<void>::failure(package_error(a_assertContext,
                                                            cue::package::PackageError::InvalidRuntimeData,
                                                            "Game Module Metadata is incompatible with the Package"));
        }
        return cue::Result<void>::success();
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}
#endif

/// @brief Executable Moduleの完全Pathを切捨てなしで取得する
[[nodiscard]] cue::Result<std::filesystem::path> executable_path(const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::vector<wchar_t> buffer(512U, L'\0');
        for (;;)
        {
            SetLastError(ERROR_SUCCESS);
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0U)
            {
                return cue::Result<std::filesystem::path>::failure(
                    windows_package_error(a_assertContext, cue::package::PackageError::InvalidPackagePath,
                                          GetLastError(), "RuntimeHost executable path could not be resolved"));
            }
            if (length < buffer.size() - 1U)
            {
                return cue::Result<std::filesystem::path>::success(
                    std::filesystem::path(std::wstring_view(buffer.data(), length)));
            }
            if (buffer.size() >= 32768U)
            {
                return cue::Result<std::filesystem::path>::failure(
                    package_error(a_assertContext, cue::package::PackageError::InvalidPackagePath,
                                  "RuntimeHost executable path exceeds the Windows limit"));
            }
            buffer.resize(std::min<std::size_t>(buffer.size() * 2U, 32768U));
        }
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}

/// @brief FilesystemRootからManifestに列挙された一Fileだけを読んでByte Identityを再検証する
[[nodiscard]] cue::Result<std::vector<std::byte>> read_manifest_file(cue::FilesystemRoot &a_filesystem,
                                                                     const cue::package::PackageFileEntry &a_entry,
                                                                     const cue::AssertContext &a_assertContext) noexcept
{
    auto path = cue::RelativePath::parse(a_entry.relative_path(), a_assertContext);
    if (!path)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*path.try_error()));
    }
    auto bytes = a_filesystem.read_file(*path.try_value(), static_cast<std::size_t>(a_entry.byte_size()));
    if (!bytes)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*bytes.try_error()));
    }
    auto verified = cue::package::verify_package_file_bytes(a_entry, *bytes.try_value(), a_assertContext);
    if (!verified)
    {
        return cue::Result<std::vector<std::byte>>::failure(std::move(*verified.try_error()));
    }
    return bytes;
}

/// @brief Manifest内の必須Role一件を返す
[[nodiscard]] const cue::package::PackageFileEntry *find_role(const cue::package::PackageManifest &a_manifest,
                                                              cue::package::PackageFileRole a_role) noexcept
{
    const auto found = std::find_if(a_manifest.files().begin(), a_manifest.files().end(),
                                    [a_role](const cue::package::PackageFileEntry &a_entry) noexcept
                                    { return a_entry.role() == a_role; });
    return found == a_manifest.files().end() ? nullptr : &*found;
}

/// @brief byte列をUTF-8検証済みParser入力Viewへ変換する
[[nodiscard]] std::string_view text_view(const std::vector<std::byte> &a_bytes) noexcept
{
    return {reinterpret_cast<const char *>(a_bytes.data()), a_bytes.size()};
}

/// @brief Package相対Pathから最後のFile Nameだけを借用する
#if defined(CUE_RUNTIME_PACKAGE_DYNAMIC)
[[nodiscard]] std::string_view package_file_name(std::string_view a_relativePath) noexcept
{
    const std::size_t separator = a_relativePath.find_last_of('/');
    return separator == std::string_view::npos ? a_relativePath : a_relativePath.substr(separator + 1U);
}

/// @brief Win32 DLLと関連GuardをModule接続より長く所有するCode Lease
class WindowsGameModuleCodeLifetime final : public cue::runtime_host::GameModuleCodeLifetime
{
  public:
    WindowsGameModuleCodeLifetime(HMODULE a_library, std::vector<HMODULE> a_runtimeLibraries, UniqueHandle a_rootGuard,
                                  UniqueHandle a_gameGuard, UniqueHandle a_runtimeGuard, UniqueHandle a_moduleGuard,
                                  std::vector<UniqueHandle> a_runtimeDependencyGuards) noexcept
        : m_library(a_library), m_runtimeLibraries(std::move(a_runtimeLibraries)), m_rootGuard(std::move(a_rootGuard)),
          m_gameGuard(std::move(a_gameGuard)), m_runtimeGuard(std::move(a_runtimeGuard)),
          m_moduleGuard(std::move(a_moduleGuard)), m_runtimeDependencyGuards(std::move(a_runtimeDependencyGuards))
    {
    }

    ~WindowsGameModuleCodeLifetime() noexcept override
    {
        if (m_library != nullptr)
        {
            FreeLibrary(m_library);
        }
        unload_libraries(m_runtimeLibraries);
    }

    [[nodiscard]] HMODULE library() const noexcept
    {
        return m_library;
    }

  private:
    HMODULE m_library;
    std::vector<HMODULE> m_runtimeLibraries;
    UniqueHandle m_rootGuard;
    UniqueHandle m_gameGuard;
    UniqueHandle m_runtimeGuard;
    UniqueHandle m_moduleGuard;
    std::vector<UniqueHandle> m_runtimeDependencyGuards;
};

/// @brief 固定済みPackage DLLからQuery EntryとCode Leaseを解決するDynamic Provider
class WindowsDynamicGameModuleQueryProvider final : public cue::runtime_host::GameModuleQueryProvider
{
  public:
    explicit WindowsDynamicGameModuleQueryProvider(
        std::shared_ptr<WindowsGameModuleCodeLifetime> a_codeLifetime) noexcept
        : m_codeLifetime(std::move(a_codeLifetime))
    {
    }

    [[nodiscard]] cue::Result<cue::runtime_host::ResolvedGameModuleQuery> resolve(
        const cue::AssertContext &a_assertContext) noexcept override
    {
        const FARPROC queryAddress = GetProcAddress(m_codeLifetime->library(), "cue_game_module_query");
        if (queryAddress == nullptr)
        {
            return cue::Result<cue::runtime_host::ResolvedGameModuleQuery>::failure(
                windows_package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData,
                                      ERROR_PROC_NOT_FOUND, "Game Module entry symbol is missing"));
        }
        const cue::runtime_host::GameModuleQueryFunction query =
            std::bit_cast<cue::runtime_host::GameModuleQueryFunction>(queryAddress);
        return cue::Result<cue::runtime_host::ResolvedGameModuleQuery>::success({query, m_codeLifetime});
    }

    [[nodiscard]] cue::Error make_query_contract_error(const cue::AssertContext &a_assertContext,
                                                       std::string_view a_summary) const noexcept override
    {
        return package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData, a_summary);
    }

  private:
    std::shared_ptr<WindowsGameModuleCodeLifetime> m_codeLifetime;
};

/// @brief DirectoryをReparse追跡なし、Delete共有なしで固定する
[[nodiscard]] cue::Result<UniqueHandle> guard_directory(const std::filesystem::path &a_path,
                                                        const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path extendedPath = extended_windows_path(a_path);
    UniqueHandle handle(CreateFileW(extendedPath.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr));
    if (!handle.is_valid())
    {
        return cue::Result<UniqueHandle>::failure(
            windows_package_error(a_assertContext, cue::package::PackageError::InvalidPackagePath, GetLastError(),
                                  "Runtime Package directory could not be fixed"));
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        return cue::Result<UniqueHandle>::failure(
            package_error(a_assertContext, cue::package::PackageError::InvalidPackagePath,
                          "Runtime Package directory is not a regular non-reparse directory"));
    }
    return cue::Result<UniqueHandle>::success(std::move(handle));
}

/// @brief ASCII英字だけをlowercaseへ変換してWindows File名比較Keyを返す
[[nodiscard]] std::wstring ascii_case_key(std::wstring_view a_value)
{
    std::wstring key(a_value);
    for (wchar_t &character : key)
    {
        if (character >= L'A' && character <= L'Z')
        {
            character = static_cast<wchar_t>(character + (L'a' - L'A'));
        }
    }
    return key;
}

/// @brief Runtime Directoryの通常File集合がManifestの依存Entryと完全一致するか検証する
[[nodiscard]] cue::Result<bool> validate_runtime_dependency_inventory(
    const std::filesystem::path &a_root, const cue::package::PackageManifest &a_manifest,
    const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        std::vector<std::wstring> expected;
        for (const cue::package::PackageFileEntry &entry : a_manifest.files())
        {
            if (entry.role() != cue::package::PackageFileRole::RuntimeDependency)
            {
                continue;
            }
            constexpr std::string_view prefix = "Runtime/";
            const std::string_view path = entry.relative_path();
            if (!path.starts_with(prefix) || path.size() == prefix.size())
            {
                return cue::Result<bool>::failure(
                    package_error(a_assertContext, cue::package::PackageError::InvalidPackageManifest,
                                  "Runtime dependency path is outside the direct Runtime directory"));
            }
            const std::filesystem::path name =
                cue::runtime_host::detail::filesystem_path_from_utf8(path.substr(prefix.size()));
            expected.push_back(ascii_case_key(name.native()));
        }
        std::sort(expected.begin(), expected.end());

        const std::filesystem::path runtimePath = a_root / "Runtime";
        const std::filesystem::path extendedRuntimePath = extended_windows_path(runtimePath);
        const DWORD runtimeAttributes = GetFileAttributesW(extendedRuntimePath.c_str());
        if (runtimeAttributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD code = GetLastError();
            if ((code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) && expected.empty())
            {
                return cue::Result<bool>::success(false);
            }
            return cue::Result<bool>::failure(
                windows_package_error(a_assertContext, cue::package::PackageError::PackageFileMissing, code,
                                      "Runtime dependency directory could not be inspected"));
        }
        if ((runtimeAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
            (runtimeAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        {
            return cue::Result<bool>::failure(
                package_error(a_assertContext, cue::package::PackageError::InvalidPackagePath,
                              "Runtime dependency path is not a regular non-reparse directory"));
        }

        std::vector<std::wstring> actual;
        std::error_code iteratorError;
        std::filesystem::directory_iterator iterator(extendedRuntimePath, iteratorError);
        const std::filesystem::directory_iterator end;
        while (!iteratorError && iterator != end)
        {
            const std::filesystem::path extendedEntryPath = extended_windows_path(iterator->path());
            const DWORD attributes = GetFileAttributesW(extendedEntryPath.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            {
                return cue::Result<bool>::failure(
                    package_error(a_assertContext, cue::package::PackageError::PackageFileMismatch,
                                  "Runtime directory contains a non-regular or reparse entry"));
            }
            actual.push_back(ascii_case_key(iterator->path().filename().wstring()));
            iterator.increment(iteratorError);
        }
        if (iteratorError)
        {
            return cue::Result<bool>::failure(package_error(a_assertContext,
                                                            cue::package::PackageError::InvalidPackagePath,
                                                            "Runtime dependency directory could not be enumerated"));
        }
        std::sort(actual.begin(), actual.end());
        if (actual != expected)
        {
            return cue::Result<bool>::failure(package_error(a_assertContext,
                                                            cue::package::PackageError::PackageFileMismatch,
                                                            "Runtime directory inventory differs from the Manifest"));
        }
        return cue::Result<bool>::success(!expected.empty());
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}

/// @brief Load対象Package FileをWrite／Delete共有なしで固定する
[[nodiscard]] cue::Result<UniqueHandle> guard_package_file(const std::filesystem::path &a_path,
                                                           const cue::AssertContext &a_assertContext) noexcept
{
    const std::filesystem::path extendedPath = extended_windows_path(a_path);
    UniqueHandle handle(CreateFileW(extendedPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.is_valid())
    {
        return cue::Result<UniqueHandle>::failure(
            windows_package_error(a_assertContext, cue::package::PackageError::PackageFileMissing, GetLastError(),
                                  "Runtime Package load target could not be fixed"));
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) == FALSE ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U || GetFileType(handle.get()) != FILE_TYPE_DISK)
    {
        return cue::Result<UniqueHandle>::failure(
            package_error(a_assertContext, cue::package::PackageError::InvalidPackagePath,
                          "Runtime Package load target is not a regular non-reparse file"));
    }
    return cue::Result<UniqueHandle>::success(std::move(handle));
}

/// @brief Load済みModuleが事前に固定したPackage Fileと同一実体か検証する
[[nodiscard]] cue::Result<void> validate_loaded_module_identity(HMODULE a_library, HANDLE a_guardedFile,
                                                                const cue::AssertContext &a_assertContext) noexcept
{
    const cue::runtime_host::detail::RuntimeModuleIdentityResult identity =
        cue::runtime_host::detail::inspect_loaded_module_identity(a_library, a_guardedFile);
    if (identity.status == cue::runtime_host::detail::RuntimeModuleIdentityStatus::Match)
    {
        return cue::Result<void>::success();
    }
    if (identity.status == cue::runtime_host::detail::RuntimeModuleIdentityStatus::Mismatch)
    {
        return cue::Result<void>::failure(
            package_error(a_assertContext, cue::package::PackageError::PackageFileMismatch,
                          "Loaded Runtime image identity differs from the guarded Package file"));
    }
    return cue::Result<void>::failure(
        windows_package_error(a_assertContext, cue::package::PackageError::InvalidRuntimeData, identity.nativeError,
                              "Loaded Runtime image file identity could not be queried"));
}
#endif
} // namespace

namespace cue::runtime_host
{
Result<scene::SceneSnapshot> parse_runtime_scene_data(
    std::string_view a_bytes, std::string_view a_expectedSceneId,
    const AssertContext &a_assertContext) noexcept
{
    return parse_runtime_scene(a_bytes, a_expectedSceneId, a_assertContext);
}

#if defined(CUE_RUNTIME_PACKAGE_DYNAMIC)
Result<RuntimeHostStartup> load_runtime_package(const AssertContext &a_assertContext) noexcept
{
    try
    {
        auto schemaIdentitySource = std::make_unique<schema::SchemaRegistryIdentitySource>();
        if (SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32) == FALSE)
        {
            return Result<RuntimeHostStartup>::failure(
                windows_package_error(a_assertContext, package::PackageError::InvalidPackagePath, GetLastError(),
                                      "RuntimeHost could not restrict the process DLL search policy"));
        }
        auto executable = executable_path(a_assertContext);
        if (!executable)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*executable.try_error()));
        }
        const std::filesystem::path root = executable.try_value()->parent_path();
        const std::string rootUtf8 = path_to_utf8(root);
        auto filesystem = create_windows_filesystem_root(rootUtf8, a_assertContext);
        if (!filesystem)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*filesystem.try_error()));
        }
        auto manifestPath = RelativePath::parse("CuePackage.json", a_assertContext);
        if (!manifestPath)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*manifestPath.try_error()));
        }
        auto manifestBytes =
            filesystem.try_value()->get()->read_file(*manifestPath.try_value(), package::k_maximumPackageManifestBytes);
        if (!manifestBytes)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*manifestBytes.try_error()));
        }
        auto manifest = package::parse_package_manifest(text_view(*manifestBytes.try_value()), a_assertContext);
        if (!manifest)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*manifest.try_error()));
        }
        if (manifest.try_value()->engine_version() != k_engineVersion ||
            manifest.try_value()->configuration() != host_configuration())
        {
            return Result<RuntimeHostStartup>::failure(
                package_error(a_assertContext, package::PackageError::InvalidPackageManifest,
                              "Package Engine version or Build Configuration differs from RuntimeHost"));
        }
        const package::PackageFileEntry *runtimeHostEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::RuntimeHost);
        const package::PackageFileEntry *projectEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::ProjectRuntimeData);
        const package::PackageFileEntry *sceneEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::StartupSceneRuntimeData);
        const package::PackageFileEntry *metadataEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::GameModuleMetadata);
        const package::PackageFileEntry *moduleEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::GameModule);
        if (runtimeHostEntry == nullptr || projectEntry == nullptr || sceneEntry == nullptr ||
            metadataEntry == nullptr || moduleEntry == nullptr)
        {
            return Result<RuntimeHostStartup>::failure(package_error(a_assertContext,
                                                                     package::PackageError::InvalidPackageManifest,
                                                                     "Package is missing a required Runtime role"));
        }
        if (projectEntry->byte_size() > package::k_maximumRuntimeProjectDataBytes ||
            sceneEntry->byte_size() > package::k_maximumRuntimeSceneDataBytes)
        {
            return Result<RuntimeHostStartup>::failure(
                package_error(a_assertContext, package::PackageError::InvalidPackageManifest,
                              "Runtime Data role exceeds its Package startup size limit"));
        }
        if (metadataEntry->byte_size() > k_maximumGameModuleMetadataBytes)
        {
            return Result<RuntimeHostStartup>::failure(
                package_error(a_assertContext, package::PackageError::InvalidPackageManifest,
                              "Game Module Metadata role exceeds its Package startup size limit"));
        }
        auto runtimePeMemoryContract = validate_runtime_pe_memory_contract(*manifest.try_value(), a_assertContext);
        if (!runtimePeMemoryContract)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*runtimePeMemoryContract.try_error()));
        }
        if (executable.try_value()->filename() != detail::filesystem_path_from_utf8(runtimeHostEntry->relative_path()))
        {
            return Result<RuntimeHostStartup>::failure(
                package_error(a_assertContext, package::PackageError::InvalidPackagePath,
                              "Running RuntimeHost executable does not match the Manifest role"));
        }
        auto inventory = package::verify_package_manifest_files(rootUtf8, *manifest.try_value(), a_assertContext);
        if (!inventory)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*inventory.try_error()));
        }
        auto projectBytes = read_manifest_file(**filesystem.try_value(), *projectEntry, a_assertContext);
        auto sceneBytes = read_manifest_file(**filesystem.try_value(), *sceneEntry, a_assertContext);
        auto metadataBytes = read_manifest_file(**filesystem.try_value(), *metadataEntry, a_assertContext);
        if (!projectBytes || !sceneBytes || !metadataBytes)
        {
            Error error = !projectBytes ? std::move(*projectBytes.try_error())
                                        : (!sceneBytes ? std::move(*sceneBytes.try_error())
                                                       : std::move(*metadataBytes.try_error()));
            return Result<RuntimeHostStartup>::failure(std::move(error));
        }
        auto project = parse_runtime_project(text_view(*projectBytes.try_value()), a_assertContext);
        if (!project || project.try_value()->projectId != manifest.try_value()->project_id() ||
            project.try_value()->startupSceneAssetId != manifest.try_value()->startup_scene_asset_id())
        {
            return Result<RuntimeHostStartup>::failure(
                project ? package_error(a_assertContext, package::PackageError::InvalidRuntimeData,
                                        "Runtime Project identity differs from the Manifest")
                        : std::move(*project.try_error()));
        }
        auto metadata = validate_module_metadata(text_view(*metadataBytes.try_value()), *manifest.try_value(),
                                                 *project.try_value(), a_assertContext);
        if (!metadata)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*metadata.try_error()));
        }
        auto startupScene = parse_runtime_scene_data(text_view(*sceneBytes.try_value()),
                                                manifest.try_value()->startup_scene_asset_id(), a_assertContext);
        if (!startupScene)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*startupScene.try_error()));
        }
        auto canonicalRuntimeData = validate_canonical_runtime_data(
            *project.try_value(), *startupScene.try_value(), text_view(*projectBytes.try_value()),
            text_view(*sceneBytes.try_value()), a_assertContext);
        if (!canonicalRuntimeData)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*canonicalRuntimeData.try_error()));
        }

        auto rootGuard = guard_directory(root, a_assertContext);
        auto gameGuard = guard_directory(root / "Game", a_assertContext);
        if (!rootGuard || !gameGuard)
        {
            return Result<RuntimeHostStartup>::failure(!rootGuard ? std::move(*rootGuard.try_error())
                                                                  : std::move(*gameGuard.try_error()));
        }
        UniqueHandle runtimeGuard;
        std::vector<UniqueHandle> runtimeDependencyGuards;
        std::vector<const package::PackageFileEntry *> runtimeDependencyEntries;
        auto runtimeInventory = validate_runtime_dependency_inventory(root, *manifest.try_value(), a_assertContext);
        if (!runtimeInventory)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*runtimeInventory.try_error()));
        }
        const bool hasRuntimeDependencies = *runtimeInventory.try_value();
        if (hasRuntimeDependencies)
        {
            auto guarded = guard_directory(root / "Runtime", a_assertContext);
            if (!guarded)
            {
                return Result<RuntimeHostStartup>::failure(std::move(*guarded.try_error()));
            }
            runtimeGuard = std::move(*guarded.try_value());
            for (const package::PackageFileEntry &entry : manifest.try_value()->files())
            {
                if (entry.role() != package::PackageFileRole::RuntimeDependency)
                {
                    continue;
                }
                auto dependencyGuard = guard_package_file(
                    root / detail::filesystem_path_from_utf8(entry.relative_path()), a_assertContext);
                if (!dependencyGuard)
                {
                    return Result<RuntimeHostStartup>::failure(std::move(*dependencyGuard.try_error()));
                }
                runtimeDependencyGuards.push_back(std::move(*dependencyGuard.try_value()));
                runtimeDependencyEntries.push_back(&entry);
            }
        }
        const std::filesystem::path modulePath = root / detail::filesystem_path_from_utf8(moduleEntry->relative_path());
        auto moduleGuard = guard_package_file(modulePath, a_assertContext);
        if (!moduleGuard)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*moduleGuard.try_error()));
        }
        auto moduleBytes = read_manifest_file(**filesystem.try_value(), *moduleEntry, a_assertContext);
        if (!moduleBytes)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*moduleBytes.try_error()));
        }
        std::vector<std::vector<std::byte>> runtimeDependencyBytes;
        runtimeDependencyBytes.reserve(runtimeDependencyEntries.size());
        for (const package::PackageFileEntry *entry : runtimeDependencyEntries)
        {
            auto bytes = read_manifest_file(**filesystem.try_value(), *entry, a_assertContext);
            if (!bytes)
            {
                return Result<RuntimeHostStartup>::failure(std::move(*bytes.try_error()));
            }
            runtimeDependencyBytes.push_back(std::move(*bytes.try_value()));
        }
        std::vector<package::RuntimePeImageView> runtimeDependencyViews;
        runtimeDependencyViews.reserve(runtimeDependencyEntries.size());
        for (std::size_t index = 0U; index < runtimeDependencyEntries.size(); ++index)
        {
            runtimeDependencyViews.push_back(
                {package_file_name(runtimeDependencyEntries[index]->relative_path()), runtimeDependencyBytes[index]});
        }
        auto runtimeLoadOrder = package::create_runtime_dependency_load_order(
            manifest.try_value()->configuration(),
            {package_file_name(moduleEntry->relative_path()), *moduleBytes.try_value()}, runtimeDependencyViews,
            a_assertContext);
        if (!runtimeLoadOrder)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*runtimeLoadOrder.try_error()));
        }
        auto guardedInventory =
            package::verify_package_manifest_files(rootUtf8, *manifest.try_value(), a_assertContext);
        auto guardedRuntimeInventory =
            guardedInventory ? validate_runtime_dependency_inventory(root, *manifest.try_value(), a_assertContext)
                             : Result<bool>::failure(std::move(*guardedInventory.try_error()));
        if (!guardedRuntimeInventory || *guardedRuntimeInventory.try_value() != hasRuntimeDependencies)
        {
            return Result<RuntimeHostStartup>::failure(
                guardedRuntimeInventory
                    ? package_error(a_assertContext, package::PackageError::PackageFileMismatch,
                                    "Runtime dependency inventory changed after load targets were fixed")
                    : std::move(*guardedRuntimeInventory.try_error()));
        }
        std::vector<HMODULE> runtimeLibraries;
        runtimeLibraries.reserve(runtimeLoadOrder.try_value()->size());
        for (const std::size_t index : *runtimeLoadOrder.try_value())
        {
            const std::filesystem::path dependencyPath = extended_windows_path(
                root / detail::filesystem_path_from_utf8(runtimeDependencyEntries[index]->relative_path()));
            HMODULE dependency = LoadLibraryExW(dependencyPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (dependency == nullptr)
            {
                const DWORD code = GetLastError();
                unload_libraries(runtimeLibraries);
                return Result<RuntimeHostStartup>::failure(
                    windows_package_error(a_assertContext, package::PackageError::InvalidRuntimeData, code,
                                          "Manifest Runtime dependency could not be loaded from its fixed path"));
            }
            auto dependencyIdentity =
                validate_loaded_module_identity(dependency, runtimeDependencyGuards[index].get(), a_assertContext);
            if (!dependencyIdentity)
            {
                FreeLibrary(dependency);
                unload_libraries(runtimeLibraries);
                return Result<RuntimeHostStartup>::failure(std::move(*dependencyIdentity.try_error()));
            }
            runtimeLibraries.push_back(dependency);
        }
        const std::filesystem::path moduleLoadPath = extended_windows_path(modulePath);
        HMODULE library = LoadLibraryExW(moduleLoadPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library == nullptr)
        {
            const DWORD code = GetLastError();
            unload_libraries(runtimeLibraries);
            return Result<RuntimeHostStartup>::failure(
                windows_package_error(a_assertContext, package::PackageError::InvalidRuntimeData, code,
                                      "Game Module could not be loaded from the Manifest path"));
        }
        auto moduleIdentity = validate_loaded_module_identity(library, moduleGuard.try_value()->get(), a_assertContext);
        if (!moduleIdentity)
        {
            FreeLibrary(library);
            unload_libraries(runtimeLibraries);
            return Result<RuntimeHostStartup>::failure(std::move(*moduleIdentity.try_error()));
        }
        auto codeLifetime = std::make_shared<WindowsGameModuleCodeLifetime>(
            library, std::move(runtimeLibraries), std::move(*rootGuard.try_value()), std::move(*gameGuard.try_value()),
            std::move(runtimeGuard), std::move(*moduleGuard.try_value()), std::move(runtimeDependencyGuards));
        WindowsDynamicGameModuleQueryProvider provider(codeLifetime);
        Result<PreparedGameModule> prepared = connect_game_module(provider, manifest.try_value()->project_id(),
                                                                  std::move(schemaIdentitySource), a_assertContext);
        if (!prepared)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*prepared.try_error()));
        }
        return Result<RuntimeHostStartup>::success(
            RuntimeHostStartup(std::move(*prepared.try_value()), std::move(*startupScene.try_value())));
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}
#endif

#if defined(CUE_RUNTIME_PACKAGE_STATIC)
Result<RuntimeHostStartup> load_static_runtime_package(GameModuleQueryFunction a_query,
                                                       std::string_view a_expectedProjectId,
                                                       StaticRuntimeTrustMode a_expectedTrustMode,
                                                       std::string_view a_expectedPublisherKeyId,
                                                       const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_expectedTrustMode != StaticRuntimeTrustMode::UnsignedLocal || !a_expectedPublisherKeyId.empty())
        {
            return Result<RuntimeHostStartup>::failure(package_error(
                a_assertContext, package::PackageError::InvalidPackageManifest,
                "Only UnsignedLocal Package startup without a Publisher Key is enabled until detached signature "
                "and external trust anchor verification are implemented"));
        }
        auto executable = executable_path(a_assertContext);
        if (!executable)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*executable.try_error()));
        }
        const std::filesystem::path root = executable.try_value()->parent_path();
        const std::string rootUtf8 = path_to_utf8(root);
        auto filesystem = create_windows_filesystem_root(rootUtf8, a_assertContext);
        if (!filesystem)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*filesystem.try_error()));
        }
        auto manifestPath = RelativePath::parse("CuePackage.json", a_assertContext);
        if (!manifestPath)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*manifestPath.try_error()));
        }
        auto manifestBytes =
            filesystem.try_value()->get()->read_file(*manifestPath.try_value(), package::k_maximumPackageManifestBytes);
        if (!manifestBytes)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*manifestBytes.try_error()));
        }
        auto manifest = package::parse_package_manifest(text_view(*manifestBytes.try_value()), a_assertContext);
        if (!manifest)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*manifest.try_error()));
        }
        const std::optional<std::string_view> applicationExecutable = manifest.try_value()->application_executable();
        const std::optional<ShippingTrustMode> trustMode = manifest.try_value()->trust_mode();
        const std::optional<std::string_view> publisherKeyId = manifest.try_value()->publisher_key_id();
        constexpr ShippingTrustMode expectedManifestTrustMode = ShippingTrustMode::UnsignedLocal;
        const bool publisherMatches = !publisherKeyId.has_value();
        if (manifest.try_value()->schema_version() != package::k_monolithicPackageManifestSchemaVersion ||
            manifest.try_value()->execution_model() != package::PackageExecutionModel::Monolithic ||
            manifest.try_value()->engine_version() != k_engineVersion ||
            manifest.try_value()->configuration() != BuildConfiguration::Release ||
            manifest.try_value()->configuration() != host_configuration() ||
            manifest.try_value()->project_id() != a_expectedProjectId || !applicationExecutable.has_value() ||
            !trustMode.has_value() || *trustMode != expectedManifestTrustMode || !publisherMatches)
        {
            return Result<RuntimeHostStartup>::failure(package_error(
                a_assertContext, package::PackageError::InvalidPackageManifest,
                "Monolithic Package identity, execution model, or trust policy differs from the Product"));
        }
        if (path_to_utf8(executable.try_value()->filename()) != *applicationExecutable)
        {
            return Result<RuntimeHostStartup>::failure(
                package_error(a_assertContext, package::PackageError::InvalidPackagePath,
                              "Running Product executable does not match the Manifest application executable"));
        }

        const package::PackageFileEntry *applicationEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::ApplicationExecutable);
        const package::PackageFileEntry *projectEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::ProjectRuntimeData);
        const package::PackageFileEntry *sceneEntry =
            find_role(*manifest.try_value(), package::PackageFileRole::StartupSceneRuntimeData);
        if (applicationEntry == nullptr || projectEntry == nullptr || sceneEntry == nullptr ||
            applicationEntry->relative_path() != *applicationExecutable ||
            projectEntry->byte_size() > package::k_maximumRuntimeProjectDataBytes ||
            sceneEntry->byte_size() > package::k_maximumRuntimeSceneDataBytes)
        {
            return Result<RuntimeHostStartup>::failure(
                package_error(a_assertContext, package::PackageError::InvalidPackageManifest,
                              "Monolithic Package is missing a valid required Runtime role"));
        }
        auto inventory = package::verify_package_manifest_files(rootUtf8, *manifest.try_value(), a_assertContext);
        if (!inventory)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*inventory.try_error()));
        }
        auto projectBytes = read_manifest_file(**filesystem.try_value(), *projectEntry, a_assertContext);
        auto sceneBytes = read_manifest_file(**filesystem.try_value(), *sceneEntry, a_assertContext);
        if (!projectBytes || !sceneBytes)
        {
            return Result<RuntimeHostStartup>::failure(!projectBytes ? std::move(*projectBytes.try_error())
                                                                     : std::move(*sceneBytes.try_error()));
        }
        auto project = parse_runtime_project(text_view(*projectBytes.try_value()), a_assertContext);
        if (!project || project.try_value()->projectId != manifest.try_value()->project_id() ||
            project.try_value()->startupSceneAssetId != manifest.try_value()->startup_scene_asset_id())
        {
            return Result<RuntimeHostStartup>::failure(
                project ? package_error(a_assertContext, package::PackageError::InvalidRuntimeData,
                                        "Runtime Project identity differs from the Monolithic Manifest")
                        : std::move(*project.try_error()));
        }
        auto startupScene = parse_runtime_scene_data(text_view(*sceneBytes.try_value()),
                                                manifest.try_value()->startup_scene_asset_id(), a_assertContext);
        if (!startupScene)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*startupScene.try_error()));
        }
        auto canonicalRuntimeData = validate_canonical_runtime_data(
            *project.try_value(), *startupScene.try_value(), text_view(*projectBytes.try_value()),
            text_view(*sceneBytes.try_value()), a_assertContext);
        if (!canonicalRuntimeData)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*canonicalRuntimeData.try_error()));
        }

        auto provider = create_static_game_module_query_provider(a_query, a_assertContext);
        if (!provider)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*provider.try_error()));
        }
        auto schemaIdentitySource = std::make_unique<schema::SchemaRegistryIdentitySource>();
        Result<PreparedGameModule> prepared =
            connect_game_module(**provider.try_value(), manifest.try_value()->project_id(),
                                std::move(schemaIdentitySource), a_assertContext);
        if (!prepared)
        {
            return Result<RuntimeHostStartup>::failure(std::move(*prepared.try_error()));
        }
        return Result<RuntimeHostStartup>::success(
            RuntimeHostStartup(std::move(*prepared.try_value()), std::move(*startupScene.try_value())));
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}
#endif
} // namespace cue::runtime_host
