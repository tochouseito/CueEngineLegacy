#include <Cue/Scene/Instantiation.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/EmergencyHandler.h>
#include <Cue/Scene/Error.h>

#include <algorithm>
#include <limits>
#include <new>
#include <set>
#include <utility>

namespace
{
struct PlannedComponent final
{
    const cue::scene::KnownComponentData *data;
    cue::scene::RuntimeComponentBuilder *builder;
};

struct PlannedObject final
{
    const cue::scene::SceneObject *source;
    bool isEffectiveActive;
    std::vector<PlannedComponent> components;
};

/// @brief SceneObjectをStable ObjectId辞書順に比較する
[[nodiscard]] bool scene_object_id_less(
    const cue::scene::SceneObject &a_left,
    const cue::scene::SceneObject &a_right) noexcept
{
    return a_left.id() < a_right.id();
}

/// @brief Allocation失敗を追加Allocationなしの終了経路へ送る
[[noreturn]] void terminate_allocation(
    cue::EmergencyHandler &a_emergencyHandler) noexcept
{
    a_emergencyHandler.terminate("Scene runtime instantiation allocation failed");
}

/// @brief 予期しない例外を追加Allocationなしの終了経路へ送る
[[noreturn]] void terminate_exception(
    cue::EmergencyHandler &a_emergencyHandler) noexcept
{
    a_emergencyHandler.terminate("Scene runtime instantiation raised an unexpected exception");
}

/// @brief 実体化失敗時にOperationが生成した全生存Entityを逆順に破棄する
void rollback_created_entities(
    cue::game_core::World &a_world,
    const std::vector<std::pair<cue::scene::ObjectId,
                                cue::game_core::EntityHandle>> &a_creationOrder,
    cue::EmergencyHandler &a_emergencyHandler) noexcept
{
    for (auto iterator = a_creationOrder.rbegin();
         iterator != a_creationOrder.rend(); ++iterator)
    {
        const auto entity = iterator->second;

        if (!a_world.is_alive(entity))
        {
            continue;
        }

        auto destroyResult = a_world.destroy_entity(entity);

        if (!destroyResult)
        {
            a_emergencyHandler.terminate(
                "Scene runtime instantiation rollback failed");
        }
    }
}

/// @brief Object自身と全AncestorのActive状態から実効Activeを計算する
[[nodiscard]] bool calculate_effective_active(
    const cue::scene::SceneObject &a_object,
    const std::map<cue::scene::ObjectId, const cue::scene::SceneObject *> &a_objectsById) noexcept
{
    bool isEffectiveActive = a_object.is_active();
    const cue::scene::ObjectId *parentId = a_object.try_parent_id();

    while (parentId != nullptr)
    {
        const auto parentIterator = a_objectsById.find(*parentId);

        if (parentIterator == a_objectsById.end())
        {
            return false;
        }

        const auto &parent = *parentIterator->second;
        isEffectiveActive = isEffectiveActive && parent.is_active();
        parentId = parent.try_parent_id();
    }

    return isEffectiveActive;
}

/// @brief Structural EpochにForward処理とRollbackの両方を収容できるか返す
[[nodiscard]] bool has_required_structural_capacity(
    const cue::game_core::World &a_world, std::size_t a_objectCount,
    std::size_t a_customComponentCount) noexcept
{
    constexpr auto maximumEpoch = std::numeric_limits<std::uint64_t>::max();
    const auto available = maximumEpoch - a_world.structural_epoch();

    if (a_objectCount > maximumEpoch / 4U)
    {
        return false;
    }

    const auto coreAndRollback =
        static_cast<std::uint64_t>(a_objectCount) * 4U;

    if (a_customComponentCount > maximumEpoch - coreAndRollback)
    {
        return false;
    }

    return coreAndRollback +
               static_cast<std::uint64_t>(a_customComponentCount) <=
           available;
}
} // namespace

