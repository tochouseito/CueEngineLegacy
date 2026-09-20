#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Runtime/RuntimeApplicationSession.h>
#include <Cue/RuntimeHost/RuntimeHostStartup.h>

#include <cstdint>
#include <memory>

namespace cue
{
class AssertContext;
class Window;
} // namespace cue

namespace cue::renderer
{
class RenderSnapshot;
} // namespace cue::renderer

namespace cue::runtime_host
{
/// @brief Standalone Host固有のProcess ScopeとPortable Runtime Sessionを明示所有するComposition
class RuntimeHostApplication final
{
    class State;

  public:
    /// @brief FactoryだけがRuntime Host Application Constructorへ渡せる生成権限
    class ConstructionKey final
    {
      public:
        /// @brief 生成権限を値として複製する
        ConstructionKey(const ConstructionKey &) noexcept = default;
        /// @brief 生成権限を値として複製代入する
        ConstructionKey &operator=(const ConstructionKey &) noexcept = default;
        /// @brief 生成権限を値として移動する
        ConstructionKey(ConstructionKey &&) noexcept = default;
        /// @brief 生成権限を値として移動代入する
        ConstructionKey &operator=(ConstructionKey &&) noexcept = default;
        /// @brief bit_castによる権限生成を防ぐnon-trivialな破棄を行う
        ~ConstructionKey() noexcept
        {
        }

      private:
        friend class RuntimeHostApplication;

        /// @brief RuntimeHostApplication Factoryだけに生成権限を発行する
        ConstructionKey() noexcept = default;
    };

    /// @brief Loader方式に依存しないStartup入力とWindows Input AdapterからRunning Sessionを構築する
    [[nodiscard]] static Result<std::unique_ptr<RuntimeHostApplication>> start(
        Window &a_window, RuntimeHostStartup a_startup,
        const AssertContext &a_assertContext) noexcept;

    /// @brief Test用固定空SceneからRunning Sessionを構築する
    [[nodiscard]] static Result<std::unique_ptr<RuntimeHostApplication>> start_fixed_smoke(
        Window &a_window, const AssertContext &a_assertContext) noexcept;

    /// @brief Factory外からの既定構築を禁止する
    RuntimeHostApplication() = delete;
    /// @brief Host Composition所有権の複製を禁止する
    RuntimeHostApplication(const RuntimeHostApplication &) = delete;
    /// @brief Host Composition所有権の複製代入を禁止する
    RuntimeHostApplication &operator=(const RuntimeHostApplication &) = delete;
    /// @brief 保持参照と内部OwnerのAddressを固定するためMove構築を禁止する
    RuntimeHostApplication(RuntimeHostApplication &&) = delete;
    /// @brief 保持参照と内部OwnerのAddressを固定するためMove代入を禁止する
    RuntimeHostApplication &operator=(RuntimeHostApplication &&) = delete;
    /// @brief Runtime停止とWindows Input Sink切断の完了を検証してCompositionを破棄する
    ~RuntimeHostApplication() noexcept;

    /// @brief Factoryが完全構築した非公開Stateの一意所有権を固定する
    RuntimeHostApplication(ConstructionKey, std::unique_ptr<State> a_state) noexcept;

    /// @brief 現在Pumpで蓄積したPortable InputからRuntime Frameを一回進める
    [[nodiscard]] Result<void> advance_frame() noexcept;
    /// @brief 指定Host理由を記録しRuntimeを逆順停止してInput Sinkを切断する
    /// @details Runtime更新失敗を保持していた場合はCleanup完了後に元Errorを返す
    [[nodiscard]] Result<void> stop(runtime::RuntimeApplicationStopReason a_reason) noexcept;

    /// @brief Runtime OwnerとInput Sinkが安全に終了した場合にtrueを返す
    [[nodiscard]] bool is_cleanup_complete() const noexcept;
    /// @brief Host診断用のStable Session Generationを返す
    [[nodiscard]] std::uint64_t generation() const noexcept;
    /// @brief Host診断用のWorld Identityを返す
    [[nodiscard]] std::uint64_t world_id() const noexcept;
    /// @brief Clockが確定したRuntime Frame数を返す
    [[nodiscard]] std::uint64_t frame_count() const noexcept;
    /// @brief 次のFrame更新または停止まで有効なWorld Pointer非保持の描画Snapshotを借用する
    /// @details Owner Threadだけで呼び出し、advance_frame、stop、Application破棄後の参照を保持しない
    [[nodiscard]] const renderer::RenderSnapshot &render_snapshot() const noexcept;
    /// @brief Runtimeが記録した最初の停止理由を返す
    [[nodiscard]] runtime::RuntimeApplicationStopReason stop_reason() const noexcept;

  private:
    std::unique_ptr<State> m_state;
};
} // namespace cue::runtime_host
