#include <Cue/ToolHost/WindowsD3D12/ToolHost.h>

#include <Cue/EngineAssets/BuiltInMesh.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Input/InputEventQueue.h>
#include <Cue/Input/InputState.h>
#include <Cue/Input/Windows/WindowsInputMessageSink.h>
#include <Cue/Platform/Window.h>
#include <Cue/Platform/WindowSystem.h>
#include <Cue/Platform/Windows/WindowsMessageSink.h>
#include <Cue/Platform/Windows/WindowsPlatform.h>
#include <Cue/Platform/Windows/WindowsWindowInterop.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <wrl/client.h>

/// @brief Win32 MessageをDear ImGui公式Backendへ転送する外部Entry Point
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND a_window, UINT a_message, WPARAM a_wordParameter,
                                                             LPARAM a_longParameter);

namespace
{
using Microsoft::WRL::ComPtr;

constexpr std::uint32_t k_frameCount = 2;
constexpr std::uint32_t k_srvDescriptorCount = 64;
constexpr std::size_t k_retiredRenderSurfaceCapacity =
    (k_frameCount + 1U) * cue::tool_host::k_toolHostRenderSurfaceCount;
constexpr std::uint32_t k_maxDredNodes = 4096;
constexpr DWORD k_fenceTimeoutMilliseconds = 5000;
constexpr DXGI_FORMAT k_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT k_depthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr std::size_t k_maximumRenderMeshInstances = 4096U;
constexpr std::size_t k_constantBufferStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

constexpr std::string_view k_sceneShader = R"(
cbuffer ObjectConstants : register(b0)
{
    row_major float4x4 worldViewProjection;
};

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct VertexOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
};

// Transforms a built-in mesh vertex into clip space.
VertexOutput vs_main(VertexInput input)
{
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0F), worldViewProjection);
    output.normal = input.normal;
    return output;
}

// Converts the face normal into a visible unlit diagnostic color.
float4 ps_main(VertexOutput input) : SV_TARGET
{
    const float3 color = 0.25F + abs(normalize(input.normal)) * 0.65F;
    return float4(color, 1.0F);
}
)";

/// @brief Windows標準日本語FontをProject Hubの既定Fontへ設定できたか返す
[[nodiscard]] bool configure_japanese_font(const cue::AssertContext &a_context) noexcept
{
    std::array<char, MAX_PATH> windowsDirectory{};
    const UINT length = GetWindowsDirectoryA(windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
    if (length == 0 || length >= static_cast<UINT>(windowsDirectory.size()))
    {
        return false;
    }
    constexpr std::array<std::string_view, 2> k_fontNames = {"meiryo.ttc", "YuGothR.ttc"};
    for (const std::string_view fontName : k_fontNames)
    {
        std::string locator;
        try
        {
            locator.assign(windowsDirectory.data(), length);
            locator.append("\\Fonts\\");
            locator.append(fontName);
        }
        catch (...)
        {
            a_context.fatal_handler().terminate("Tool Host font locator allocation failed");
        }
        if (GetFileAttributesA(locator.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            continue;
        }
        ImFont *font = ImGui::GetIO().Fonts->AddFontFromFileTTF(locator.c_str(), 18.0F, nullptr,
                                                                ImGui::GetIO().Fonts->GetGlyphRangesJapanese());
        if (font != nullptr)
        {
            ImGui::GetIO().FontDefault = font;
            return true;
        }
    }
    return false;
}

/// @brief Tool Host ErrorをNative診断なしで生成する
[[nodiscard]] cue::Error make_error(const cue::AssertContext &a_context, cue::tool_host::ToolHostError a_code,
                                    std::string_view a_summary) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_context.fatal_handler(), "Cue.ToolHost", static_cast<std::int64_t>(a_code));
    return cue::Error::create(a_context.fatal_handler(), std::move(code), a_summary);
}

/// @brief Tool Host ErrorをNative診断と共に生成する
[[nodiscard]] cue::Error make_native_error(const cue::AssertContext &a_context, cue::tool_host::ToolHostError a_code,
                                           std::string_view a_summary, std::string_view a_nativeDomain,
                                           std::int64_t a_nativeCode) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_context.fatal_handler(), "Cue.ToolHost", static_cast<std::int64_t>(a_code));
    cue::NativeError native = cue::NativeError::create(a_context.fatal_handler(), a_nativeDomain, a_nativeCode);
    return cue::Error::create(a_context.fatal_handler(), std::move(code), a_summary, std::move(native));
}

/// @brief Executable Resourceの大・小IconをTool Windowへ関連付ける
[[nodiscard]] cue::Result<void> apply_window_icon(HWND a_window, std::uint16_t a_resourceId,
                                                  const cue::AssertContext &a_context) noexcept
{
    if (a_resourceId == 0U)
    {
        return cue::Result<void>::success();
    }

    SetLastError(ERROR_SUCCESS);
    HINSTANCE module = GetModuleHandleW(nullptr);
    if (module == nullptr)
    {
        return cue::Result<void>::failure(
            make_native_error(a_context, cue::tool_host::ToolHostError::WindowInitializationFailed,
                              "Tool Host application module could not be acquired", "Win32", GetLastError()));
    }

    constexpr UINT k_loadFlags = LR_DEFAULTCOLOR | LR_SHARED;
    SetLastError(ERROR_SUCCESS);
    HICON largeIcon =
        static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(a_resourceId), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                      GetSystemMetrics(SM_CYICON), k_loadFlags));
    if (largeIcon == nullptr)
    {
        return cue::Result<void>::failure(
            make_native_error(a_context, cue::tool_host::ToolHostError::WindowInitializationFailed,
                              "Tool Host large application icon could not be loaded", "Win32", GetLastError()));
    }

    SetLastError(ERROR_SUCCESS);
    HICON smallIcon =
        static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(a_resourceId), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                      GetSystemMetrics(SM_CYSMICON), k_loadFlags));
    if (smallIcon == nullptr)
    {
        return cue::Result<void>::failure(
            make_native_error(a_context, cue::tool_host::ToolHostError::WindowInitializationFailed,
                              "Tool Host small application icon could not be loaded", "Win32", GetLastError()));
    }

    static_cast<void>(SendMessageW(a_window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon)));
    static_cast<void>(SendMessageW(a_window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon)));
    return cue::Result<void>::success();
}

/// @brief DREDのUTF-16 Object名をUTF-8へ変換できたか返す
[[nodiscard]] bool try_convert_dred_name(const wchar_t *a_name, std::string &a_storage,
                                         const cue::AssertContext &a_context) noexcept
{
    if (a_name == nullptr)
    {
        return false;
    }
    const std::size_t length = std::char_traits<wchar_t>::length(a_name);
    if (length == 0 || length > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return false;
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_name, static_cast<int>(length), nullptr,
                                             0, nullptr, nullptr);
    if (required <= 0)
    {
        return false;
    }
    try
    {
        a_storage.resize(static_cast<std::size_t>(required));
    }
    catch (...)
    {
        a_context.fatal_handler().terminate("Tool Host DRED name allocation failed");
    }
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_name, static_cast<int>(length), a_storage.data(),
                               required, nullptr, nullptr) == required;
}

/// @brief DRED Object名を利用可能な表現から選択する
[[nodiscard]] std::string_view select_dred_name(const char *a_utf8Name, const wchar_t *a_utf16Name,
                                                std::string_view a_fallback, std::string &a_storage,
                                                const cue::AssertContext &a_context) noexcept
{
    if (a_utf8Name != nullptr && a_utf8Name[0] != '\0')
    {
        return a_utf8Name;
    }
    if (try_convert_dred_name(a_utf16Name, a_storage, a_context))
    {
        return a_storage;
    }
    return a_fallback;
}

/// @brief DRED詳細のLogger失敗をPrimary Errorへ二次診断として保持する
void retain_dred_log_failure(cue::Error &a_primary, cue::LogResult a_result, std::string_view a_context,
                             const cue::AssertContext &a_assertContext) noexcept
{
    if (a_result == cue::LogResult::Success)
    {
        return;
    }
    cue::Error logging = make_error(a_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                    "Foundation Logger could not record Tool Host DRED diagnostics");
    a_primary.append_secondary_diagnostics(a_assertContext, logging, a_context, "DRED");
}

/// @brief DRED Breadcrumb NodeをDevice解放前に診断へ転記する
void log_dred_breadcrumbs(const D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 &a_breadcrumbs, cue::Error &a_primary,
                          const cue::AssertContext &a_context) noexcept
{
    const D3D12_AUTO_BREADCRUMB_NODE1 *node = a_breadcrumbs.pHeadAutoBreadcrumbNode;
    std::uint32_t nodeCount = 0;
    while (node != nullptr && nodeCount < k_maxDredNodes)
    {
        const std::uint32_t lastBreadcrumb = node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
        std::int64_t operation = -1;
        if (node->pCommandHistory != nullptr && node->BreadcrumbCount > 0)
        {
            const std::uint32_t operationIndex = (std::min)(lastBreadcrumb, node->BreadcrumbCount - 1);
            operation = static_cast<std::int64_t>(node->pCommandHistory[operationIndex]);
        }
        std::string commandListNameStorage;
        const std::string_view commandListName =
            select_dred_name(node->pCommandListDebugNameA, node->pCommandListDebugNameW,
                             "Unnamed Tool Host Command List", commandListNameStorage, a_context);
        cue::ErrorCode code = cue::ErrorCode::create(a_context.fatal_handler(), "D3D12.DRED.Breadcrumb",
                                                     static_cast<std::int64_t>(lastBreadcrumb));
        cue::NativeError native =
            cue::NativeError::create(a_context.fatal_handler(), "D3D12.BreadcrumbOperation", operation);
        cue::Error detail =
            cue::Error::create(a_context.fatal_handler(), std::move(code), commandListName, std::move(native));
        std::string commandQueueNameStorage;
        const std::string_view commandQueueName =
            select_dred_name(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW, std::string_view(),
                             commandQueueNameStorage, a_context);
        if (!commandQueueName.empty())
        {
            detail.add_context(a_context.fatal_handler(), commandQueueName);
        }
        const cue::LogResult logResult = a_context.logger().log(
            cue::LogLevel::Error, "Tool Host D3D12 Device RemovalのBreadcrumb詳細を取得しました", std::move(detail));
        retain_dred_log_failure(a_primary, logResult, "Breadcrumb detail logging also failed", a_context);
        node = node->pNext;
        ++nodeCount;
    }
    if (node != nullptr)
    {
        const cue::LogResult logResult = a_context.logger().log(
            cue::LogLevel::Warning, "Tool Host DRED Breadcrumb Node上限を超えたため列挙を停止します");
        retain_dred_log_failure(a_primary, logResult, "Breadcrumb limit logging also failed", a_context);
    }
}

