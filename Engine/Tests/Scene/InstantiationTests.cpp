#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Scene/Error.h>
#include <Cue/Scene/Instantiation.h>
#include <Cue/Schema/Descriptor.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
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

    /// @brief Test中のEmergency FatalをProcess失敗へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

class SequentialIdentitySource final : public cue::scene::SceneIdentitySource
{
  public:
    /// @brief 末尾byteだけを進める有効なUUID Version 4を返す
    [[nodiscard]] cue::scene::IdentityBytes next_identity() noexcept override
    {
        cue::scene::IdentityBytes bytes{};
        bytes[6] = 0x40U;
        bytes[8] = 0x80U;
        bytes[14] = static_cast<std::uint8_t>((m_next >> 8U) & 0xFFU);
        bytes[15] = static_cast<std::uint8_t>(m_next & 0xFFU);
        ++m_next;
        return bytes;
    }

  private:
    std::uint16_t m_next = 1U;
};

struct TestRuntimeComponent final
{
    std::int64_t value;
};

/// @brief 条件が偽ならTest Processを失敗終了する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::abort();
    }
}

/// @brief 成功Resultから所有Valueを取り出す
template <typename T>
T take_value(cue::Result<T> &&a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

/// @brief Test用Stable TypeIdを生成する
[[nodiscard]] cue::schema::TypeId make_type_id(
    std::string_view a_text,
    const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::TypeId::parse(a_text, a_assertContext));
}

/// @brief Test用Schema Version 1を生成する
[[nodiscard]] cue::schema::SchemaVersion make_version(
    const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::SchemaVersion::create(1U,
                                                         a_assertContext));
}

/// @brief Fieldを持たないTest用Type Descriptorを生成する
[[nodiscard]] cue::schema::TypeDescriptor make_type_descriptor(
    std::string_view a_typeId, std::string_view a_name,
    const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::schema::FieldDescriptor> fields;
    std::vector<cue::schema::FieldId> reservedFields;
    return take_value(cue::schema::create_type_descriptor(
        make_type_id(a_typeId, a_assertContext), a_name,
        make_version(a_assertContext), std::move(fields),
        std::move(reservedFields), a_assertContext));
}

/// @brief Transform、SceneObjectState、Test Componentを登録したRegistryを生成する
[[nodiscard]] std::unique_ptr<cue::schema::SchemaRegistry> make_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource,
    const cue::AssertContext &a_assertContext) noexcept
{
    cue::schema::SchemaRegistryBuilder builder(a_identitySource,
                                               a_assertContext);
    require(builder.add_type(make_type_descriptor(
                "10000000-0000-4000-8000-000000000001",
                "Cue.Scene.SceneObjectState", a_assertContext))
                .has_value());
    require(builder.add_type(make_type_descriptor(
                "20000000-0000-4000-8000-000000000002",
                "Cue.Test.RuntimeComponent", a_assertContext))
                .has_value());
    require(builder.add_type(make_type_descriptor(
                "50000000-0000-4000-8000-000000000005",
                "Cue.Core.Transform", a_assertContext))
                .has_value());
    return take_value(builder.seal());
}

/// @brief Test Component向けのFieldなしValue Schema Registryを生成する
[[nodiscard]] cue::scene::ComponentValueSchemaRegistry make_value_registry(
    const cue::schema::SchemaRegistry &a_registry,
    const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::scene::FieldKindBinding> bindings;
    std::vector<cue::scene::ComponentValueSchema> schemas;
    schemas.push_back(take_value(
        cue::scene::create_component_value_schema(
            make_type_id("20000000-0000-4000-8000-000000000002",
                         a_assertContext),
            make_version(a_assertContext), std::move(bindings), a_registry,
            a_assertContext)));
    return take_value(cue::scene::ComponentValueSchemaRegistry::create(
        std::move(schemas), a_registry, a_assertContext));
}

