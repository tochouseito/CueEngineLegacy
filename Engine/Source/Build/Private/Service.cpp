#include <Cue/Build/Service.h>

#include <Cue/Foundation/Assert.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
/// @brief 一Artifact Versionへ記録できるFile数の上限
constexpr std::size_t k_maximumArtifactFiles = 128U;
/// @brief JSON整数として情報を失わず表現できるArtifact File Size上限
constexpr std::uint64_t k_maximumArtifactByteSize = 9007199254740991ULL;

/// @brief 回復不能なService内部例外をFatalHandlerへ通知してProcessを停止する
[[noreturn]] void terminate_service_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Game build service failed unexpectedly");
    std::abort();
}

/// @brief Service固有の回復可能Errorを一貫したDomainで構築する
[[nodiscard]] cue::Error make_service_error(const cue::AssertContext &a_assertContext,
                                            cue::GameBuildServiceError a_code, std::string_view a_summary) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_assertContext.fatal_handler(), "Cue.Build.Service", static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_assertContext.fatal_handler(), std::move(code), a_summary);
}

/// @brief 設定済み待機時間をPublisherへ渡す単調Clock上のDeadlineへ変換する
[[nodiscard]] cue::BuildArtifactLockDeadline make_lock_deadline(
    std::optional<std::chrono::milliseconds> a_timeout) noexcept
{
    if (!a_timeout)
    {
        return std::nullopt;
    }
    return std::chrono::steady_clock::now() + *a_timeout;
}

/// @brief Error ChainのRootがPublisher内のTimeoutか判定する
[[nodiscard]] bool is_publisher_timeout(const cue::Error &a_error) noexcept
{
    const cue::ErrorCode &root = a_error.root_code();
    if (root.domain() != "Cue.Build.Publisher")
    {
        return false;
    }
    return root.value() == static_cast<std::int64_t>(cue::BuildArtifactPublisherError::LockWaitTimedOut) ||
           root.value() == static_cast<std::int64_t>(cue::BuildArtifactPublisherError::ModuleProbeTimedOut);
}

/// @brief Artifact IDとHashで許可するlowercase hexadecimal文字か判定する
[[nodiscard]] bool is_lower_hex(char a_value) noexcept
{
    return (a_value >= '0' && a_value <= '9') || (a_value >= 'a' && a_value <= 'f');
}

/// @brief Artifact IDがlowercase UUID v4のCanonical形式か検証する
[[nodiscard]] bool is_uuid_v4(std::string_view a_text) noexcept
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

/// @brief Artifact Root外参照とWindowsで危険な要素を含まない相対Pathか検証する
[[nodiscard]] bool is_safe_relative_path(std::string_view a_path) noexcept
{
    if (a_path.empty() || a_path.front() == '/' || a_path.back() == '/' ||
        a_path.find('\\') != std::string_view::npos || a_path.find('\0') != std::string_view::npos)
    {
        return false;
    }
    std::size_t begin = 0U;
    while (begin < a_path.size())
    {
        const std::size_t end = a_path.find('/', begin);
        const std::string_view component =
            a_path.substr(begin, end == std::string_view::npos ? a_path.size() - begin : end - begin);
        if (component.empty() || component == "." || component == "..")
        {
            return false;
        }
        if (component.back() == ' ' || component.back() == '.')
        {
            return false;
        }
        for (const unsigned char value : component)
        {
            if (value < 0x20U || value == ':' || value == '*' || value == '?' || value == '"' || value == '<' ||
                value == '>' || value == '|')
            {
                return false;
            }
        }
        if (end == std::string_view::npos)
        {
            break;
        }
        begin = end + 1U;
    }
    return true;
}

/// @brief Windows上で重複するASCII大小文字違いのPathを検出する
[[nodiscard]] bool equals_path_ascii_case_insensitive(std::string_view a_left, std::string_view a_right) noexcept
{
    if (a_left.size() != a_right.size())
    {
        return false;
    }
    for (std::size_t index = 0U; index < a_left.size(); ++index)
    {
        const unsigned char left = static_cast<unsigned char>(a_left[index]);
        const unsigned char right = static_cast<unsigned char>(a_right[index]);
        const unsigned char foldedLeft =
            left >= 'A' && left <= 'Z' ? static_cast<unsigned char>(left - 'A' + 'a') : left;
        const unsigned char foldedRight =
            right >= 'A' && right <= 'Z' ? static_cast<unsigned char>(right - 'A' + 'a') : right;
        if (foldedLeft != foldedRight)
        {
            return false;
        }
    }
    return true;
}

/// @brief Content Hashがlowercase SHA-256文字列表現か検証する
[[nodiscard]] bool has_valid_hash(std::string_view a_hash) noexcept
{
    return a_hash.size() == 64U && std::all_of(a_hash.begin(), a_hash.end(), is_lower_hex);
}

/// @brief Artifact Fileを決定的な相対Path順へ整列する比較結果を返す
[[nodiscard]] bool artifact_path_less(const cue::BuildArtifactFile &a_left,
                                      const cue::BuildArtifactFile &a_right) noexcept
{
    return a_left.relativePath < a_right.relativePath;
}

/// @brief Current Manifest用のConfiguration文字列を返す
[[nodiscard]] std::string_view current_configuration_name(cue::BuildConfiguration a_configuration) noexcept
{
    switch (a_configuration)
    {
    case cue::BuildConfiguration::Debug:
        return "Debug";
    case cue::BuildConfiguration::Development:
        return "Development";
    case cue::BuildConfiguration::Release:
        return "Release";
    }
    return {};
}

/// @brief Current Manifest用のTarget文字列を返す
[[nodiscard]] std::string_view current_target_name(cue::BuildTarget a_target) noexcept
{
    switch (a_target)
    {
    case cue::BuildTarget::GameModule:
        return "GameModule";
    case cue::BuildTarget::ShippingProduct:
        return "ShippingProduct";
    }
    return {};
}