/// @brief DRED Allocation NodeをDevice解放前に診断へ転記する
void log_dred_allocations(const D3D12_DRED_ALLOCATION_NODE1 *a_head, std::string_view a_domain,
                          std::string_view a_message, cue::Error &a_primary,
                          const cue::AssertContext &a_context) noexcept
{
    const D3D12_DRED_ALLOCATION_NODE1 *node = a_head;
    std::uint32_t nodeCount = 0;
    while (node != nullptr && nodeCount < k_maxDredNodes)
    {
        std::string objectNameStorage;
        const std::string_view objectName = select_dred_name(
            node->ObjectNameA, node->ObjectNameW, "Unnamed Tool Host D3D12 allocation", objectNameStorage, a_context);
        cue::ErrorCode code = cue::ErrorCode::create(a_context.fatal_handler(), a_domain,
                                                     static_cast<std::int64_t>(node->AllocationType));
        cue::Error detail = cue::Error::create(a_context.fatal_handler(), std::move(code), objectName);
        const cue::LogResult logResult = a_context.logger().log(cue::LogLevel::Error, a_message, std::move(detail));
        retain_dred_log_failure(a_primary, logResult, "Allocation detail logging also failed", a_context);
        node = node->pNext;
        ++nodeCount;
    }
    if (node != nullptr)
    {
        const cue::LogResult logResult = a_context.logger().log(
            cue::LogLevel::Warning, "Tool Host DRED Allocation Node上限を超えたため列挙を停止します");
        retain_dred_log_failure(a_primary, logResult, "Allocation limit logging also failed", a_context);
    }
}

/// @brief 下位ErrorをTool Hostの回復不能なGPU完了不能Errorへ再分類する
[[nodiscard]] cue::Error make_gpu_completion_unavailable_error(const cue::AssertContext &a_context,
                                                               cue::Error &&a_cause) noexcept
{
    cue::ErrorCode code =
        cue::ErrorCode::create(a_context.fatal_handler(), "Cue.ToolHost",
                               static_cast<std::int64_t>(cue::tool_host::ToolHostError::GpuCompletionUnavailable));
    return cue::Error::reclassify(a_context.fatal_handler(), std::move(code),
                                  "Tool Host GPU completion could not be proven", std::move(a_cause));
}

/// @brief 集約済みErrorをLog／FlushしてResource解放前にFatal終端する
[[noreturn]] void terminate_unproven_completion(const cue::AssertContext &a_context, cue::Error &&a_cause) noexcept
{
    cue::Error unavailable = make_gpu_completion_unavailable_error(a_context, std::move(a_cause));
    cue::report_fatal(a_context.logger(), a_context.fatal_handler(),
                      "Tool Host GPU完了を証明できないためResource解放前に終了します", std::move(unavailable));
}

/// @brief Native WindowよりOwnerが先に解放されることを防ぐためProcessをFatal終端する
[[noreturn]] void terminate_window_destruction_failure(const cue::AssertContext &a_context,
                                                       cue::Error &&a_failure) noexcept
{
    cue::report_fatal(a_context.logger(), a_context.fatal_handler(),
                      "Tool Host Native Windowを破棄できないためOwner解放前に終了します", std::move(a_failure));
}

/// @brief Win32 HANDLEを一意所有してScope終了時に閉じる
class UniqueHandle final
{
  public:
    /// @brief 空のHANDLE所有者を生成する
    UniqueHandle() noexcept = default;
    /// @brief 有効なHANDLEの所有権を取得する
    explicit UniqueHandle(HANDLE a_handle) noexcept : m_handle(a_handle)
    {
    }
    /// @brief HANDLE所有権の複製を禁止する
    UniqueHandle(const UniqueHandle &) = delete;
    /// @brief HANDLE所有権の複製を禁止する
    UniqueHandle &operator=(const UniqueHandle &) = delete;

    /// @brief 所有HANDLEを閉じる
    ~UniqueHandle() noexcept
    {
        if (m_handle != nullptr)
        {
            CloseHandle(m_handle);
        }
    }

    /// @brief 所有HANDLEを非所有で返す
    [[nodiscard]] HANDLE get() const noexcept
    {
        return m_handle;
    }

  private:
    HANDLE m_handle = nullptr;
};

/// @brief ImGui Texture用Shader-visible SRV Descriptorを固定Poolから管理する
class DescriptorPool final
{
  public:
    /// @brief 未初期化Poolを生成する
    DescriptorPool() noexcept = default;

    /// @brief Descriptor HeapとIncrementを非所有で関連付ける
    void initialize(ID3D12DescriptorHeap &a_heap, std::uint32_t a_increment,
                    const cue::AssertContext &a_context) noexcept
    {
        m_heap = &a_heap;
        m_increment = a_increment;
        m_assertContext = &a_context;
        m_used.fill(false);
    }

    /// @brief 空きDescriptorを確保してCPU／GPU Handleを返す
    void allocate(D3D12_CPU_DESCRIPTOR_HANDLE &a_cpu, D3D12_GPU_DESCRIPTOR_HANDLE &a_gpu) noexcept
    {
        if (try_allocate(a_cpu, a_gpu))
        {
            return;
        }
        m_assertContext->fatal_handler().terminate("Tool Host ImGui SRV descriptor pool is exhausted");
        std::abort();
    }

    /// @brief 空きDescriptorがある場合だけ確保してHandleを返す
    [[nodiscard]] bool try_allocate(D3D12_CPU_DESCRIPTOR_HANDLE &a_cpu, D3D12_GPU_DESCRIPTOR_HANDLE &a_gpu) noexcept
    {
        for (std::size_t index = 0; index < m_used.size(); ++index)
        {
            if (!m_used[index])
            {
                m_used[index] = true;
                a_cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
                a_gpu = m_heap->GetGPUDescriptorHandleForHeapStart();
                a_cpu.ptr += index * m_increment;
                a_gpu.ptr += index * m_increment;
                return true;
            }
        }
        return false;
    }

    /// @brief Poolが発行したDescriptorを再利用可能に戻す
    void release(D3D12_CPU_DESCRIPTOR_HANDLE a_cpu) noexcept
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE first = m_heap->GetCPUDescriptorHandleForHeapStart();
        if (a_cpu.ptr < first.ptr || m_increment == 0)
        {
            m_assertContext->fatal_handler().terminate("Tool Host ImGui SRV descriptor is invalid");
        }
        const SIZE_T offset = a_cpu.ptr - first.ptr;
        if (offset % m_increment != 0 || offset / m_increment >= m_used.size())
        {
            m_assertContext->fatal_handler().terminate("Tool Host ImGui SRV descriptor is outside the pool");
        }
        if (!m_used[offset / m_increment])
        {
            m_assertContext->fatal_handler().terminate("Tool Host ImGui SRV descriptor was already released");
        }
        m_used[offset / m_increment] = false;
    }

  private:
    ID3D12DescriptorHeap *m_heap = nullptr;
    const cue::AssertContext *m_assertContext = nullptr;
    std::uint32_t m_increment = 0;
    std::array<bool, k_srvDescriptorCount> m_used{};
};

/// @brief ImGui DX12 BackendからDescriptor Poolへ確保要求を転送する
void allocate_imgui_descriptor(ImGui_ImplDX12_InitInfo *a_info, D3D12_CPU_DESCRIPTOR_HANDLE *a_cpu,
                               D3D12_GPU_DESCRIPTOR_HANDLE *a_gpu)
{
    auto *pool = static_cast<DescriptorPool *>(a_info->UserData);
    pool->allocate(*a_cpu, *a_gpu);
}

/// @brief ImGui DX12 BackendからDescriptor Poolへ解放要求を転送する
void release_imgui_descriptor(ImGui_ImplDX12_InitInfo *a_info, D3D12_CPU_DESCRIPTOR_HANDLE a_cpu,
                              D3D12_GPU_DESCRIPTOR_HANDLE)
{
    auto *pool = static_cast<DescriptorPool *>(a_info->UserData);
    pool->release(a_cpu);
}

/// @brief Platform Message Sinkを公式ImGui Win32 Backendへ接続する
class ImGuiWindowsMessageSink final : public cue::WindowsMessageSink
{
  public:
    /// @brief Win32 Messageを公式Backendへ変換してHandled結果を返す
    [[nodiscard]] cue::WindowsMessageResult process_message(const cue::WindowsMessageView &a_message) noexcept override
    {
        HWND window = static_cast<HWND>(const_cast<void *>(a_message.nativeWindow));
        const LRESULT result = ImGui_ImplWin32_WndProcHandler(window, static_cast<UINT>(a_message.message),
                                                              static_cast<WPARAM>(a_message.wordParameter),
                                                              static_cast<LPARAM>(a_message.longParameter));
        return {result != 0, static_cast<std::intptr_t>(result)};
    }
};

struct FrameResource final
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> constantBuffer;
    std::byte *mappedConstants = nullptr;
    std::uint64_t reuseFenceValue = 0;
};

/// @brief 一世代のOffscreen Render Surfaceと専用Descriptor所有権
struct RenderSurfaceGeneration final
{
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Resource> depthResource;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

/// @brief GPU完了確認まで保持する旧Render Surface世代
struct RetiredRenderSurface final
{
    RenderSurfaceGeneration generation;
    std::uint64_t fenceValue = 0U;
};

/// @brief Windows WindowとTool専用D3D12／ImGui Resourceの全寿命を所有する
class WindowsD3d12ToolHost final
{
  public:
    /// @brief 診断ContextをHost全寿命へ非所有で関連付ける
    explicit WindowsD3d12ToolHost(const cue::AssertContext &a_context) noexcept
        : m_assertContext(&a_context), m_messageSink(m_inputEvents, &m_imguiMessageSink)
    {
    }
    /// @brief Native Resource所有権の複製を禁止する
    WindowsD3d12ToolHost(const WindowsD3d12ToolHost &) = delete;
    /// @brief Native Resource所有権の複製を禁止する
    WindowsD3d12ToolHost &operator=(const WindowsD3d12ToolHost &) = delete;