namespace cue::scene
{
/// @brief 検証済みScene IdentityとObject集合を所有するSnapshotを構築する
SceneSnapshot::SceneSnapshot(SceneAssetId a_sceneAssetId,
                             std::vector<SceneObject> a_objects) noexcept
    : m_sceneAssetId(std::move(a_sceneAssetId)),
      m_objects(std::move(a_objects))
{
}

/// @brief 検証済みRuntime入力を所有Scene Objectへ変換する
SceneObject SceneSnapshot::make_runtime_object(RuntimeSceneObjectData a_object) noexcept
{
    const IdentityText name = a_object.id.canonical_text();
    return SceneObject(std::move(a_object.id), std::string(name.data(), name.size()), a_object.isActive,
                       std::move(a_object.parentId), std::move(a_object.transform), std::move(a_object.components));
}

/// @brief Snapshotが表す永続Scene Identityを返す
const SceneAssetId &SceneSnapshot::scene_asset_id() const noexcept
{
    return m_sceneAssetId;
}

/// @brief ObjectId辞書順へ固定した不変Object集合を返す
std::span<const SceneObject> SceneSnapshot::objects() const noexcept
{
    return m_objects;
}

/// @brief SceneDocumentを再検証しRuntimeから独立した不変Snapshotへ複製する
Result<SceneSnapshot> create_scene_snapshot(
    const SceneDocument &a_document,
    const AssertContext &a_assertContext) noexcept
{
    auto validation = a_document.validate();

    if (!validation)
    {
        return Result<SceneSnapshot>::failure(
            std::move(*validation.try_error()));
    }

    try
    {
        const auto sourceObjects = a_document.objects();
        std::vector<SceneObject> objects(sourceObjects.begin(),
                                         sourceObjects.end());
        std::sort(objects.begin(), objects.end(), scene_object_id_less);
        return Result<SceneSnapshot>::success(SceneSnapshot(
            a_document.scene_asset_id(), std::move(objects)));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext.fatal_handler());
    }
    catch (...)
    {
        terminate_exception(a_assertContext.fatal_handler());
    }
}

