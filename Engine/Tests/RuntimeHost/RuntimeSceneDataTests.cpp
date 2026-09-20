#include "RuntimePackage.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Package/RuntimeData.h>
#include <Cue/Project/Descriptor.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
constexpr std::string_view k_sceneId = "10000000-0000-4000-8000-000000000001";
constexpr std::string_view k_sceneV2 =
    R"({"schemaVersion":2,"sceneAssetId":"10000000-0000-4000-8000-000000000001","objects":[{"objectId":"20000000-0000-4000-8000-000000000001","parentObjectId":null,"active":true,"transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},"components":[{"instanceId":"30000000-0000-4000-8000-000000000001","typeId":"70000000-0000-4000-8000-000000000001","schemaVersion":1,"fields":[{"fieldId":1,"value":true},{"fieldId":2,"value":60},{"fieldId":3,"value":0.1},{"fieldId":4,"value":1000}]},{"instanceId":"30000000-0000-4000-8000-000000000002","typeId":"70000000-0000-4000-8000-000000000002","schemaVersion":1,"fields":[{"fieldId":1,"value":"cue://engine/mesh/cube"}]}]}]})"
    "\n";

/// @brief Test中のFatalを固定Exit Codeで報告する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalを固定Exit Codeで報告する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(76);
    }
    /// @brief Message付きFatalを固定Exit Codeで報告する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

/// @brief 一つのFixture Tokenだけを置換しParser負例を作る
[[nodiscard]] std::string replace_once(std::string_view a_source, std::string_view a_before,
                                       std::string_view a_after)
{
    std::string changed(a_source);
    const std::size_t offset = changed.find(a_before);
    if (offset != std::string::npos)
    {
        changed.replace(offset, a_before.size(), a_after);
    }
    return changed;
}

/// @brief v1互換とv2所有Snapshot／Canonical Writerを検証する
[[nodiscard]] bool test_roundtrip(const cue::AssertContext &a_assertContext)
{
    constexpr std::string_view v1 =
        "{\"schemaVersion\":1,\"sceneAssetId\":\"10000000-0000-4000-8000-000000000001\",\"objects\":[]}\n";
    auto oldScene = cue::runtime_host::parse_runtime_scene_data(v1, k_sceneId, a_assertContext);
    auto scene = cue::runtime_host::parse_runtime_scene_data(k_sceneV2, k_sceneId, a_assertContext);
    auto projectId = cue::ProjectId::parse("00000000-0000-4000-8000-000000000901", a_assertContext);
    if (!oldScene || !scene || oldScene.try_value()->objects().size() != 0U ||
        scene.try_value()->objects().size() != 1U ||
        scene.try_value()->objects()[0].components().size() != 2U || !projectId)
    {
        return false;
    }
    auto descriptor = cue::create_blank_project_descriptor(
        *projectId.try_value(), "Runtime Scene Test",
        cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, cue::EngineVersion{2U, 0U, 0U}},
        k_sceneId, a_assertContext);
    if (!descriptor)
    {
        return false;
    }
    auto oldPublished = cue::package::publish_minimal_runtime_data(*descriptor.try_value(),
                                                                    *oldScene.try_value(), a_assertContext);
    auto published = cue::package::publish_minimal_runtime_data(*descriptor.try_value(),
                                                                 *scene.try_value(), a_assertContext);
    const std::string nonCanonical = replace_once(k_sceneV2, "\"value\":60", "\"value\":60.0");
    auto reinterpreted = cue::runtime_host::parse_runtime_scene_data(nonCanonical, k_sceneId, a_assertContext);
    auto reproduced = reinterpreted
                          ? cue::package::publish_minimal_runtime_data(*descriptor.try_value(),
                                                                        *reinterpreted.try_value(), a_assertContext)
                          : cue::Result<cue::package::MinimalRuntimeDataPublication>::failure(
                                std::move(*reinterpreted.try_error()));
    return oldPublished && published && oldPublished.try_value()->startup_scene_data().bytes() == v1 &&
           published.try_value()->startup_scene_data().bytes() == k_sceneV2 && reproduced &&
           reproduced.try_value()->startup_scene_data().bytes() != nonCanonical;
}

