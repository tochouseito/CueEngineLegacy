#include <Cue/Package/RuntimeData.h>

#include "Sha256.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Math/Scalar.h>
#include <Cue/Package/Error.h>
#include <Cue/Renderer/RendererSchema.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{
/// @brief Package処理中の予期しない例外をFatal境界へ渡す
[[noreturn]] void terminate_package_exception(const cue::AssertContext &a_assertContext) noexcept
{
    a_assertContext.fatal_handler().terminate("Unexpected exception escaped Runtime Data publication");
}

/// @brief 16-byte Stable IDをlowercase UUID JSON Stringへ追加する
void append_uuid(std::string &a_output, std::span<const std::uint8_t, 16U> a_bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    a_output.push_back('"');
    std::size_t position = 0U;
    for (const std::uint8_t byte : a_bytes)
    {
        if (position == 4U || position == 6U || position == 8U || position == 10U)
        {
            a_output.push_back('-');
        }
        a_output.push_back(digits[(byte >> 4U) & 0x0fU]);
        a_output.push_back(digits[byte & 0x0fU]);
        ++position;
    }
    a_output.push_back('"');
}

/// @brief JSON StringをUTF-8 Byte列のまま必要なASCII文字だけEscapeして追加する
void append_json_string(std::string &a_output, std::string_view a_value)
{
    constexpr char digits[] = "0123456789abcdef";
    a_output.push_back('"');
    for (const unsigned char byte : a_value)
    {
        switch (byte)
        {
        case '"':
            a_output.append("\\\"");
            break;
        case '\\':
            a_output.append("\\\\");
            break;
        case '\b':
            a_output.append("\\b");
            break;
        case '\f':
            a_output.append("\\f");
            break;
        case '\n':
            a_output.append("\\n");
            break;
        case '\r':
            a_output.append("\\r");
            break;
        case '\t':
            a_output.append("\\t");
            break;
        default:
            if (byte < 0x20U)
            {
                a_output.append("\\u00");
                a_output.push_back(digits[byte >> 4U]);
                a_output.push_back(digits[byte & 0x0fU]);
            }
            else
            {
                a_output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    a_output.push_back('"');
}

/// @brief Numberをlocale非依存の最短Round-trip表現で追加する
template <typename Value> void append_number(std::string &a_output, Value a_value)
{
    std::array<char, 64U> buffer{};
    std::to_chars_result result{};
    if constexpr (std::is_floating_point_v<Value>)
    {
        result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), a_value, std::chars_format::general);
    }
    else
    {
        result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), a_value);
    }
    if (result.ec != std::errc{})
    {
        std::abort();
    }
    a_output.append(buffer.data(), result.ptr);
}

/// @brief Engine Versionをcanonical major.minor.patch JSON Stringへ追加する
void append_engine_version(std::string &a_output, const cue::EngineVersion &a_version)
{
    a_output.push_back('"');
    append_number(a_output, a_version.major);
    a_output.push_back('.');
    append_number(a_output, a_version.minor);
    a_output.push_back('.');
    append_number(a_output, a_version.patch);
    a_output.push_back('"');
}

/// @brief Transformを固定Member順のRuntime JSONへ追加する
void append_transform(std::string &a_output, const cue::math::Transform &a_transform)
{
    const cue::math::Vector3 translation = a_transform.translation();
    const cue::math::Quaternion rotation = a_transform.rotation();
    const cue::math::Vector3 scale = a_transform.scale();
    a_output.append("{\"translation\":[");
    append_number(a_output, translation.x);
    a_output.push_back(',');
    append_number(a_output, translation.y);
    a_output.push_back(',');
    append_number(a_output, translation.z);
    a_output.append("],\"rotation\":[");
    append_number(a_output, rotation.x);
    a_output.push_back(',');
    append_number(a_output, rotation.y);
    a_output.push_back(',');
    append_number(a_output, rotation.z);
    a_output.push_back(',');
    append_number(a_output, rotation.w);
    a_output.append("],\"scale\":[");
    append_number(a_output, scale.x);
    a_output.push_back(',');
    append_number(a_output, scale.y);
    a_output.push_back(',');
    append_number(a_output, scale.z);
    a_output.append("]}");
}