/// @brief Runtime Scene DataをAuthoring上限から独立した不変Snapshotへ変換する
Result<SceneSnapshot> create_runtime_scene_snapshot(
    SceneAssetId a_sceneAssetId, std::vector<RuntimeSceneObjectData> a_objects,
    const AssertContext &a_assertContext) noexcept
{
    if (a_objects.size() > k_maximumRuntimeSceneObjectCount)
    {
        return Result<SceneSnapshot>::failure(make_scene_error(
            a_assertContext, SceneError::ResourceLimitExceeded,
            "Runtime Scene object count exceeds the 1000000 element limit"));
    }

    try
    {
        const auto objectIdLess = [](const RuntimeSceneObjectData &a_left,
                                     const RuntimeSceneObjectData &a_right) noexcept
        {
            return a_left.id < a_right.id;
        };
        std::sort(a_objects.begin(), a_objects.end(), objectIdLess);
        for (std::size_t index = 1U; index < a_objects.size(); ++index)
        {
            if (!(a_objects[index - 1U].id < a_objects[index].id))
            {
                return Result<SceneSnapshot>::failure(make_scene_error(
                    a_assertContext, SceneError::DuplicateObjectId,
                    "Runtime Scene object identities must be unique"));
            }
        }

        std::vector<ComponentInstanceId> componentIds;
        for (const RuntimeSceneObjectData &object : a_objects)
        {
            if (object.components.size() > k_maximumSceneComponentsPerObject)
            {
                return Result<SceneSnapshot>::failure(make_scene_error(
                    a_assertContext, SceneError::ResourceLimitExceeded,
                    "Runtime Scene component count exceeds the 4096 element limit"));
            }
            const ComponentInstanceId *previousId = nullptr;
            for (const SceneComponent &component : object.components)
            {
                if (!component.is_valid())
                {
                    return Result<SceneSnapshot>::failure(make_scene_error(
                        a_assertContext, SceneError::InvalidComponentData,
                        "Runtime Scene contains moved-from component data"));
                }
                const ComponentInstanceId &componentId = component.instance_id();
                if (previousId != nullptr && !(*previousId < componentId))
                {
                    return Result<SceneSnapshot>::failure(make_scene_error(
                        a_assertContext, SceneError::DuplicateComponentId,
                        "Runtime Scene component identities must be stable ordered"));
                }
                componentIds.push_back(componentId);
                previousId = &componentId;
            }
        }
        std::sort(componentIds.begin(), componentIds.end());
        if (std::adjacent_find(componentIds.begin(), componentIds.end()) != componentIds.end())
        {
            return Result<SceneSnapshot>::failure(make_scene_error(
                a_assertContext, SceneError::DuplicateComponentId,
                "Runtime Scene component identities must be unique across objects"));
        }

        constexpr std::size_t k_noParent = std::numeric_limits<std::size_t>::max();
        std::vector<std::size_t> parentIndices(a_objects.size(), k_noParent);
        for (std::size_t index = 0U; index < a_objects.size(); ++index)
        {
            if (!a_objects[index].parentId.has_value())
            {
                continue;
            }
            const ObjectId &parentId = *a_objects[index].parentId;
            const auto parent = std::lower_bound(
                a_objects.begin(), a_objects.end(), parentId,
                [](const RuntimeSceneObjectData &a_object, const ObjectId &a_id) noexcept
                {
                    return a_object.id < a_id;
                });
            if (parent == a_objects.end() || parent->id != parentId)
            {
                return Result<SceneSnapshot>::failure(make_scene_error(
                    a_assertContext, SceneError::DanglingParent,
                    "Runtime Scene parent identity is not present in the object set"));
            }
            const std::size_t parentIndex = static_cast<std::size_t>(parent - a_objects.begin());
            if (parentIndex == index)
            {
                return Result<SceneSnapshot>::failure(make_scene_error(
                    a_assertContext, SceneError::HierarchyCycle,
                    "Runtime Scene object cannot parent itself"));
            }
            parentIndices[index] = parentIndex;
        }

        std::vector<std::uint8_t> visitStates(a_objects.size(), 0U);
        std::vector<std::size_t> hierarchyDepths(a_objects.size(), 0U);
        std::vector<std::size_t> path;
        path.reserve(std::min(a_objects.size(), SceneDocument::maximum_hierarchy_depth() + 1U));
        for (std::size_t start = 0U; start < a_objects.size(); ++start)
        {
            if (visitStates[start] == 2U)
            {
                continue;
            }
            path.clear();
            std::size_t current = start;
            while (current != k_noParent && visitStates[current] == 0U)
            {
                visitStates[current] = 1U;
                path.push_back(current);
                current = parentIndices[current];
            }
            if (current != k_noParent && visitStates[current] == 1U)
            {
                return Result<SceneSnapshot>::failure(make_scene_error(
                    a_assertContext, SceneError::HierarchyCycle,
                    "Runtime Scene hierarchy contains a cycle"));
            }

            std::size_t depth = current == k_noParent ? 0U : hierarchyDepths[current];
            while (!path.empty())
            {
                if (depth >= SceneDocument::maximum_hierarchy_depth())
                {
                    return Result<SceneSnapshot>::failure(make_scene_error(
                        a_assertContext, SceneError::HierarchyDepthExceeded,
                        "Runtime Scene hierarchy exceeds the supported depth"));
                }
                ++depth;
                const std::size_t objectIndex = path.back();
                path.pop_back();
                hierarchyDepths[objectIndex] = depth;
                visitStates[objectIndex] = 2U;
            }
        }

        std::vector<SceneObject> sceneObjects;
        sceneObjects.reserve(a_objects.size());
        for (RuntimeSceneObjectData &object : a_objects)
        {
            sceneObjects.push_back(SceneSnapshot::make_runtime_object(std::move(object)));
        }
        return Result<SceneSnapshot>::success(
            SceneSnapshot(std::move(a_sceneAssetId), std::move(sceneObjects)));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext.fatal_handler());
    }
    catch (...)
    {
        terminate_exception(a_assertContext.fatal_handler());
    }
}

/// @brief 失敗したEntityと回復可能Errorを所有する
SceneInstanceEndFailure::SceneInstanceEndFailure(
    game_core::EntityHandle a_entity, Error a_error) noexcept
    : m_entity(a_entity), m_error(std::move(a_error))
{
}

/// @brief 破棄できなかったEntityを返す
game_core::EntityHandle SceneInstanceEndFailure::entity() const noexcept
{
    return m_entity;
}

/// @brief Entity破棄Errorを返す
const Error &SceneInstanceEndFailure::error() const noexcept
{
    return m_error;
}

/// @brief 終了処理が保持した失敗集合を所有する
SceneInstanceEndReport::SceneInstanceEndReport(
    std::vector<SceneInstanceEndFailure> a_failures) noexcept
    : m_failures(std::move(a_failures))
{
}

/// @brief 全Entityの終了に成功した場合にtrueを返す
bool SceneInstanceEndReport::succeeded() const noexcept
{
    return m_failures.empty();
}

