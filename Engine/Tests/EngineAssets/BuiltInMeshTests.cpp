#include <Cue/EngineAssets/BuiltInAssetCatalog.h>
#include <Cue/EngineAssets/BuiltInMesh.h>
#include <Cue/EngineAssets/Error.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Math/Vector.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test中の通常FatalをProcess失敗へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }

    /// @brief Test中のMessage付きFatalをProcess失敗へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

/// @brief 条件が偽ならTest Processを失敗終了する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::abort();
    }
}

/// @brief 浮動小数値が指定Tolerance内で一致する場合にtrueを返す
[[nodiscard]] bool nearly_equal(float a_left, float a_right, float a_tolerance = 0.00001F) noexcept
{
    return std::abs(a_left - a_right) <= a_tolerance;
}

/// @brief Triangle Index列で指定した非向きEdgeが使用される回数を返す
[[nodiscard]] std::size_t count_edge_uses(const cue::engine_assets::MeshView &a_mesh, std::uint16_t a_first,
                                          std::uint16_t a_second) noexcept
{
    std::size_t count = 0U;
    for (std::size_t index = 0U; index < a_mesh.indices.size(); index += 3U)
    {
        const std::array triangle = {
            a_mesh.indices[index],
            a_mesh.indices[index + 1U],
            a_mesh.indices[index + 2U],
        };
        for (std::size_t edgeIndex = 0U; edgeIndex < triangle.size(); ++edgeIndex)
        {
            const std::uint16_t edgeFirst = triangle[edgeIndex];
            const std::uint16_t edgeSecond = triangle[(edgeIndex + 1U) % triangle.size()];
            if ((edgeFirst == a_first && edgeSecond == a_second) ||
                (edgeFirst == a_second && edgeSecond == a_first))
            {
                ++count;
            }
        }
    }

    return count;
}

/// @brief Cube Corner Positionを配列順に依存しない3-bit値へ変換する
[[nodiscard]] std::uint8_t cube_corner_code(const cue::math::Vector3 &a_position) noexcept
{
    return static_cast<std::uint8_t>((a_position.x > 0.0F ? 1U : 0U) | (a_position.y > 0.0F ? 2U : 0U) |
                                     (a_position.z > 0.0F ? 4U : 0U));
}

/// @brief Cube Face Normalを配列順に依存しないFace値へ変換する
[[nodiscard]] std::uint8_t cube_face_code(const cue::math::Vector3 &a_normal) noexcept
{
    if (a_normal.x == 1.0F)
    {
        return 0U;
    }
    if (a_normal.x == -1.0F)
    {
        return 1U;
    }
    if (a_normal.y == 1.0F)
    {
        return 2U;
    }
    if (a_normal.y == -1.0F)
    {
        return 3U;
    }
    if (a_normal.z == 1.0F)
    {
        return 4U;
    }
    if (a_normal.z == -1.0F)
    {
        return 5U;
    }

    std::abort();
}

/// @brief Resultが指定したEngineAssets Errorを保持する場合にtrueを返す
template <typename T>
[[nodiscard]] bool has_engine_assets_error(const cue::Result<T> &a_result,
                                           cue::engine_assets::EngineAssetsError a_error) noexcept
{
    return !a_result && a_result.try_error()->code().domain() == "Cue.EngineAssets" &&
           a_result.try_error()->code().value() == static_cast<std::int64_t>(a_error);
}

