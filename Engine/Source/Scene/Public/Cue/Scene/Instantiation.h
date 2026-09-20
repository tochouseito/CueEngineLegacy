#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/GameCore/RuntimeWorld.h>
#include <Cue/Scene/SceneDocument.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cue
{
class AssertContext;
class EmergencyHandler;
}

namespace cue::scene
{
inline constexpr std::size_t k_maximumRuntimeSceneObjectCount = 1'000'000U;

/// @brief Authoring固有Dataを含まないRuntime Scene ObjectのSnapshot構築入力
/// @details Componentは所有値であり、一時Schema RegistryやAuthoring Documentへの参照を保持しない
struct RuntimeSceneObjectData final
{
    ObjectId id;
    std::optional<ObjectId> parentId;
    bool isActive;
    math::Transform transform;
    std::vector<SceneComponent> components;
};

/// @brief Mutable SceneDocumentから切り離したRuntime実体化用の不変所有Snapshot
///
/// Snapshotは生成時点のScene Dataを複製所有し、元Documentの変更、Move、破棄から独立して生存する。
class SceneSnapshot final
{
  public:
    /// @brief Snapshot Dataの単一所有を保つためCopy構築を禁止する
    SceneSnapshot(const SceneSnapshot &) = delete;
    /// @brief Snapshot Dataの単一所有を保つためCopy代入を禁止する
    SceneSnapshot &operator=(const SceneSnapshot &) = delete;
    /// @brief Snapshot Dataを移動し、移動元を有効だが内容未規定の状態にする
    SceneSnapshot(SceneSnapshot &&) noexcept = default;
    /// @brief Snapshot Dataを移動代入する
    SceneSnapshot &operator=(SceneSnapshot &&) noexcept = default;
    /// @brief Snapshotが所有するScene Dataを破棄する
    ~SceneSnapshot() noexcept = default;

    /// @brief Snapshotが表す永続Scene Identityを返す
    /// @details 返却参照はSnapshotのMove、Move代入、または破棄で無効になる
    [[nodiscard]] const SceneAssetId &scene_asset_id() const noexcept;
    /// @brief ObjectId辞書順へ固定した不変Object集合を返す
    /// @details 返却ViewはSnapshotのMove、Move代入、または破棄で無効になる
    [[nodiscard]] std::span<const SceneObject> objects() const noexcept;

  private:
    friend Result<SceneSnapshot> create_scene_snapshot(
        const SceneDocument &, const AssertContext &) noexcept;
    friend Result<SceneSnapshot> create_runtime_scene_snapshot(
        SceneAssetId, std::vector<RuntimeSceneObjectData>, const AssertContext &) noexcept;

    /// @brief 検証済みScene IdentityとObject集合を所有するSnapshotを構築する
    SceneSnapshot(SceneAssetId a_sceneAssetId,
                  std::vector<SceneObject> a_objects) noexcept;
    /// @brief 検証済みRuntime入力を所有Scene Objectへ変換する
    [[nodiscard]] static SceneObject make_runtime_object(RuntimeSceneObjectData a_object) noexcept;

    SceneAssetId m_sceneAssetId;
    std::vector<SceneObject> m_objects;
};

/// @brief SceneDocumentを再検証しRuntimeから独立した不変Snapshotへ複製する
[[nodiscard]] Result<SceneSnapshot> create_scene_snapshot(
    const SceneDocument &a_document,
    const AssertContext &a_assertContext) noexcept;

/// @brief Runtime Scene DataをAuthoring上限から独立した不変Snapshotへ変換する
[[nodiscard]] Result<SceneSnapshot> create_runtime_scene_snapshot(
    SceneAssetId a_sceneAssetId, std::vector<RuntimeSceneObjectData> a_objects,
    const AssertContext &a_assertContext) noexcept;

/// @brief Runtime Entityへ保持するAuthoring Object由来のCore状態
struct SceneObjectState final
{
    ObjectId objectId;
    std::optional<game_core::EntityHandle> parent;
    bool isSelfActive;
    bool isEffectiveActive;
};

/// @brief Authoring Component Dataを型固有Runtime Componentへ変換する注入境界
///
/// Builderと内部ComponentType Tokenはinstantiate呼び出し完了まで生存させる。
/// validateはWorldを変更せず、buildは渡されたEntityへ正確に一つのComponentを追加する。
class RuntimeComponentBuilder
{
  public:
    /// @brief 派生Builderを基底Pointerから正しく破棄する
    virtual ~RuntimeComponentBuilder() = default;