/// @brief Current Manifest用のTrust Mode文字列を返す
[[nodiscard]] std::string_view current_trust_mode_name(cue::ShippingTrustMode a_mode) noexcept
{
    switch (a_mode)
    {
    case cue::ShippingTrustMode::UnsignedLocal:
        return "UnsignedLocal";
    case cue::ShippingTrustMode::PublisherSigned:
        return "PublisherSigned";
    }
    return {};
}

/// @brief Manifest文字列をArtifact File用途へ厳格に変換する
[[nodiscard]] bool parse_file_purpose(std::string_view a_text,
                                      cue::BuildArtifactFilePurpose &a_output) noexcept
{
    if (a_text == "DistributionPayload")
    {
        a_output = cue::BuildArtifactFilePurpose::DistributionPayload;
        return true;
    }
    if (a_text == "RuntimeMetadata")
    {
        a_output = cue::BuildArtifactFilePurpose::RuntimeMetadata;
        return true;
    }
    if (a_text == "DevelopmentSymbol")
    {
        a_output = cue::BuildArtifactFilePurpose::DevelopmentSymbol;
        return true;
    }
    return false;
}

/// @brief Current Manifest v1／v2を意味的に読む上限付きJSON Cursor
class CurrentManifestReader final
{
  public:
    /// @brief 借用JSON全体をCursorへ設定する
    explicit CurrentManifestReader(std::string_view a_input) noexcept : m_input(a_input)
    {
    }

    /// @brief 空白後に固定記号があれば消費する
    [[nodiscard]] bool consume(char a_expected) noexcept
    {
        skip_space();
        if (m_cursor >= m_input.size() || m_input[m_cursor] != a_expected)
        {
            return false;
        }
        ++m_cursor;
        return true;
    }