/// @brief CubeのIdentity、Bounds、Normal、Triangle Contractを検証する
void test_cube_mesh() noexcept
{
    const auto cube = cue::engine_assets::built_in_cube_mesh();
    const auto secondView = cue::engine_assets::built_in_cube_mesh();

    require(cube.assetId == cue::engine_assets::k_cubeMeshAssetId);
    require(cube.revision == cue::engine_assets::k_cubeMeshRevision);
    require(cube.vertices.size() == 24U);
    require(cube.indices.size() == 36U);
    require(cube.vertices.data() == secondView.vertices.data());
    require(cube.indices.data() == secondView.indices.data());

    cue::math::Vector3 minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    cue::math::Vector3 maximum{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };

    for (const auto &vertex : cube.vertices)
    {
        minimum.x = (std::min)(minimum.x, vertex.position.x);
        minimum.y = (std::min)(minimum.y, vertex.position.y);
        minimum.z = (std::min)(minimum.z, vertex.position.z);
        maximum.x = (std::max)(maximum.x, vertex.position.x);
        maximum.y = (std::max)(maximum.y, vertex.position.y);
        maximum.z = (std::max)(maximum.z, vertex.position.z);
        require(cue::math::dot(vertex.normal, vertex.normal) == 1.0F);
    }

    require(minimum == cue::math::Vector3{-0.5F, -0.5F, -0.5F});
    require(maximum == cue::math::Vector3{0.5F, 0.5F, 0.5F});

    using VertexSignature = std::array<float, 6U>;
    std::array<VertexSignature, 24U> vertexSet{};
    std::size_t vertexIndex = 0U;
    for (const auto &vertex : cube.vertices)
    {
        vertexSet[vertexIndex] = {vertex.position.x, vertex.position.y, vertex.position.z,
                                  vertex.normal.x,   vertex.normal.y,   vertex.normal.z};
        ++vertexIndex;
    }

    std::array<VertexSignature, 24U> expectedVertexSet = {
        VertexSignature{0.5F, -0.5F, -0.5F, 1.0F, 0.0F, 0.0F},
        VertexSignature{0.5F, 0.5F, -0.5F, 1.0F, 0.0F, 0.0F},
        VertexSignature{0.5F, 0.5F, 0.5F, 1.0F, 0.0F, 0.0F},
        VertexSignature{0.5F, -0.5F, 0.5F, 1.0F, 0.0F, 0.0F},
        VertexSignature{-0.5F, -0.5F, 0.5F, -1.0F, 0.0F, 0.0F},
        VertexSignature{-0.5F, 0.5F, 0.5F, -1.0F, 0.0F, 0.0F},
        VertexSignature{-0.5F, 0.5F, -0.5F, -1.0F, 0.0F, 0.0F},
        VertexSignature{-0.5F, -0.5F, -0.5F, -1.0F, 0.0F, 0.0F},
        VertexSignature{-0.5F, 0.5F, -0.5F, 0.0F, 1.0F, 0.0F},
        VertexSignature{-0.5F, 0.5F, 0.5F, 0.0F, 1.0F, 0.0F},
        VertexSignature{0.5F, 0.5F, 0.5F, 0.0F, 1.0F, 0.0F},
        VertexSignature{0.5F, 0.5F, -0.5F, 0.0F, 1.0F, 0.0F},
        VertexSignature{-0.5F, -0.5F, 0.5F, 0.0F, -1.0F, 0.0F},
        VertexSignature{-0.5F, -0.5F, -0.5F, 0.0F, -1.0F, 0.0F},
        VertexSignature{0.5F, -0.5F, -0.5F, 0.0F, -1.0F, 0.0F},
        VertexSignature{0.5F, -0.5F, 0.5F, 0.0F, -1.0F, 0.0F},
        VertexSignature{-0.5F, -0.5F, 0.5F, 0.0F, 0.0F, 1.0F},
        VertexSignature{0.5F, -0.5F, 0.5F, 0.0F, 0.0F, 1.0F},
        VertexSignature{0.5F, 0.5F, 0.5F, 0.0F, 0.0F, 1.0F},
        VertexSignature{-0.5F, 0.5F, 0.5F, 0.0F, 0.0F, 1.0F},
        VertexSignature{0.5F, -0.5F, -0.5F, 0.0F, 0.0F, -1.0F},
        VertexSignature{-0.5F, -0.5F, -0.5F, 0.0F, 0.0F, -1.0F},
        VertexSignature{-0.5F, 0.5F, -0.5F, 0.0F, 0.0F, -1.0F},
        VertexSignature{0.5F, 0.5F, -0.5F, 0.0F, 0.0F, -1.0F},
    };
    std::ranges::sort(vertexSet);
    std::ranges::sort(expectedVertexSet);
    require(vertexSet == expectedVertexSet);

    using TriangleSignature = std::array<std::uint8_t, 4U>;
    std::array<TriangleSignature, 12U> triangleSet{};
    std::size_t triangleIndex = 0U;

    for (std::size_t index = 0U; index < cube.indices.size(); index += 3U)
    {
        const auto firstIndex = cube.indices[index];
        const auto secondIndex = cube.indices[index + 1U];
        const auto thirdIndex = cube.indices[index + 2U];
        require(firstIndex < cube.vertices.size());
        require(secondIndex < cube.vertices.size());
        require(thirdIndex < cube.vertices.size());

        const auto &first = cube.vertices[firstIndex];
        const auto &second = cube.vertices[secondIndex];
        const auto &third = cube.vertices[thirdIndex];
        const auto edgeA = second.position - first.position;
        const auto edgeB = third.position - first.position;
        const auto triangleNormal = cue::math::cross(edgeA, edgeB);

        require(cue::math::dot(triangleNormal, triangleNormal) > 0.0F);
        require(cue::math::dot(triangleNormal, first.normal) > 0.0F);
        require(first.normal == second.normal);
        require(first.normal == third.normal);

        std::array corners = {
            cube_corner_code(first.position),
            cube_corner_code(second.position),
            cube_corner_code(third.position),
        };
        std::ranges::sort(corners);
        triangleSet[triangleIndex] = {cube_face_code(first.normal), corners[0], corners[1], corners[2]};
        ++triangleIndex;
    }

    std::array<TriangleSignature, 12U> expectedTriangleSet = {
        TriangleSignature{0U, 1U, 3U, 7U}, TriangleSignature{0U, 1U, 5U, 7U}, TriangleSignature{1U, 2U, 4U, 6U},
        TriangleSignature{1U, 0U, 2U, 4U}, TriangleSignature{2U, 2U, 6U, 7U}, TriangleSignature{2U, 2U, 3U, 7U},
        TriangleSignature{3U, 0U, 1U, 4U}, TriangleSignature{3U, 1U, 4U, 5U}, TriangleSignature{4U, 4U, 5U, 7U},
        TriangleSignature{4U, 4U, 6U, 7U}, TriangleSignature{5U, 0U, 1U, 2U}, TriangleSignature{5U, 1U, 2U, 3U},
    };
    std::ranges::sort(triangleSet);
    std::ranges::sort(expectedTriangleSet);
    require(triangleSet == expectedTriangleSet);
}