    /// @brief Builderが処理するStable Component Type Identityを返す
    [[nodiscard]] virtual schema::TypeId type_id() const noexcept = 0;
    /// @brief 保持するRuntime Component Tokenが指定Worldへ登録済みか返す
    [[nodiscard]] virtual bool is_compatible(
        const game_core::World &a_world) const noexcept = 0;
    /// @brief Worldを変更せずAuthoring DataをRuntime変換可能か検証する
    [[nodiscard]] virtual Result<void> validate(
        const KnownComponentData &a_data,
        const AssertContext &a_assertContext) const noexcept = 0;
    /// @brief 検証済みAuthoring Dataから指定Entityへ一つのRuntime Componentを追加する
    [[nodiscard]] virtual Result<void> build(
        const KnownComponentData &a_data,
        game_core::World &a_world,
        game_core::EntityHandle a_entity,
        const AssertContext &a_assertContext) noexcept = 0;
};

/// @brief RuntimeWorld初期化後にSession-local Component Builderを生成するProject Scope境界
///
/// Factoryと生成したBuilderは一つのRuntime Sessionだけが所有し、対象WorldのComponent Tokenを
/// 同じSessionのRuntime Systemと共有できる。Project ScopeへWorld-local Tokenを持ち出さない。
class RuntimeComponentBuilderFactory
{
  public:
    /// @brief 派生Factoryを基底Pointerから正しく破棄する
    virtual ~RuntimeComponentBuilderFactory() noexcept = default;

    /// @brief Factoryが登録するStable Component Type Identityを返す
    [[nodiscard]] virtual schema::TypeId type_id() const noexcept = 0;
    /// @brief 指定Worldへ型を登録しScene実体化中だけ使用するBuilderを生成する
    [[nodiscard]] virtual Result<std::unique_ptr<RuntimeComponentBuilder>> create(
        game_core::World &a_world, game_core::ComponentType<math::Transform> a_transformType,
        game_core::ComponentType<SceneObjectState> a_sceneObjectStateType,
        const AssertContext &a_assertContext) const noexcept = 0;

  protected:
    /// @brief 派生Factoryだけが基底部分を構築できるようにする
    RuntimeComponentBuilderFactory() noexcept = default;
    /// @brief Project Scope Factory Identityの複製を禁止する
    RuntimeComponentBuilderFactory(const RuntimeComponentBuilderFactory &) = delete;
    /// @brief Project Scope Factory Identityの複製代入を禁止する
    RuntimeComponentBuilderFactory &operator=(const RuntimeComponentBuilderFactory &) = delete;
    /// @brief Project Scope Factory Addressを固定するためMove構築を禁止する
    RuntimeComponentBuilderFactory(RuntimeComponentBuilderFactory &&) = delete;
    /// @brief Project Scope Factory Addressを固定するためMove代入を禁止する
    RuntimeComponentBuilderFactory &operator=(RuntimeComponentBuilderFactory &&) = delete;
};

/// @brief SceneInstance終了中に破棄できず所有を維持したEntityとError
class SceneInstanceEndFailure final
{
  public:
    /// @brief 失敗したEntityと回復可能Errorを所有する
    SceneInstanceEndFailure(game_core::EntityHandle a_entity,
                            Error a_error) noexcept;

    /// @brief 破棄できなかったEntityを返す
    [[nodiscard]] game_core::EntityHandle entity() const noexcept;
    /// @brief Entity破棄Errorを返す
    /// @details 返却参照はFailureのMove、Move代入、または破棄で無効になる
    [[nodiscard]] const Error &error() const noexcept;

  private:
    game_core::EntityHandle m_entity;
    Error m_error;
};

/// @brief SceneInstance終了で試行した全Entityの順序付き結果
class SceneInstanceEndReport final
{
  public:
    /// @brief 終了処理が保持した失敗集合を所有する
    explicit SceneInstanceEndReport(
        std::vector<SceneInstanceEndFailure> a_failures) noexcept;

    /// @brief 全Entityの終了に成功した場合にtrueを返す
    [[nodiscard]] bool succeeded() const noexcept;
    /// @brief 逆生成順で収集したEntity破棄失敗を返す
    /// @details 返却Viewと要素参照はReportのMove、Move代入、または破棄で無効になる
    [[nodiscard]] std::span<const SceneInstanceEndFailure> failures() const noexcept;