/// @brief 逆生成順で収集したEntity破棄失敗を返す
std::span<const SceneInstanceEndFailure>
SceneInstanceEndReport::failures() const noexcept
{
    return m_failures;
}

/// @brief 実体化済みScene Identity、World Identity、Entity集合を所有する
SceneInstance::SceneInstance(
    SceneAssetId a_sceneAssetId, std::uint64_t a_worldId,
    std::map<ObjectId, game_core::EntityHandle> a_mapping,
    std::vector<std::pair<ObjectId, game_core::EntityHandle>> a_creationOrder,
    EmergencyHandler &a_emergencyHandler) noexcept
    : m_sceneAssetId(std::move(a_sceneAssetId)), m_worldId(a_worldId),
      m_mapping(std::move(a_mapping)),
      m_creationOrder(std::move(a_creationOrder)),
      m_emergencyHandler(&a_emergencyHandler)
{
}

/// @brief 所有集合を移動し移動元を終了済み状態にする
SceneInstance::SceneInstance(SceneInstance &&a_other) noexcept
    : m_sceneAssetId(std::move(a_other.m_sceneAssetId)),
      m_worldId(std::move(a_other.m_worldId)),
      m_mapping(std::move(a_other.m_mapping)),
      m_creationOrder(std::move(a_other.m_creationOrder)),
      m_emergencyHandler(a_other.m_emergencyHandler)
{
    a_other.clear_moved_from();
}

/// @brief 終了済み移動先へ所有集合を移しlive移動先ならFatal終了する
SceneInstance &SceneInstance::operator=(SceneInstance &&a_other) noexcept
{
    if (this == &a_other)
    {
        return *this;
    }

    if (is_live())
    {
        m_emergencyHandler->terminate(
            "Live SceneInstance move assignment would abandon runtime entities");
    }

    m_sceneAssetId = std::move(a_other.m_sceneAssetId);
    m_worldId = std::move(a_other.m_worldId);
    m_mapping = std::move(a_other.m_mapping);
    m_creationOrder = std::move(a_other.m_creationOrder);
    m_emergencyHandler = a_other.m_emergencyHandler;
    a_other.clear_moved_from();
    return *this;
}

/// @brief 終了済み状態を破棄しlive状態ならFatal終了する
SceneInstance::~SceneInstance() noexcept
{
    if (is_live())
    {
        m_emergencyHandler->terminate(
            "Live SceneInstance destruction would abandon runtime entities");
    }
}

/// @brief 空Sceneを含めRuntime Worldとの未終了の関連付けがある場合にtrueを返す
bool SceneInstance::is_live() const noexcept
{
    return m_worldId.has_value();
}

/// @brief 元Scene Identityまたは終了済み状態のnullptrを返す
const SceneAssetId *SceneInstance::try_scene_asset_id() const noexcept
{
    return m_sceneAssetId ? &*m_sceneAssetId : nullptr;
}

/// @brief ObjectIdに対応する現在生存所有Entityまたはnullptrを返す
const game_core::EntityHandle *SceneInstance::find_entity(
    const ObjectId &a_objectId) const noexcept
{
    const auto iterator = m_mapping.find(a_objectId);
    return iterator == m_mapping.end() ? nullptr : &iterator->second;
}

/// @brief 現在生存している所有Entity数を返す
std::size_t SceneInstance::entity_count() const noexcept
{
    return m_creationOrder.size();
}

