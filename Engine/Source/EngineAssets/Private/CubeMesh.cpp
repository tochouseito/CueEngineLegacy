#include <Cue/EngineAssets/BuiltInMesh.h>

#include <array>

namespace cue::engine_assets
{
namespace
{
constexpr float k_halfExtent = 0.5F;

constexpr std::array<MeshVertex, 24U> k_vertices = {
    MeshVertex{{k_halfExtent, -k_halfExtent, -k_halfExtent}, {1.0F, 0.0F, 0.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, -k_halfExtent}, {1.0F, 0.0F, 0.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, k_halfExtent}, {1.0F, 0.0F, 0.0F}},
    MeshVertex{{k_halfExtent, -k_halfExtent, k_halfExtent}, {1.0F, 0.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, -k_halfExtent, k_halfExtent}, {-1.0F, 0.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, k_halfExtent, k_halfExtent}, {-1.0F, 0.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, k_halfExtent, -k_halfExtent}, {-1.0F, 0.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, -k_halfExtent, -k_halfExtent}, {-1.0F, 0.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, k_halfExtent, -k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, k_halfExtent, k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, -k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, -k_halfExtent, k_halfExtent}, {0.0F, -1.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, -k_halfExtent, -k_halfExtent}, {0.0F, -1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, -k_halfExtent, -k_halfExtent}, {0.0F, -1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, -k_halfExtent, k_halfExtent}, {0.0F, -1.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, -k_halfExtent, k_halfExtent}, {0.0F, 0.0F, 1.0F}},
    MeshVertex{{k_halfExtent, -k_halfExtent, k_halfExtent}, {0.0F, 0.0F, 1.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, k_halfExtent}, {0.0F, 0.0F, 1.0F}},
    MeshVertex{{-k_halfExtent, k_halfExtent, k_halfExtent}, {0.0F, 0.0F, 1.0F}},
    MeshVertex{{k_halfExtent, -k_halfExtent, -k_halfExtent}, {0.0F, 0.0F, -1.0F}},
    MeshVertex{{-k_halfExtent, -k_halfExtent, -k_halfExtent}, {0.0F, 0.0F, -1.0F}},
    MeshVertex{{-k_halfExtent, k_halfExtent, -k_halfExtent}, {0.0F, 0.0F, -1.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, -k_halfExtent}, {0.0F, 0.0F, -1.0F}},
};

constexpr std::array<std::uint16_t, 36U> k_indices = {
    0U,  1U,  2U,  0U,  2U,  3U,  4U,  5U,  6U,  4U,  6U,  7U,  8U,  9U,  10U, 8U,  10U, 11U,
    12U, 13U, 14U, 12U, 14U, 15U, 16U, 17U, 18U, 16U, 18U, 19U, 20U, 21U, 22U, 20U, 22U, 23U,
};
} // namespace

MeshView built_in_cube_mesh() noexcept
{
    return MeshView{k_cubeMeshAssetId, k_cubeMeshRevision, k_vertices, k_indices};
}

BuiltInMeshProvider built_in_cube_mesh_provider() noexcept
{
    return BuiltInMeshProvider{k_cubeMeshAssetId, k_cubeMeshRevision, &built_in_cube_mesh};
}
} // namespace cue::engine_assets
