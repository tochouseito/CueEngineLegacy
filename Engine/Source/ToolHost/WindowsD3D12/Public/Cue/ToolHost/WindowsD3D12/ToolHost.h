#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Input/FrameInputSnapshot.h>
#include <Cue/Input/InputEvent.h>
#include <Cue/Platform/WindowEvent.h>
#include <Cue/Renderer/RenderSnapshot.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace cue
{
class AssertContext;
class Window;
} // namespace cue

namespace cue::tool_host
{
/// @brief Windows D3D12 Tool Hostの回復可能な失敗分類
enum class ToolHostError : std::int64_t
{
    InvalidConfiguration = 1,
    WindowInitializationFailed = 2,
    D3d12InitializationFailed = 3,
    ImGuiInitializationFailed = 4,
    FenceSignalFailed = 5,
    FenceWaitFailed = 6,
    GpuCompletionUnavailable = 7,
    DeviceRemoved = 8,
    PresentFailed = 9,
    SwapChainResizeFailed = 10,
    FenceValueExhausted = 11,
    RenderSurfaceInvalidSize = 12,
    RenderSurfaceCreationFailed = 13,
    RenderSurfaceDescriptorExhausted = 14,
    RenderSurfaceRetirementCapacityExceeded = 15,
};

/// @brief Tool HostがD3D12 Device生成へ使用するAdapter選択方針
enum class ToolHostAdapterPreference : std::uint8_t
{
    HardwarePreferred,
    Warp,
};

inline constexpr std::uint32_t k_maximumToolHostRenderSurfaceDimension = 16384U;
inline constexpr std::size_t k_toolHostRenderSurfaceCount = 2U;

/// @brief Editor Viewと固定Surface Slotの対応
enum class ToolHostRenderSurfaceSlot : std::uint8_t
{
    GameView = 0,
    DebugView = 1
};

/// @brief Clientが次Frameで必要とする単一Offscreen Render Surfaceの状態
struct ToolHostRenderSurfaceRequest final
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    bool isVisible = false;
};

/// @brief 現在FrameでImGuiへ渡せる非所有Offscreen Render Surface View
struct ToolHostRenderSurfaceView final
{
    std::uint64_t textureId = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

/// @brief GameView／DebugViewの固定順Surface要求集合
using ToolHostRenderSurfaceRequests = std::array<ToolHostRenderSurfaceRequest, k_toolHostRenderSurfaceCount>;
/// @brief GameView／DebugViewの固定順Surface View集合
using ToolHostRenderSurfaceViews = std::array<ToolHostRenderSurfaceView, k_toolHostRenderSurfaceCount>;

/// @brief UI構築後にToolHostが描画するPortable SceneとCamera View
struct ToolHostRenderFrameView final
{
    const renderer::RenderSnapshot *snapshot = nullptr;
    const renderer::PerspectiveCamera *gameCamera = nullptr;
    const renderer::PerspectiveCamera *debugCamera = nullptr;
};

/// @brief Tool Hostが一Frameで受理したPortable Event、状態、UI Captureの借用View
///
/// eventsとsnapshotは次回通知またはHost終了までだけ有効であり、別Threadまたは次Frameへ保持しない
struct ToolHostInputFrameView final
{
    std::span<const InputEvent> events;
    const FrameInputSnapshot &snapshot;
    InputCapture uiCapture;
};

/// @brief Tool Host Windowと自動Smoke終了条件を指定する
struct ToolHostDescriptor final
{
    std::string_view title;
    WindowSize clientSize;
    std::uint64_t maximumFrameCount;
    /// @brief Main Executableに埋め込まれたInteger Icon Resourceを選択し、0ではWindow Iconを設定しない
    /// @details 非0のResourceが存在しない場合はWindowInitializationFailedでHost起動を中止する
    std::uint16_t iconResourceId = 0U;
    ToolHostAdapterPreference adapterPreference = ToolHostAdapterPreference::HardwarePreferred;
};

/// @brief Tool固有Presentationを共通Windows D3D12 Hostへ接続する
class ToolHostClient
{
  public:
    /// @brief Presentation Callback所有権の複製を禁止する
    ToolHostClient(const ToolHostClient &) = delete;
    /// @brief Presentation Callback所有権の複製を禁止する
    ToolHostClient &operator=(const ToolHostClient &) = delete;

    /// @brief 派生ClientをHostより先に破棄しない所有契約を提供する
    virtual ~ToolHostClient() = default;

    /// @brief 現在FrameのImGui Widgetを構築する
    virtual void draw_frame() noexcept = 0;

    /// @brief 初期化済みWindowを最初のFrame前に通知する。参照はHost実行中だけ有効
    virtual void window_ready(Window &) noexcept
    {
    }

    /// @brief 次Frameの固定色Clear対象となるOffscreen Surface要求をOwner Threadから取得する
    [[nodiscard]] virtual ToolHostRenderSurfaceRequests render_surface_requests() const noexcept
    {
        return {};
    }

    /// @brief 現在FrameでImGui Imageへ使用できる非所有Texture Viewをdraw_frame直前に通知する
    /// @details Viewは次回通知またはHost終了までだけ有効で、textureIdはImGui以外へ使用しない
    virtual void render_surfaces_ready(ToolHostRenderSurfaceViews) noexcept
    {
    }

    /// @brief Message Pump後に構築したPortable Inputを現在FrameのUI構築前に通知する
    virtual void input_frame(ToolHostInputFrameView) noexcept
    {
    }

    /// @brief UI構築後の現在Frameで描画するPortable SnapshotとCameraを返す
    [[nodiscard]] virtual ToolHostRenderFrameView render_frame_view() const noexcept
    {
        return {};
    }

    /// @brief Native Window終了要求をTool固有の保存確認または終了状態へ変換する
    virtual void request_close() noexcept = 0;

    /// @brief PresentationがTool Session終了を要求したか返す
    [[nodiscard]] virtual bool should_close() const noexcept = 0;

  protected:
    /// @brief 非所有Presentation Callback境界だけを派生実装へ提供する
    ToolHostClient() noexcept = default;
};

/// @brief Windows Window、D3D12、ImGui Backendを所有してTool UI Loopを実行する
[[nodiscard]] Result<void> run_windows_d3d12_tool_host(const ToolHostDescriptor &a_descriptor, ToolHostClient &a_client,
                                                       const AssertContext &a_assertContext) noexcept;
} // namespace cue::tool_host