    /// @brief 初期化済みResourceを依存順序の逆順で解放する
    ~WindowsD3d12ToolHost() noexcept
    {
        cleanup();
    }

    /// @brief Window、D3D12、ImGui Backendを描画開始前まで初期化する
    [[nodiscard]] cue::Result<void> initialize(const cue::tool_host::ToolHostDescriptor &a_descriptor) noexcept;

    /// @brief Message PumpとUI Frame提出を終了要求まで実行する
    [[nodiscard]] cue::Result<void> run(cue::tool_host::ToolHostClient &a_client) noexcept;

  private:
    /// @brief DXGI Device、Queue、Swap Chain、Frame Resourceを生成する
    [[nodiscard]] cue::Result<void> initialize_d3d12(
        HWND a_window, cue::WindowSize a_size, cue::tool_host::ToolHostAdapterPreference a_adapterPreference) noexcept;
    /// @brief ImGui Contextと公式Win32／DX12 Backendを生成する
    [[nodiscard]] cue::Result<void> initialize_imgui(HWND a_window) noexcept;
    /// @brief Built-in Cubeを描画する固定D3D12 PipelineとFrame Bufferを生成する
    [[nodiscard]] cue::Result<void> initialize_scene_renderer() noexcept;
    /// @brief Swap Chain Back BufferとRTVを再取得する
    [[nodiscard]] cue::Result<void> create_back_buffers() noexcept;
    /// @brief Back Buffer参照を全て解放する
    void release_back_buffers() noexcept;
    /// @brief 一つのImGui Frameを記録、Execute、Present、Signalする
    [[nodiscard]] cue::Result<void> render_frame(cue::tool_host::ToolHostClient &a_client) noexcept;
    /// @brief Client要求と現在世代を比較し必要な生成または退役を行う
    [[nodiscard]] cue::Result<void> prepare_render_surfaces(
        cue::tool_host::ToolHostRenderSurfaceRequests a_requests) noexcept;
    /// @brief 指定寸法のRender Target、RTV、SRVを新規生成する
    [[nodiscard]] cue::Result<RenderSurfaceGeneration> create_render_surface(std::uint32_t a_width,
                                                                             std::uint32_t a_height) noexcept;
    /// @brief Active Surfaceを最後の提出Fenceに関連付けて退役させる
    [[nodiscard]] cue::Result<void> retire_active_render_surface(std::size_t a_slot) noexcept;
    /// @brief 完了済みFenceに対応する退役Surfaceを解放する
    [[nodiscard]] cue::Result<void> collect_retired_render_surfaces() noexcept;
    /// @brief 現在世代のClear、Cube描画、Shader Resource遷移を記録する
    void record_render_surfaces(FrameResource &a_frame, cue::tool_host::ToolHostRenderFrameView a_renderFrame) noexcept;
    /// @brief Active SurfaceのImGui表示用非所有Viewを返す
    [[nodiscard]] cue::tool_host::ToolHostRenderSurfaceViews render_surface_views() const noexcept;
    /// @brief 一世代のSRV DescriptorとD3D12 Resourceを解放する
    void release_render_surface(RenderSurfaceGeneration &a_generation) noexcept;
    /// @brief Cleanup前に全Surface世代を解放する
    void release_all_render_surfaces() noexcept;
    /// @brief 全提出WorkをDrainしてSwap Chain SizeとRTVを再構築する
    [[nodiscard]] cue::Result<void> resize(cue::WindowSize a_size) noexcept;
    /// @brief Fence値をWrapさせず一度だけ予約する
    [[nodiscard]] cue::Result<std::uint64_t> reserve_fence_value() noexcept;
    /// @brief 指定Fence値を有限Waitし、失敗後の完了／Removalを再検査する
    [[nodiscard]] cue::Result<void> wait_for_fence(std::uint64_t a_value) noexcept;
    /// @brief Device RemovalをNative Reason付きErrorへ変換する
    [[nodiscard]] cue::Error make_device_removed_error() noexcept;
    /// @brief 先行ErrorをCauseとしてDevice Removalへ再分類する
    [[nodiscard]] cue::Error make_device_removed_error(cue::Error &&a_cause) noexcept;
    /// @brief ErrorがTool Host Device Removalを表すか判定する
    [[nodiscard]] static bool is_device_removed_error(const cue::Error &a_error) noexcept;
    /// @brief ErrorがGPU完了不能のFatal分類を表すか判定する
    [[nodiscard]] static bool is_gpu_completion_unavailable_error(const cue::Error &a_error) noexcept;
    /// @brief Device Removal時にDREDを一度だけBest-effort収集する
    void collect_device_removed_diagnostics(cue::Error &a_primary) noexcept;
    /// @brief DRED収集後に待機なしCleanupを行いDevice Removalを返す
    [[nodiscard]] cue::Result<void> finish_device_removed(cue::Error &&a_removed) noexcept;
    /// @brief Wait失敗後に全提出WorkをDrainして安全に終了する
    [[nodiscard]] cue::Result<void> finish_after_wait_error(cue::Error &&a_primary) noexcept;
    /// @brief 最後のUI提出後へTerminal Signalを置いて正常終了可能にする
    [[nodiscard]] cue::Result<void> drain_for_shutdown() noexcept;
    /// @brief GPU完了確認後またはDevice Removal時にBackendとNative Resourceを逆順解放する
    void cleanup(cue::Error *a_secondaryDiagnostics = nullptr) noexcept;

    const cue::AssertContext *m_assertContext;
    cue::InputEventQueue m_inputEvents;
    cue::InputState m_inputState;
    std::array<cue::InputEvent, cue::InputEventQueue::k_capacity> m_frameInputEvents;
    std::size_t m_frameInputEventCount = 0U;
    ImGuiWindowsMessageSink m_imguiMessageSink;
    cue::WindowsInputMessageSink m_messageSink;
    std::unique_ptr<cue::WindowSystem> m_windowSystem;
    std::unique_ptr<cue::Window> m_window;
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    std::array<ComPtr<ID3D12Resource>, k_frameCount> m_backBuffers;
    std::array<FrameResource, k_frameCount> m_frames;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12Fence> m_fence;
    ComPtr<ID3D12RootSignature> m_sceneRootSignature;
    ComPtr<ID3D12PipelineState> m_scenePipelineState;
    ComPtr<ID3D12Resource> m_sceneVertexBuffer;
    ComPtr<ID3D12Resource> m_sceneIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_sceneVertexView{};
    D3D12_INDEX_BUFFER_VIEW m_sceneIndexView{};
    DescriptorPool m_descriptorPool;
    std::array<RenderSurfaceGeneration, cue::tool_host::k_toolHostRenderSurfaceCount> m_renderSurfaces;
    std::array<RetiredRenderSurface, k_retiredRenderSurfaceCapacity> m_retiredRenderSurfaces;
    std::uint32_t m_rtvIncrement = 0;
    std::uint64_t m_nextFenceValue = 1;
    std::uint64_t m_lastSignaledFence = 0;
    std::uint64_t m_frameSequence = 0;
    std::uint64_t m_maximumFrameCount = 0;
    bool m_isMessageSinkAttached = false;
    bool m_isImGuiContextInitialized = false;
    bool m_isWin32BackendInitialized = false;
    bool m_isDx12BackendInitialized = false;
    bool m_isMinimized = false;
    bool m_wasDredCollectionAttempted = false;
};

cue::Result<void> WindowsD3d12ToolHost::initialize(const cue::tool_host::ToolHostDescriptor &a_descriptor) noexcept
{
    if (a_descriptor.title.empty() || a_descriptor.clientSize.width == 0 || a_descriptor.clientSize.height == 0)
    {
        return cue::Result<void>::failure(make_error(*m_assertContext,
                                                     cue::tool_host::ToolHostError::InvalidConfiguration,
                                                     "Tool Host configuration is invalid"));
    }
    m_maximumFrameCount = a_descriptor.maximumFrameCount;

    auto system = cue::create_windows_window_system(*m_assertContext);
    if (!system)
    {
        return cue::Result<void>::failure(std::move(*system.try_error()));
    }
    m_windowSystem = std::move(*system.try_value());
    cue::WindowDescriptor windowDescriptor = {a_descriptor.title, a_descriptor.clientSize};
    auto window = m_windowSystem->create_window(windowDescriptor);
    if (!window)
    {
        return cue::Result<void>::failure(std::move(*window.try_error()));
    }
    m_window = std::move(*window.try_value());
    auto nativeView = cue::get_native_window_view(*m_window, *m_assertContext);
    if (!nativeView)
    {
        return cue::Result<void>::failure(std::move(*nativeView.try_error()));
    }
    HWND nativeWindow = static_cast<HWND>(const_cast<void *>(nativeView.try_value()->value()));

    cue::Result<void> icon = apply_window_icon(nativeWindow, a_descriptor.iconResourceId, *m_assertContext);
    if (!icon)
    {
        return icon;
    }

    cue::Result<void> d3d12 = initialize_d3d12(nativeWindow, a_descriptor.clientSize, a_descriptor.adapterPreference);
    if (!d3d12)
    {
        return d3d12;
    }
    cue::Result<void> imgui = initialize_imgui(nativeWindow);
    if (!imgui)
    {
        return imgui;
    }
    cue::Result<void> attached = cue::attach_windows_message_sink(*m_window, m_messageSink, *m_assertContext);
    if (!attached)
    {
        return attached;
    }
    m_isMessageSinkAttached = true;
    return m_window->show();
}

cue::Result<void> WindowsD3d12ToolHost::initialize_d3d12(
    HWND a_window, cue::WindowSize a_size, cue::tool_host::ToolHostAdapterPreference a_adapterPreference) noexcept
{
    UINT factoryFlags = 0;
#if CUE_ENABLE_ASSERTS
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
    {
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetWatsonDumpEnablement(D3D12_DRED_ENABLEMENT_FORCED_OFF);
    }
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    HRESULT result = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
    if (FAILED(result) && factoryFlags != 0)
    {
        result = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
    }
    if (FAILED(result))
    {
        return cue::Result<void>::failure(make_native_error(*m_assertContext,
                                                            cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                                            "DXGI Factory creation failed", "HRESULT", result));
    }

    if (a_adapterPreference == cue::tool_host::ToolHostAdapterPreference::HardwarePreferred)
    {
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> adapter;
            result = m_factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                           IID_PPV_ARGS(&adapter));
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(result))
            {
                return cue::Result<void>::failure(
                    make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                      "DXGI Adapter enumeration failed", "HRESULT", result));
            }
            DXGI_ADAPTER_DESC1 description{};
            if (SUCCEEDED(adapter->GetDesc1(&description)) && (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device))))
            {
                break;
            }
        }
    }
    if (m_device == nullptr)
    {
        ComPtr<IDXGIAdapter> warpAdapter;
        result = m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
        if (SUCCEEDED(result))
        {
            result = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
        }
        if (FAILED(result) || m_device == nullptr)
        {
            return cue::Result<void>::failure(
                make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                  "No D3D12 adapter satisfies the Tool Host requirements", "HRESULT", result));
        }
    }

    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = m_device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&m_queue));
    if (FAILED(result))
    {
        return cue::Result<void>::failure(make_native_error(*m_assertContext,
                                                            cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                                            "D3D12 command queue creation failed", "HRESULT", result));
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDescription{};
    swapChainDescription.Width = a_size.width;
    swapChainDescription.Height = a_size.height;
    swapChainDescription.Format = k_backBufferFormat;
    swapChainDescription.SampleDesc.Count = 1;
    swapChainDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDescription.BufferCount = k_frameCount;
    swapChainDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapChain;
    result =
        m_factory->CreateSwapChainForHwnd(m_queue.Get(), a_window, &swapChainDescription, nullptr, nullptr, &swapChain);
    if (SUCCEEDED(result))
    {
        result = swapChain.As(&m_swapChain);
    }
    if (FAILED(result))
    {
        return cue::Result<void>::failure(make_native_error(*m_assertContext,
                                                            cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                                            "Tool Host Swap Chain creation failed", "HRESULT", result));
    }
    static_cast<void>(m_factory->MakeWindowAssociation(a_window, DXGI_MWA_NO_ALT_ENTER));

    D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
    rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDescription.NumDescriptors = k_frameCount;
    result = m_device->CreateDescriptorHeap(&rtvDescription, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(result))
    {
        return cue::Result<void>::failure(make_native_error(*m_assertContext,
                                                            cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                                            "Tool Host RTV Heap creation failed", "HRESULT", result));
    }
    m_rtvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvDescription{};
    srvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDescription.NumDescriptors = k_srvDescriptorCount;
    srvDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result = m_device->CreateDescriptorHeap(&srvDescription, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(result))
    {
        return cue::Result<void>::failure(make_native_error(*m_assertContext,
                                                            cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                                            "Tool Host SRV Heap creation failed", "HRESULT", result));
    }
    m_descriptorPool.initialize(*m_srvHeap.Get(),
                                m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
                                *m_assertContext);

    for (FrameResource &frame : m_frames)
    {
        result = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator));
        if (FAILED(result))
        {
            return cue::Result<void>::failure(
                make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                  "Tool Host command allocator creation failed", "HRESULT", result));
        }
    }
    result = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&m_commandList));
    if (SUCCEEDED(result))
    {
        result = m_commandList->Close();
    }
    if (FAILED(result))
    {
        return cue::Result<void>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                              "Tool Host command list creation failed", "HRESULT", result));
    }
    result = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(result))
    {
        return cue::Result<void>::failure(make_native_error(*m_assertContext,
                                                            cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                                            "Tool Host Fence creation failed", "HRESULT", result));
    }
    cue::Result<void> sceneRenderer = initialize_scene_renderer();
    if (!sceneRenderer)
    {
        return sceneRenderer;
    }
    cue::Result<void> buffers = create_back_buffers();
    if (!buffers && FAILED(m_device->GetDeviceRemovedReason()))
    {
        cue::Error removed = make_device_removed_error(std::move(*buffers.try_error()));
        return finish_device_removed(std::move(removed));
    }
    return buffers;
}

