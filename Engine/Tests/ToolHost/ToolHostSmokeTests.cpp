#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Platform/Windows/WindowsWindowInterop.h>
#include <Cue/ToolHost/WindowsD3D12/ToolHost.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>
#include <imgui.h>

namespace
{
/// @brief Smoke Test中の回復不能状態を固定Exit Codeへ変換する
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief MessageなしFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(75);
    }

    /// @brief Message付きFatalを固定Exit Codeへ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(76);
    }
};

/// @brief 注入したMouse、Key、Buttonの順序を外部Window Eventと区別して検証する
[[nodiscard]] bool contains_injected_input_sequence(std::span<const cue::InputEvent> a_events) noexcept
{
    std::uint32_t stage = 0U;
    for (const cue::InputEvent &event : a_events)
    {
        if (stage == 0U && event.type == cue::InputEventType::MouseMove && event.mousePosition.x == 10 &&
            event.mousePosition.y == 20)
        {
            stage = 1U;
        }
        else if (stage == 1U && event.type == cue::InputEventType::KeyDown && event.key == cue::InputKey::A)
        {
            stage = 2U;
        }
        else if (stage == 2U && event.type == cue::InputEventType::MouseButtonDown &&
                 event.mouseButton == cue::InputMouseButton::Right && event.mousePosition.x == 10 &&
                 event.mousePosition.y == 20)
        {
            return true;
        }
    }
    return false;
}

/// @brief Tool HostへResizeと非表示を含む最小Offscreen Surface要求を3 Frame提供する
class SmokeClient final : public cue::tool_host::ToolHostClient
{
  public:
    /// @brief Smoke Client Stateの複製を禁止する
    SmokeClient(const SmokeClient &) = delete;
    /// @brief Smoke Client Stateの複製を禁止する
    SmokeClient &operator=(const SmokeClient &) = delete;
    /// @brief 描画可能なCameraとCube Snapshotを所有してFrame Counterを0から開始する
    explicit SmokeClient(const cue::AssertContext &a_assertContext) noexcept : m_assertContext(&a_assertContext)
    {
        cue::Result<cue::renderer::DebugCamera> debugCamera =
            cue::renderer::DebugCamera::create_default(a_assertContext.fatal_handler());
        if (!debugCamera)
        {
            std::_Exit(77);
        }
        m_debugCamera.emplace(std::move(*debugCamera.try_value()));
        std::vector<cue::renderer::RenderMeshInstance> meshes{
            {cue::math::Transform{}, cue::renderer::RenderMesh::Cube}};
        m_snapshot = cue::renderer::RenderSnapshot(cue::renderer::MainCameraStatus::Ready, m_debugCamera->camera(),
                                                   std::move(meshes), 1U);
    }
    /// @brief 所有Resourceを持たないSmoke Clientを破棄する
    ~SmokeClient() override = default;

