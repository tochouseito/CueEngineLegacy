#pragma once

#include <Cue/EngineAssets/BuiltInMesh.h>

#include <Cue/Foundation/Result.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::engine_assets
{
/// @brief Engine所有Assetを万能基底型へ統合せず識別するKind
enum class BuiltInAssetKind : std::uint8_t
{
    Mesh
};

/// @brief Built-in Meshが提供するRevision 1のVertex Capability
enum class BuiltInMeshCapability : std::uint8_t
{
    PositionNormal
};

/// @brief Local Space内のBuilt-in Mesh Axis-aligned Bounds
struct BuiltInMeshBounds final
{
    math::Vector3 minimum;
    math::Vector3 maximum;
};

/// @brief Engine所有MeshのIdentityと互換Geometry Contractを表す値型
struct BuiltInMeshDescriptor final
{
    /// File Pathから独立したEngine予約Namespace内のStable ID
    std::string_view assetId;
    /// Typed Resolverが受理するAsset Kind
    BuiltInAssetKind kind;
    /// Stable IDへ不変に対応するGeometry Revision
    std::uint32_t revision;
    /// Editor表示用で永続Identityに使用しない名称
    std::string_view displayName;
    /// Revision Contractに含まれるLocal Bounds
    BuiltInMeshBounds bounds;
    /// Revision Contractが提供するVertex Attribute
    BuiltInMeshCapability capability;
};

/// @brief TokenがEngine所有Assetの予約Namespaceを要求する場合にtrueを返す
[[nodiscard]] bool is_engine_asset_namespace(std::string_view a_assetId) noexcept;

/// @brief Canonical Catalog Descriptorを不変かつProcess寿命のViewとして返す
///
/// 呼び出し側Descriptorを登録または差し替えるAPIは提供しない。
[[nodiscard]] std::span<const BuiltInMeshDescriptor> built_in_mesh_catalog() noexcept;

/// @brief Descriptor集合のCanonical ID、Kind、Revision、Bounds、重複を検証する
[[nodiscard]] Result<void> validate_builtin_mesh_catalog(std::span<const BuiltInMeshDescriptor> a_descriptors,
                                                         const AssertContext &a_assertContext) noexcept;

/// @brief Canonical IDをTyped Mesh Catalogから解決し不変Descriptorを返す
///
/// 成功時のPointerは非所有であり、Process終了まで有効なCanonical Catalog要素を指す。
/// Catalogは不変であるため、複数Threadから同じPointerを同時にReadできる。
[[nodiscard]] Result<const BuiltInMeshDescriptor *> resolve_builtin_mesh_descriptor(
    std::string_view a_assetId, const AssertContext &a_assertContext) noexcept;

/// @brief Canonical IDをProcess寿命のAllocationなしMesh Viewへ解決する
[[nodiscard]] Result<MeshView> resolve_builtin_mesh(std::string_view a_assetId,
                                                    const AssertContext &a_assertContext) noexcept;
} // namespace cue::engine_assets