    /// @brief JSON StringをUnicode Escapeを含む意味値へ復号する
    [[nodiscard]] bool read_string(std::string &a_output)
    {
        skip_space();
        if (m_cursor >= m_input.size() || m_input[m_cursor] != '"')
        {
            return false;
        }
        ++m_cursor;
        a_output.clear();
        while (m_cursor < m_input.size())
        {
            const unsigned char value = static_cast<unsigned char>(m_input[m_cursor++]);
            if (value == '"')
            {
                return true;
            }
            if (value < 0x20U)
            {
                return false;
            }
            if (value != '\\')
            {
                a_output.push_back(static_cast<char>(value));
                continue;
            }
            if (m_cursor >= m_input.size())
            {
                return false;
            }
            const char escaped = m_input[m_cursor++];
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
            case 'u':
            {
                std::uint32_t scalar = 0U;
                if (!read_hex_quad(scalar))
                {
                    return false;
                }
                if (scalar >= 0xd800U && scalar <= 0xdbffU)
                {
                    if (m_cursor + 2U > m_input.size() || m_input[m_cursor] != '\\' ||
                        m_input[m_cursor + 1U] != 'u')
                    {
                        return false;
                    }
                    m_cursor += 2U;
                    std::uint32_t low = 0U;
                    if (!read_hex_quad(low) || low < 0xdc00U || low > 0xdfffU)
                    {
                        return false;
                    }
                    scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + (low - 0xdc00U);
                }
                else if (scalar >= 0xdc00U && scalar <= 0xdfffU)
                {
                    return false;
                }
                append_utf8(a_output, scalar);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    /// @brief 指数、小数、符号、先頭Zeroを許さず符号なしJSON整数を読む
    [[nodiscard]] bool read_unsigned(std::uint64_t &a_output) noexcept
    {
        skip_space();
        if (m_cursor >= m_input.size() || m_input[m_cursor] < '0' || m_input[m_cursor] > '9')
        {
            return false;
        }
        if (m_input[m_cursor] == '0' && m_cursor + 1U < m_input.size() && m_input[m_cursor + 1U] >= '0' &&
            m_input[m_cursor + 1U] <= '9')
        {
            return false;
        }
        std::uint64_t value = 0U;
        do
        {
            const std::uint64_t digit = static_cast<std::uint64_t>(m_input[m_cursor] - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
            {
                return false;
            }
            value = value * 10U + digit;
            ++m_cursor;
        } while (m_cursor < m_input.size() && m_input[m_cursor] >= '0' && m_input[m_cursor] <= '9');
        if (m_cursor < m_input.size() &&
            (m_input[m_cursor] == '.' || m_input[m_cursor] == 'e' || m_input[m_cursor] == 'E'))
        {
            return false;
        }
        a_output = value;
        return true;
    }

    /// @brief JSON nullを厳格に消費する
    [[nodiscard]] bool read_null() noexcept
    {
        skip_space();
        constexpr std::string_view value = "null";
        if (m_input.substr(m_cursor, value.size()) != value)
        {
            return false;
        }
        m_cursor += value.size();
        return true;
    }

    /// @brief 意味を持たない末尾空白以外が残っていないか返す
    [[nodiscard]] bool at_end() noexcept
    {
        skip_space();
        return m_cursor == m_input.size();
    }

  private:
    /// @brief JSON空白を読み飛ばす
    void skip_space() noexcept
    {
        while (m_cursor < m_input.size() &&
               (m_input[m_cursor] == ' ' || m_input[m_cursor] == '\t' || m_input[m_cursor] == '\r' ||
                m_input[m_cursor] == '\n'))
        {
            ++m_cursor;
        }
    }

    /// @brief 4桁のUnicode Hex Escapeを読む
    [[nodiscard]] bool read_hex_quad(std::uint32_t &a_output) noexcept
    {
        if (m_cursor + 4U > m_input.size())
        {
            return false;
        }
        std::uint32_t value = 0U;
        for (std::size_t index = 0U; index < 4U; ++index)
        {
            const char character = m_input[m_cursor++];
            std::uint32_t digit = 0U;
            if (character >= '0' && character <= '9')
            {
                digit = static_cast<std::uint32_t>(character - '0');
            }
            else if (character >= 'a' && character <= 'f')
            {
                digit = static_cast<std::uint32_t>(character - 'a' + 10);
            }
            else if (character >= 'A' && character <= 'F')
            {
                digit = static_cast<std::uint32_t>(character - 'A' + 10);
            }
            else
            {
                return false;
            }
            value = (value << 4U) | digit;
        }
        a_output = value;
        return true;
    }

    /// @brief Unicode ScalarをCanonical UTF-8 Byte列として追加する
    static void append_utf8(std::string &a_output, std::uint32_t a_scalar)
    {
        if (a_scalar <= 0x7fU)
        {
            a_output.push_back(static_cast<char>(a_scalar));
        }
        else if (a_scalar <= 0x7ffU)
        {
            a_output.push_back(static_cast<char>(0xc0U | (a_scalar >> 6U)));
            a_output.push_back(static_cast<char>(0x80U | (a_scalar & 0x3fU)));
        }
        else if (a_scalar <= 0xffffU)
        {
            a_output.push_back(static_cast<char>(0xe0U | (a_scalar >> 12U)));
            a_output.push_back(static_cast<char>(0x80U | ((a_scalar >> 6U) & 0x3fU)));
            a_output.push_back(static_cast<char>(0x80U | (a_scalar & 0x3fU)));
        }
        else
        {
            a_output.push_back(static_cast<char>(0xf0U | (a_scalar >> 18U)));
            a_output.push_back(static_cast<char>(0x80U | ((a_scalar >> 12U) & 0x3fU)));
            a_output.push_back(static_cast<char>(0x80U | ((a_scalar >> 6U) & 0x3fU)));
            a_output.push_back(static_cast<char>(0x80U | (a_scalar & 0x3fU)));
        }
    }

    std::string_view m_input;
    std::size_t m_cursor = 0U;
};

/// @brief Current Manifest v1／v2の一File Entryを厳格に読む
[[nodiscard]] bool read_current_file(CurrentManifestReader &a_reader, cue::BuildArtifactFile &a_output)
{
    if (!a_reader.consume('{'))
    {
        return false;
    }
    bool hasPath = false;
    bool hasSize = false;
    bool hasAlgorithm = false;
    bool hasHash = false;
    bool hasPurpose = false;
    std::string algorithm;
    bool closed = false;
    for (std::size_t memberIndex = 0U; memberIndex < 5U; ++memberIndex)
    {
        if (memberIndex != 0U)
        {
            if (a_reader.consume('}'))
            {
                closed = true;
                break;
            }
            if (!a_reader.consume(','))
            {
                return false;
            }
        }
        std::string name;
        if (!a_reader.read_string(name) || !a_reader.consume(':'))
        {
            return false;
        }
        if (name == "path" && !hasPath)
        {
            hasPath = a_reader.read_string(a_output.relativePath);
        }
        else if (name == "sizeBytes" && !hasSize)
        {
            hasSize = a_reader.read_unsigned(a_output.byteSize);
        }
        else if (name == "hashAlgorithm" && !hasAlgorithm)
        {
            hasAlgorithm = a_reader.read_string(algorithm);
        }
        else if (name == "contentHash" && !hasHash)
        {
            hasHash = a_reader.read_string(a_output.contentHash);
        }
        else if (name == "purpose" && !hasPurpose)
        {
            std::string purpose;
            hasPurpose = a_reader.read_string(purpose) && parse_file_purpose(purpose, a_output.purpose);
        }
        else
        {
            return false;
        }
        if ((!hasPath && name == "path") || (!hasSize && name == "sizeBytes") ||
            (!hasAlgorithm && name == "hashAlgorithm") || (!hasHash && name == "contentHash") ||
            (!hasPurpose && name == "purpose"))
        {
            return false;
        }
    }
    if (!closed)
    {
        closed = a_reader.consume('}');
    }
    return closed && hasPath && hasSize && hasAlgorithm && hasHash && algorithm == "sha256";
}

/// @brief Current Manifest v1／v2のFile配列を順序を保持して読む
[[nodiscard]] bool read_current_files(CurrentManifestReader &a_reader,
                                       std::vector<cue::BuildArtifactFile> &a_output)
{
    if (!a_reader.consume('['))
    {
        return false;
    }
    if (a_reader.consume(']'))
    {
        return true;
    }
    for (;;)
    {
        if (a_output.size() >= k_maximumArtifactFiles)
        {
            return false;
        }
        cue::BuildArtifactFile file;
        if (!read_current_file(a_reader, file))
        {
            return false;
        }
        a_output.push_back(std::move(file));
        if (a_reader.consume(']'))
        {
            return true;
        }
        if (!a_reader.consume(','))
        {
            return false;
        }
    }
}

/// @brief JSON Stringまたはnullを所有Optionalへ読む
[[nodiscard]] bool read_nullable_string(CurrentManifestReader &a_reader,
                                        std::optional<std::string> &a_output)
{
    std::string value;
    if (a_reader.read_string(value))
    {
        a_output.emplace(std::move(value));
        return true;
    }
    if (!a_reader.read_null())
    {
        return false;
    }
    a_output.reset();
    return true;
}

/// @brief Native ErrorがあればUI再表示可能な所有Snapshotへ変換する
[[nodiscard]] std::optional<cue::BuildNativeErrorSnapshot> flatten_native_error(const cue::NativeError *a_nativeError)
{
    if (a_nativeError == nullptr)
    {
        return std::nullopt;
    }
    return cue::BuildNativeErrorSnapshot{std::string(a_nativeError->domain()), a_nativeError->value()};
}

/// @brief 所有関係を持つError ChainをUI再表示可能な診断値へ平坦化する
[[nodiscard]] std::vector<cue::BuildDiagnosticSnapshot> flatten_error(const cue::Error &a_error)
{
    std::vector<cue::BuildDiagnosticSnapshot> diagnostics;
    diagnostics.reserve(a_error.causes().size() + 1U);
    std::vector<std::string> primaryContexts;
    primaryContexts.reserve(a_error.contexts().size());
    for (const cue::ErrorContext &context : a_error.contexts())
    {
        primaryContexts.emplace_back(context.message());
    }
    diagnostics.push_back({std::string(a_error.code().domain()), a_error.code().value(), std::string(a_error.summary()),
                           std::move(primaryContexts), flatten_native_error(a_error.try_native_error())});
    for (const cue::ErrorCause &cause : a_error.causes())
    {
        std::vector<std::string> contexts;
        contexts.reserve(cause.contexts().size());
        for (const cue::ErrorContext &context : cause.contexts())
        {
            contexts.emplace_back(context.message());
        }
        diagnostics.push_back({std::string(cause.code().domain()), cause.code().value(), std::string(cause.summary()),
                               std::move(contexts), flatten_native_error(cause.try_native_error())});
    }
    return diagnostics;
}
} // namespace

namespace cue
{
BuildArtifactInventory::BuildArtifactInventory(std::string a_artifactId, BuildProfile a_profile,
                                               std::string a_versionDirectory,
                                               std::vector<BuildArtifactFile> a_files) noexcept
    : m_artifactId(std::move(a_artifactId)), m_profile(std::move(a_profile)),
      m_versionDirectory(std::move(a_versionDirectory)), m_files(std::move(a_files))
{
}

Result<BuildArtifactInventory> BuildArtifactInventory::create(const BuildPlan &a_plan, std::string a_artifactId,
                                                              std::vector<BuildArtifactFile> a_files,
                                                              const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!is_uuid_v4(a_artifactId) || a_files.empty() || a_files.size() > k_maximumArtifactFiles)
        {
            return Result<BuildArtifactInventory>::failure(
                make_service_error(a_assertContext, GameBuildServiceError::InvalidArtifact,
                                   "Build artifact identity or file count is invalid"));
        }
        std::sort(a_files.begin(), a_files.end(), artifact_path_less);
        const BuildTarget target = a_plan.profile().target();
        bool hasPayload = false;
        bool hasMetadata = false;
        for (std::size_t index = 0U; index < a_files.size(); ++index)
        {
            BuildArtifactFile &file = a_files[index];
            bool duplicatePath = false;
            for (std::size_t previous = 0U; previous < index; ++previous)
            {
                duplicatePath = duplicatePath ||
                                equals_path_ascii_case_insensitive(a_files[previous].relativePath, file.relativePath);
            }
            if (!is_safe_relative_path(file.relativePath) || !has_valid_hash(file.contentHash) || duplicatePath ||
                file.byteSize > k_maximumArtifactByteSize)
            {
                return Result<BuildArtifactInventory>::failure(
                    make_service_error(a_assertContext, GameBuildServiceError::InvalidArtifact,
                                       "Build artifact file inventory is invalid"));
            }
            std::optional<BuildArtifactFilePurpose> requiredPurpose;
            if (target == BuildTarget::GameModule && file.relativePath == "CueGameModule.dll")
            {
                requiredPurpose = BuildArtifactFilePurpose::DistributionPayload;
                hasPayload = file.byteSize > 0U;
            }
            else if (target == BuildTarget::GameModule && file.relativePath == "CueGameModule.metadata.json")
            {
                requiredPurpose = BuildArtifactFilePurpose::RuntimeMetadata;
                hasMetadata = file.byteSize > 0U;
            }
            else if (target == BuildTarget::GameModule && file.relativePath == "CueGameModule.pdb")
            {
                requiredPurpose = BuildArtifactFilePurpose::DevelopmentSymbol;
            }
            else if (target == BuildTarget::ShippingProduct && file.relativePath == "CueGameProduct.exe")
            {
                requiredPurpose = BuildArtifactFilePurpose::DistributionPayload;
                hasPayload = file.byteSize > 0U;
            }
            else if (target == BuildTarget::ShippingProduct && file.relativePath == "CueGameProduct.metadata.json")
            {
                requiredPurpose = BuildArtifactFilePurpose::RuntimeMetadata;
                hasMetadata = file.byteSize > 0U;
            }
            else if (target == BuildTarget::ShippingProduct && file.relativePath == "CueGameProduct.pdb")
            {
                requiredPurpose = BuildArtifactFilePurpose::DevelopmentSymbol;
            }
            else if (target == BuildTarget::ShippingProduct)
            {
                return Result<BuildArtifactInventory>::failure(make_service_error(
                    a_assertContext, GameBuildServiceError::InvalidArtifact,
                    "Shipping Product artifact contains an unexpected file"));
            }
            else
            {
                requiredPurpose = BuildArtifactFilePurpose::DistributionPayload;
            }
            if (file.purpose == BuildArtifactFilePurpose::Unspecified)
            {
                file.purpose = *requiredPurpose;
            }
            else if (file.purpose != *requiredPurpose)
            {
                return Result<BuildArtifactInventory>::failure(make_service_error(
                    a_assertContext, GameBuildServiceError::InvalidArtifact,
                    "Build artifact file purpose does not match its target contract"));
            }
        }
        if (!hasPayload || !hasMetadata)
        {
            return Result<BuildArtifactInventory>::failure(make_service_error(
                a_assertContext, GameBuildServiceError::InvalidArtifact, "Build artifact is missing required files"));
        }
        std::string versionDirectory(a_plan.artifact_store_directory());
        versionDirectory.append("/Versions/");
        versionDirectory.append(a_artifactId);
        return Result<BuildArtifactInventory>::success(
            BuildArtifactInventory(std::move(a_artifactId), a_plan.profile(),
                                    std::move(versionDirectory), std::move(a_files)));
    }
    catch (...)
    {
        terminate_service_exception(a_assertContext);
    }
}

Result<BuildArtifactInventory>
BuildArtifactInventory::create_legacy_game_module(const BuildPlan &a_plan, std::string a_artifactId,
                                                  std::vector<BuildArtifactFile> a_files,
                                                  const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (a_plan.profile().target() != BuildTarget::GameModule)
        {
            return Result<BuildArtifactInventory>::failure(
                make_service_error(a_assertContext, GameBuildServiceError::InvalidArtifact,
                                   "Legacy artifact inventory requires a GameModule build plan"));
        }
        Result<BuildArtifactInventory> inventory =
            create(a_plan, std::move(a_artifactId), std::move(a_files), a_assertContext);
        if (!inventory)
        {
            return inventory;
        }
        std::string versionDirectory(a_plan.project_root());
        if (!versionDirectory.empty() && versionDirectory.back() != '/')
        {
            versionDirectory.push_back('/');
        }
        versionDirectory.append("Generated/Artifacts/");
        versionDirectory.append(current_configuration_name(a_plan.profile().configuration()));
        versionDirectory.append("/Versions/");
        versionDirectory.append(inventory.try_value()->artifact_id());
        inventory.try_value()->m_versionDirectory = std::move(versionDirectory);
        return inventory;
    }
    catch (...)
    {
        terminate_service_exception(a_assertContext);
    }
}

std::string_view BuildArtifactInventory::artifact_id() const noexcept
{
    return m_artifactId;
}

BuildConfiguration BuildArtifactInventory::configuration() const noexcept
{
    return m_profile.configuration();
}

const BuildProfile &BuildArtifactInventory::profile() const noexcept
{
    return m_profile;
}

std::string_view BuildArtifactInventory::version_directory() const noexcept
{
    return m_versionDirectory;
}

std::span<const BuildArtifactFile> BuildArtifactInventory::files() const noexcept
{
    return m_files;
}

Result<void> validate_build_artifact_current_manifest(std::string_view a_json,
                                                      const BuildArtifactInventory &a_expected,
                                                      const AssertContext &a_assertContext) noexcept
{
    try
    {
        CurrentManifestReader reader(a_json);
        if (!reader.consume('{'))
        {
            return Result<void>::failure(make_service_error(
                a_assertContext, GameBuildServiceError::InvalidArtifact, "Current artifact manifest is invalid"));
        }
        bool hasSchema = false;
        bool hasArtifactId = false;
        bool hasConfiguration = false;
        bool hasTarget = false;
        bool hasMinimumTrustMode = false;
        bool hasPublisherKeyId = false;
        bool hasFiles = false;
        std::uint64_t schemaVersion = 0U;
        std::string artifactId;
        std::string configuration;
        std::string target;
        std::optional<std::string> minimumTrustMode;
        std::optional<std::string> publisherKeyId;
        std::vector<BuildArtifactFile> files;
        files.reserve(a_expected.files().size());
        std::size_t memberCount = 0U;
        bool closed = false;
        while (memberCount < 7U)
        {
            if (memberCount != 0U && !reader.consume(','))
            {
                return Result<void>::failure(make_service_error(
                    a_assertContext, GameBuildServiceError::InvalidArtifact, "Current artifact manifest is invalid"));
            }
            std::string name;
            if (!reader.read_string(name) || !reader.consume(':'))
            {
                return Result<void>::failure(make_service_error(
                    a_assertContext, GameBuildServiceError::InvalidArtifact, "Current artifact manifest is invalid"));
            }
            bool memberValid = false;
            if (name == "schemaVersion" && !hasSchema)
            {
                hasSchema = reader.read_unsigned(schemaVersion);
                memberValid = hasSchema;
            }
            else if (name == "artifactId" && !hasArtifactId)
            {
                hasArtifactId = reader.read_string(artifactId);
                memberValid = hasArtifactId;
            }
            else if (name == "configuration" && !hasConfiguration)
            {
                hasConfiguration = reader.read_string(configuration);
                memberValid = hasConfiguration;
            }
            else if (name == "target" && !hasTarget)
            {
                hasTarget = reader.read_string(target);
                memberValid = hasTarget;
            }
            else if (name == "minimumTrustMode" && !hasMinimumTrustMode)
            {
                hasMinimumTrustMode = read_nullable_string(reader, minimumTrustMode);
                memberValid = hasMinimumTrustMode;
            }
            else if (name == "publisherKeyId" && !hasPublisherKeyId)
            {
                hasPublisherKeyId = read_nullable_string(reader, publisherKeyId);
                memberValid = hasPublisherKeyId;
            }
            else if (name == "files" && !hasFiles)
            {
                hasFiles = read_current_files(reader, files);
                memberValid = hasFiles;
            }
            if (!memberValid)
            {
                return Result<void>::failure(make_service_error(
                    a_assertContext, GameBuildServiceError::InvalidArtifact, "Current artifact manifest is invalid"));
            }
            ++memberCount;
            if (reader.consume('}'))
            {
                closed = true;
                break;
            }
        }
        if (!closed || !reader.at_end() || !hasSchema || !hasArtifactId || !hasConfiguration || !hasFiles ||
            artifactId != a_expected.artifact_id() ||
            configuration != current_configuration_name(a_expected.configuration()) ||
            files.size() != a_expected.files().size())
        {
            return Result<void>::failure(make_service_error(
                a_assertContext, GameBuildServiceError::InvalidArtifact,
                "Current artifact manifest does not select the expected inventory"));
        }
        if (schemaVersion == 1U)
        {
            if (memberCount != 4U || hasTarget || hasMinimumTrustMode || hasPublisherKeyId ||
                a_expected.profile().target() != BuildTarget::GameModule)
            {
                return Result<void>::failure(make_service_error(
                    a_assertContext, GameBuildServiceError::InvalidArtifact,
                    "Legacy Current artifact manifest is not valid for the expected profile"));
            }
        }
        else if (schemaVersion == 2U)
        {
            const std::optional<ShippingTrustMode> expectedTrust = a_expected.profile().minimum_trust_mode();
            const bool trustMatches =
                (!expectedTrust && !minimumTrustMode) ||
                (expectedTrust && minimumTrustMode &&
                 *minimumTrustMode == current_trust_mode_name(*expectedTrust));
            const std::string_view expectedPublisher = a_expected.profile().publisher_key_id();
            const bool publisherMatches =
                (expectedPublisher.empty() && !publisherKeyId) ||
                (!expectedPublisher.empty() && publisherKeyId && *publisherKeyId == expectedPublisher);
            if (memberCount != 7U || !hasTarget || !hasMinimumTrustMode || !hasPublisherKeyId ||
                target != current_target_name(a_expected.profile().target()) || !trustMatches || !publisherMatches)
            {
                return Result<void>::failure(make_service_error(
                    a_assertContext, GameBuildServiceError::InvalidArtifact,
                    "Current artifact manifest profile does not match the expected inventory"));
            }
        }
        else
        {
            return Result<void>::failure(make_service_error(
                a_assertContext, GameBuildServiceError::InvalidArtifact,
                "Current artifact manifest schema is unsupported"));
        }
        for (std::size_t index = 0U; index < files.size(); ++index)
        {
            const BuildArtifactFile &actual = files[index];
            const BuildArtifactFile &expected = a_expected.files()[index];
            if (actual.relativePath != expected.relativePath || actual.byteSize != expected.byteSize ||
                actual.contentHash != expected.contentHash ||
                (schemaVersion == 1U && actual.purpose != BuildArtifactFilePurpose::Unspecified) ||
                (schemaVersion == 2U && actual.purpose != expected.purpose))
            {
                return Result<void>::failure(make_service_error(
                    a_assertContext, GameBuildServiceError::InvalidArtifact,
                    "Current artifact manifest does not select the expected inventory"));
            }
        }
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_service_exception(a_assertContext);
    }
}

struct GameBuildService::Impl final
{
    class Observer final : public CMakeStageObserver
    {
      public:
        /// @brief Stage通知をOperation ID付きで所有Serviceへ転送するObserverを構築する
        Observer(Impl &a_owner, std::string_view a_operationId) noexcept
            : m_owner(&a_owner), m_operationId(a_operationId)
        {
        }