cue::Result<void> WindowsD3d12ToolHost::initialize_scene_renderer() noexcept
{
    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> diagnostics;
    constexpr UINT k_compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
    HRESULT result = D3DCompile(k_sceneShader.data(), k_sceneShader.size(), "Cue.ToolHost.SceneShader", nullptr,
                                nullptr, "vs_main", "vs_5_1", k_compileFlags, 0U, &vertexShader, &diagnostics);
    if (FAILED(result))
    {
        return cue::Result<void>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                              "Tool Host scene vertex shader compilation failed", "HRESULT", result));
    }
    diagnostics.Reset();
    result = D3DCompile(k_sceneShader.data(), k_sceneShader.size(), "Cue.ToolHost.SceneShader", nullptr, nullptr,
                        "ps_main", "ps_5_1", k_compileFlags, 0U, &pixelShader, &diagnostics);
    if (FAILED(result))
    {
        return cue::Result<void>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                              "Tool Host scene pixel shader compilation failed", "HRESULT", result));
    }

    D3D12_ROOT_PARAMETER rootParameter{};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.Descriptor.ShaderRegister = 0U;
    rootParameter.Descriptor.RegisterSpace = 0U;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 1U;
    rootDescription.pParameters = &rootParameter;
    rootDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
                            D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> serializedRoot;
    diagnostics.Reset();
    result = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serializedRoot, &diagnostics);
    if (SUCCEEDED(result))
    {
        result = m_device->CreateRootSignature(0U, serializedRoot->GetBufferPointer(), serializedRoot->GetBufferSize(),
                                               IID_PPV_ARGS(&m_sceneRootSignature));
    }
    if (FAILED(result))
    {
        return cue::Result<void>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                              "Tool Host scene root signature creation failed", "HRESULT", result));
    }

    constexpr std::array<D3D12_INPUT_ELEMENT_DESC, 2U> k_inputElements{{
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    }};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = m_sceneRootSignature.Get();
    pipeline.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    pipeline.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    pipeline.BlendState.AlphaToCoverageEnable = FALSE;
    pipeline.BlendState.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC renderTargetBlend{};
    renderTargetBlend.BlendEnable = FALSE;
    renderTargetBlend.LogicOpEnable = FALSE;
    renderTargetBlend.SrcBlend = D3D12_BLEND_ONE;
    renderTargetBlend.DestBlend = D3D12_BLEND_ZERO;
    renderTargetBlend.BlendOp = D3D12_BLEND_OP_ADD;
    renderTargetBlend.SrcBlendAlpha = D3D12_BLEND_ONE;
    renderTargetBlend.DestBlendAlpha = D3D12_BLEND_ZERO;
    renderTargetBlend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    renderTargetBlend.LogicOp = D3D12_LOGIC_OP_NOOP;
    renderTargetBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.BlendState.RenderTarget[0] = renderTargetBlend;
    pipeline.SampleMask = UINT_MAX;
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pipeline.RasterizerState.FrontCounterClockwise = TRUE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.DepthStencilState.DepthEnable = TRUE;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pipeline.DepthStencilState.StencilEnable = FALSE;
    pipeline.InputLayout = {k_inputElements.data(), static_cast<UINT>(k_inputElements.size())};
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1U;
    pipeline.RTVFormats[0] = k_backBufferFormat;
    pipeline.DSVFormat = k_depthFormat;
    pipeline.SampleDesc.Count = 1U;
    result = m_device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&m_scenePipelineState));
    if (FAILED(result))
    {
        return cue::Result<void>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                              "Tool Host scene pipeline state creation failed", "HRESULT", result));
    }

    const cue::engine_assets::MeshView cube = cue::engine_assets::built_in_cube_mesh();
    const UINT64 vertexBytes = static_cast<UINT64>(cube.vertices.size_bytes());
    const UINT64 indexBytes = static_cast<UINT64>(cube.indices.size_bytes());
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    /// @brief Scene固定DataとFrame Constants用のUpload Bufferを生成する
    const auto createUploadBuffer = [&](UINT64 a_size, ComPtr<ID3D12Resource> &a_resource) noexcept -> HRESULT
    {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = a_size;
        description.Height = 1U;
        description.DepthOrArraySize = 1U;
        description.MipLevels = 1U;
        description.Format = DXGI_FORMAT_UNKNOWN;
        description.SampleDesc.Count = 1U;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return m_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &description,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&a_resource));
    };
    result = createUploadBuffer(vertexBytes, m_sceneVertexBuffer);
    if (SUCCEEDED(result))
    {
        result = createUploadBuffer(indexBytes, m_sceneIndexBuffer);
    }
    if (FAILED(result))
    {
        return cue::Result<void>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                              "Tool Host scene geometry buffer creation failed", "HRESULT", result));
    }
    void *mapped = nullptr;
    result = m_sceneVertexBuffer->Map(0U, nullptr, &mapped);
    if (SUCCEEDED(result))
    {
        std::memcpy(mapped, cube.vertices.data(), cube.vertices.size_bytes());
        m_sceneVertexBuffer->Unmap(0U, nullptr);
        mapped = nullptr;
        result = m_sceneIndexBuffer->Map(0U, nullptr, &mapped);
    }
    if (SUCCEEDED(result))
    {
        std::memcpy(mapped, cube.indices.data(), cube.indices.size_bytes());
        m_sceneIndexBuffer->Unmap(0U, nullptr);
    }
    if (FAILED(result))
    {
        return cue::Result<void>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                              "Tool Host scene geometry upload failed", "HRESULT", result));
    }
    m_sceneVertexView = {m_sceneVertexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(vertexBytes),
                         static_cast<UINT>(sizeof(cue::engine_assets::MeshVertex))};
    m_sceneIndexView = {m_sceneIndexBuffer->GetGPUVirtualAddress(), static_cast<UINT>(indexBytes),
                        DXGI_FORMAT_R16_UINT};

    const UINT64 constantBytes = static_cast<UINT64>(k_constantBufferStride) *
                                 cue::tool_host::k_toolHostRenderSurfaceCount * k_maximumRenderMeshInstances;
    for (FrameResource &frame : m_frames)
    {
        result = createUploadBuffer(constantBytes, frame.constantBuffer);
        if (SUCCEEDED(result))
        {
            void *constantMapping = nullptr;
            result = frame.constantBuffer->Map(0U, nullptr, &constantMapping);
            frame.mappedConstants = static_cast<std::byte *>(constantMapping);
        }
        if (FAILED(result))
        {
            return cue::Result<void>::failure(
                make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                  "Tool Host scene constant buffer creation failed", "HRESULT", result));
        }
    }
    return cue::Result<void>::success();
}

