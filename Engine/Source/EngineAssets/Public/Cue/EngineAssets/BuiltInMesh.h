#pragma once

#include <Cue/Math/Vector.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace cue::engine_assets
{
/// Built-in Cube MeshをProject File Pathから独立して識別するStable Asset ID
inline constexpr std::string_view k_cubeMeshAssetId = "cue://engine/mesh/cube";
/// Built-in Cube Geometryの互換Revision
inline constexpr std::uint32_t k_cubeMeshRevision = 1U;
/// Built-in Plane MeshをProject File Pathから独立して識別するStable Asset ID
inline constexpr std::string_view k_planeMeshAssetId = "cue://engine/mesh/plane";
/// Built-in Plane Geometryの互換Revision
inline constexpr std::uint32_t k_planeMeshRevision = 1U;
/// Built-in Sphere MeshをProject File Pathから独立して識別するStable Asset ID
inline constexpr std::string_view k_sphereMeshAssetId = "cue://engine/mesh/sphere";
/// Built-in Sphere Geometryの互換Revision
inline constexpr std::uint32_t k_sphereMeshRevision = 1U;

/// @brief CPU側のBuilt-in Meshが保持する最小Vertex Data
struct MeshVertex final
{
    /// World Space規約に従うLocal Position
    math::Vector3 position;
    /// Face外側を向く単位長Local Normal
    math::Vector3 normal;
};

/// @brief Engine所有の不変なBuilt-in Mesh Dataへの非所有View
///
/// Viewと参照先はProcess終了まで有効であり、複数Threadから同時にReadできる。
struct MeshView final
{
    /// File Pathに依存しないBuilt-in Asset Identity
    std::string_view assetId;
    /// Geometry Contractの互換Revision
    std::uint32_t revision;
    /// Engine所有Vertex Storageへの非所有View
    std::span<const MeshVertex> vertices;
    /// Engine所有Triangle Index Storageへの非所有View
    std::span<const std::uint16_t> indices;
};

/// @brief 原点中心で一辺1mのEngine所有Cube GeometryをAllocationなしで返す
[[nodiscard]] MeshView built_in_cube_mesh() noexcept;

/// @brief 原点中心で一辺1m、+Yを表とするEngine所有XZ Plane GeometryをAllocationなしで返す
[[nodiscard]] MeshView built_in_plane_mesh() noexcept;

/// @brief 原点中心で直径1m、16 Slice／8 StackのEngine所有Sphere GeometryをAllocationなしで返す
[[nodiscard]] MeshView built_in_sphere_mesh() noexcept;
} // namespace cue::engine_assets