        /// @brief 現在OperationのActive Stageを安全に更新する
        void on_stage_started(BuildStage a_stage) noexcept override
        {
            m_owner->stage_started(m_operationId, a_stage);
        }

        /// @brief 完了Stageの出力を現在OperationのLogへ関連付ける
        void on_stage_completed(const CMakeStageRecord &a_record) noexcept override
        {
            m_owner->stage_completed(m_operationId, a_record);
        }

      private:
        Impl *m_owner;
        std::string m_operationId;
    };

    /// @brief 注入依存とOwner Threadを記録してIdle Service状態を準備する
    Impl(CMakeRunnerSettings a_settings, std::unique_ptr<ChildProcessRunner> a_processRunner,
         std::unique_ptr<BuildArtifactPublisher> a_artifactPublisher,
         std::unique_ptr<BuildInputLeaseProvider> a_inputLeaseProvider,
         const AssertContext &a_assertContext) noexcept
        : settings(std::move(a_settings)), processRunner(std::move(a_processRunner)),
          artifactPublisher(std::move(a_artifactPublisher)), inputLeaseProvider(std::move(a_inputLeaseProvider)),
          assertContext(&a_assertContext), ownerThread(std::this_thread::get_id())
    {
    }

    /// @brief Owner Thread限定操作を呼出Threadから検証する
    [[nodiscard]] bool is_owner_thread() const noexcept
    {
        return std::this_thread::get_id() == ownerThread;
    }