cue::Result<void> WindowsD3d12ToolHost::initialize_imgui(HWND a_window) noexcept
{
    IMGUI_CHECKVERSION();
    if (ImGui::CreateContext() == nullptr)
    {
        return cue::Result<void>::failure(make_error(*m_assertContext,
                                                     cue::tool_host::ToolHostError::ImGuiInitializationFailed,
                                                     "ImGui Context creation failed"));
    }
    m_isImGuiContextInitialized = true;
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (!configure_japanese_font(*m_assertContext))
    {
        return cue::Result<void>::failure(make_error(*m_assertContext,
                                                     cue::tool_host::ToolHostError::ImGuiInitializationFailed,
                                                     "Japanese UI font is unavailable"));
    }
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(a_window))
    {
        return cue::Result<void>::failure(make_error(*m_assertContext,
                                                     cue::tool_host::ToolHostError::ImGuiInitializationFailed,
                                                     "ImGui Win32 Backend initialization failed"));
    }
    m_isWin32BackendInitialized = true;

    ImGui_ImplDX12_InitInfo information{};
    information.Device = m_device.Get();
    information.CommandQueue = m_queue.Get();
    information.NumFramesInFlight = static_cast<int>(k_frameCount);
    information.RTVFormat = k_backBufferFormat;
    information.DSVFormat = DXGI_FORMAT_UNKNOWN;
    information.UserData = &m_descriptorPool;
    information.SrvDescriptorHeap = m_srvHeap.Get();
    information.SrvDescriptorAllocFn = allocate_imgui_descriptor;
    information.SrvDescriptorFreeFn = release_imgui_descriptor;
    if (!ImGui_ImplDX12_Init(&information))
    {
        return cue::Result<void>::failure(make_error(*m_assertContext,
                                                     cue::tool_host::ToolHostError::ImGuiInitializationFailed,
                                                     "ImGui DX12 Backend initialization failed"));
    }
    m_isDx12BackendInitialized = true;
    return cue::Result<void>::success();
}

cue::Result<void> WindowsD3d12ToolHost::create_back_buffers() noexcept
{
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t index = 0; index < k_frameCount; ++index)
    {
        const HRESULT result = m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_backBuffers[index]));
        if (FAILED(result))
        {
            return cue::Result<void>::failure(
                make_native_error(*m_assertContext, cue::tool_host::ToolHostError::SwapChainResizeFailed,
                                  "Tool Host Back Buffer acquisition failed", "HRESULT", result));
        }
        m_device->CreateRenderTargetView(m_backBuffers[index].Get(), nullptr, descriptor);
        descriptor.ptr += m_rtvIncrement;
    }
    return cue::Result<void>::success();
}

void WindowsD3d12ToolHost::release_back_buffers() noexcept
{
    for (ComPtr<ID3D12Resource> &buffer : m_backBuffers)
    {
        buffer.Reset();
    }
}

cue::Result<RenderSurfaceGeneration> WindowsD3d12ToolHost::create_render_surface(std::uint32_t a_width,
                                                                                 std::uint32_t a_height) noexcept
{
    RenderSurfaceGeneration generation;
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC resourceDescription{};
    resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDescription.Width = a_width;
    resourceDescription.Height = a_height;
    resourceDescription.DepthOrArraySize = 1U;
    resourceDescription.MipLevels = 1U;
    resourceDescription.Format = k_backBufferFormat;
    resourceDescription.SampleDesc.Count = 1U;
    resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = k_backBufferFormat;
    clearValue.Color[0] = 0.035F;
    clearValue.Color[1] = 0.105F;
    clearValue.Color[2] = 0.19F;
    clearValue.Color[3] = 1.0F;
    HRESULT result = m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &resourceDescription,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
                                                       IID_PPV_ARGS(&generation.resource));
    if (FAILED(result))
    {
        return cue::Result<RenderSurfaceGeneration>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::RenderSurfaceCreationFailed,
                              "Tool Host render surface resource creation failed", "HRESULT", result));
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
    rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDescription.NumDescriptors = 1U;
    result = m_device->CreateDescriptorHeap(&rtvDescription, IID_PPV_ARGS(&generation.rtvHeap));
    if (FAILED(result))
    {
        return cue::Result<RenderSurfaceGeneration>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::RenderSurfaceCreationFailed,
                              "Tool Host render surface RTV heap creation failed", "HRESULT", result));
    }
    m_device->CreateRenderTargetView(generation.resource.Get(), nullptr,
                                     generation.rtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_DESC depthDescription = resourceDescription;
    depthDescription.Format = k_depthFormat;
    depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = k_depthFormat;
    depthClear.DepthStencil.Depth = 1.0F;
    result = m_device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &depthDescription,
                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                                               IID_PPV_ARGS(&generation.depthResource));
    if (FAILED(result))
    {
        return cue::Result<RenderSurfaceGeneration>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::RenderSurfaceCreationFailed,
                              "Tool Host render surface depth resource creation failed", "HRESULT", result));
    }
    D3D12_DESCRIPTOR_HEAP_DESC dsvDescription{};
    dsvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDescription.NumDescriptors = 1U;
    result = m_device->CreateDescriptorHeap(&dsvDescription, IID_PPV_ARGS(&generation.dsvHeap));
    if (FAILED(result))
    {
        return cue::Result<RenderSurfaceGeneration>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::RenderSurfaceCreationFailed,
                              "Tool Host render surface DSV heap creation failed", "HRESULT", result));
    }
    m_device->CreateDepthStencilView(generation.depthResource.Get(), nullptr,
                                     generation.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    if (!m_descriptorPool.try_allocate(generation.srvCpu, generation.srvGpu))
    {
        return cue::Result<RenderSurfaceGeneration>::failure(
            make_error(*m_assertContext, cue::tool_host::ToolHostError::RenderSurfaceDescriptorExhausted,
                       "Tool Host render surface SRV descriptor pool is exhausted"));
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDescription{};
    srvDescription.Format = k_backBufferFormat;
    srvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDescription.Texture2D.MipLevels = 1U;
    m_device->CreateShaderResourceView(generation.resource.Get(), &srvDescription, generation.srvCpu);
    generation.width = a_width;
    generation.height = a_height;
    return cue::Result<RenderSurfaceGeneration>::success(std::move(generation));
}

void WindowsD3d12ToolHost::release_render_surface(RenderSurfaceGeneration &a_generation) noexcept
{
    if (a_generation.width != 0U)
    {
        m_descriptorPool.release(a_generation.srvCpu);
    }
    a_generation = {};
}

cue::Result<void> WindowsD3d12ToolHost::collect_retired_render_surfaces() noexcept
{
    const std::uint64_t completedFence = m_fence->GetCompletedValue();
    if (completedFence == (std::numeric_limits<std::uint64_t>::max)())
    {
        return cue::Result<void>::failure(make_device_removed_error());
    }
    for (RetiredRenderSurface &retired : m_retiredRenderSurfaces)
    {
        if (retired.generation.resource != nullptr && retired.fenceValue <= completedFence)
        {
            release_render_surface(retired.generation);
            retired.fenceValue = 0U;
        }
    }
    return cue::Result<void>::success();
}

cue::Result<void> WindowsD3d12ToolHost::retire_active_render_surface(std::size_t a_slot) noexcept
{
    RenderSurfaceGeneration &active = m_renderSurfaces[a_slot];
    if (active.resource == nullptr)
    {
        return cue::Result<void>::success();
    }
    for (RetiredRenderSurface &retired : m_retiredRenderSurfaces)
    {
        if (retired.generation.resource == nullptr)
        {
            retired.generation = std::move(active);
            retired.fenceValue = m_lastSignaledFence;
            active = {};
            return cue::Result<void>::success();
        }
    }
    return cue::Result<void>::failure(make_error(*m_assertContext,
                                                 cue::tool_host::ToolHostError::RenderSurfaceRetirementCapacityExceeded,
                                                 "Tool Host render surface retirement capacity is exhausted"));
}

cue::Result<void> WindowsD3d12ToolHost::prepare_render_surfaces(
    cue::tool_host::ToolHostRenderSurfaceRequests a_requests) noexcept
{
    cue::Result<void> collected = collect_retired_render_surfaces();
    if (!collected)
    {
        return collected;
    }
    for (std::size_t slot = 0U; slot < a_requests.size(); ++slot)
    {
        const cue::tool_host::ToolHostRenderSurfaceRequest request = a_requests[slot];
        RenderSurfaceGeneration &active = m_renderSurfaces[slot];
        if (!request.isVisible)
        {
            cue::Result<void> retired = retire_active_render_surface(slot);
            if (!retired)
            {
                return retired;
            }
            continue;
        }
        if (request.width == 0U || request.height == 0U ||
            request.width > cue::tool_host::k_maximumToolHostRenderSurfaceDimension ||
            request.height > cue::tool_host::k_maximumToolHostRenderSurfaceDimension)
        {
            return cue::Result<void>::failure(make_error(*m_assertContext,
                                                         cue::tool_host::ToolHostError::RenderSurfaceInvalidSize,
                                                         "Tool Host render surface size is invalid"));
        }
        if (active.resource != nullptr && active.width == request.width && active.height == request.height)
        {
            continue;
        }

        cue::Result<RenderSurfaceGeneration> created = create_render_surface(request.width, request.height);
        if (!created)
        {
            return cue::Result<void>::failure(std::move(*created.try_error()));
        }
        cue::Result<void> retired = retire_active_render_surface(slot);
        if (!retired)
        {
            release_render_surface(*created.try_value());
            return retired;
        }
        active = std::move(*created.try_value());
    }
    return cue::Result<void>::success();
}

cue::tool_host::ToolHostRenderSurfaceViews WindowsD3d12ToolHost::render_surface_views() const noexcept
{
    cue::tool_host::ToolHostRenderSurfaceViews views;
    for (std::size_t slot = 0U; slot < m_renderSurfaces.size(); ++slot)
    {
        const RenderSurfaceGeneration &surface = m_renderSurfaces[slot];
        if (surface.resource != nullptr)
        {
            views[slot] = {static_cast<std::uint64_t>(surface.srvGpu.ptr), surface.width, surface.height};
        }
    }
    return views;
}

void WindowsD3d12ToolHost::record_render_surfaces(FrameResource &a_frame,
                                                  cue::tool_host::ToolHostRenderFrameView a_renderFrame) noexcept
{
    for (std::size_t slot = 0U; slot < m_renderSurfaces.size(); ++slot)
    {
        RenderSurfaceGeneration &surface = m_renderSurfaces[slot];
        if (surface.resource == nullptr)
        {
            continue;
        }
        D3D12_RESOURCE_BARRIER toRenderTarget{};
        toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRenderTarget.Transition.pResource = surface.resource.Get();
        toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1U, &toRenderTarget);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = surface.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        constexpr std::array<std::array<float, 4U>, cue::tool_host::k_toolHostRenderSurfaceCount> k_clearColors{{
            {0.035F, 0.105F, 0.19F, 1.0F},
            {0.08F, 0.075F, 0.095F, 1.0F},
        }};
        m_commandList->ClearRenderTargetView(rtv, k_clearColors[slot].data(), 0U, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = surface.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0F, 0U, 0U, nullptr);

        const cue::renderer::PerspectiveCamera *camera =
            slot == static_cast<std::size_t>(cue::tool_host::ToolHostRenderSurfaceSlot::GameView)
                ? a_renderFrame.gameCamera
                : a_renderFrame.debugCamera;
        if (a_renderFrame.snapshot != nullptr && camera != nullptr && cue::renderer::is_valid(*camera))
        {
            const D3D12_VIEWPORT viewport{
                0.0F, 0.0F, static_cast<float>(surface.width), static_cast<float>(surface.height), 0.0F, 1.0F};
            const D3D12_RECT scissor{0, 0, static_cast<LONG>(surface.width), static_cast<LONG>(surface.height)};
            m_commandList->RSSetViewports(1U, &viewport);
            m_commandList->RSSetScissorRects(1U, &scissor);
            m_commandList->OMSetRenderTargets(1U, &rtv, FALSE, &dsv);
            m_commandList->SetGraphicsRootSignature(m_sceneRootSignature.Get());
            m_commandList->SetPipelineState(m_scenePipelineState.Get());
            m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_commandList->IASetVertexBuffers(0U, 1U, &m_sceneVertexView);
            m_commandList->IASetIndexBuffer(&m_sceneIndexView);

            const float aspect = static_cast<float>(surface.width) / static_cast<float>(surface.height);
            const cue::math::Matrix4 viewProjection = cue::renderer::make_view_projection(*camera, aspect);
            const std::span<const cue::renderer::RenderMeshInstance> meshes = a_renderFrame.snapshot->meshes();
            const std::size_t drawCount = (std::min)(meshes.size(), k_maximumRenderMeshInstances);
            for (std::size_t draw = 0U; draw < drawCount; ++draw)
            {
                if (meshes[draw].mesh != cue::renderer::RenderMesh::Cube)
                {
                    continue;
                }
                const cue::math::Matrix4 worldViewProjection =
                    cue::renderer::make_world_matrix(meshes[draw].transform) * viewProjection;
                const std::size_t constantIndex = slot * k_maximumRenderMeshInstances + draw;
                const std::size_t constantOffset = constantIndex * k_constantBufferStride;
                std::memcpy(a_frame.mappedConstants + constantOffset, &worldViewProjection,
                            sizeof(worldViewProjection));
                m_commandList->SetGraphicsRootConstantBufferView(0U, a_frame.constantBuffer->GetGPUVirtualAddress() +
                                                                         constantOffset);
                m_commandList->DrawIndexedInstanced(
                    static_cast<UINT>(cue::engine_assets::built_in_cube_mesh().indices.size()), 1U, 0U, 0, 0U);
            }
        }
        D3D12_RESOURCE_BARRIER toShaderResource = toRenderTarget;
        toShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toShaderResource.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        m_commandList->ResourceBarrier(1U, &toShaderResource);
    }
}

