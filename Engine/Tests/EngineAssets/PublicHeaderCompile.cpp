#include <Cue/EngineAssets/BuiltInAssetCatalog.h>
#include <Cue/EngineAssets/BuiltInMesh.h>
#include <Cue/EngineAssets/Error.h>

#include <type_traits>

static_assert(std::is_standard_layout_v<cue::engine_assets::MeshVertex>);
static_assert(std::is_trivially_copyable_v<cue::engine_assets::MeshVertex>);
static_assert(std::is_standard_layout_v<cue::engine_assets::BuiltInMeshDescriptor>);

/// @brief EngineAssets Public Headerの独立Compile契約を実行する
int main()
{
    const auto cube = cue::engine_assets::built_in_cube_mesh();
    const auto plane = cue::engine_assets::built_in_plane_mesh();
    const auto sphere = cue::engine_assets::built_in_sphere_mesh();
    const auto catalog = cue::engine_assets::built_in_mesh_catalog();
    return cube.vertices.empty() || plane.vertices.empty() || sphere.vertices.empty() || catalog.empty() ? 1 : 0;
}