    /// @brief 一致する実行中OperationだけActive Stageを更新する
    void stage_started(std::string_view a_operationId, BuildStage a_stage) noexcept
    {
        try
        {
            std::scoped_lock lock(mutex);
            if (current.state == GameBuildOperationState::Running && current.operationId == a_operationId)
            {
                current.activeStage = a_stage;
            }
        }
        catch (...)
        {
            terminate_service_exception(*assertContext);
        }
    }

    /// @brief 一致する実行中Operationだけ完了StageのProcess出力を記録する
    void stage_completed(std::string_view a_operationId, const CMakeStageRecord &a_record) noexcept
    {
        try
        {
            std::scoped_lock lock(mutex);
            if (current.state != GameBuildOperationState::Running || current.operationId != a_operationId)
            {
                return;
            }
            current.stages.push_back({a_record.result.stage(), a_record.result.outcome(), a_record.result.exit_code()});
            for (const ChildProcessOutputChunk &chunk : a_record.output)
            {
                current.logs.push_back(
                    {std::string(a_operationId), a_record.result.stage(), chunk.sequence, chunk.stream, chunk.bytes});
            }
        }
        catch (...)
        {
            terminate_service_exception(*assertContext);
        }
    }

    /// @brief Error Chainを診断へ変換して一致するOperationを失敗状態へ確定する
    void finish_error(std::string_view a_operationId, const Error &a_error) noexcept
    {
        try
        {
            std::vector<BuildDiagnosticSnapshot> diagnostics = flatten_error(a_error);
            std::scoped_lock lock(mutex);
            if (current.state == GameBuildOperationState::Running && current.operationId == a_operationId)
            {
                current.activeStage.reset();
                current.diagnostics = std::move(diagnostics);
                current.state =
                    is_publisher_timeout(a_error) ? GameBuildOperationState::TimedOut : GameBuildOperationState::Failed;
            }
        }
        catch (...)
        {
            terminate_service_exception(*assertContext);
        }
    }