    /// @brief Host Backendへ送る最小ImGui Windowを構築する
    void draw_frame() noexcept override
    {
        ImGui::Begin("CueEngine Tool Host Smoke");
        ImGui::TextUnformatted("ImGui / Win32 / D3D12");
        if (m_surfaces[0U].textureId != 0U)
        {
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(m_surfaces[0U].textureId)), ImVec2(160.0F, 90.0F));
        }
        if (m_surfaces[1U].textureId != 0U)
        {
            ImGui::Image(ImTextureRef(static_cast<ImTextureID>(m_surfaces[1U].textureId)), ImVec2(160.0F, 90.0F));
        }
        ImGui::End();
        ++m_drawCount;
        if (m_drawCount == 1U)
        {
            m_requests[0U] = {256U, 144U, true};
            m_requests[1U] = {300U, 180U, true};
        }
        else if (m_drawCount == 2U)
        {
            m_requests = {};
        }
    }

    /// @brief 初期化済みWindowが最初のFrame前に通知されたことを記録する
    void window_ready(cue::Window &a_window) noexcept override
    {
        m_windowWasReady = m_drawCount == 0U;
        cue::Result<cue::NativeWindowView> nativeView = cue::get_native_window_view(a_window, *m_assertContext);
        if (!nativeView)
        {
            m_inputInjectionWasValid = false;
            return;
        }
        m_nativeWindow = static_cast<HWND>(const_cast<void *>(nativeView.try_value()->value()));
        m_inputInjectionWasValid = m_nativeWindow != nullptr && IsWindow(m_nativeWindow) != FALSE;
    }

    /// @brief Portable Input Event順とFrame Snapshotの押下、解放、一時値Resetを検証する
    void input_frame(cue::tool_host::ToolHostInputFrameView a_input) noexcept override
    {
        ++m_inputFrameCount;
        if (m_inputFrameCount == 1U)
        {
            m_firstInputFrameWasValid =
                contains_injected_input_sequence(a_input.events) && a_input.snapshot.is_key_down(cue::InputKey::A) &&
                a_input.snapshot.was_key_pressed(cue::InputKey::A) &&
                a_input.snapshot.is_mouse_button_down(cue::InputMouseButton::Right) &&
                a_input.snapshot.was_mouse_button_pressed(cue::InputMouseButton::Right) &&
                a_input.snapshot.has_mouse_position();
        }
        else if (m_inputFrameCount == 2U)
        {
            m_secondInputFrameWasValid = !a_input.snapshot.is_key_down(cue::InputKey::A) &&
                                         a_input.snapshot.was_key_released(cue::InputKey::A) &&
                                         !a_input.snapshot.is_mouse_button_down(cue::InputMouseButton::Right) &&
                                         a_input.snapshot.was_mouse_button_released(cue::InputMouseButton::Right);
        }
        else if (m_inputFrameCount == 3U)
        {
            m_thirdInputFrameWasValid = !a_input.snapshot.was_key_pressed(cue::InputKey::A) &&
                                        !a_input.snapshot.was_key_released(cue::InputKey::A) &&
                                        !a_input.snapshot.was_mouse_button_pressed(cue::InputMouseButton::Right) &&
                                        !a_input.snapshot.was_mouse_button_released(cue::InputMouseButton::Right);
        }
    }

    /// @brief 現在FrameのSurface要求を返す
    [[nodiscard]] cue::tool_host::ToolHostRenderSurfaceRequests render_surface_requests() const noexcept override
    {
        // Message Pump後かつInput Frame更新前に同期配送し、CIのForeground Window変動とFrame境界を分離する
        if (m_inputInjectionWasValid && m_inputFrameCount == 0U)
        {
            SendMessageW(m_nativeWindow, WM_SETFOCUS, 0U, 0);
            SendMessageW(m_nativeWindow, WM_MOUSEMOVE, 0U, MAKELPARAM(10, 20));
            SendMessageW(m_nativeWindow, WM_KEYDOWN, 'A', 0);
            SendMessageW(m_nativeWindow, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(10, 20));
        }
        else if (m_inputInjectionWasValid && m_inputFrameCount == 1U)
        {
            SendMessageW(m_nativeWindow, WM_SETFOCUS, 0U, 0);
            SendMessageW(m_nativeWindow, WM_MOUSEMOVE, 0U, MAKELPARAM(13, 25));
            SendMessageW(m_nativeWindow, WM_KEYDOWN, 'A', 0);
            SendMessageW(m_nativeWindow, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(13, 25));
            SendMessageW(m_nativeWindow, WM_KEYUP, 'A', 0);
            SendMessageW(m_nativeWindow, WM_RBUTTONUP, 0U, MAKELPARAM(13, 25));
        }
        return m_requests;
    }

    /// @brief Hostが要求通りのSurface世代または非表示Viewを通知したことを検証する
    void render_surfaces_ready(cue::tool_host::ToolHostRenderSurfaceViews a_surfaces) noexcept override
    {
        m_surfaces = a_surfaces;
        if (m_drawCount == 0U)
        {
            m_surfaceWasValid = a_surfaces[0U].textureId != 0U && a_surfaces[0U].width == 320U &&
                                a_surfaces[0U].height == 180U && a_surfaces[1U].textureId != 0U &&
                                a_surfaces[1U].width == 200U && a_surfaces[1U].height == 120U;
        }
        else if (m_drawCount == 1U)
        {
            m_resizeWasValid = a_surfaces[0U].textureId != 0U && a_surfaces[0U].width == 256U &&
                               a_surfaces[0U].height == 144U && a_surfaces[1U].textureId != 0U &&
                               a_surfaces[1U].width == 300U && a_surfaces[1U].height == 180U;
        }
        else if (m_drawCount == 2U)
        {
            m_hiddenWasValid = a_surfaces[0U].textureId == 0U && a_surfaces[0U].width == 0U &&
                               a_surfaces[0U].height == 0U && a_surfaces[1U].textureId == 0U &&
                               a_surfaces[1U].width == 0U && a_surfaces[1U].height == 0U;
        }
    }

    /// @brief GameViewとDebugViewへ同じCube Snapshotを異なるCameraで描画させる
    [[nodiscard]] cue::tool_host::ToolHostRenderFrameView render_frame_view() const noexcept override
    {
        return {&m_snapshot, &m_debugCamera->camera(), &m_debugCamera->camera()};
    }

    /// @brief 有限Frame Smokeでは予期しないNative Window終了要求を状態へ反映しない
    void request_close() noexcept override
    {
    }

    /// @brief 最大Frame条件だけで終了するためClient起因の終了を要求しない
    [[nodiscard]] bool should_close() const noexcept override
    {
        return false;
    }

    /// @brief HostがClientを描画したFrame数を返す
    [[nodiscard]] std::uint32_t draw_count() const noexcept
    {
        return m_drawCount;
    }

    /// @brief Window通知が最初の描画より前に届いたか返す
    [[nodiscard]] bool window_was_ready() const noexcept
    {
        return m_windowWasReady;
    }

    /// @brief Surface生成、Resize、非表示の全観測が成功したか返す
    [[nodiscard]] bool surface_lifecycle_was_valid() const noexcept
    {
        return m_surfaceWasValid && m_resizeWasValid && m_hiddenWasValid;
    }

    /// @brief Native Message注入と3 FrameのPortable Input契約を最初に満たさない固定Exit Codeで返す
    [[nodiscard]] int input_lifecycle_exit_code() const noexcept
    {
        if (!m_inputInjectionWasValid)
        {
            return 5;
        }
        if (m_inputFrameCount != 3U)
        {
            return 6;
        }
        if (!m_firstInputFrameWasValid)
        {
            return 7;
        }
        if (!m_secondInputFrameWasValid)
        {
            return 8;
        }
        return m_thirdInputFrameWasValid ? 0 : 9;
    }

  private:
    const cue::AssertContext *m_assertContext;
    HWND m_nativeWindow = nullptr;
    cue::tool_host::ToolHostRenderSurfaceRequests m_requests{{{320U, 180U, true}, {200U, 120U, true}}};
    cue::tool_host::ToolHostRenderSurfaceViews m_surfaces;
    std::optional<cue::renderer::DebugCamera> m_debugCamera;
    cue::renderer::RenderSnapshot m_snapshot;
    std::uint32_t m_drawCount = 0;
    std::uint32_t m_inputFrameCount = 0;
    bool m_windowWasReady = false;
    bool m_surfaceWasValid = false;
    bool m_resizeWasValid = false;
    bool m_hiddenWasValid = false;
    bool m_inputInjectionWasValid = false;
    bool m_firstInputFrameWasValid = false;
    bool m_secondInputFrameWasValid = false;
    bool m_thirdInputFrameWasValid = false;
};