/// @brief 同じRuntime World上の所有Entityを逆生成順に終了して結果を返す
Result<SceneInstanceEndReport> SceneInstance::end(
    game_core::RuntimeWorld &a_runtimeWorld,
    const AssertContext &a_assertContext) noexcept
{
    if (!m_worldId)
    {
        return Result<SceneInstanceEndReport>::success(
            SceneInstanceEndReport({}));
    }

    auto *world = a_runtimeWorld.try_world();

    if (world == nullptr || world->id() != *m_worldId)
    {
        return Result<SceneInstanceEndReport>::failure(make_scene_error(
            a_assertContext, SceneError::RuntimeWorldMismatch,
            "SceneInstance can only end against its original runtime world"));
    }

    try
    {
        std::vector<SceneInstanceEndFailure> failures;
        std::vector<std::pair<ObjectId, game_core::EntityHandle>> retained;
        failures.reserve(m_creationOrder.size());
        retained.reserve(m_creationOrder.size());

        for (auto iterator = m_creationOrder.rbegin();
             iterator != m_creationOrder.rend(); ++iterator)
        {
            const auto &objectId = iterator->first;
            const auto entity = iterator->second;

            if (!world->is_alive(entity))
            {
                m_mapping.erase(objectId);
                continue;
            }

            auto destroyResult = world->destroy_entity(entity);

            if (destroyResult)
            {
                m_mapping.erase(objectId);
                continue;
            }

            failures.emplace_back(
                entity, std::move(*destroyResult.try_error()));
            retained.push_back(*iterator);
        }

        std::reverse(retained.begin(), retained.end());
        m_creationOrder = std::move(retained);

        if (m_creationOrder.empty())
        {
            m_mapping.clear();
            m_sceneAssetId.reset();
            m_worldId.reset();
        }

        return Result<SceneInstanceEndReport>::success(
            SceneInstanceEndReport(std::move(failures)));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(*m_emergencyHandler);
    }
    catch (...)
    {
        terminate_exception(*m_emergencyHandler);
    }
}

/// @brief 移動元をWorld参照不要な終了済み状態へ移す
void SceneInstance::clear_moved_from() noexcept
{
    m_sceneAssetId.reset();
    m_worldId.reset();
    m_mapping.clear();
    m_creationOrder.clear();
}