/// @brief 検証済みRenderer Componentを固定Member順のRuntime Scene v2へ追加する
void append_component(std::string &a_output, const cue::scene::KnownComponentData &a_component)
{
    a_output.append("{\"instanceId\":");
    append_uuid(a_output, a_component.instance_id().bytes());
    a_output.append(",\"typeId\":");
    append_uuid(a_output, a_component.type_id().bytes());
    a_output.append(",\"schemaVersion\":");
    append_number(a_output, a_component.schema_version().value());
    a_output.append(",\"fields\":[");
    bool firstField = true;
    for (const cue::scene::KnownFieldData &field : a_component.known_fields())
    {
        if (!firstField)
        {
            a_output.push_back(',');
        }
        firstField = false;
        a_output.append("{\"fieldId\":");
        append_number(a_output, field.id().value());
        a_output.append(",\"value\":");
        if (const bool *booleanValue = field.value().try_boolean(); booleanValue != nullptr)
        {
            a_output.append(*booleanValue ? "true" : "false");
        }
        else if (const double *floatingValue = field.value().try_floating_point(); floatingValue != nullptr)
        {
            append_number(a_output, *floatingValue);
        }
        else
        {
            append_json_string(a_output, field.value().try_asset_reference()->token());
        }
        a_output.push_back('}');
    }
    a_output.append("]}");
}

/// @brief Project DescriptorからCanonical Runtime Project Dataを生成する
[[nodiscard]] std::string make_project_data(const cue::ProjectDescriptor &a_descriptor, std::string_view a_sceneAssetId)
{
    std::string output;
    output.reserve(256U);
    output.append("{\"schemaVersion\":");
    append_number(output, cue::package::k_runtimeProjectDataSchemaVersion);
    output.append(",\"projectId\":");
    append_json_string(output, a_descriptor.project_id().text());
    output.append(",\"engineCompatibility\":{\"minimum\":");
    append_engine_version(output, a_descriptor.engine_compatibility().minimum);
    output.append(",\"maximumExclusive\":");
    if (a_descriptor.engine_compatibility().maximumExclusive.has_value())
    {
        append_engine_version(output, *a_descriptor.engine_compatibility().maximumExclusive);
    }
    else
    {
        output.append("null");
    }
    output.append("},\"requiredCapabilities\":[],\"startupSceneAssetId\":");
    append_json_string(output, a_sceneAssetId);
    output.append("}\n");
    return output;
}

