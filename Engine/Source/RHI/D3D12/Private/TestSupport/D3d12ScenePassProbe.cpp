#include <Cue/RHI/D3D12/TestSupport/D3d12ScenePassProbe.h>

#include "D3d12ScenePass.h"

#include <Cue/Foundation/Assert.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace
{
struct EventOwner final
{
    HANDLE handle = nullptr;

    /// @brief WARP Probe用Fence Eventを所有Scopeの終端で閉じる
    ~EventOwner() noexcept
    {
        if (handle != nullptr)
        {
            static_cast<void>(CloseHandle(handle));
        }
    }
};

/// @brief GPUへ投入済みResourceの完了を証明できない場合に早期解放を防ぐ
[[noreturn]] void stop_without_gpu_completion() noexcept
{
    std::abort();
}
} // namespace

namespace cue
{
D3d12SceneProbeResult verify_d3d12_scene_pixel_for_probe(const AssertContext &a_assertContext,
                                                         D3d12ScenePixelCase a_case, D3d12SceneProbeAdapter a_adapter,
                                                         std::uint32_t a_surfaceSize) noexcept
{
    if (a_surfaceSize < 32U || a_surfaceSize > 256U)
    {
        return D3d12SceneProbeResult::Failed;
    }
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()))))
    {
        return D3d12SceneProbeResult::Failed;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (a_adapter == D3d12SceneProbeAdapter::Warp)
    {
        if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(adapter.GetAddressOf()))) ||
            FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()))))
        {
            return D3d12SceneProbeResult::Failed;
        }
    }
    else
    {
        for (UINT index = 0U;; ++index)
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumerateResult = factory->EnumAdapters1(index, candidate.GetAddressOf());
            if (enumerateResult == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(enumerateResult))
            {
                return D3d12SceneProbeResult::Failed;
            }
            DXGI_ADAPTER_DESC1 description = {};
            if (FAILED(candidate->GetDesc1(&description)))
            {
                return D3d12SceneProbeResult::Failed;
            }
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0U &&
                SUCCEEDED(
                    D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.GetAddressOf()))))
            {
                adapter = candidate;
                break;
            }
        }
        if (!device)
        {
            return D3d12SceneProbeResult::HardwareUnavailable;
        }
    }
    D3D12_COMMAND_QUEUE_DESC queueDescriptor = {};
    queueDescriptor.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&queueDescriptor, IID_PPV_ARGS(queue.GetAddressOf()))))
    {
        return D3d12SceneProbeResult::Failed;
    }
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()))))
    {
        return D3d12SceneProbeResult::Failed;
    }
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    if (FAILED(device->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(commandList.GetAddressOf()))))
    {
        return D3d12SceneProbeResult::Failed;
    }

    D3D12_RESOURCE_DESC textureDescriptor = {};
    textureDescriptor.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDescriptor.Width = a_surfaceSize;
    textureDescriptor.Height = a_surfaceSize;
    textureDescriptor.DepthOrArraySize = 1U;
    textureDescriptor.MipLevels = 1U;
    textureDescriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDescriptor.SampleDesc.Count = 1U;
    textureDescriptor.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES renderHeap = {};
    renderHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> renderTarget;
    if (FAILED(device->CreateCommittedResource(&renderHeap, D3D12_HEAP_FLAG_NONE, &textureDescriptor,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                               IID_PPV_ARGS(renderTarget.GetAddressOf()))))
    {
        return D3d12SceneProbeResult::Failed;
    }
    D3D12_DESCRIPTOR_HEAP_DESC rtvDescriptor = {};
    rtvDescriptor.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDescriptor.NumDescriptors = 1U;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (FAILED(device->CreateDescriptorHeap(&rtvDescriptor, IID_PPV_ARGS(rtvHeap.GetAddressOf()))))
    {
        return D3d12SceneProbeResult::Failed;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(renderTarget.Get(), nullptr, rtv);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rowCount = 0U;
    UINT64 rowSize = 0U;
    UINT64 readbackSize = 0U;
    device->GetCopyableFootprints(&textureDescriptor, 0U, 1U, 0U, &footprint, &rowCount, &rowSize, &readbackSize);
    D3D12_RESOURCE_DESC readbackDescriptor = {};
    readbackDescriptor.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDescriptor.Width = readbackSize;
    readbackDescriptor.Height = 1U;
    readbackDescriptor.DepthOrArraySize = 1U;
    readbackDescriptor.MipLevels = 1U;
    readbackDescriptor.SampleDesc.Count = 1U;
    readbackDescriptor.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescriptor,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(readback.GetAddressOf()))))
    {
        return D3d12SceneProbeResult::Failed;
    }
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))))
    {
        return D3d12SceneProbeResult::Failed;
    }
    EventOwner event;
    event.handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event.handle == nullptr)
    {
        return D3d12SceneProbeResult::Failed;
    }

    D3d12ScenePass pass;
    if (!pass.initialize(device.Get(), textureDescriptor.Format, a_surfaceSize, a_surfaceSize, a_assertContext))
    {
        return D3d12SceneProbeResult::Failed;
    }
    constexpr std::array<float, 16> identity = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                                0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    constexpr std::array<float, 16> rotationY90 = {0.0F, 0.0F, -1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                                   1.0F, 0.0F, 0.0F,  0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    std::array<PresentationSceneCube, 2> cubes = {PresentationSceneCube{identity}, PresentationSceneCube{rotationY90}};
    if (a_case != D3d12ScenePixelCase::BackFaceCull)
    {
        cubes[0].localToWorld[14] = 0.75F;
    }
    cubes[1].localToWorld[14] = 0.95F;
    if (a_case == D3d12ScenePixelCase::DepthReverseOrder)
    {
        const PresentationSceneCube nearCube = cubes[0];
        cubes[0] = cubes[1];
        cubes[1] = nearCube;
    }
    const std::size_t cubeCount =
        a_case == D3d12ScenePixelCase::Depth || a_case == D3d12ScenePixelCase::DepthReverseOrder ? 2U : 1U;
    const PresentationSceneCube *firstCube =
        a_case == D3d12ScenePixelCase::DepthFarReference ? cubes.data() + 1U : cubes.data();
    const PresentationSceneFrameDescriptor scene = {
        {0.0F, 0.0F, 0.0F, 1.0F}, identity, std::span<const PresentationSceneCube>(firstCube, cubeCount)};
    commandList->ClearRenderTargetView(rtv, scene.clearColor.data(), 0U, nullptr);
    pass.record(commandList.Get(), rtv, 0U, a_surfaceSize, a_surfaceSize, scene);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = renderTarget.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->ResourceBarrier(1U, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = renderTarget.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    commandList->CopyTextureRegion(&destination, 0U, 0U, 0U, &source, nullptr);
    if (FAILED(commandList->Close()))
    {
        return D3d12SceneProbeResult::Failed;
    }
    ID3D12CommandList *lists[] = {commandList.Get()};
    queue->ExecuteCommandLists(1U, lists);
    if (FAILED(queue->Signal(fence.Get(), 1U)) || FAILED(fence->SetEventOnCompletion(1U, event.handle)) ||
        WaitForSingleObject(event.handle, 10'000U) != WAIT_OBJECT_0)
    {
        stop_without_gpu_completion();
    }
    const std::size_t pixelOffset =
        footprint.Offset + (a_surfaceSize / 2U) * footprint.Footprint.RowPitch + (a_surfaceSize / 2U) * 4U;
    const std::size_t cornerOffset = footprint.Offset + 2U * footprint.Footprint.RowPitch + 2U * 4U;
    const D3D12_RANGE readRange = {cornerOffset, pixelOffset + 4U};
    void *mapped = nullptr;
    if (FAILED(readback->Map(0U, &readRange, &mapped)))
    {
        return D3d12SceneProbeResult::Failed;
    }
    const auto *pixels = static_cast<const std::uint8_t *>(mapped);
    const auto *pixel = pixels + pixelOffset;
    const auto *corner = pixels + cornerOffset;
    const bool hasDrawnColor = (pixel[0] > 30U || pixel[1] > 30U || pixel[2] > 30U) && pixel[3] == 255U;
    const bool hasClearCorner = corner[0] == 0U && corner[1] == 0U && corner[2] == 0U && corner[3] == 255U;
    const bool farCubeIsVisible = pixel[0] > 200U && pixel[2] < 100U && pixel[3] == 255U;
    const bool nearCubeWinsDepth = pixel[2] > 200U && pixel[0] < 100U && pixel[3] == 255U;
    const bool insideCubeIsCulled = pixel[0] == 0U && pixel[1] == 0U && pixel[2] == 0U && pixel[3] == 255U;
    const D3D12_RANGE writtenRange = {0U, 0U};
    readback->Unmap(0U, &writtenRange);
    pass.release();
    const bool accepted =
        hasClearCorner && (a_case == D3d12ScenePixelCase::Basic               ? hasDrawnColor
                           : a_case == D3d12ScenePixelCase::DepthFarReference ? farCubeIsVisible
                           : a_case == D3d12ScenePixelCase::Depth || a_case == D3d12ScenePixelCase::DepthReverseOrder
                               ? nearCubeWinsDepth
                               : insideCubeIsCulled);
    return accepted ? D3d12SceneProbeResult::Passed : D3d12SceneProbeResult::Failed;
}
} // namespace cue
