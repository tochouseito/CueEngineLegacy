#include <Cue/EngineAssets/BuiltInMesh.h>

#include <array>
#include <cstddef>

namespace cue::engine_assets
{
namespace
{
constexpr float k_halfExtent = 0.5F;

constexpr std::array<MeshVertex, 24> k_cubeVertices = {
    // +X
    MeshVertex{{k_halfExtent, -k_halfExtent, -k_halfExtent}, {1.0F, 0.0F, 0.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, -k_halfExtent}, {1.0F, 0.0F, 0.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, k_halfExtent}, {1.0F, 0.0F, 0.0F}},
    MeshVertex{{k_halfExtent, -k_halfExtent, k_halfExtent}, {1.0F, 0.0F, 0.0F}},
    // -X
    MeshVertex{{-k_halfExtent, -k_halfExtent, k_halfExtent}, {-1.0F, 0.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, k_halfExtent, k_halfExtent}, {-1.0F, 0.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, k_halfExtent, -k_halfExtent}, {-1.0F, 0.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, -k_halfExtent, -k_halfExtent}, {-1.0F, 0.0F, 0.0F}},
    // +Y
    MeshVertex{{-k_halfExtent, k_halfExtent, -k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, k_halfExtent, k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, -k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    // -Y
    MeshVertex{{-k_halfExtent, -k_halfExtent, k_halfExtent}, {0.0F, -1.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, -k_halfExtent, -k_halfExtent}, {0.0F, -1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, -k_halfExtent, -k_halfExtent}, {0.0F, -1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, -k_halfExtent, k_halfExtent}, {0.0F, -1.0F, 0.0F}},
    // +Z
    MeshVertex{{-k_halfExtent, -k_halfExtent, k_halfExtent}, {0.0F, 0.0F, 1.0F}},
    MeshVertex{{k_halfExtent, -k_halfExtent, k_halfExtent}, {0.0F, 0.0F, 1.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, k_halfExtent}, {0.0F, 0.0F, 1.0F}},
    MeshVertex{{-k_halfExtent, k_halfExtent, k_halfExtent}, {0.0F, 0.0F, 1.0F}},
    // -Z
    MeshVertex{{k_halfExtent, -k_halfExtent, -k_halfExtent}, {0.0F, 0.0F, -1.0F}},
    MeshVertex{{-k_halfExtent, -k_halfExtent, -k_halfExtent}, {0.0F, 0.0F, -1.0F}},
    MeshVertex{{-k_halfExtent, k_halfExtent, -k_halfExtent}, {0.0F, 0.0F, -1.0F}},
    MeshVertex{{k_halfExtent, k_halfExtent, -k_halfExtent}, {0.0F, 0.0F, -1.0F}},
};

constexpr std::array<std::uint16_t, 36> k_cubeIndices = {
    0U,  1U,  2U,  0U,  2U,  3U,  4U,  5U,  6U,  4U,  6U,  7U,  8U,  9U,  10U, 8U,  10U, 11U,
    12U, 13U, 14U, 12U, 14U, 15U, 16U, 17U, 18U, 16U, 18U, 19U, 20U, 21U, 22U, 20U, 22U, 23U,
};

constexpr std::array<MeshVertex, 4U> k_planeVertices = {
    MeshVertex{{-k_halfExtent, 0.0F, -k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{-k_halfExtent, 0.0F, k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, 0.0F, k_halfExtent}, {0.0F, 1.0F, 0.0F}},
    MeshVertex{{k_halfExtent, 0.0F, -k_halfExtent}, {0.0F, 1.0F, 0.0F}},
};

constexpr std::array<std::uint16_t, 6U> k_planeIndices = {0U, 1U, 2U, 0U, 2U, 3U};

constexpr std::size_t k_sphereSliceCount = 16U;
constexpr std::size_t k_sphereStackCount = 8U;
constexpr std::size_t k_sphereRingCount = k_sphereStackCount - 1U;
constexpr std::size_t k_sphereVertexCount = 2U + k_sphereRingCount * k_sphereSliceCount;
constexpr std::size_t k_sphereTriangleCount =
    k_sphereSliceCount * 2U + (k_sphereRingCount - 1U) * k_sphereSliceCount * 2U;
constexpr std::size_t k_sphereIndexCount = k_sphereTriangleCount * 3U;
constexpr double k_sphereStepCos = 0.9238795325112867;
constexpr double k_sphereStepSin = 0.3826834323650898;

struct SphereGeometry final
{
    std::array<MeshVertex, k_sphereVertexCount> vertices;
    std::array<std::uint16_t, k_sphereIndexCount> indices;
};

/// @brief 固定TessellationのSphereをCompile-time Storageへ生成する
[[nodiscard]] constexpr SphereGeometry make_sphere_geometry() noexcept
{
    SphereGeometry geometry{};
    geometry.vertices[0U] = MeshVertex{{0.0F, k_halfExtent, 0.0F}, {0.0F, 1.0F, 0.0F}};

    std::size_t vertexIndex = 1U;
    double stackSin = 0.0;
    double stackCos = 1.0;
    for (std::size_t stackIndex = 1U; stackIndex < k_sphereStackCount; ++stackIndex)
    {
        const double nextStackSin = stackSin * k_sphereStepCos + stackCos * k_sphereStepSin;
        const double nextStackCos = stackCos * k_sphereStepCos - stackSin * k_sphereStepSin;
        stackSin = nextStackSin;
        stackCos = nextStackCos;

        double sliceSin = 0.0;
        double sliceCos = 1.0;
        for (std::size_t sliceIndex = 0U; sliceIndex < k_sphereSliceCount; ++sliceIndex)
        {
            const float normalX = static_cast<float>(stackSin * sliceCos);
            const float normalY = static_cast<float>(stackCos);
            const float normalZ = static_cast<float>(stackSin * sliceSin);
            geometry.vertices[vertexIndex] = MeshVertex{
                {normalX * k_halfExtent, normalY * k_halfExtent, normalZ * k_halfExtent},
                {normalX, normalY, normalZ},
            };
            ++vertexIndex;

            const double nextSliceSin = sliceSin * k_sphereStepCos + sliceCos * k_sphereStepSin;
            const double nextSliceCos = sliceCos * k_sphereStepCos - sliceSin * k_sphereStepSin;
            sliceSin = nextSliceSin;
            sliceCos = nextSliceCos;
        }
    }
    geometry.vertices[k_sphereVertexCount - 1U] =
        MeshVertex{{0.0F, -k_halfExtent, 0.0F}, {0.0F, -1.0F, 0.0F}};

    std::size_t index = 0U;
    for (std::size_t sliceIndex = 0U; sliceIndex < k_sphereSliceCount; ++sliceIndex)
    {
        const std::uint16_t current = static_cast<std::uint16_t>(1U + sliceIndex);
        const std::uint16_t next = static_cast<std::uint16_t>(1U + (sliceIndex + 1U) % k_sphereSliceCount);
        geometry.indices[index++] = 0U;
        geometry.indices[index++] = next;
        geometry.indices[index++] = current;
    }

    for (std::size_t ringIndex = 0U; ringIndex + 1U < k_sphereRingCount; ++ringIndex)
    {
        const std::size_t upperStart = 1U + ringIndex * k_sphereSliceCount;
        const std::size_t lowerStart = upperStart + k_sphereSliceCount;
        for (std::size_t sliceIndex = 0U; sliceIndex < k_sphereSliceCount; ++sliceIndex)
        {
            const std::size_t nextSlice = (sliceIndex + 1U) % k_sphereSliceCount;
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

    const std::uint16_t bottomIndex = static_cast<std::uint16_t>(k_sphereVertexCount - 1U);
    const std::size_t lastRingStart = 1U + (k_sphereRingCount - 1U) * k_sphereSliceCount;
    for (std::size_t sliceIndex = 0U; sliceIndex < k_sphereSliceCount; ++sliceIndex)
    {
        const std::uint16_t current = static_cast<std::uint16_t>(lastRingStart + sliceIndex);
        const std::uint16_t next =
            static_cast<std::uint16_t>(lastRingStart + (sliceIndex + 1U) % k_sphereSliceCount);
        geometry.indices[index++] = current;
        geometry.indices[index++] = next;
        geometry.indices[index++] = bottomIndex;
    }

    return geometry;
}

constexpr SphereGeometry k_sphereGeometry = make_sphere_geometry();
static_assert(k_sphereVertexCount == 114U);
static_assert(k_sphereIndexCount == 672U);
} // namespace

/// @brief 静的Storageの寿命を持つCube Geometry Viewを返す
MeshView built_in_cube_mesh() noexcept
{
    return MeshView{k_cubeMeshAssetId, k_cubeMeshRevision, k_cubeVertices, k_cubeIndices};
}

MeshView built_in_plane_mesh() noexcept
{
    return MeshView{k_planeMeshAssetId, k_planeMeshRevision, k_planeVertices, k_planeIndices};
}

MeshView built_in_sphere_mesh() noexcept
{
    return MeshView{k_sphereMeshAssetId, k_sphereMeshRevision, k_sphereGeometry.vertices,
                    k_sphereGeometry.indices};
}
} // namespace cue::engine_assets
