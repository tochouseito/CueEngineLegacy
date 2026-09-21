#include <Cue/EngineAssets/BuiltInAssetCatalog.h>

#include <Cue/EngineAssets/Error.h>

#include <array>
#include <cmath>
#include <utility>

namespace cue::engine_assets
{
namespace
{
constexpr std::string_view k_engineAssetPrefix = "cue://engine/";

constexpr std::array k_meshDescriptors = {
    BuiltInMeshDescriptor{
        k_cubeMeshAssetId,
        BuiltInAssetKind::Mesh,
        k_cubeMeshRevision,
        "Cube",
        BuiltInMeshBounds{{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}},
        BuiltInMeshCapability::PositionNormal,
    },
};

struct AssetIdParts final
{
    std::string_view kind;
    std::string_view name;
};

/// @brief SegmentがBuilt-in Asset IDの文字数とASCII規則を満たす場合にtrueを返す
[[nodiscard]] bool is_canonical_segment(std::string_view a_segment) noexcept
{
    if (a_segment.empty() || a_segment.size() > 64U || a_segment.back() == '-')
    {
        return false;
    }

    if (a_segment.front() < 'a' || a_segment.front() > 'z')
    {
        return false;
    }

    for (const char character : a_segment.substr(1U))
    {
        const bool isLetter = character >= 'a' && character <= 'z';
        const bool isDigit = character >= '0' && character <= '9';
        if (!isLetter && !isDigit && character != '-')
        {
            return false;
        }
    }

    return true;
}

/// @brief Canonical Engine Asset IDをAllocationなしでKindとNameへ分解する
[[nodiscard]] bool try_parse_asset_id(std::string_view a_assetId, AssetIdParts &a_parts) noexcept
{
    if (!a_assetId.starts_with(k_engineAssetPrefix))
    {
        return false;
    }

    const std::string_view suffix = a_assetId.substr(k_engineAssetPrefix.size());
    const std::size_t separator = suffix.find('/');
    if (separator == std::string_view::npos || suffix.find('/', separator + 1U) != std::string_view::npos)
    {
        return false;
    }

    const std::string_view kind = suffix.substr(0U, separator);
    const std::string_view name = suffix.substr(separator + 1U);
    if (!is_canonical_segment(kind) || !is_canonical_segment(name))
    {
        return false;
    }

    a_parts = AssetIdParts{kind, name};
    return true;
}

/// @brief Boundsが有限かつ各Axisで昇順の場合にtrueを返す
[[nodiscard]] bool is_valid_bounds(const BuiltInMeshBounds &a_bounds) noexcept
{
    const std::array values = {
        a_bounds.minimum.x, a_bounds.minimum.y, a_bounds.minimum.z,
        a_bounds.maximum.x, a_bounds.maximum.y, a_bounds.maximum.z,
    };
    for (const float value : values)
    {
        if (!std::isfinite(value))
        {
            return false;
        }
    }

    return a_bounds.minimum.x <= a_bounds.maximum.x && a_bounds.minimum.y <= a_bounds.maximum.y &&
           a_bounds.minimum.z <= a_bounds.maximum.z;
}
} // namespace

bool is_engine_asset_namespace(std::string_view a_assetId) noexcept
{
    return a_assetId.starts_with(k_engineAssetPrefix);
}

std::span<const BuiltInMeshDescriptor> built_in_mesh_catalog() noexcept
{
    return k_meshDescriptors;
}

Result<void> validate_builtin_mesh_catalog(std::span<const BuiltInMeshDescriptor> a_descriptors,
                                           const AssertContext &a_assertContext) noexcept
{
    for (std::size_t descriptorIndex = 0U; descriptorIndex < a_descriptors.size(); ++descriptorIndex)
    {
        const BuiltInMeshDescriptor &descriptor = a_descriptors[descriptorIndex];
        AssetIdParts parts{};
        if (!try_parse_asset_id(descriptor.assetId, parts))
        {
            return Result<void>::failure(
                make_engine_assets_error(a_assertContext, EngineAssetsError::InvalidAssetId,
                                         "Built-in Mesh descriptor contains a non-canonical Asset ID"));
        }
        if (parts.kind != "mesh" || descriptor.kind != BuiltInAssetKind::Mesh)
        {
            return Result<void>::failure(
                make_engine_assets_error(a_assertContext, EngineAssetsError::KindMismatch,
                                         "Built-in Mesh descriptor uses a non-Mesh Asset Kind"));
        }
        if (descriptor.revision == 0U)
        {
            return Result<void>::failure(
                make_engine_assets_error(a_assertContext, EngineAssetsError::InvalidRevision,
                                         "Built-in Mesh descriptor Geometry Revision must be non-zero"));
        }
        if (descriptor.displayName.empty() || !is_valid_bounds(descriptor.bounds))
        {
            return Result<void>::failure(
                make_engine_assets_error(a_assertContext, EngineAssetsError::InvalidDescriptor,
                                         "Built-in Mesh descriptor Display Name or Bounds is invalid"));
        }

        for (std::size_t priorIndex = 0U; priorIndex < descriptorIndex; ++priorIndex)
        {
            if (a_descriptors[priorIndex].assetId == descriptor.assetId)
            {
                return Result<void>::failure(
                    make_engine_assets_error(a_assertContext, EngineAssetsError::DuplicateAssetId,
                                             "Built-in Mesh Catalog contains a duplicate Stable Asset ID"));
            }
        }
    }

    return Result<void>::success();
}

Result<const BuiltInMeshDescriptor *> resolve_builtin_mesh_descriptor(std::string_view a_assetId,
                                                                      const AssertContext &a_assertContext) noexcept
{
    AssetIdParts parts{};
    if (!try_parse_asset_id(a_assetId, parts))
    {
        return Result<const BuiltInMeshDescriptor *>::failure(
            make_engine_assets_error(a_assertContext, EngineAssetsError::InvalidAssetId,
                                     "Built-in Mesh resolution requires a canonical Engine Asset ID"));
    }
    if (parts.kind != "mesh")
    {
        return Result<const BuiltInMeshDescriptor *>::failure(make_engine_assets_error(
            a_assertContext, EngineAssetsError::KindMismatch, "Built-in Asset ID does not identify a Mesh"));
    }

    for (const BuiltInMeshDescriptor &descriptor : k_meshDescriptors)
    {
        if (descriptor.assetId == a_assetId)
        {
            return Result<const BuiltInMeshDescriptor *>::success(&descriptor);
        }
    }

    return Result<const BuiltInMeshDescriptor *>::failure(
        make_engine_assets_error(a_assertContext, EngineAssetsError::UnknownAsset,
                                 "Built-in Mesh Asset ID is not present in the Engine Catalog"));
}

Result<MeshView> resolve_builtin_mesh(std::string_view a_assetId, const AssertContext &a_assertContext) noexcept
{
    Result<const BuiltInMeshDescriptor *> descriptor = resolve_builtin_mesh_descriptor(a_assetId, a_assertContext);
    if (!descriptor)
    {
        return Result<MeshView>::failure(std::move(*descriptor.try_error()));
    }

    if ((*descriptor.try_value())->assetId == k_cubeMeshAssetId)
    {
        return Result<MeshView>::success(built_in_cube_mesh());
    }

    return Result<MeshView>::failure(make_engine_assets_error(a_assertContext, EngineAssetsError::PayloadUnavailable,
                                                              "Built-in Mesh descriptor has no Geometry Payload"));
}
} // namespace cue::engine_assets
