#include <Cue/EngineAssets/BuiltInAssetCatalog.h>
#include <Cue/EngineAssets/BuiltInMesh.h>
#include <Cue/EngineAssets/Error.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Math/Vector.h>

#include <algorithm>
#include <array>
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

/// @brief Canonical Mesh CatalogのDescriptor、Typed Resolution、Error分類を検証する
void test_mesh_catalog(const cue::AssertContext &a_assertContext) noexcept
{
    using cue::engine_assets::BuiltInAssetKind;
    using cue::engine_assets::BuiltInMeshCapability;
    using cue::engine_assets::EngineAssetsError;

    const auto catalog = cue::engine_assets::built_in_mesh_catalog();
    require(catalog.size() == 1U);
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
    test_mesh_catalog(assertContext);
    test_catalog_rejection(assertContext);
    return 0;
}