/// @brief PlaneのIdentity、Geometry、Bounds、Winding Contractを検証する
void test_plane_mesh() noexcept
{
    const auto plane = cue::engine_assets::built_in_plane_mesh();
    const auto secondView = cue::engine_assets::built_in_plane_mesh();

    require(plane.assetId == cue::engine_assets::k_planeMeshAssetId);
    require(plane.revision == cue::engine_assets::k_planeMeshRevision);
    require(plane.vertices.size() == 4U);
    require(plane.indices.size() == 6U);
    require(plane.vertices.data() == secondView.vertices.data());
    require(plane.indices.data() == secondView.indices.data());

    using PlanePosition = std::array<float, 3U>;
    std::array<PlanePosition, 4U> positions{};
    std::size_t vertexIndex = 0U;
    for (const auto &vertex : plane.vertices)
    {
        require(vertex.position.y == 0.0F);
        require(vertex.normal == cue::math::Vector3{0.0F, 1.0F, 0.0F});
        positions[vertexIndex++] = {vertex.position.x, vertex.position.y, vertex.position.z};
    }

    std::array<PlanePosition, 4U> expectedPositions = {
        PlanePosition{-0.5F, 0.0F, -0.5F},
        PlanePosition{-0.5F, 0.0F, 0.5F},
        PlanePosition{0.5F, 0.0F, 0.5F},
        PlanePosition{0.5F, 0.0F, -0.5F},
    };
    std::ranges::sort(positions);
    std::ranges::sort(expectedPositions);
    require(positions == expectedPositions);

    for (std::size_t index = 0U; index < plane.indices.size(); index += 3U)
    {
        const auto firstIndex = plane.indices[index];
        const auto secondIndex = plane.indices[index + 1U];
        const auto thirdIndex = plane.indices[index + 2U];
        require(firstIndex < plane.vertices.size());
        require(secondIndex < plane.vertices.size());
        require(thirdIndex < plane.vertices.size());

        const auto edgeA = plane.vertices[secondIndex].position - plane.vertices[firstIndex].position;
        const auto edgeB = plane.vertices[thirdIndex].position - plane.vertices[firstIndex].position;
        const auto triangleNormal = cue::math::cross(edgeA, edgeB);
        require(cue::math::dot(triangleNormal, triangleNormal) > 0.0F);
        require(cue::math::dot(triangleNormal, cue::math::Vector3{0.0F, 1.0F, 0.0F}) > 0.0F);
    }
}