/// @brief FieldなしTest用Authoring Component Dataを生成する
[[nodiscard]] cue::scene::KnownComponentData make_known_component(
    SequentialIdentitySource &a_identitySource,
    const cue::schema::SchemaRegistry &a_registry,
    const cue::scene::ComponentValueSchemaRegistry &a_valueRegistry,
    const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::scene::KnownFieldData> knownFields;
    std::vector<cue::scene::OpaqueFieldData> unknownFields;
    return take_value(cue::scene::create_known_component(
        take_value(cue::scene::ComponentInstanceId::generate(
            a_identitySource, a_assertContext)),
        make_type_id("20000000-0000-4000-8000-000000000002",
                     a_assertContext),
        make_version(a_assertContext), std::move(knownFields),
        std::move(unknownFields), a_registry, a_valueRegistry,
        a_assertContext));
}

class TestComponentBuilder final
    : public cue::scene::RuntimeComponentBuilder
{
  public:
    /// @brief Runtime Tokenと失敗させるBuild回数を保持するBuilderを生成する
    TestComponentBuilder(
        cue::schema::TypeId a_typeId,
        cue::game_core::ComponentType<TestRuntimeComponent> a_componentType,
        std::uint32_t a_failureBuild) noexcept
        : m_typeId(a_typeId), m_componentType(a_componentType),
          m_failureBuild(a_failureBuild)
    {
    }

    /// @brief Test ComponentのStable Type Identityを返す
    [[nodiscard]] cue::schema::TypeId type_id() const noexcept override
    {
        return m_typeId;
    }

    /// @brief Test Component Tokenが指定Worldへ登録済みか返す
    [[nodiscard]] bool is_compatible(
        const cue::game_core::World &a_world) const noexcept override
    {
        return a_world.is_component_type_registered(m_componentType);
    }

    /// @brief FieldなしAuthoring DataだけをRuntime変換可能として受理する
    [[nodiscard]] cue::Result<void> validate(
        const cue::scene::KnownComponentData &a_data,
        const cue::AssertContext &a_assertContext) const noexcept override
    {
        if (a_data.type_id() != m_typeId ||
            !a_data.known_fields().empty() ||
            !a_data.unknown_fields().empty())
        {
            return cue::Result<void>::failure(cue::scene::make_scene_error(
                a_assertContext,
                cue::scene::SceneError::UnsupportedRuntimeComponent,
                "Test component data is not runtime compatible"));
        }

        return cue::Result<void>::success();
    }

    /// @brief 指定回数目なら失敗し、それ以外はTest値42を追加する
    [[nodiscard]] cue::Result<void> build(
        const cue::scene::KnownComponentData &,
        cue::game_core::World &a_world,
        cue::game_core::EntityHandle a_entity,
        const cue::AssertContext &a_assertContext) noexcept override
    {
        ++m_buildCount;

        if (m_failureBuild != 0U && m_buildCount == m_failureBuild)
        {
            return cue::Result<void>::failure(cue::scene::make_scene_error(
                a_assertContext,
                cue::scene::SceneError::RuntimeInstantiationFailed,
                "Injected runtime component build failure"));
        }

        auto result = a_world.add_component(
            m_componentType, a_entity, TestRuntimeComponent{42});

        if (!result)
        {
            return cue::Result<void>::failure(
                std::move(*result.try_error()));
        }

        return cue::Result<void>::success();
    }

  private:
    cue::schema::TypeId m_typeId;
    cue::game_core::ComponentType<TestRuntimeComponent> m_componentType;
    std::uint32_t m_failureBuild;
    std::uint32_t m_buildCount = 0U;
};

/// @brief RuntimeWorldを生成してCore Transformを初期化する
[[nodiscard]] std::unique_ptr<cue::game_core::RuntimeWorld> make_runtime_world(
    cue::game_core::WorldIdentitySource &a_identitySource,
    const cue::schema::SchemaRegistry &a_registry,
    const cue::AssertContext &a_assertContext) noexcept
{
    auto runtime = cue::game_core::RuntimeWorld::create(
        a_identitySource, a_registry,
        make_type_id("50000000-0000-4000-8000-000000000005",
                     a_assertContext),
        a_assertContext);
    require(runtime != nullptr);
    require(runtime->initialize().has_value());
    return runtime;
}