/// @brief 指定Adapter方針でTool Host Surface Lifecycleを実Frame検証する
[[nodiscard]] int run_smoke(cue::tool_host::ToolHostAdapterPreference a_preference,
                            const cue::AssertContext &a_context) noexcept
{
    SmokeClient client(a_context);
    const cue::tool_host::ToolHostDescriptor descriptor{"Cue Tool Host Smoke", {640U, 360U}, 3U, 0U, a_preference};
    cue::Result<void> result = cue::tool_host::run_windows_d3d12_tool_host(descriptor, client, a_context);
    if (!result)
    {
        return 1;
    }
    if (!client.window_was_ready())
    {
        return 2;
    }
    if (client.draw_count() != 3U)
    {
        return 3;
    }
    if (!client.surface_lifecycle_was_valid())
    {
        return 4;
    }
    return client.input_lifecycle_exit_code();
}
} // namespace

/// @brief Win32 Window、D3D12、ImGui Backend、有限Fence Drainを実Frameで検証する
int main(int a_argumentCount, char **a_arguments)
{
    TestFatalHandler handler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(handler, std::move(sinks));
    cue::AssertContext context(logger, handler);
    const bool useWarp = a_argumentCount == 2 && std::string_view(a_arguments[1]) == "--warp";
    const int exitCode = run_smoke(useWarp ? cue::tool_host::ToolHostAdapterPreference::Warp
                                          : cue::tool_host::ToolHostAdapterPreference::HardwarePreferred,
                                   context);
    if (exitCode != 0)
    {
        std::fprintf(stderr, "ToolHost smoke failed with exit code %d\n", exitCode);
    }
    return exitCode;
}