    /// @brief 一致するOperationをArtifact更新なしの取消状態へ確定する
    void finish_cancelled(std::string_view a_operationId) noexcept
    {
        try
        {
            std::scoped_lock lock(mutex);
            if (current.state == GameBuildOperationState::Running && current.operationId == a_operationId)
            {
                current.activeStage.reset();
                current.state = GameBuildOperationState::Cancelled;
            }
        }
        catch (...)
        {
            terminate_service_exception(*assertContext);
        }
    }

    /// @brief Runner結果と公開Artifactから一致するOperationの終端状態を確定する
    void finish_result(std::string_view a_operationId, const CMakeBuildResult &a_result,
                       std::optional<BuildArtifactInventory> a_artifact) noexcept
    {
        try
        {
            GameBuildOperationState state = GameBuildOperationState::Failed;
            if (a_result.succeeded() && a_artifact)
            {
                state = GameBuildOperationState::Succeeded;
            }
            else if (!a_result.stages().empty())
            {
                switch (a_result.stages().back().result.outcome())
                {
                case BuildStageOutcome::Succeeded:
                case BuildStageOutcome::Failed:
                    state = GameBuildOperationState::Failed;
                    break;
                case BuildStageOutcome::Cancelled:
                    state = GameBuildOperationState::Cancelled;
                    break;
                case BuildStageOutcome::TimedOut:
                    state = GameBuildOperationState::TimedOut;
                    break;
                }
            }
            std::scoped_lock lock(mutex);
            if (current.state != GameBuildOperationState::Running || current.operationId != a_operationId)
            {
                return;
            }
            current.activeStage.reset();
            current.state = state;
            if (a_artifact)
            {
                latestSuccessful = *a_artifact;
                current.artifact = std::move(*a_artifact);
                current.latestSuccessfulArtifact = latestSuccessful;
            }
        }
        catch (...)
        {
            terminate_service_exception(*assertContext);
        }
    }

