#include <Cue/EngineAssets/BuiltInAssetCatalog.h>

#include <array>

namespace cue::engine_assets
{
namespace
{
constexpr std::array k_authoringMeshProviders = {
    BuiltInMeshProvider{k_cubeMeshAssetId, k_cubeMeshRevision, &built_in_cube_mesh},
    BuiltInMeshProvider{k_planeMeshAssetId, k_planeMeshRevision, &built_in_plane_mesh},
    BuiltInMeshProvider{k_sphereMeshAssetId, k_sphereMeshRevision, &built_in_sphere_mesh},
};
} // namespace

std::span<const BuiltInMeshProvider> built_in_authoring_mesh_providers() noexcept
{
    return k_authoringMeshProviders;
}
} // namespace cue::engine_assets