void WindowsD3d12ToolHost::release_all_render_surfaces() noexcept
{
    for (RenderSurfaceGeneration &surface : m_renderSurfaces)
    {
        release_render_surface(surface);
    }
    for (RetiredRenderSurface &retired : m_retiredRenderSurfaces)
    {
        release_render_surface(retired.generation);
        retired.fenceValue = 0U;
    }
}

cue::Result<std::uint64_t> WindowsD3d12ToolHost::reserve_fence_value() noexcept
{
    if (m_nextFenceValue == std::numeric_limits<std::uint64_t>::max())
    {
        return cue::Result<std::uint64_t>::failure(make_error(*m_assertContext,
                                                              cue::tool_host::ToolHostError::FenceValueExhausted,
                                                              "Tool Host Fence value is exhausted"));
    }
    std::uint64_t value = m_nextFenceValue;
    ++m_nextFenceValue;
    return cue::Result<std::uint64_t>::success(std::move(value));
}

cue::Error WindowsD3d12ToolHost::make_device_removed_error() noexcept
{
    const HRESULT reason = m_device != nullptr ? m_device->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
    return make_native_error(*m_assertContext, cue::tool_host::ToolHostError::DeviceRemoved,
                             "Tool Host D3D12 Device was removed", "HRESULT", reason);
}

cue::Error WindowsD3d12ToolHost::make_device_removed_error(cue::Error &&a_cause) noexcept
{
    const HRESULT reason = m_device != nullptr ? m_device->GetDeviceRemovedReason() : DXGI_ERROR_DEVICE_REMOVED;
    cue::ErrorCode code =
        cue::ErrorCode::create(m_assertContext->fatal_handler(), "Cue.ToolHost",
                               static_cast<std::int64_t>(cue::tool_host::ToolHostError::DeviceRemoved));
    cue::NativeError native =
        cue::NativeError::create(m_assertContext->fatal_handler(), "HRESULT", static_cast<std::int64_t>(reason));
    return cue::Error::reclassify(m_assertContext->fatal_handler(), std::move(code),
                                  "Tool Host D3D12 Device was removed", std::move(native), std::move(a_cause));
}

bool WindowsD3d12ToolHost::is_device_removed_error(const cue::Error &a_error) noexcept
{
    return a_error.code().domain() == "Cue.ToolHost" &&
           a_error.code().value() == static_cast<std::int64_t>(cue::tool_host::ToolHostError::DeviceRemoved);
}

bool WindowsD3d12ToolHost::is_gpu_completion_unavailable_error(const cue::Error &a_error) noexcept
{
    return a_error.code().domain() == "Cue.ToolHost" &&
           a_error.code().value() == static_cast<std::int64_t>(cue::tool_host::ToolHostError::GpuCompletionUnavailable);
}

void WindowsD3d12ToolHost::collect_device_removed_diagnostics(cue::Error &a_primary) noexcept
{
    if (m_wasDredCollectionAttempted || m_device == nullptr)
    {
        return;
    }
    m_wasDredCollectionAttempted = true;

    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    HRESULT result = m_device.As(&dred);
    if (FAILED(result))
    {
        cue::Error query = make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                             "Tool Host DRED interface query failed", "HRESULT", result);
        a_primary.append_secondary_diagnostics(*m_assertContext, query,
                                               "Device Removal DRED interface query also failed", "DRED");
        return;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
    result = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
    if (FAILED(result))
    {
        cue::Error breadcrumb =
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                              "Tool Host DRED breadcrumb query failed", "HRESULT", result);
        a_primary.append_secondary_diagnostics(*m_assertContext, breadcrumb,
                                               "Device Removal DRED breadcrumb query also failed", "DRED");
    }
    else if (breadcrumbs.pHeadAutoBreadcrumbNode != nullptr)
    {
        log_dred_breadcrumbs(breadcrumbs, a_primary, *m_assertContext);
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
    result = dred->GetPageFaultAllocationOutput1(&pageFault);
    if (FAILED(result))
    {
        cue::Error pageFaultError =
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                              "Tool Host DRED page fault query failed", "HRESULT", result);
        a_primary.append_secondary_diagnostics(*m_assertContext, pageFaultError,
                                               "Device Removal DRED page fault query also failed", "DRED");
    }
    else
    {
        cue::Error address = make_native_error(*m_assertContext, cue::tool_host::ToolHostError::DeviceRemoved,
                                               "Tool Host DRED page fault address", "D3D12.GpuVirtualAddress",
                                               static_cast<std::int64_t>(pageFault.PageFaultVA));
        const cue::LogResult pageFaultLogResult = m_assertContext->logger().log(
            cue::LogLevel::Error, "Tool Host D3D12 Device RemovalのPage Fault情報を取得しました", std::move(address));
        retain_dred_log_failure(a_primary, pageFaultLogResult, "Page fault address logging also failed",
                                *m_assertContext);
        log_dred_allocations(pageFault.pHeadExistingAllocationNode, "D3D12.DRED.ExistingAllocation",
                             "Tool Host D3D12 Device Removalの既存Allocation詳細を取得しました", a_primary,
                             *m_assertContext);
        log_dred_allocations(pageFault.pHeadRecentFreedAllocationNode, "D3D12.DRED.RecentFreedAllocation",
                             "Tool Host D3D12 Device Removalの解放済みAllocation詳細を取得しました", a_primary,
                             *m_assertContext);
    }
}

cue::Result<void> WindowsD3d12ToolHost::finish_device_removed(cue::Error &&a_removed) noexcept
{
    collect_device_removed_diagnostics(a_removed);
    cleanup(&a_removed);
    return cue::Result<void>::failure(std::move(a_removed));
}