  private:
    std::vector<SceneInstanceEndFailure> m_failures;
};

/// @brief 一回のScene実体化が生成したRuntime Entity集合とStable ID対応を所有するHandle
///
/// live状態のDestructorとlive状態を上書きするMove代入はEmergencyHandlerでProcessを停止する。
/// instantiateへ渡すAssertContextのFatalHandler Ownerは、移動元と終了済み状態を含む全Instanceより長く生存させる。
/// live Instanceは、関連付けられたRuntimeWorldのshutdownまたは破棄より先にendで終了する。
/// RuntimeWorldを先に終了するとendはRuntimeWorldMismatchとなり、残ったlive InstanceのDestructorはProcessを停止する。
class SceneInstance final
{
  public:
    /// @brief SceneInstanceの所有権複製を禁止する
    SceneInstance(const SceneInstance &) = delete;
    /// @brief SceneInstanceの所有権複製代入を禁止する
    SceneInstance &operator=(const SceneInstance &) = delete;
    /// @brief 所有集合を移動し、移動元を終了済み状態にする
    SceneInstance(SceneInstance &&a_other) noexcept;
    /// @brief 終了済み移動先へ所有集合を移し、live移動先ならFatal終了する
    SceneInstance &operator=(SceneInstance &&a_other) noexcept;
    /// @brief 終了済み状態を破棄し、live状態ならFatal終了する
    ~SceneInstance() noexcept;

    /// @brief 空Sceneを含めRuntime Worldとの未終了の関連付けがある場合にtrueを返す
    [[nodiscard]] bool is_live() const noexcept;
    /// @brief live状態なら元Scene Identityを、終了済み状態ならnullptrを返す
    /// @details 返却Pointerは全Entityを終了したend、InstanceのMove、またはInstance破棄で無効になる
    [[nodiscard]] const SceneAssetId *try_scene_asset_id() const noexcept;
    /// @brief ObjectIdに対応する現在生存所有Entityまたはnullptrを返す
    /// @details 返却Pointerはendの呼び出し、InstanceのMove、またはInstance破棄で無効になる
    [[nodiscard]] const game_core::EntityHandle *find_entity(
        const ObjectId &a_objectId) const noexcept;
    /// @brief 現在生存している所有Entity数を返す
    [[nodiscard]] std::size_t entity_count() const noexcept;

    /// @brief 同じRuntime World上の所有Entityを逆生成順に終了して結果を返す
    /// @details RuntimeWorldのOwner ThreadかつStructural Safe Pointでのみ呼び出す。違反は回復可能なResultではなくFatalとなる
    /// @details 既に破棄済みのEntityは成功扱いし、失敗して生存するEntityだけを所有集合へ残す
    [[nodiscard]] Result<SceneInstanceEndReport> end(
        game_core::RuntimeWorld &a_runtimeWorld,
        const AssertContext &a_assertContext) noexcept;

  private:
    friend class SceneInstantiator;

    /// @brief 実体化済みScene Identity、World Identity、Entity集合を所有する
    SceneInstance(
        SceneAssetId a_sceneAssetId, std::uint64_t a_worldId,
        std::map<ObjectId, game_core::EntityHandle> a_mapping,
        std::vector<std::pair<ObjectId, game_core::EntityHandle>> a_creationOrder,
        EmergencyHandler &a_emergencyHandler) noexcept;
    /// @brief 移動元をWorld参照不要な終了済み状態へ移す
    void clear_moved_from() noexcept;

    std::optional<SceneAssetId> m_sceneAssetId;
    std::optional<std::uint64_t> m_worldId;
    std::map<ObjectId, game_core::EntityHandle> m_mapping;
    std::vector<std::pair<ObjectId, game_core::EntityHandle>> m_creationOrder;
    EmergencyHandler *m_emergencyHandler;
};

/// @brief SceneSnapshotを事前検証したPlanからRuntimeWorldへ一方向実体化する
class SceneInstantiator final
{
  public:
    /// @brief Safe Point上でSnapshot全体を検証し、成功時だけSceneInstanceを返す
    /// @details RuntimeWorldのOwner ThreadかつStructural Safe Pointで呼び出し、返却InstanceをRuntimeWorld終了前にendする
    /// @details 途中失敗時は本Operationで生成した全生存Entityを逆順にRollbackする
    /// @param a_assertContext FatalHandler Ownerを返されたSceneInstanceより長く生存させる診断Context
    [[nodiscard]] static Result<SceneInstance> instantiate(
        const SceneSnapshot &a_snapshot,
        game_core::RuntimeWorld &a_runtimeWorld,
        game_core::ComponentType<SceneObjectState> a_sceneObjectStateType,
        std::span<RuntimeComponentBuilder *const> a_componentBuilders,
        const AssertContext &a_assertContext) noexcept;
};
} // namespace cue::scene