    /// @brief ConfigureとBuildを順に実行し、成功時だけArtifactを公開する
    void execute(BuildPlan a_plan, std::string a_operationId, CMakeConfigureMode a_configureMode,
                 ChildProcessCancellation &a_cancellation) noexcept
    {
        Observer observer(*this, a_operationId);
        std::unique_ptr<BuildInputLease> inputLease;
        if (inputLeaseProvider)
        {
            auto acquiredInput = inputLeaseProvider->acquire(a_plan, a_cancellation);
            if (!acquiredInput)
            {
                finish_error(a_operationId, *acquiredInput.try_error());
                return;
            }
            if (a_cancellation.is_cancel_requested())
            {
                finish_cancelled(a_operationId);
                return;
            }
            if (!*acquiredInput.try_value())
            {
                finish_error(a_operationId,
                             make_service_error(*assertContext, GameBuildServiceError::MissingDependency,
                                                "Build input lease provider returned no lease"));
                return;
            }
            inputLease = std::move(*acquiredInput.try_value());
        }
        static_cast<void>(inputLease);
        auto acquired = artifactPublisher->acquire_build_lease(a_plan, a_cancellation,
                                                               make_lock_deadline(settings.configureTimeout));
        if (!acquired)
        {
            finish_error(a_operationId, *acquired.try_error());
            return;
        }
        if (!acquired.try_value()->has_value())
        {
            finish_cancelled(a_operationId);
            return;
        }
        std::unique_ptr<BuildWorkspaceLease> buildLease = std::move(**acquired.try_value());
        const CMakeConfigureMode configureMode =
            inputLeaseProvider ? CMakeConfigureMode::Required : a_configureMode;
        auto built = run_cmake_build(a_plan, settings, configureMode, *processRunner, a_cancellation, observer,
                                     *assertContext);
        if (!built)
        {
            finish_error(a_operationId, *built.try_error());
            return;
        }
        if (!built.try_value()->succeeded())
        {
            finish_result(a_operationId, *built.try_value(), std::nullopt);
            return;
        }
        if (a_cancellation.is_cancel_requested())
        {
            finish_cancelled(a_operationId);
            return;
        }
        if (inputLease)
        {
            auto inputValid = inputLease->validate_before_artifact_publish(a_cancellation, *assertContext);
            if (!inputValid)
            {
                finish_error(a_operationId, *inputValid.try_error());
                return;
            }
            if (a_cancellation.is_cancel_requested())
            {
                finish_cancelled(a_operationId);
                return;
            }
        }
        auto published = artifactPublisher->publish(a_plan, a_cancellation, std::move(buildLease),
                                                    make_lock_deadline(settings.buildTimeout));
        if (!published)
        {
            ErrorCode code =
                ErrorCode::create(assertContext->fatal_handler(), "Cue.Build.Service",
                                  static_cast<std::int64_t>(GameBuildServiceError::ArtifactPublicationFailed));
            Error classified =
                Error::reclassify(assertContext->fatal_handler(), std::move(code), "Build artifact publication failed",
                                  std::move(*published.try_error()));
            finish_error(a_operationId, classified);
            return;
        }
        if (!published.try_value()->has_value())
        {
            finish_cancelled(a_operationId);
            return;
        }
        finish_result(a_operationId, *built.try_value(), std::move(**published.try_value()));
    }