cue::Result<void> WindowsD3d12ToolHost::wait_for_fence(std::uint64_t a_value) noexcept
{
    if (a_value == 0)
    {
        return cue::Result<void>::success();
    }
    std::uint64_t completed = m_fence->GetCompletedValue();
    if (completed == std::numeric_limits<std::uint64_t>::max())
    {
        return cue::Result<void>::failure(make_device_removed_error());
    }
    if (completed >= a_value)
    {
        return cue::Result<void>::success();
    }

    UniqueHandle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    DWORD waitCode = ERROR_SUCCESS;
    HRESULT registrationCode = S_OK;
    if (event.get() == nullptr)
    {
        waitCode = GetLastError();
    }
    else
    {
        const HRESULT registered = m_fence->SetEventOnCompletion(a_value, event.get());
        if (FAILED(registered))
        {
            registrationCode = registered;
        }
        else
        {
            const DWORD waited = WaitForSingleObject(event.get(), k_fenceTimeoutMilliseconds);
            if (waited != WAIT_OBJECT_0)
            {
                waitCode = waited == WAIT_FAILED ? GetLastError() : waited;
            }
        }
    }

    completed = m_fence->GetCompletedValue();
    if (completed == std::numeric_limits<std::uint64_t>::max() || FAILED(m_device->GetDeviceRemovedReason()))
    {
        if (FAILED(registrationCode))
        {
            cue::Error registration =
                make_native_error(*m_assertContext, cue::tool_host::ToolHostError::FenceWaitFailed,
                                  "Tool Host Fence completion event registration failed before Device Removal",
                                  "HRESULT", registrationCode);
            return cue::Result<void>::failure(make_device_removed_error(std::move(registration)));
        }
        if (waitCode != ERROR_SUCCESS)
        {
            cue::Error wait = make_native_error(*m_assertContext, cue::tool_host::ToolHostError::FenceWaitFailed,
                                                "Tool Host Fence wait failed before Device Removal", "Win32", waitCode);
            return cue::Result<void>::failure(make_device_removed_error(std::move(wait)));
        }
        return cue::Result<void>::failure(make_device_removed_error());
    }
    if (completed >= a_value)
    {
        if (FAILED(registrationCode))
        {
            return cue::Result<void>::failure(make_native_error(
                *m_assertContext, cue::tool_host::ToolHostError::FenceWaitFailed,
                "Tool Host Fence completion event registration failed after completion", "HRESULT", registrationCode));
        }
        if (waitCode == ERROR_SUCCESS)
        {
            return cue::Result<void>::success();
        }
        return cue::Result<void>::failure(
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::FenceWaitFailed,
                              "Tool Host Fence wait failed after completion", "Win32", waitCode));
    }

    cue::Error waitError =
        FAILED(registrationCode)
            ? make_native_error(*m_assertContext, cue::tool_host::ToolHostError::FenceWaitFailed,
                                "Tool Host Fence completion event registration failed", "HRESULT", registrationCode)
        : waitCode != ERROR_SUCCESS
            ? make_native_error(*m_assertContext, cue::tool_host::ToolHostError::FenceWaitFailed,
                                "Tool Host Fence wait did not prove completion", "Win32", waitCode)
            : make_error(*m_assertContext, cue::tool_host::ToolHostError::FenceWaitFailed,
                         "Tool Host Fence completed event did not prove completion");
    return cue::Result<void>::failure(make_gpu_completion_unavailable_error(*m_assertContext, std::move(waitError)));
}

cue::Result<void> WindowsD3d12ToolHost::finish_after_wait_error(cue::Error &&a_primary) noexcept
{
    if (is_device_removed_error(a_primary))
    {
        return finish_device_removed(std::move(a_primary));
    }
    if (is_gpu_completion_unavailable_error(a_primary))
    {
        terminate_unproven_completion(*m_assertContext, std::move(a_primary));
    }

    cue::Result<void> drained = wait_for_fence(m_lastSignaledFence);
    if (!drained)
    {
        const bool wasDeviceRemoved = is_device_removed_error(*drained.try_error());
        const bool wasCompletionUnavailable = is_gpu_completion_unavailable_error(*drained.try_error());
        a_primary.append_secondary_diagnostics(*m_assertContext, *drained.try_error(),
                                               "Tool Host final drain wait failed", "Drain");
        if (wasDeviceRemoved)
        {
            cue::Error removed = make_device_removed_error(std::move(a_primary));
            return finish_device_removed(std::move(removed));
        }
        if (wasCompletionUnavailable)
        {
            terminate_unproven_completion(*m_assertContext, std::move(a_primary));
        }
    }
    cleanup();
    return cue::Result<void>::failure(std::move(a_primary));
}

cue::Result<void> WindowsD3d12ToolHost::render_frame(cue::tool_host::ToolHostClient &a_client) noexcept
{
    const std::size_t frameIndex = static_cast<std::size_t>(m_frameSequence % k_frameCount);
    FrameResource &frame = m_frames[frameIndex];
    cue::Result<void> reusable = wait_for_fence(frame.reuseFenceValue);
    if (!reusable)
    {
        return finish_after_wait_error(std::move(*reusable.try_error()));
    }

    cue::Result<void> prepared = prepare_render_surfaces(a_client.render_surface_requests());
    if (!prepared)
    {
        cue::Error error = std::move(*prepared.try_error());
        if (is_device_removed_error(error))
        {
            return finish_device_removed(std::move(error));
        }
        return finish_after_wait_error(std::move(error));
    }

    cue::Result<std::uint64_t> reserved = reserve_fence_value();
    if (!reserved)
    {
        cue::Error exhausted = std::move(*reserved.try_error());
        cue::Result<void> drained = wait_for_fence(m_lastSignaledFence);
        if (!drained)
        {
            const bool wasDeviceRemoved = is_device_removed_error(*drained.try_error());
            const bool wasCompletionUnavailable = is_gpu_completion_unavailable_error(*drained.try_error());
            exhausted.append_secondary_diagnostics(*m_assertContext, *drained.try_error(),
                                                   "Tool Host Fence exhaustion drain failed", "Drain");
            if (wasDeviceRemoved)
            {
                cue::Error removed = make_device_removed_error(std::move(exhausted));
                return finish_device_removed(std::move(removed));
            }
            if (wasCompletionUnavailable)
            {
                terminate_unproven_completion(*m_assertContext, std::move(exhausted));
            }
            return finish_after_wait_error(std::move(exhausted));
        }
        cleanup();
        return cue::Result<void>::failure(std::move(exhausted));
    }
    const std::uint64_t fenceValue = *reserved.try_value();

    HRESULT result = frame.allocator->Reset();
    if (SUCCEEDED(result))
    {
        result = m_commandList->Reset(frame.allocator.Get(), nullptr);
    }
    if (FAILED(result))
    {
        cue::Error reset = make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                             "Tool Host command recording reset failed", "HRESULT", result);
        return finish_after_wait_error(std::move(reset));
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    m_inputState.begin_frame({});
    m_frameInputEventCount = 0U;
    cue::InputEvent inputEvent;
    while (m_inputEvents.try_pop(inputEvent))
    {
        m_frameInputEvents[m_frameInputEventCount] = inputEvent;
        ++m_frameInputEventCount;
        m_inputState.apply_event(inputEvent);
    }
    const ImGuiIO &io = ImGui::GetIO();
    a_client.input_frame({std::span<const cue::InputEvent>(m_frameInputEvents.data(), m_frameInputEventCount),
                          m_inputState.snapshot(),
                          {io.WantCaptureKeyboard, io.WantCaptureMouse}});
    a_client.render_surfaces_ready(render_surface_views());
    a_client.draw_frame();
    ImGui::Render();

    record_render_surfaces(frame, a_client.render_frame_view());

    const UINT backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource = m_backBuffers[backBufferIndex].Get();
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &toRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(backBufferIndex) * m_rtvIncrement;
    constexpr float k_clearColor[] = {0.055F, 0.065F, 0.085F, 1.0F};
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_commandList->ClearRenderTargetView(rtv, k_clearColor, 0, nullptr);
    ID3D12DescriptorHeap *heaps[] = {m_srvHeap.Get()};
    m_commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &toPresent);
    result = m_commandList->Close();
    if (FAILED(result))
    {
        cue::Error close = make_native_error(*m_assertContext, cue::tool_host::ToolHostError::D3d12InitializationFailed,
                                             "Tool Host command list close failed", "HRESULT", result);
        return finish_after_wait_error(std::move(close));
    }

    ID3D12CommandList *lists[] = {m_commandList.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    result = m_swapChain->Present(1, 0);
    if (FAILED(result))
    {
        cue::Error present = make_native_error(*m_assertContext, cue::tool_host::ToolHostError::PresentFailed,
                                               "Tool Host Present failed", "HRESULT", result);
        if (FAILED(m_device->GetDeviceRemovedReason()))
        {
            cue::Error removed = make_device_removed_error(std::move(present));
            return finish_device_removed(std::move(removed));
        }
        const HRESULT signaled = m_queue->Signal(m_fence.Get(), fenceValue);
        if (SUCCEEDED(signaled))
        {
            frame.reuseFenceValue = fenceValue;
            m_lastSignaledFence = fenceValue;
            cue::Result<void> drained = wait_for_fence(fenceValue);
            if (!drained)
            {
                const bool wasDeviceRemoved = is_device_removed_error(*drained.try_error());
                const bool wasCompletionUnavailable = is_gpu_completion_unavailable_error(*drained.try_error());
                present.append_secondary_diagnostics(*m_assertContext, *drained.try_error(),
                                                     "Present compensation drain also failed", "Drain");
                if (wasDeviceRemoved)
                {
                    cue::Error removed = make_device_removed_error(std::move(present));
                    return finish_device_removed(std::move(removed));
                }
                if (wasCompletionUnavailable)
                {
                    terminate_unproven_completion(*m_assertContext, std::move(present));
                }
                return finish_after_wait_error(std::move(present));
            }
            cleanup();
            return cue::Result<void>::failure(std::move(present));
        }
        cue::Error signal = make_native_error(*m_assertContext, cue::tool_host::ToolHostError::FenceSignalFailed,
                                              "Tool Host Present compensation Signal failed", "HRESULT", signaled);
        present.append_secondary_diagnostics(*m_assertContext, signal, "Present compensation Signal also failed",
                                             "Signal");
        cue::Result<void> uncertain = wait_for_fence(fenceValue);
        if (!uncertain)
        {
            const bool wasDeviceRemoved = is_device_removed_error(*uncertain.try_error());
            const bool wasCompletionUnavailable = is_gpu_completion_unavailable_error(*uncertain.try_error());
            present.append_secondary_diagnostics(*m_assertContext, *uncertain.try_error(),
                                                 "Present compensation completion check also failed", "Wait");
            if (wasDeviceRemoved)
            {
                cue::Error removed = make_device_removed_error(std::move(present));
                return finish_device_removed(std::move(removed));
            }
            if (wasCompletionUnavailable)
            {
                terminate_unproven_completion(*m_assertContext, std::move(present));
            }
        }
        cleanup();
        return cue::Result<void>::failure(std::move(present));
    }

    result = m_queue->Signal(m_fence.Get(), fenceValue);
    if (FAILED(result))
    {
        cue::Error signal = make_native_error(*m_assertContext, cue::tool_host::ToolHostError::FenceSignalFailed,
                                              "Tool Host frame Fence Signal failed", "HRESULT", result);
        cue::Result<void> proven = wait_for_fence(fenceValue);
        if (!proven)
        {
            const bool wasDeviceRemoved = is_device_removed_error(*proven.try_error());
            const bool wasCompletionUnavailable = is_gpu_completion_unavailable_error(*proven.try_error());
            signal.append_secondary_diagnostics(*m_assertContext, *proven.try_error(),
                                                "Failed frame Signal completion check failed", "Wait");
            if (wasDeviceRemoved)
            {
                cue::Error removed = make_device_removed_error(std::move(signal));
                return finish_device_removed(std::move(removed));
            }
            if (wasCompletionUnavailable)
            {
                terminate_unproven_completion(*m_assertContext, std::move(signal));
            }
        }
        cleanup();
        return cue::Result<void>::failure(std::move(signal));
    }
    frame.reuseFenceValue = fenceValue;
    m_lastSignaledFence = fenceValue;
    ++m_frameSequence;
    return cue::Result<void>::success();
}

