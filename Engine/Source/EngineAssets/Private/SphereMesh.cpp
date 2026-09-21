#include <Cue/EngineAssets/BuiltInMesh.h>

#include <array>
#include <cstddef>

namespace cue::engine_assets
{
namespace
{
constexpr float k_halfExtent = 0.5F;
constexpr std::size_t k_sliceCount = 16U;
constexpr std::size_t k_stackCount = 8U;
constexpr std::size_t k_ringCount = k_stackCount - 1U;
constexpr std::size_t k_vertexCount = 2U + k_ringCount * k_sliceCount;
constexpr std::size_t k_triangleCount = k_sliceCount * 2U + (k_ringCount - 1U) * k_sliceCount * 2U;
constexpr std::size_t k_indexCount = k_triangleCount * 3U;
constexpr double k_stepCos = 0.9238795325112867;
constexpr double k_stepSin = 0.3826834323650898;

struct SphereGeometry final
{
    std::array<MeshVertex, k_vertexCount> vertices;
    std::array<std::uint16_t, k_indexCount> indices;
};

/// @brief 固定TessellationのSphereをCompile-time Storageへ生成する
[[nodiscard]] constexpr SphereGeometry make_geometry() noexcept
{
    SphereGeometry geometry{};
    geometry.vertices[0U] = MeshVertex{{0.0F, k_halfExtent, 0.0F}, {0.0F, 1.0F, 0.0F}};

    std::size_t vertexIndex = 1U;
    double stackSin = 0.0;
    double stackCos = 1.0;
    for (std::size_t stackIndex = 1U; stackIndex < k_stackCount; ++stackIndex)
    {
        const double nextStackSin = stackSin * k_stepCos + stackCos * k_stepSin;
        const double nextStackCos = stackCos * k_stepCos - stackSin * k_stepSin;
        stackSin = nextStackSin;
        stackCos = nextStackCos;

        double sliceSin = 0.0;
        double sliceCos = 1.0;
        for (std::size_t sliceIndex = 0U; sliceIndex < k_sliceCount; ++sliceIndex)
        {
            const float normalX = static_cast<float>(stackSin * sliceCos);
            const float normalY = static_cast<float>(stackCos);
            const float normalZ = static_cast<float>(stackSin * sliceSin);
            geometry.vertices[vertexIndex++] = MeshVertex{
                {normalX * k_halfExtent, normalY * k_halfExtent, normalZ * k_halfExtent},
                {normalX, normalY, normalZ},
            };

            const double nextSliceSin = sliceSin * k_stepCos + sliceCos * k_stepSin;
            const double nextSliceCos = sliceCos * k_stepCos - sliceSin * k_stepSin;
            sliceSin = nextSliceSin;
            sliceCos = nextSliceCos;
        }
    }
    geometry.vertices[k_vertexCount - 1U] =
        MeshVertex{{0.0F, -k_halfExtent, 0.0F}, {0.0F, -1.0F, 0.0F}};

    std::size_t index = 0U;
    for (std::size_t sliceIndex = 0U; sliceIndex < k_sliceCount; ++sliceIndex)
    {
        const std::uint16_t current = static_cast<std::uint16_t>(1U + sliceIndex);
        const std::uint16_t next = static_cast<std::uint16_t>(1U + (sliceIndex + 1U) % k_sliceCount);
        geometry.indices[index++] = 0U;
        geometry.indices[index++] = next;
        geometry.indices[index++] = current;
    }

    for (std::size_t ringIndex = 0U; ringIndex + 1U < k_ringCount; ++ringIndex)
    {
        const std::size_t upperStart = 1U + ringIndex * k_sliceCount;
        const std::size_t lowerStart = upperStart + k_sliceCount;
        for (std::size_t sliceIndex = 0U; sliceIndex < k_sliceCount; ++sliceIndex)
        {
            const std::size_t nextSlice = (sliceIndex + 1U) % k_sliceCount;
            const std::uint16_t upperCurrent = static_cast<std::uint16_t>(upperStart + sliceIndex);
            const std::uint16_t upperNext = static_cast<std::uint16_t>(upperStart + nextSlice);
            const std::uint16_t lowerCurrent = static_cast<std::uint16_t>(lowerStart + sliceIndex);
            const std::uint16_t lowerNext = static_cast<std::uint16_t>(lowerStart + nextSlice);

            geometry.indices[index++] = upperCurrent;
            geometry.indices[index++] = lowerNext;
            geometry.indices[index++] = lowerCurrent;
            geometry.indices[index++] = upperCurrent;
            geometry.indices[index++] = upperNext;
            geometry.indices[index++] = lowerNext;
        }
    }

    const std::uint16_t bottomIndex = static_cast<std::uint16_t>(k_vertexCount - 1U);
    const std::size_t lastRingStart = 1U + (k_ringCount - 1U) * k_sliceCount;
    for (std::size_t sliceIndex = 0U; sliceIndex < k_sliceCount; ++sliceIndex)
    {
        const std::uint16_t current = static_cast<std::uint16_t>(lastRingStart + sliceIndex);
        const std::uint16_t next =
            static_cast<std::uint16_t>(lastRingStart + (sliceIndex + 1U) % k_sliceCount);
        geometry.indices[index++] = current;
        geometry.indices[index++] = next;
        geometry.indices[index++] = bottomIndex;
    }

    return geometry;
}

constexpr SphereGeometry k_geometry = make_geometry();
static_assert(k_vertexCount == 114U);
static_assert(k_indexCount == 672U);
} // namespace

MeshView built_in_sphere_mesh() noexcept
{
    return MeshView{k_sphereMeshAssetId, k_sphereMeshRevision, k_geometry.vertices, k_geometry.indices};
}

BuiltInMeshProvider built_in_sphere_mesh_provider() noexcept
{
    return BuiltInMeshProvider{k_sphereMeshAssetId, k_sphereMeshRevision, &built_in_sphere_mesh};
}
} // namespace cue::engine_assets