/// @brief Snapshot独立性、Hierarchy、Component、明示終了を検証する
void test_successful_instantiation() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource registryIdentitySource;
    auto registry = make_registry(registryIdentitySource, assertContext);
    auto valueRegistry = make_value_registry(*registry, assertContext);
    SequentialIdentitySource sceneIdentitySource;
    const auto rootId = take_value(cue::scene::ObjectId::generate(
        sceneIdentitySource, assertContext));
    const auto childId = take_value(cue::scene::ObjectId::generate(
        sceneIdentitySource, assertContext));
    auto document = cue::scene::SceneDocument::create(
        take_value(cue::scene::SceneAssetId::generate(
            sceneIdentitySource, assertContext)),
        assertContext);
    require(document.add_object(rootId, "Root", true, std::nullopt,
                                cue::math::Transform{})
                .has_value());
    require(document.add_object(childId, "Child", true, rootId,
                                cue::math::Transform{})
                .has_value());
    require(document.add_component(
                        childId, cue::scene::SceneComponent::known(
                                     make_known_component(
                                         sceneIdentitySource, *registry,
                                         valueRegistry, assertContext)))
                .has_value());
    auto snapshot = take_value(cue::scene::create_scene_snapshot(
        document, assertContext));
    require(document.set_active(rootId, false).has_value());
    require(document.set_parent(childId, std::nullopt).has_value());

    cue::game_core::WorldIdentitySource worldIdentitySource;
    auto runtime = make_runtime_world(worldIdentitySource, *registry,
                                      assertContext);
    auto *world = runtime->try_world();
    require(world != nullptr);
    auto stateType = take_value(
        world->register_component<cue::scene::SceneObjectState>(
            make_type_id("10000000-0000-4000-8000-000000000001",
                         assertContext)));
    auto customType = take_value(
        world->register_component<TestRuntimeComponent>(
            make_type_id("20000000-0000-4000-8000-000000000002",
                         assertContext)));
    TestComponentBuilder builder(
        make_type_id("20000000-0000-4000-8000-000000000002",
                     assertContext),
        customType, 0U);
    std::vector<cue::scene::RuntimeComponentBuilder *> builders{&builder};
    auto instance = take_value(cue::scene::SceneInstantiator::instantiate(
        snapshot, *runtime, stateType, builders, assertContext));
    require(instance.entity_count() == 2U);
    const auto *rootEntity = instance.find_entity(rootId);
    const auto *childEntity = instance.find_entity(childId);
    require(rootEntity != nullptr && childEntity != nullptr);
    auto childState = world->get_component(stateType, *childEntity);
    require(childState.has_value());
    require((*childState.try_value())->parent.has_value());
    require(*(*childState.try_value())->parent == *rootEntity);
    require((*childState.try_value())->isSelfActive);
    require((*childState.try_value())->isEffectiveActive);
    auto childTransform = world->get_component(
        *runtime->try_transform_type(), *childEntity);
    require(childTransform.has_value());
    require((*childTransform.try_value())->translation() ==
            cue::math::Vector3{});
    auto custom = world->get_component(customType, *childEntity);
    require(custom.has_value() && (*custom.try_value())->value == 42);

    (*childState.try_value())->isSelfActive = false;
    require(document.find_object(childId)->is_active());
    auto secondInstance = take_value(
        cue::scene::SceneInstantiator::instantiate(
            snapshot, *runtime, stateType, builders, assertContext));
    require(secondInstance.find_entity(rootId)->index() <
            secondInstance.find_entity(childId)->index());
    auto secondEnd = secondInstance.end(*runtime, assertContext);
    require(secondEnd.has_value() && secondEnd.try_value()->succeeded());
    auto firstEnd = instance.end(*runtime, assertContext);
    require(firstEnd.has_value() && firstEnd.try_value()->succeeded());
    auto repeatedEnd = instance.end(*runtime, assertContext);
    require(repeatedEnd.has_value() && repeatedEnd.try_value()->succeeded());
    require(world->entity_count() == 0U);

    auto emptyDocument = cue::scene::SceneDocument::create(
        take_value(cue::scene::SceneAssetId::generate(
            sceneIdentitySource, assertContext)),
        assertContext);
    auto emptySnapshot = take_value(cue::scene::create_scene_snapshot(
        emptyDocument, assertContext));
    auto emptyInstance = take_value(
        cue::scene::SceneInstantiator::instantiate(
            emptySnapshot, *runtime, stateType, builders, assertContext));
    require(emptyInstance.is_live());
    require(emptyInstance.try_scene_asset_id() != nullptr);
    require(emptyInstance.entity_count() == 0U);
    require(emptyInstance.end(*runtime, assertContext).has_value());
    require(!emptyInstance.is_live());
    require(emptyInstance.try_scene_asset_id() == nullptr);
    require(runtime->shutdown().has_value());
    require(document.find_object(rootId) != nullptr);
}