cue::Result<void> WindowsD3d12ToolHost::resize(cue::WindowSize a_size) noexcept
{
    cue::Result<void> drained = wait_for_fence(m_lastSignaledFence);
    if (!drained)
    {
        return finish_after_wait_error(std::move(*drained.try_error()));
    }
    release_back_buffers();
    const HRESULT result = m_swapChain->ResizeBuffers(k_frameCount, a_size.width, a_size.height, k_backBufferFormat, 0);
    if (FAILED(result))
    {
        cue::Error resizeError =
            make_native_error(*m_assertContext, cue::tool_host::ToolHostError::SwapChainResizeFailed,
                              "Tool Host Swap Chain resize failed", "HRESULT", result);
        if (FAILED(m_device->GetDeviceRemovedReason()))
        {
            cue::Error removed = make_device_removed_error(std::move(resizeError));
            return finish_device_removed(std::move(removed));
        }
        cleanup();
        return cue::Result<void>::failure(std::move(resizeError));
    }
    cue::Result<void> buffers = create_back_buffers();
    if (!buffers)
    {
        if (FAILED(m_device->GetDeviceRemovedReason()))
        {
            cue::Error removed = make_device_removed_error(std::move(*buffers.try_error()));
            return finish_device_removed(std::move(removed));
        }
        cleanup();
        return buffers;
    }
    for (FrameResource &frame : m_frames)
    {
        frame.reuseFenceValue = 0;
    }
    return cue::Result<void>::success();
}

cue::Result<void> WindowsD3d12ToolHost::drain_for_shutdown() noexcept
{
    cue::Result<std::uint64_t> reserved = reserve_fence_value();
    if (!reserved)
    {
        cue::Error exhausted = std::move(*reserved.try_error());
        cue::Result<void> drained = wait_for_fence(m_lastSignaledFence);
        if (!drained)
        {
            const bool wasDeviceRemoved = is_device_removed_error(*drained.try_error());
            const bool wasCompletionUnavailable = is_gpu_completion_unavailable_error(*drained.try_error());
            exhausted.append_secondary_diagnostics(*m_assertContext, *drained.try_error(),
                                                   "Tool Host shutdown Fence exhaustion drain failed", "Drain");
            if (wasDeviceRemoved)
            {
                cue::Error removed = make_device_removed_error(std::move(exhausted));
                return finish_device_removed(std::move(removed));
            }
            if (wasCompletionUnavailable)
            {
                terminate_unproven_completion(*m_assertContext, std::move(exhausted));
            }
            return finish_after_wait_error(std::move(exhausted));
        }
        cleanup();
        return cue::Result<void>::failure(std::move(exhausted));
    }
    const std::uint64_t terminalValue = *reserved.try_value();
    const HRESULT result = m_queue->Signal(m_fence.Get(), terminalValue);
    if (FAILED(result))
    {
        cue::Error signal = make_native_error(*m_assertContext, cue::tool_host::ToolHostError::FenceSignalFailed,
                                              "Tool Host terminal Fence Signal failed", "HRESULT", result);
        return finish_after_wait_error(std::move(signal));
    }
    m_lastSignaledFence = terminalValue;
    cue::Result<void> waited = wait_for_fence(terminalValue);
    if (!waited)
    {
        return finish_after_wait_error(std::move(*waited.try_error()));
    }
    cleanup();
    return cue::Result<void>::success();
}

cue::Result<void> WindowsD3d12ToolHost::run(cue::tool_host::ToolHostClient &a_client) noexcept
{
    a_client.window_ready(*m_window);
    while (true)
    {
        cue::Result<cue::PumpStatus> pumped = m_windowSystem->pump_events();
        if (!pumped)
        {
            return finish_after_wait_error(std::move(*pumped.try_error()));
        }
        if (*pumped.try_value() == cue::PumpStatus::QuitRequested)
        {
            break;
        }

        cue::WindowEvent event{};
        while (m_window->try_pop_event(event))
        {
            if (event.type == cue::WindowEventType::Destroyed)
            {
                return drain_for_shutdown();
            }
            if (event.type == cue::WindowEventType::CloseRequested)
            {
                a_client.request_close();
            }
            if (event.type == cue::WindowEventType::Minimized)
            {
                m_isMinimized = true;
            }
            else if (event.type == cue::WindowEventType::Resized || event.type == cue::WindowEventType::Restored)
            {
                m_isMinimized = false;
                cue::Result<void> resized = resize(event.clientSize);
                if (!resized)
                {
                    return resized;
                }
            }
        }

        if (a_client.should_close())
        {
            return drain_for_shutdown();
        }
        if (m_isMinimized)
        {
            Sleep(16);
            continue;
        }

        cue::Result<void> rendered = render_frame(a_client);
        if (!rendered)
        {
            return rendered;
        }
        if (m_maximumFrameCount != 0 && m_frameSequence >= m_maximumFrameCount)
        {
            return drain_for_shutdown();
        }
    }
    return drain_for_shutdown();
}

void WindowsD3d12ToolHost::cleanup(cue::Error *a_secondaryDiagnostics) noexcept
{
    if (m_isMessageSinkAttached && m_window != nullptr && m_window->state() != cue::WindowState::Destroyed)
    {
        cue::Result<void> detached = cue::detach_windows_message_sink(*m_window, m_messageSink, *m_assertContext);
        if (!detached)
        {
            if (a_secondaryDiagnostics != nullptr)
            {
                a_secondaryDiagnostics->append_secondary_diagnostics(*m_assertContext, *detached.try_error(),
                                                                     "Device Removal message sink detach also failed",
                                                                     "Cleanup");
            }
            else
            {
                static_cast<void>(m_assertContext->logger().log(cue::LogLevel::Warning,
                                                                "Tool Host message sink detach failed during cleanup",
                                                                std::move(*detached.try_error())));
            }
        }
    }
    m_isMessageSinkAttached = false;
    release_all_render_surfaces();
    if (m_isDx12BackendInitialized)
    {
        ImGui_ImplDX12_Shutdown();
        m_isDx12BackendInitialized = false;
    }
    if (m_isWin32BackendInitialized)
    {
        ImGui_ImplWin32_Shutdown();
        m_isWin32BackendInitialized = false;
    }
    if (m_isImGuiContextInitialized)
    {
        ImGui::DestroyContext();
        m_isImGuiContextInitialized = false;
    }

    m_commandList.Reset();
    for (FrameResource &frame : m_frames)
    {
        if (frame.constantBuffer != nullptr && frame.mappedConstants != nullptr)
        {
            frame.constantBuffer->Unmap(0U, nullptr);
        }
        frame.mappedConstants = nullptr;
        frame.constantBuffer.Reset();
        frame.allocator.Reset();
        frame.reuseFenceValue = 0;
    }
    m_sceneIndexBuffer.Reset();
    m_sceneVertexBuffer.Reset();
    m_scenePipelineState.Reset();
    m_sceneRootSignature.Reset();
    m_sceneIndexView = {};
    m_sceneVertexView = {};
    release_back_buffers();
    m_rtvHeap.Reset();
    m_srvHeap.Reset();
    m_swapChain.Reset();
    m_fence.Reset();
    m_queue.Reset();
    m_device.Reset();
    m_factory.Reset();

    if (m_window != nullptr && m_window->state() != cue::WindowState::Destroyed)
    {
        cue::Result<void> destroyed = m_window->destroy();
        if (!destroyed)
        {
            if (m_window->state() != cue::WindowState::Destroyed)
            {
                if (a_secondaryDiagnostics != nullptr)
                {
                    a_secondaryDiagnostics->append_secondary_diagnostics(
                        *m_assertContext, *destroyed.try_error(), "Device Removal window destruction also failed",
                        "Cleanup");
                    terminate_window_destruction_failure(*m_assertContext, std::move(*a_secondaryDiagnostics));
                }
                terminate_window_destruction_failure(*m_assertContext, std::move(*destroyed.try_error()));
            }
            if (a_secondaryDiagnostics != nullptr)
            {
                a_secondaryDiagnostics->append_secondary_diagnostics(*m_assertContext, *destroyed.try_error(),
                                                                     "Device Removal window class cleanup also failed",
                                                                     "Cleanup");
            }
            else
            {
                static_cast<void>(m_assertContext->logger().log(
                    cue::LogLevel::Warning, "Tool Host window class cleanup failed after native window destruction",
                    std::move(*destroyed.try_error())));
            }
        }
    }
    m_window.reset();
    m_windowSystem.reset();
}
} // namespace

namespace cue::tool_host
{
Result<void> run_windows_d3d12_tool_host(const ToolHostDescriptor &a_descriptor, ToolHostClient &a_client,
                                         const AssertContext &a_assertContext) noexcept
{
    WindowsD3d12ToolHost host(a_assertContext);
    Result<void> initialized = host.initialize(a_descriptor);
    if (!initialized)
    {
        return initialized;
    }
    return host.run(a_client);
}
} // namespace cue::tool_host