    CMakeRunnerSettings settings;
    std::unique_ptr<ChildProcessRunner> processRunner;
    std::unique_ptr<BuildArtifactPublisher> artifactPublisher;
    std::unique_ptr<BuildInputLeaseProvider> inputLeaseProvider;
    const AssertContext *assertContext;
    std::thread::id ownerThread;
    mutable std::mutex mutex;
    std::thread worker;
    std::unique_ptr<ChildProcessCancellation> cancellation;
    BuildOperationSnapshot current;
    std::optional<BuildArtifactInventory> latestSuccessful;
    std::optional<BuildRequest> lastRequest;
};

GameBuildService::GameBuildService(std::unique_ptr<Impl> a_impl) noexcept : m_impl(std::move(a_impl))
{
}

GameBuildService::~GameBuildService()
{
    try
    {
        if (!m_impl)
        {
            return;
        }
        {
            std::scoped_lock lock(m_impl->mutex);
            if (m_impl->cancellation)
            {
                m_impl->cancellation->request_cancel();
            }
        }
        if (m_impl->worker.joinable())
        {
            m_impl->worker.join();
        }
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}

Result<std::unique_ptr<GameBuildService>> GameBuildService::create(
    CMakeRunnerSettings a_settings, std::unique_ptr<ChildProcessRunner> a_processRunner,
    std::unique_ptr<BuildArtifactPublisher> a_artifactPublisher, const AssertContext &a_assertContext) noexcept
{
    return create(std::move(a_settings), std::move(a_processRunner), std::move(a_artifactPublisher), nullptr,
                  a_assertContext);
}

Result<std::unique_ptr<GameBuildService>> GameBuildService::create(
    CMakeRunnerSettings a_settings, std::unique_ptr<ChildProcessRunner> a_processRunner,
    std::unique_ptr<BuildArtifactPublisher> a_artifactPublisher,
    std::unique_ptr<BuildInputLeaseProvider> a_inputLeaseProvider,
    const AssertContext &a_assertContext) noexcept
{
    try
    {
        if (!a_processRunner || !a_artifactPublisher)
        {
            return Result<std::unique_ptr<GameBuildService>>::failure(make_service_error(
                a_assertContext, GameBuildServiceError::MissingDependency, "Build service dependencies are missing"));
        }
        auto implementation =
            std::make_unique<Impl>(std::move(a_settings), std::move(a_processRunner),
                                   std::move(a_artifactPublisher), std::move(a_inputLeaseProvider), a_assertContext);
        return Result<std::unique_ptr<GameBuildService>>::success(
            std::unique_ptr<GameBuildService>(new GameBuildService(std::move(implementation))));
    }
    catch (...)
    {
        terminate_service_exception(a_assertContext);
    }
}

Result<void> GameBuildService::start(BuildRequest a_request, CMakeConfigureMode a_configureMode) noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                            GameBuildServiceError::OwnerThreadViolation,
                                                            "Build start requires owner thread"));
        }
        {
            std::scoped_lock lock(m_impl->mutex);
            if (m_impl->current.state == GameBuildOperationState::Running)
            {
                return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                                GameBuildServiceError::OperationAlreadyRunning,
                                                                "A build operation is already running"));
            }
        }
        if (m_impl->worker.joinable())
        {
            m_impl->worker.join();
        }
        auto plan = create_build_plan(a_request, *m_impl->assertContext);
        if (!plan)
        {
            return Result<void>::failure(std::move(*plan.try_error()));
        }
        auto cancellation = std::make_unique<ChildProcessCancellation>();
        ChildProcessCancellation *cancellationPointer = cancellation.get();
        const std::string operationId(a_request.operationId);
        {
            std::scoped_lock lock(m_impl->mutex);
            m_impl->lastRequest = a_request;
            m_impl->cancellation = std::move(cancellation);
            m_impl->current = {};
            m_impl->current.state = GameBuildOperationState::Running;
            m_impl->current.operationId = operationId;
            m_impl->current.profile = a_request.profile;
            m_impl->current.latestSuccessfulArtifact = m_impl->latestSuccessful;
        }
        m_impl->worker = std::thread(&Impl::execute, m_impl.get(), std::move(*plan.try_value()), operationId,
                                     a_configureMode, std::ref(*cancellationPointer));
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}

Result<void> GameBuildService::retry(std::string a_operationId) noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                            GameBuildServiceError::OwnerThreadViolation,
                                                            "Build retry requires owner thread"));
        }
        std::optional<BuildRequest> request;
        {
            std::scoped_lock lock(m_impl->mutex);
            if (m_impl->current.state == GameBuildOperationState::Running)
            {
                return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                                GameBuildServiceError::OperationAlreadyRunning,
                                                                "A build operation is already running"));
            }
            if (m_impl->lastRequest)
            {
                request = m_impl->lastRequest;
            }
        }
        if (!request)
        {
            return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                            GameBuildServiceError::NoRetryableOperation,
                                                            "No build operation is available for retry"));
        }
        request->operationId = std::move(a_operationId);
        return start(std::move(*request), CMakeConfigureMode::Required);
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}

Result<void> GameBuildService::request_cancel() noexcept
{
    try
    {
        std::scoped_lock lock(m_impl->mutex);
        if (m_impl->current.state != GameBuildOperationState::Running || !m_impl->cancellation)
        {
            return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                            GameBuildServiceError::NoActiveOperation,
                                                            "No active build operation can be cancelled"));
        }
        m_impl->cancellation->request_cancel();
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}

BuildOperationSnapshot GameBuildService::snapshot() const noexcept
{
    try
    {
        std::scoped_lock lock(m_impl->mutex);
        return m_impl->current;
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}

Result<void> GameBuildService::wait_for_completion() noexcept
{
    try
    {
        if (!m_impl->is_owner_thread())
        {
            return Result<void>::failure(make_service_error(*m_impl->assertContext,
                                                            GameBuildServiceError::OwnerThreadViolation,
                                                            "Build wait requires owner thread"));
        }
        if (m_impl->worker.joinable())
        {
            m_impl->worker.join();
        }
        return Result<void>::success();
    }
    catch (...)
    {
        terminate_service_exception(*m_impl->assertContext);
    }
}
} // namespace cue