/// @brief 未対応Data拒否、途中失敗Rollback、World一致検証を確認する
void test_failure_and_world_identity() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource registryIdentitySource;
    auto registry = make_registry(registryIdentitySource, assertContext);
    auto valueRegistry = make_value_registry(*registry, assertContext);
    SequentialIdentitySource sceneIdentitySource;
    const auto objectId = take_value(cue::scene::ObjectId::generate(
        sceneIdentitySource, assertContext));
    const auto secondObjectId = take_value(cue::scene::ObjectId::generate(
        sceneIdentitySource, assertContext));
    auto document = cue::scene::SceneDocument::create(
        take_value(cue::scene::SceneAssetId::generate(
            sceneIdentitySource, assertContext)),
        assertContext);
    require(document.add_object(objectId, "Object", true, std::nullopt,
                                cue::math::Transform{})
                .has_value());
    require(document.add_object(secondObjectId, "Second", true,
                                std::nullopt, cue::math::Transform{})
                .has_value());
    require(document.add_component(
                        objectId, cue::scene::SceneComponent::known(
                                      make_known_component(
                                          sceneIdentitySource, *registry,
                                          valueRegistry, assertContext)))
                .has_value());
    require(document.add_component(
                        secondObjectId,
                        cue::scene::SceneComponent::known(
                            make_known_component(
                                sceneIdentitySource, *registry,
                                valueRegistry, assertContext)))
                .has_value());
    auto snapshot = take_value(cue::scene::create_scene_snapshot(
        document, assertContext));
    cue::game_core::WorldIdentitySource worldIdentitySource;
    auto runtime = make_runtime_world(worldIdentitySource, *registry,
                                      assertContext);
    auto *world = runtime->try_world();
    require(world != nullptr);
    auto stateType = take_value(
        world->register_component<cue::scene::SceneObjectState>(
            make_type_id("10000000-0000-4000-8000-000000000001",
                         assertContext)));
    auto customType = take_value(
        world->register_component<TestRuntimeComponent>(
            make_type_id("20000000-0000-4000-8000-000000000002",
                         assertContext)));
    TestComponentBuilder failingBuilder(
        make_type_id("20000000-0000-4000-8000-000000000002",
                     assertContext),
        customType, 2U);
    std::vector<cue::scene::RuntimeComponentBuilder *> failingBuilders{
        &failingBuilder};
    const auto entityCountBefore = world->entity_count();
    auto failed = cue::scene::SceneInstantiator::instantiate(
        snapshot, *runtime, stateType, failingBuilders, assertContext);
    require(!failed.has_value());
    require(world->entity_count() == entityCountBefore);

    std::vector<cue::scene::RuntimeComponentBuilder *> noBuilders;
    auto unsupported = cue::scene::SceneInstantiator::instantiate(
        snapshot, *runtime, stateType, noBuilders, assertContext);
    require(!unsupported.has_value());
    require(unsupported.try_error()->code().value() ==
            static_cast<std::int64_t>(
                cue::scene::SceneError::UnsupportedRuntimeComponent));
    require(world->entity_count() == entityCountBefore);

    TestComponentBuilder builder(
        make_type_id("20000000-0000-4000-8000-000000000002",
                     assertContext),
        customType, 0U);
    std::vector<cue::scene::RuntimeComponentBuilder *> builders{&builder};
    auto instance = take_value(cue::scene::SceneInstantiator::instantiate(
        snapshot, *runtime, stateType, builders, assertContext));
    auto otherRuntime = make_runtime_world(worldIdentitySource, *registry,
                                           assertContext);
    auto wrongWorld = instance.end(*otherRuntime, assertContext);
    require(!wrongWorld.has_value());
    require(wrongWorld.try_error()->code().value() ==
            static_cast<std::int64_t>(
                cue::scene::SceneError::RuntimeWorldMismatch));
    require(instance.is_live());
    require(instance.end(*runtime, assertContext).has_value());
    require(runtime->request_stop().has_value());
    auto stoppingInstantiation =
        cue::scene::SceneInstantiator::instantiate(
            snapshot, *runtime, stateType, builders, assertContext);
    require(!stoppingInstantiation.has_value());
    require(stoppingInstantiation.try_error()->code().value() ==
            static_cast<std::int64_t>(
                cue::scene::SceneError::RuntimeInstantiationFailed));
    require(runtime->tick().has_value());
    require(otherRuntime->shutdown().has_value());
}