/// @brief 不変Scene SnapshotからCanonical Runtime Scene Dataを生成する
[[nodiscard]] cue::Result<std::string> make_scene_data(const cue::scene::SceneSnapshot &a_scene,
                                                       const cue::AssertContext &a_assertContext) noexcept
{
    try
    {
        bool hasComponents = false;
        std::size_t cubeCount = 0U;
        std::set<cue::scene::ComponentInstanceId> componentIds;
        for (const cue::scene::SceneObject &object : a_scene.objects())
        {
            const auto components = object.components();
            hasComponents = hasComponents || !components.empty();
            if (components.size() > 2U)
            {
                return cue::Result<std::string>::failure(cue::package::make_package_error(
                    a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
                    "Runtime Scene v2 permits at most two components per object"));
            }
            for (std::size_t index = 0U; index < components.size(); ++index)
            {
                const cue::scene::SceneComponent &component = components[index];
                auto validated = cue::renderer::validate_runtime_scene_component(component, a_assertContext);
                if (!validated)
                {
                    return cue::Result<std::string>::failure(cue::package::make_package_error(
                        a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
                        "Runtime Scene contains an unsupported Renderer component"));
                }
                if (!componentIds.insert(component.instance_id()).second)
                {
                    return cue::Result<std::string>::failure(cue::package::make_package_error(
                        a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
                        "Runtime Scene component identities must be unique across the Scene"));
                }
                const cue::scene::KnownComponentData *known = component.try_known();
                if (index > 0U &&
                    (!(components[index - 1U].instance_id() < component.instance_id()) ||
                     components[index - 1U].try_known()->type_id() == known->type_id()))
                {
                    return cue::Result<std::string>::failure(cue::package::make_package_error(
                        a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
                        "Runtime Scene components must have unique types and ordered identities"));
                }
                auto ids = cue::renderer::make_renderer_schema_type_ids(a_assertContext);
                if (!ids)
                {
                    return cue::Result<std::string>::failure(std::move(*ids.try_error()));
                }
                if (known->type_id() == ids.try_value()->mesh)
                {
                    ++cubeCount;
                    if (cubeCount > 4096U)
                    {
                        return cue::Result<std::string>::failure(cue::package::make_package_error(
                            a_assertContext, cue::package::PackageError::RuntimeDataResourceLimitExceeded,
                            "Runtime Scene Cube count exceeds the v2 limit"));
                    }
                }
            }
        }
        if (hasComponents && a_scene.objects().size() > 4096U)
        {
            return cue::Result<std::string>::failure(cue::package::make_package_error(
                a_assertContext, cue::package::PackageError::RuntimeDataResourceLimitExceeded,
                "Runtime Scene v2 object count exceeds the supported limit"));
        }
        const cue::Result<cue::math::Tolerance> runtimeTolerance =
            cue::math::Tolerance::create(a_assertContext.fatal_handler(), 0.00001F, 0.00001F);
        if (!runtimeTolerance)
        {
            return cue::Result<std::string>::failure(cue::package::make_package_error(
                a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
                "Runtime Transform tolerance could not be initialized"));
        }

        std::string output;
        output.reserve(512U);
        output.append("{\"schemaVersion\":");
        append_number(output, hasComponents ? cue::package::k_runtimeSceneDataWithRendererSchemaVersion
                                            : cue::package::k_runtimeSceneDataSchemaVersion);
        output.append(",\"sceneAssetId\":");
        append_uuid(output, a_scene.scene_asset_id().bytes());
        output.append(",\"objects\":[");
        bool firstObject = true;
        for (const cue::scene::SceneObject &object : a_scene.objects())
        {
            const cue::math::Transform &transform = object.transform();
            if (!cue::math::is_finite(transform.translation()) || !cue::math::is_finite(transform.scale()) ||
                !cue::math::is_unit_rotation(transform.rotation(), *runtimeTolerance.try_value()))
            {
                return cue::Result<std::string>::failure(cue::package::make_package_error(
                    a_assertContext, cue::package::PackageError::UnsupportedRuntimeSceneData,
                    "Runtime Transform must contain finite values and a unit rotation"));
            }
            if (!firstObject)
            {
                output.push_back(',');
            }
            firstObject = false;
            output.append("{\"objectId\":");
            append_uuid(output, object.id().bytes());
            output.append(",\"parentObjectId\":");
            if (object.try_parent_id() != nullptr)
            {
                append_uuid(output, object.try_parent_id()->bytes());
            }
            else
            {
                output.append("null");
            }
            output.append(",\"active\":");
            output.append(object.is_active() ? "true" : "false");
            output.append(",\"transform\":");
            append_transform(output, transform);
            output.append(",\"components\":[");
            bool firstComponent = true;
            for (const cue::scene::SceneComponent &component : object.components())
            {
                if (!firstComponent)
                {
                    output.push_back(',');
                }
                firstComponent = false;
                append_component(output, *component.try_known());
            }
            output.append("]}");
            if (output.size() > cue::package::k_maximumRuntimeSceneDataBytes)
            {
                return cue::Result<std::string>::failure(cue::package::make_package_error(
                    a_assertContext, cue::package::PackageError::RuntimeDataResourceLimitExceeded,
                    "Runtime Scene Data exceeds its byte limit"));
            }
        }
        output.append("]}\n");
        if (output.size() > cue::package::k_maximumRuntimeSceneDataBytes)
        {
            return cue::Result<std::string>::failure(cue::package::make_package_error(
                a_assertContext, cue::package::PackageError::RuntimeDataResourceLimitExceeded,
                "Runtime Scene Data exceeds its byte limit"));
        }
        return cue::Result<std::string>::success(std::move(output));
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}

/// @brief Byte列のSHA-256 Digestをlowercase hexadecimalへ変換する
[[nodiscard]] std::string make_sha256(std::string_view a_bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    const auto bytes = std::as_bytes(std::span(a_bytes.data(), a_bytes.size()));
    const cue::package_private::Sha256Digest digest = cue::package_private::compute_sha256(bytes);
    std::string output;
    output.reserve(digest.size() * 2U);
    for (const std::uint8_t byte : digest)
    {
        output.push_back(digits[(byte >> 4U) & 0x0fU]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}
} // namespace

namespace cue::package
{
RuntimeDataFile::RuntimeDataFile(std::string a_relativePath, std::string a_bytes, std::string a_sha256) noexcept
    : m_relativePath(std::move(a_relativePath)), m_bytes(std::move(a_bytes)), m_sha256(std::move(a_sha256))
{
}

std::string_view RuntimeDataFile::relative_path() const noexcept
{
    return m_relativePath;
}

std::string_view RuntimeDataFile::bytes() const noexcept
{
    return m_bytes;
}

std::uint64_t RuntimeDataFile::byte_size() const noexcept
{
    return static_cast<std::uint64_t>(m_bytes.size());
}

std::string_view RuntimeDataFile::sha256() const noexcept
{
    return m_sha256;
}

MinimalRuntimeDataPublication::MinimalRuntimeDataPublication(RuntimeDataFile a_projectData,
                                                             RuntimeDataFile a_startupSceneData,
                                                             std::string a_projectId,
                                                             std::string a_startupSceneAssetId) noexcept
    : m_projectData(std::move(a_projectData)), m_startupSceneData(std::move(a_startupSceneData)),
      m_projectId(std::move(a_projectId)), m_startupSceneAssetId(std::move(a_startupSceneAssetId))
{
}

const RuntimeDataFile &MinimalRuntimeDataPublication::project_data() const noexcept
{
    return m_projectData;
}

const RuntimeDataFile &MinimalRuntimeDataPublication::startup_scene_data() const noexcept
{
    return m_startupSceneData;
}

std::string_view MinimalRuntimeDataPublication::project_id() const noexcept
{
    return m_projectId;
}

std::string_view MinimalRuntimeDataPublication::startup_scene_asset_id() const noexcept
{
    return m_startupSceneAssetId;
}

Result<MinimalRuntimeDataPublication> publish_minimal_runtime_data(const ProjectDescriptor &a_descriptor,
                                                                   const scene::SceneSnapshot &a_startupScene,
                                                                   const AssertContext &a_assertContext) noexcept
{
    try
    {
        Result<void> descriptorValidation = validate_project_descriptor(a_descriptor, a_assertContext);
        if (!descriptorValidation)
        {
            return Result<MinimalRuntimeDataPublication>::failure(std::move(*descriptorValidation.try_error()));
        }
        if (!a_descriptor.default_scene().has_value())
        {
            return Result<MinimalRuntimeDataPublication>::failure(make_package_error(
                a_assertContext, PackageError::MissingStartupScene, "Project Descriptor has no startup scene"));
        }

        const scene::IdentityText sceneIdentityText = a_startupScene.scene_asset_id().canonical_text();
        const std::string sceneAssetId(sceneIdentityText.data(), sceneIdentityText.size());
        if (sceneAssetId != a_descriptor.default_scene()->scene_asset_id())
        {
            return Result<MinimalRuntimeDataPublication>::failure(
                make_package_error(a_assertContext, PackageError::StartupSceneIdentityMismatch,
                                   "Startup Scene identity differs from the Project Descriptor"));
        }

        std::string projectData = make_project_data(a_descriptor, sceneAssetId);
        if (projectData.size() > k_maximumRuntimeProjectDataBytes)
        {
            return Result<MinimalRuntimeDataPublication>::failure(
                make_package_error(a_assertContext, PackageError::RuntimeDataResourceLimitExceeded,
                                   "Runtime Project Data exceeds its byte limit"));
        }
        Result<std::string> sceneData = make_scene_data(a_startupScene, a_assertContext);
        if (!sceneData)
        {
            return Result<MinimalRuntimeDataPublication>::failure(std::move(*sceneData.try_error()));
        }

        std::string scenePath = "Data/Scenes/";
        scenePath.append(sceneAssetId);
        scenePath.append(".cueruntime.json");
        std::string projectHash = make_sha256(projectData);
        RuntimeDataFile projectFile("Data/CueProject.runtime.json", std::move(projectData), std::move(projectHash));
        std::string sceneHash = make_sha256(*sceneData.try_value());
        RuntimeDataFile sceneFile(std::move(scenePath), std::move(*sceneData.try_value()), std::move(sceneHash));
        std::string projectId(a_descriptor.project_id().text());
        return Result<MinimalRuntimeDataPublication>::success(MinimalRuntimeDataPublication(
            std::move(projectFile), std::move(sceneFile), std::move(projectId), std::move(sceneAssetId)));
    }
    catch (...)
    {
        terminate_package_exception(a_assertContext);
    }
}
} // namespace cue::package