/// @brief SphereのTessellation、Bounds、Normal、Winding、Seam／Pole Contractを検証する
void test_sphere_mesh() noexcept
{
    constexpr std::size_t sliceCount = 16U;
    constexpr std::size_t ringCount = 7U;
    const auto sphere = cue::engine_assets::built_in_sphere_mesh();
    const auto secondView = cue::engine_assets::built_in_sphere_mesh();

    require(sphere.assetId == cue::engine_assets::k_sphereMeshAssetId);
    require(sphere.revision == cue::engine_assets::k_sphereMeshRevision);
    require(sphere.vertices.size() == 114U);
    require(sphere.indices.size() == 672U);
    require(sphere.vertices.data() == secondView.vertices.data());
    require(sphere.indices.data() == secondView.indices.data());

    cue::math::Vector3 minimum{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    cue::math::Vector3 maximum{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };
    for (const auto &vertex : sphere.vertices)
    {
        minimum.x = (std::min)(minimum.x, vertex.position.x);
        minimum.y = (std::min)(minimum.y, vertex.position.y);
        minimum.z = (std::min)(minimum.z, vertex.position.z);
        maximum.x = (std::max)(maximum.x, vertex.position.x);
        maximum.y = (std::max)(maximum.y, vertex.position.y);
        maximum.z = (std::max)(maximum.z, vertex.position.z);

        require(nearly_equal(cue::math::dot(vertex.normal, vertex.normal), 1.0F));
        require(nearly_equal(vertex.position.x * 2.0F, vertex.normal.x));
        require(nearly_equal(vertex.position.y * 2.0F, vertex.normal.y));
        require(nearly_equal(vertex.position.z * 2.0F, vertex.normal.z));
    }
    require(nearly_equal(minimum.x, -0.5F));
    require(nearly_equal(minimum.y, -0.5F));
    require(nearly_equal(minimum.z, -0.5F));
    require(nearly_equal(maximum.x, 0.5F));
    require(nearly_equal(maximum.y, 0.5F));
    require(nearly_equal(maximum.z, 0.5F));

    const std::uint16_t bottomIndex = static_cast<std::uint16_t>(sphere.vertices.size() - 1U);
    require(sphere.vertices.front().position == cue::math::Vector3{0.0F, 0.5F, 0.0F});
    require(sphere.vertices.front().normal == cue::math::Vector3{0.0F, 1.0F, 0.0F});
    require(sphere.vertices.back().position == cue::math::Vector3{0.0F, -0.5F, 0.0F});
    require(sphere.vertices.back().normal == cue::math::Vector3{0.0F, -1.0F, 0.0F});

    std::size_t topTriangleCount = 0U;
    std::size_t bottomTriangleCount = 0U;
    for (std::size_t index = 0U; index < sphere.indices.size(); index += 3U)
    {
        const auto firstIndex = sphere.indices[index];
        const auto secondIndex = sphere.indices[index + 1U];
        const auto thirdIndex = sphere.indices[index + 2U];
        require(firstIndex < sphere.vertices.size());
        require(secondIndex < sphere.vertices.size());
        require(thirdIndex < sphere.vertices.size());
        require(firstIndex != secondIndex && secondIndex != thirdIndex && firstIndex != thirdIndex);

        const auto &first = sphere.vertices[firstIndex];
        const auto &second = sphere.vertices[secondIndex];
        const auto &third = sphere.vertices[thirdIndex];
        const auto triangleNormal =
            cue::math::cross(second.position - first.position, third.position - first.position);
        const cue::math::Vector3 centroid{
            first.position.x + second.position.x + third.position.x,
            first.position.y + second.position.y + third.position.y,
            first.position.z + second.position.z + third.position.z,
        };
        require(cue::math::dot(triangleNormal, triangleNormal) > 0.0000001F);
        require(cue::math::dot(triangleNormal, centroid) > 0.0F);

        if (firstIndex == 0U || secondIndex == 0U || thirdIndex == 0U)
        {
            ++topTriangleCount;
        }
        if (firstIndex == bottomIndex || secondIndex == bottomIndex || thirdIndex == bottomIndex)
        {
            ++bottomTriangleCount;
        }
    }
    require(topTriangleCount == sliceCount);
    require(bottomTriangleCount == sliceCount);

    for (std::size_t ringIndex = 0U; ringIndex < ringCount; ++ringIndex)
    {
        const std::uint16_t ringStart = static_cast<std::uint16_t>(1U + ringIndex * sliceCount);
        const std::uint16_t ringEnd = static_cast<std::uint16_t>(ringStart + sliceCount - 1U);
        require(count_edge_uses(sphere, ringEnd, ringStart) == 2U);

        for (std::size_t firstSlice = 0U; firstSlice < sliceCount; ++firstSlice)
        {
            for (std::size_t secondSlice = firstSlice + 1U; secondSlice < sliceCount; ++secondSlice)
            {
                require(sphere.vertices[ringStart + firstSlice].position !=
                        sphere.vertices[ringStart + secondSlice].position);
            }
        }
    }
}

/// @brief Canonical Mesh CatalogのDescriptor、Typed Resolution、Error分類を検証する
void test_mesh_catalog(const cue::AssertContext &a_assertContext) noexcept
{
    using cue::engine_assets::BuiltInAssetKind;
    using cue::engine_assets::BuiltInMeshCapability;
    using cue::engine_assets::EngineAssetsError;

    const auto catalog = cue::engine_assets::built_in_mesh_catalog();
    require(catalog.size() == 3U);
    require(catalog.front().assetId == cue::engine_assets::k_cubeMeshAssetId);
    require(catalog.front().kind == BuiltInAssetKind::Mesh);
    require(catalog.front().revision == cue::engine_assets::k_cubeMeshRevision);
    require(catalog.front().displayName == "Cube");
    require(catalog.front().bounds.minimum == cue::math::Vector3{-0.5F, -0.5F, -0.5F});
    require(catalog.front().bounds.maximum == cue::math::Vector3{0.5F, 0.5F, 0.5F});
    require(catalog.front().capability == BuiltInMeshCapability::PositionNormal);
    require(cue::engine_assets::validate_builtin_mesh_catalog(catalog, a_assertContext).has_value());

    auto descriptor =
        cue::engine_assets::resolve_builtin_mesh_descriptor(cue::engine_assets::k_cubeMeshAssetId, a_assertContext);
    auto mesh = cue::engine_assets::resolve_builtin_mesh(cue::engine_assets::k_cubeMeshAssetId, a_assertContext);
    require(descriptor.has_value() && *descriptor.try_value() == &catalog.front());
    require(mesh.has_value());
    require(mesh.try_value()->vertices.data() == cue::engine_assets::built_in_cube_mesh().vertices.data());
    require(mesh.try_value()->indices.data() == cue::engine_assets::built_in_cube_mesh().indices.data());

    auto planeDescriptor =
        cue::engine_assets::resolve_builtin_mesh_descriptor(cue::engine_assets::k_planeMeshAssetId, a_assertContext);
    auto sphereDescriptor =
        cue::engine_assets::resolve_builtin_mesh_descriptor(cue::engine_assets::k_sphereMeshAssetId, a_assertContext);
    auto plane = cue::engine_assets::resolve_builtin_mesh(cue::engine_assets::k_planeMeshAssetId, a_assertContext);
    auto sphere = cue::engine_assets::resolve_builtin_mesh(cue::engine_assets::k_sphereMeshAssetId, a_assertContext);
    require(planeDescriptor.has_value());
    require((*planeDescriptor.try_value())->revision == cue::engine_assets::k_planeMeshRevision);
    require((*planeDescriptor.try_value())->displayName == "Plane");
    require((*planeDescriptor.try_value())->bounds.minimum == cue::math::Vector3{-0.5F, 0.0F, -0.5F});
    require((*planeDescriptor.try_value())->bounds.maximum == cue::math::Vector3{0.5F, 0.0F, 0.5F});
    require((*planeDescriptor.try_value())->capability == BuiltInMeshCapability::PositionNormal);
    require(sphereDescriptor.has_value());
    require((*sphereDescriptor.try_value())->revision == cue::engine_assets::k_sphereMeshRevision);
    require((*sphereDescriptor.try_value())->displayName == "Sphere");
    require((*sphereDescriptor.try_value())->bounds.minimum == cue::math::Vector3{-0.5F, -0.5F, -0.5F});
    require((*sphereDescriptor.try_value())->bounds.maximum == cue::math::Vector3{0.5F, 0.5F, 0.5F});
    require((*sphereDescriptor.try_value())->capability == BuiltInMeshCapability::PositionNormal);
    require(plane.has_value());
    require(sphere.has_value());
    require(plane.try_value()->vertices.data() == cue::engine_assets::built_in_plane_mesh().vertices.data());
    require(sphere.try_value()->vertices.data() == cue::engine_assets::built_in_sphere_mesh().vertices.data());

    auto unknown = cue::engine_assets::resolve_builtin_mesh_descriptor("cue://engine/mesh/unknown", a_assertContext);
    auto kindMismatch =
        cue::engine_assets::resolve_builtin_mesh_descriptor("cue://engine/material/cube", a_assertContext);
    require(has_engine_assets_error(unknown, EngineAssetsError::UnknownAsset));
    require(has_engine_assets_error(kindMismatch, EngineAssetsError::KindMismatch));
}

/// @brief Built-in ID Grammar、重複、Revision、Engine Namespace予約を検証する
void test_catalog_rejection(const cue::AssertContext &a_assertContext) noexcept
{
    using cue::engine_assets::EngineAssetsError;

    constexpr std::array invalidIds = {
        std::string_view("cue://engine/mesh"),
        std::string_view("cue://engine//cube"),
        std::string_view("cue://engine/mesh/cube/extra"),
        std::string_view("cue://engine/Mesh/cube"),
        std::string_view("cue://engine/1mesh/cube"),
        std::string_view("cue://engine/mesh/cube-"),
        std::string_view("cue://engine/mesh/cu_be"),
        std::string_view("cue://engine/mesh/cube?query"),
        std::string_view("cue://engine/mesh/cube#fragment"),
        std::string_view("cue://engine/mesh/cu%62e"),
        std::string_view("cue://project/mesh/cube"),
    };
    for (const std::string_view invalidId : invalidIds)
    {
        auto result = cue::engine_assets::resolve_builtin_mesh_descriptor(invalidId, a_assertContext);
        require(has_engine_assets_error(result, EngineAssetsError::InvalidAssetId));
    }

    const auto catalog = cue::engine_assets::built_in_mesh_catalog();
    std::array duplicateCatalog = {catalog.front(), catalog.front()};
    auto duplicate = cue::engine_assets::validate_builtin_mesh_catalog(duplicateCatalog, a_assertContext);
    require(has_engine_assets_error(duplicate, EngineAssetsError::DuplicateAssetId));

    auto invalidRevisionDescriptor = catalog.front();
    invalidRevisionDescriptor.revision = 0U;
    const std::array invalidRevisionCatalog = {invalidRevisionDescriptor};
    auto invalidRevision = cue::engine_assets::validate_builtin_mesh_catalog(invalidRevisionCatalog, a_assertContext);
    require(has_engine_assets_error(invalidRevision, EngineAssetsError::InvalidRevision));

    require(cue::engine_assets::is_engine_asset_namespace(cue::engine_assets::k_cubeMeshAssetId));
    require(!cue::engine_assets::is_engine_asset_namespace("cue://project/mesh/cube"));

    auto projectOverrideDescriptor = catalog.front();
    projectOverrideDescriptor.displayName = "Project Override";
    const std::array overrideAttempt = {catalog.front(), projectOverrideDescriptor};
    auto projectOverride = cue::engine_assets::validate_builtin_mesh_catalog(overrideAttempt, a_assertContext);
    require(has_engine_assets_error(projectOverride, EngineAssetsError::DuplicateAssetId));
    require(cue::engine_assets::built_in_mesh_catalog().front().displayName == "Cube");
}
} // namespace

/// @brief Built-in Mesh Contract Testを実行する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);

    test_cube_mesh();
    test_plane_mesh();
    test_sphere_mesh();
    test_mesh_catalog(assertContext);
    test_catalog_rejection(assertContext);
    return 0;
}