/// @brief Snapshotを事前検証したPlanからRuntimeWorldへ一方向実体化する
Result<SceneInstance> SceneInstantiator::instantiate(
    const SceneSnapshot &a_snapshot,
    game_core::RuntimeWorld &a_runtimeWorld,
    game_core::ComponentType<SceneObjectState> a_sceneObjectStateType,
    std::span<RuntimeComponentBuilder *const> a_componentBuilders,
    const AssertContext &a_assertContext) noexcept
{
    if (a_runtimeWorld.state() != game_core::RuntimeWorldState::Running)
    {
        return Result<SceneInstance>::failure(make_scene_error(
            a_assertContext, SceneError::RuntimeInstantiationFailed,
            "Scene instantiation requires a running runtime world"));
    }

    auto *world = a_runtimeWorld.try_world();
    const auto *transformType = a_runtimeWorld.try_transform_type();

    if (world == nullptr || transformType == nullptr)
    {
        return Result<SceneInstance>::failure(make_scene_error(
            a_assertContext, SceneError::RuntimeInstantiationFailed,
            "Scene instantiation requires an operational runtime world"));
    }

    if (!world->is_component_type_registered(a_sceneObjectStateType) ||
        a_sceneObjectStateType.dense_index() == transformType->dense_index())
    {
        return Result<SceneInstance>::failure(make_scene_error(
            a_assertContext, SceneError::RuntimeInstantiationFailed,
            "SceneObjectState component token is not registered for this runtime world"));
    }

    try
    {
        std::map<schema::TypeId, RuntimeComponentBuilder *> buildersByType;

        for (auto *builder : a_componentBuilders)
        {
            if (builder == nullptr || !builder->is_compatible(*world) ||
                !buildersByType.emplace(builder->type_id(), builder).second)
            {
                return Result<SceneInstance>::failure(make_scene_error(
                    a_assertContext, SceneError::UnsupportedRuntimeComponent,
                    "Runtime component builders must be non-null, unique, and compatible with the target world"));
            }
        }

        std::map<ObjectId, const SceneObject *> objectsById;

        for (const auto &object : a_snapshot.objects())
        {
            objectsById.emplace(object.id(), &object);
        }

        std::vector<PlannedObject> plan;
        plan.reserve(a_snapshot.objects().size());
        std::size_t customComponentCount = 0U;

        for (const auto &object : a_snapshot.objects())
        {
            PlannedObject plannedObject{
                &object,
                calculate_effective_active(object, objectsById),
                {}};
            plannedObject.components.reserve(object.components().size());
            std::set<schema::TypeId> objectComponentTypes;

            for (const auto &component : object.components())
            {
                const auto *knownData = component.try_known();

                if (knownData == nullptr || !knownData->unknown_fields().empty())
                {
                    return Result<SceneInstance>::failure(make_scene_error(
                        a_assertContext,
                        SceneError::UnsupportedRuntimeComponent,
                        "Runtime instantiation cannot ignore opaque component data or unknown fields"));
                }

                const auto builderIterator =
                    buildersByType.find(knownData->type_id());

                if (builderIterator == buildersByType.end() ||
                    !objectComponentTypes.insert(knownData->type_id()).second)
                {
                    return Result<SceneInstance>::failure(make_scene_error(
                        a_assertContext,
                        SceneError::UnsupportedRuntimeComponent,
                        "Every runtime component type requires exactly one builder and one value per object"));
                }

                auto componentValidation =
                    builderIterator->second->validate(*knownData,
                                                      a_assertContext);

                if (!componentValidation)
                {
                    return Result<SceneInstance>::failure(
                        std::move(*componentValidation.try_error()));
                }

                plannedObject.components.push_back(
                    PlannedComponent{knownData, builderIterator->second});
                ++customComponentCount;
            }

            plan.push_back(std::move(plannedObject));
        }

        if (!has_required_structural_capacity(
                *world, plan.size(), customComponentCount))
        {
            return Result<SceneInstance>::failure(make_scene_error(
                a_assertContext, SceneError::StructuralCapacityExceeded,
                "Runtime world lacks structural capacity for instantiation and rollback"));
        }

        std::map<ObjectId, game_core::EntityHandle> mapping;
        std::vector<std::pair<ObjectId, game_core::EntityHandle>> creationOrder;
        mapping.clear();
        creationOrder.reserve(plan.size());

        for (const auto &plannedObject : plan)
        {
            auto createResult = world->create_entity();

            if (!createResult)
            {
                auto error = std::move(*createResult.try_error());
                rollback_created_entities(*world, creationOrder,
                                          a_assertContext.fatal_handler());
                return Result<SceneInstance>::failure(std::move(error));
            }

            const auto entity = *createResult.try_value();
            mapping.emplace(plannedObject.source->id(), entity);
            creationOrder.emplace_back(plannedObject.source->id(), entity);
        }

        for (const auto &plannedObject : plan)
        {
            const auto entity = mapping.find(plannedObject.source->id())->second;
            std::optional<game_core::EntityHandle> parent;
            const auto *parentId = plannedObject.source->try_parent_id();

            if (parentId != nullptr)
            {
                parent = mapping.find(*parentId)->second;
            }

            auto transformResult = world->add_component(
                *transformType, entity, plannedObject.source->transform());

            if (!transformResult)
            {
                auto error = std::move(*transformResult.try_error());
                rollback_created_entities(*world, creationOrder,
                                          a_assertContext.fatal_handler());
                return Result<SceneInstance>::failure(std::move(error));
            }

            SceneObjectState state{
                plannedObject.source->id(), parent,
                plannedObject.source->is_active(),
                plannedObject.isEffectiveActive};
            auto stateResult = world->add_component(
                a_sceneObjectStateType, entity, std::move(state));

            if (!stateResult)
            {
                auto error = std::move(*stateResult.try_error());
                rollback_created_entities(*world, creationOrder,
                                          a_assertContext.fatal_handler());
                return Result<SceneInstance>::failure(std::move(error));
            }

            for (const auto &component : plannedObject.components)
            {
                const auto entityCountBefore = world->entity_count();
                const auto epochBefore = world->structural_epoch();
                auto buildResult = component.builder->build(
                    *component.data, *world, entity, a_assertContext);

                if (world->entity_count() != entityCountBefore ||
                    !world->is_alive(entity))
                {
                    a_assertContext.fatal_handler().terminate(
                        "Runtime component builder changed entity ownership");
                }

                if (buildResult &&
                    world->structural_epoch() != epochBefore + 1U)
                {
                    a_assertContext.fatal_handler().terminate(
                        "Runtime component builder did not add exactly one component");
                }

                if (!buildResult)
                {
                    auto error = std::move(*buildResult.try_error());
                    rollback_created_entities(
                        *world, creationOrder,
                        a_assertContext.fatal_handler());
                    return Result<SceneInstance>::failure(std::move(error));
                }
            }
        }

        return Result<SceneInstance>::success(SceneInstance(
            a_snapshot.scene_asset_id(), world->id(), std::move(mapping),
            std::move(creationOrder), a_assertContext.fatal_handler()));
    }
    catch (const std::bad_alloc &)
    {
        terminate_allocation(a_assertContext.fatal_handler());
    }
    catch (...)
    {
        terminate_exception(a_assertContext.fatal_handler());
    }
}
} // namespace cue::scene