/// @brief Runtime SnapshotがAuthoring上限とHierarchy検証を分離するか確認する
void test_runtime_scene_snapshot_boundary() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    SequentialIdentitySource identitySource;

    std::vector<cue::scene::RuntimeSceneObjectData> objects;
    objects.reserve(cue::scene::k_maximumSceneObjectCount + 1U);
    for (std::size_t index = 0U; index <= cue::scene::k_maximumSceneObjectCount; ++index)
    {
        objects.push_back({take_value(cue::scene::ObjectId::generate(identitySource, assertContext)),
                           std::nullopt, true, cue::math::Transform{}});
    }
    auto snapshot = cue::scene::create_runtime_scene_snapshot(
        take_value(cue::scene::SceneAssetId::generate(identitySource, assertContext)), std::move(objects),
        assertContext);
    require(snapshot.has_value());
    require(snapshot.try_value()->objects().size() == cue::scene::k_maximumSceneObjectCount + 1U);

    const auto duplicateId = take_value(cue::scene::ObjectId::generate(identitySource, assertContext));
    std::vector<cue::scene::RuntimeSceneObjectData> duplicateObjects{
        {duplicateId, std::nullopt, true, cue::math::Transform{}},
        {duplicateId, std::nullopt, true, cue::math::Transform{}}};
    auto duplicate = cue::scene::create_runtime_scene_snapshot(
        take_value(cue::scene::SceneAssetId::generate(identitySource, assertContext)),
        std::move(duplicateObjects), assertContext);
    require(!duplicate.has_value());
    require(duplicate.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::DuplicateObjectId));

    const auto danglingId = take_value(cue::scene::ObjectId::generate(identitySource, assertContext));
    std::vector<cue::scene::RuntimeSceneObjectData> danglingObjects{
        {take_value(cue::scene::ObjectId::generate(identitySource, assertContext)), danglingId, true,
         cue::math::Transform{}}};
    auto dangling = cue::scene::create_runtime_scene_snapshot(
        take_value(cue::scene::SceneAssetId::generate(identitySource, assertContext)),
        std::move(danglingObjects), assertContext);
    require(!dangling.has_value());
    require(dangling.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::DanglingParent));

    const auto firstCycleId = take_value(cue::scene::ObjectId::generate(identitySource, assertContext));
    const auto secondCycleId = take_value(cue::scene::ObjectId::generate(identitySource, assertContext));
    std::vector<cue::scene::RuntimeSceneObjectData> cycleObjects{
        {firstCycleId, secondCycleId, true, cue::math::Transform{}},
        {secondCycleId, firstCycleId, true, cue::math::Transform{}}};
    auto cycle = cue::scene::create_runtime_scene_snapshot(
        take_value(cue::scene::SceneAssetId::generate(identitySource, assertContext)), std::move(cycleObjects),
        assertContext);
    require(!cycle.has_value());
    require(cycle.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::HierarchyCycle));

    std::vector<cue::scene::RuntimeSceneObjectData> deepObjects;
    deepObjects.reserve(cue::scene::SceneDocument::maximum_hierarchy_depth() + 1U);
    std::optional<cue::scene::ObjectId> parentId;
    for (std::size_t depth = 0U; depth <= cue::scene::SceneDocument::maximum_hierarchy_depth(); ++depth)
    {
        const auto objectId = take_value(cue::scene::ObjectId::generate(identitySource, assertContext));
        deepObjects.push_back({objectId, parentId, true, cue::math::Transform{}});
        parentId = objectId;
    }
    auto tooDeep = cue::scene::create_runtime_scene_snapshot(
        take_value(cue::scene::SceneAssetId::generate(identitySource, assertContext)), std::move(deepObjects),
        assertContext);
    require(!tooDeep.has_value());
    require(tooDeep.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::HierarchyDepthExceeded));

    cue::schema::SchemaRegistryIdentitySource registryIdentitySource;
    std::unique_ptr<cue::schema::SchemaRegistry> registry = make_registry(registryIdentitySource, assertContext);
    cue::scene::ComponentValueSchemaRegistry valueRegistry = make_value_registry(*registry, assertContext);
    const auto makeObject = [&](std::vector<cue::scene::SceneComponent> a_components)
    {
        return cue::scene::RuntimeSceneObjectData{
            take_value(cue::scene::ObjectId::generate(identitySource, assertContext)), std::nullopt, true,
            cue::math::Transform{}, std::move(a_components)};
    };
    const auto snapshotFrom = [&](std::vector<cue::scene::RuntimeSceneObjectData> a_objects)
    {
        return cue::scene::create_runtime_scene_snapshot(
            take_value(cue::scene::SceneAssetId::generate(identitySource, assertContext)),
            std::move(a_objects), assertContext);
    };

    const cue::scene::KnownComponentData duplicateData =
        make_known_component(identitySource, *registry, valueRegistry, assertContext);
    std::vector<cue::scene::SceneComponent> firstDuplicate;
    firstDuplicate.push_back(cue::scene::SceneComponent::known(duplicateData));
    std::vector<cue::scene::SceneComponent> secondDuplicate;
    secondDuplicate.push_back(cue::scene::SceneComponent::known(duplicateData));
    std::vector<cue::scene::RuntimeSceneObjectData> duplicateComponentObjects;
    duplicateComponentObjects.push_back(makeObject(std::move(firstDuplicate)));
    duplicateComponentObjects.push_back(makeObject(std::move(secondDuplicate)));
    auto duplicateComponent = snapshotFrom(std::move(duplicateComponentObjects));
    require(!duplicateComponent.has_value());
    require(duplicateComponent.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::DuplicateComponentId));

    const cue::scene::KnownComponentData earlierData =
        make_known_component(identitySource, *registry, valueRegistry, assertContext);
    const cue::scene::KnownComponentData laterData =
        make_known_component(identitySource, *registry, valueRegistry, assertContext);
    std::vector<cue::scene::SceneComponent> unorderedComponents;
    unorderedComponents.push_back(cue::scene::SceneComponent::known(laterData));
    unorderedComponents.push_back(cue::scene::SceneComponent::known(earlierData));
    std::vector<cue::scene::RuntimeSceneObjectData> unorderedObjects;
    unorderedObjects.push_back(makeObject(std::move(unorderedComponents)));
    auto unordered = snapshotFrom(std::move(unorderedObjects));
    require(!unordered.has_value());
    require(unordered.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::DuplicateComponentId));

    std::vector<cue::scene::SceneComponent> excessiveComponents;
    excessiveComponents.reserve(cue::scene::k_maximumSceneComponentsPerObject + 1U);
    for (std::size_t index = 0U; index <= cue::scene::k_maximumSceneComponentsPerObject; ++index)
    {
        excessiveComponents.push_back(cue::scene::SceneComponent::known(duplicateData));
    }
    std::vector<cue::scene::RuntimeSceneObjectData> excessiveObjects;
    excessiveObjects.push_back(makeObject(std::move(excessiveComponents)));
    auto excessive = snapshotFrom(std::move(excessiveObjects));
    require(!excessive.has_value());
    require(excessive.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::ResourceLimitExceeded));

    cue::scene::SceneComponent movedFrom = cue::scene::SceneComponent::known(duplicateData);
    cue::scene::SceneComponent movedTo = std::move(movedFrom);
    require(movedTo.is_valid() && !movedFrom.is_valid());
    std::vector<cue::scene::SceneComponent> invalidComponents;
    invalidComponents.push_back(std::move(movedFrom));
    std::vector<cue::scene::RuntimeSceneObjectData> invalidObjects;
    invalidObjects.push_back(makeObject(std::move(invalidComponents)));
    auto invalid = snapshotFrom(std::move(invalidObjects));
    require(!invalid.has_value());
    require(invalid.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::InvalidComponentData));
}
} // namespace

/// @brief Cue.Scene Runtime実体化契約のUnit Testを実行する
int main()
{
    test_successful_instantiation();
    test_failure_and_world_identity();
    test_runtime_scene_snapshot_boundary();
    return 0;
}