/// @brief 未知・重複・不正投影・非Canonical入力をRuntime Readerで拒否する
[[nodiscard]] bool test_invalid_data(const cue::AssertContext &a_assertContext)
{
    std::string duplicateAcrossObjects(k_sceneV2);
    const std::size_t objectsEnd = duplicateAcrossObjects.rfind("]}\n");
    if (objectsEnd == std::string::npos)
    {
        return false;
    }
    duplicateAcrossObjects.insert(
        objectsEnd,
        R"(,{"objectId":"20000000-0000-4000-8000-000000000002","parentObjectId":null,"active":true,"transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},"components":[{"instanceId":"30000000-0000-4000-8000-000000000002","typeId":"70000000-0000-4000-8000-000000000002","schemaVersion":1,"fields":[{"fieldId":1,"value":"cue://engine/mesh/cube"}]}]})");
    std::string unorderedComponents = replace_once(
        k_sceneV2, "\"instanceId\":\"30000000-0000-4000-8000-000000000001\"",
        "\"instanceId\":\"30000000-0000-4000-8000-000000000003\"");
    unorderedComponents = replace_once(unorderedComponents,
                                       "\"instanceId\":\"30000000-0000-4000-8000-000000000002\"",
                                       "\"instanceId\":\"30000000-0000-4000-8000-000000000001\"");
    const std::vector<std::string> invalid{
        replace_once(k_sceneV2, "\"schemaVersion\":2", "\"schemaVersion\":1"),
        replace_once(k_sceneV2, "\"fieldId\":2", "\"fieldId\":1"),
        replace_once(k_sceneV2, "\"fieldId\":4", "\"fieldId\":5"),
        replace_once(k_sceneV2,
                     "\"typeId\":\"70000000-0000-4000-8000-000000000001\",\"schemaVersion\":1",
                     "\"typeId\":\"70000000-0000-4000-8000-000000000001\",\"schemaVersion\":2"),
        replace_once(k_sceneV2, "\"typeId\":\"70000000-0000-4000-8000-000000000001\"",
                     "\"typeId\":\"40000000-0000-4000-8000-000000000001\""),
        replace_once(k_sceneV2, "\"value\":60", "\"value\":200"),
        replace_once(k_sceneV2, "cue://engine/mesh/cube", "cue://engine/mesh/unknown"),
        replace_once(k_sceneV2, "\"instanceId\":\"30000000-0000-4000-8000-000000000002\"",
                     "\"instanceId\":\"30000000-0000-4000-8000-000000000001\""),
        std::move(duplicateAcrossObjects), std::move(unorderedComponents)};
    for (const std::string &bytes : invalid)
    {
        if (cue::runtime_host::parse_runtime_scene_data(bytes, k_sceneId, a_assertContext))
        {
            return false;
        }
    }
    return !cue::runtime_host::parse_runtime_scene_data(k_sceneV2,
                                                         "10000000-0000-4000-8000-000000000002",
                                                         a_assertContext);
}

/// @brief v2のObjectとComponent数を上限より先に拒否する
[[nodiscard]] bool test_v2_limits(const cue::AssertContext &a_assertContext)
{
    std::string tooManyObjects =
        "{\"schemaVersion\":2,\"sceneAssetId\":\"10000000-0000-4000-8000-000000000001\",\"objects\":[";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 1U; index <= 4097U; ++index)
    {
        if (index > 1U)
        {
            tooManyObjects.push_back(',');
        }
        std::string id = "20000000-0000-4000-8000-000000000000";
        for (std::size_t digit = 0U; digit < 4U; ++digit)
        {
            id[35U - digit] = digits[(index >> (digit * 4U)) & 0x0fU];
        }
        tooManyObjects.append("{\"objectId\":\"");
        tooManyObjects.append(id);
        tooManyObjects.append(
            "\",\"parentObjectId\":null,\"active\":true,\"transform\":{\"translation\":[0,0,0],"
            "\"rotation\":[0,0,0,1],\"scale\":[1,1,1]},\"components\":[]}");
    }
    tooManyObjects.append("]}\n");
    constexpr std::string_view meshEntry =
        R"({"instanceId":"30000000-0000-4000-8000-000000000002","typeId":"70000000-0000-4000-8000-000000000002","schemaVersion":1,"fields":[{"fieldId":1,"value":"cue://engine/mesh/cube"}]})";
    std::string tooManyComponents(k_sceneV2);
    const std::size_t meshOffset = tooManyComponents.find(meshEntry);
    if (meshOffset == std::string::npos)
    {
        return false;
    }
    const std::string third = replace_once(meshEntry,
                                            "30000000-0000-4000-8000-000000000002",
                                            "30000000-0000-4000-8000-000000000003");
    tooManyComponents.insert(meshOffset + meshEntry.size(), "," + third);
    return !cue::runtime_host::parse_runtime_scene_data(tooManyObjects, k_sceneId, a_assertContext) &&
           !cue::runtime_host::parse_runtime_scene_data(tooManyComponents, k_sceneId, a_assertContext);
}
} // namespace

/// @brief Runtime Scene v1／v2のReaderとPublisher境界を検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_roundtrip(assertContext) && test_invalid_data(assertContext) &&
                   test_v2_limits(assertContext)
               ? 0
               : 1;
}
