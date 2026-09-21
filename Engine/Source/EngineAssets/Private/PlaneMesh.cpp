#include <Cue/EngineAssets/BuiltInMesh.h>

#include <array>

namespace cue::engine_assets
{
namespace
{
constexpr float k_halfExtent = 0.5F;

constexpr std::array<MeshVertex, 4U> k_vertices = {
    MeshVertex{{-k_halfExtent, 0.0F, -k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, 0.0F, k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, 0.0F, k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, 0.0F, -k_halfExtent}, {0.0F, 1.0F, 0.0F}},
};

constexpr std::array<std::uint16_t, 6U> k_indices = {0U, 1U, 2U, 0U, 2U, 3U};
} // namespace

MeshView built_in_plane_mesh() noexcept
{
    return MeshView{k_planeMeshAssetId, k_planeMeshRevision, k_vertices, k_indices};
}

BuiltInMeshProvider built_in_plane_mesh_provider() noexcept
{
    return BuiltInMeshProvider{k_planeMeshAssetId, k_planeMeshRevision, &built_in_plane_mesh};
}
} // namespace cue::engine_assets
