#include "D3d12ScenePass.h"

#include "D3d12Error.h"

#include <Cue/EngineAssets/BuiltInMesh.h>

#include <d3dcompiler.h>

#include <array>
#include <cstring>
#include <limits>
#include <utility>

#include "ScenePixelShader.h"
#include "SceneVertexShader.h"

namespace
{
constexpr std::int64_t k_scenePassAlreadyInitialized = 300;
constexpr std::int64_t k_sceneRootSignatureSerializationFailed = 301;
constexpr std::int64_t k_sceneRootSignatureCreationFailed = 302;
constexpr std::int64_t k_scenePipelineCreationFailed = 303;
constexpr std::int64_t k_sceneMeshContractInvalid = 304;
constexpr std::int64_t k_sceneBufferCreationFailed = 305;
constexpr std::int64_t k_sceneBufferMapFailed = 306;
constexpr std::uint32_t k_sceneConstantAlignment = 256U;

struct SceneVertex final
{
    float position[3];
    float normal[3];
};

struct SceneConstants final
{
    float viewProjection[16];
    float localToWorld[16];
};

static_assert(sizeof(SceneConstants) == 128U);
static_assert(sizeof(SceneVertex) == 24U);

/// @brief GPUが直接Readできる固定Scene BufferをUpload Heap上へ生成する
[[nodiscard]] cue::Result<Microsoft::WRL::ComPtr<ID3D12Resource>> create_upload_buffer(
    ID3D12Device *a_device, std::uint64_t a_size, const cue::AssertContext &a_assertContext) noexcept
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC descriptor = {};
    descriptor.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    descriptor.Width = a_size;
    descriptor.Height = 1U;
    descriptor.DepthOrArraySize = 1U;
    descriptor.MipLevels = 1U;
    descriptor.Format = DXGI_FORMAT_UNKNOWN;
    descriptor.SampleDesc.Count = 1U;
    descriptor.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    const HRESULT result =
        a_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &descriptor, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(resource.GetAddressOf()));
    if (FAILED(result))
    {
        return cue::Result<Microsoft::WRL::ComPtr<ID3D12Resource>>::failure(cue::d3d12_private::make_native_error(
            a_assertContext, k_sceneBufferCreationFailed, "D3D12 Scene Buffer creation failed", result));
    }
    return cue::Result<Microsoft::WRL::ComPtr<ID3D12Resource>>::success(std::move(resource));
}

/// @brief 静的Cube DataをGPU Read用Bufferへ一度だけ書き込む
[[nodiscard]] cue::Result<void> write_upload_buffer(ID3D12Resource *a_resource, const void *a_data, std::size_t a_size,
                                                    const cue::AssertContext &a_assertContext) noexcept
{
    void *mapped = nullptr;
    const HRESULT result = a_resource->Map(0U, nullptr, &mapped);
    if (FAILED(result))
    {
        return cue::Result<void>::failure(cue::d3d12_private::make_native_error(
            a_assertContext, k_sceneBufferMapFailed, "D3D12 Scene Buffer Map failed", result));
    }
    std::memcpy(mapped, a_data, a_size);
    a_resource->Unmap(0U, nullptr);
    return cue::Result<void>::success();
}
} // namespace

namespace cue
{
D3d12ScenePass::~D3d12ScenePass() noexcept
{
    release();
}

Result<void> D3d12ScenePass::initialize(ID3D12Device *a_device, DXGI_FORMAT a_format,
                                        const AssertContext &a_assertContext) noexcept
{
    if (has_native_objects())
    {
        return Result<void>::failure(d3d12_private::make_error(a_assertContext, k_scenePassAlreadyInitialized,
                                                               "D3D12 Scene Pass is already initialized"));
    }
    Result<void> result = create_resources(a_device, a_format, a_assertContext);
    if (!result)
    {
        release();
    }
    return result;
}

Result<void> D3d12ScenePass::create_resources(ID3D12Device *a_device, DXGI_FORMAT a_format,
                                              const AssertContext &a_assertContext) noexcept
{
    D3D12_ROOT_PARAMETER rootParameter = {};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.Descriptor.ShaderRegister = 0U;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rootDescriptor = {};
    rootDescriptor.NumParameters = 1U;
    rootDescriptor.pParameters = &rootParameter;
    rootDescriptor.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> diagnostics;
    HRESULT result = D3D12SerializeRootSignature(&rootDescriptor, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 signature.GetAddressOf(), diagnostics.GetAddressOf());
    if (FAILED(result))
    {
        return Result<void>::failure(
            d3d12_private::make_native_error(a_assertContext, k_sceneRootSignatureSerializationFailed,
                                             "D3D12 Scene Root Signature serialization failed", result));
    }
    result = a_device->CreateRootSignature(0U, signature->GetBufferPointer(), signature->GetBufferSize(),
                                           IID_PPV_ARGS(m_rootSignature.GetAddressOf()));
    if (FAILED(result))
    {
        return Result<void>::failure(d3d12_private::make_native_error(
            a_assertContext, k_sceneRootSignatureCreationFailed, "D3D12 Scene Root Signature creation failed", result));
    }

    const D3D12_INPUT_ELEMENT_DESC inputElements[2] = {
        {"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
        {"NORMAL", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 12U, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0U},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = m_rootSignature.Get();
    pipeline.VS = {g_cueSceneVertexShader, sizeof(g_cueSceneVertexShader)};
    pipeline.PS = {g_cueScenePixelShader, sizeof(g_cueScenePixelShader)};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask = (std::numeric_limits<UINT>::max)();
    pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable = TRUE;
    pipeline.DepthStencilState.DepthEnable = FALSE;
    pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipeline.InputLayout = {inputElements, 2U};
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1U;
    pipeline.RTVFormats[0] = a_format;
    pipeline.SampleDesc.Count = 1U;
    result = a_device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(m_pipeline.GetAddressOf()));
    if (FAILED(result))
    {
        return Result<void>::failure(d3d12_private::make_native_error(a_assertContext, k_scenePipelineCreationFailed,
                                                                      "D3D12 Scene Pipeline creation failed", result));
    }

    const engine_assets::MeshView cube = engine_assets::built_in_cube_mesh();
    if (cube.assetId != engine_assets::k_cubeMeshAssetId || cube.revision != engine_assets::k_cubeMeshRevision ||
        cube.vertices.size() != 24U || cube.indices.size() != 36U)
    {
        return Result<void>::failure(d3d12_private::make_error(a_assertContext, k_sceneMeshContractInvalid,
                                                               "Built-in Cube geometry contract is invalid"));
    }
    std::array<SceneVertex, 24> vertices = {};
    for (std::size_t index = 0U; index < vertices.size(); ++index)
    {
        const engine_assets::MeshVertex &source = cube.vertices[index];
        vertices[index] = {{source.position.x, source.position.y, source.position.z},
                           {source.normal.x, source.normal.y, source.normal.z}};
    }
    Result<Microsoft::WRL::ComPtr<ID3D12Resource>> vertexBuffer =
        create_upload_buffer(a_device, sizeof(vertices), a_assertContext);
    if (!vertexBuffer)
    {
        return Result<void>::failure(std::move(*vertexBuffer.try_error()));
    }
    m_vertices = std::move(*vertexBuffer.try_value());
    Result<void> vertexWrite =
        write_upload_buffer(m_vertices.Get(), vertices.data(), sizeof(vertices), a_assertContext);
    if (!vertexWrite)
    {
        return vertexWrite;
    }

    Result<Microsoft::WRL::ComPtr<ID3D12Resource>> indexBuffer =
        create_upload_buffer(a_device, cube.indices.size_bytes(), a_assertContext);
    if (!indexBuffer)
    {
        return Result<void>::failure(std::move(*indexBuffer.try_error()));
    }
    m_indices = std::move(*indexBuffer.try_value());
    Result<void> indexWrite =
        write_upload_buffer(m_indices.Get(), cube.indices.data(), cube.indices.size_bytes(), a_assertContext);
    if (!indexWrite)
    {
        return indexWrite;
    }

    for (std::uint32_t index = 0U; index < k_d3d12FrameContextCount; ++index)
    {
        Result<Microsoft::WRL::ComPtr<ID3D12Resource>> constantBuffer = create_upload_buffer(
            a_device, static_cast<std::uint64_t>(k_presentationSceneMaxCubeCount) * k_sceneConstantAlignment,
            a_assertContext);
        if (!constantBuffer)
        {
            return Result<void>::failure(std::move(*constantBuffer.try_error()));
        }
        m_constants[index] = std::move(*constantBuffer.try_value());
        void *mapped = nullptr;
        result = m_constants[index]->Map(0U, nullptr, &mapped);
        if (FAILED(result))
        {
            return Result<void>::failure(d3d12_private::make_native_error(a_assertContext, k_sceneBufferMapFailed,
                                                                          "D3D12 Scene Constant Map failed", result));
        }
        m_mappedConstants[index] = static_cast<std::byte *>(mapped);
    }
    return Result<void>::success();
}

void D3d12ScenePass::release() noexcept
{
    for (std::uint32_t index = 0U; index < k_d3d12FrameContextCount; ++index)
    {
        if (m_mappedConstants[index] != nullptr)
        {
            m_constants[index]->Unmap(0U, nullptr);
            m_mappedConstants[index] = nullptr;
        }
        m_constants[index].Reset();
    }
    m_indices.Reset();
    m_vertices.Reset();
    m_pipeline.Reset();
    m_rootSignature.Reset();
}

bool D3d12ScenePass::has_native_objects() const noexcept
{
    if (m_rootSignature || m_pipeline || m_vertices || m_indices)
    {
        return true;
    }
    for (const Microsoft::WRL::ComPtr<ID3D12Resource> &constant : m_constants)
    {
        if (constant)
        {
            return true;
        }
    }
    return false;
}

void D3d12ScenePass::record(ID3D12GraphicsCommandList *a_commandList, D3D12_CPU_DESCRIPTOR_HANDLE a_rtv,
                            std::uint32_t a_frameIndex, std::uint32_t a_width, std::uint32_t a_height,
                            const PresentationSceneFrameDescriptor &a_descriptor) noexcept
{
    const D3D12_VIEWPORT viewport = {0.0F, 0.0F, static_cast<float>(a_width), static_cast<float>(a_height), 0.0F, 1.0F};
    const D3D12_RECT scissor = {0, 0, static_cast<LONG>(a_width), static_cast<LONG>(a_height)};
    const D3D12_VERTEX_BUFFER_VIEW vertexView = {m_vertices->GetGPUVirtualAddress(),
                                                 static_cast<UINT>(sizeof(SceneVertex) * 24U),
                                                 static_cast<UINT>(sizeof(SceneVertex))};
    const D3D12_INDEX_BUFFER_VIEW indexView = {m_indices->GetGPUVirtualAddress(),
                                               static_cast<UINT>(sizeof(std::uint16_t) * 36U), DXGI_FORMAT_R16_UINT};
    a_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
    a_commandList->SetPipelineState(m_pipeline.Get());
    a_commandList->RSSetViewports(1U, &viewport);
    a_commandList->RSSetScissorRects(1U, &scissor);
    a_commandList->OMSetRenderTargets(1U, &a_rtv, FALSE, nullptr);
    a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    a_commandList->IASetVertexBuffers(0U, 1U, &vertexView);
    a_commandList->IASetIndexBuffer(&indexView);
    for (std::size_t index = 0U; index < a_descriptor.cubes.size(); ++index)
    {
        SceneConstants constants = {};
        std::memcpy(constants.viewProjection, a_descriptor.viewProjection.data(), sizeof(constants.viewProjection));
        std::memcpy(constants.localToWorld, a_descriptor.cubes[index].localToWorld.data(),
                    sizeof(constants.localToWorld));
        const std::size_t offset = index * k_sceneConstantAlignment;
        std::memcpy(m_mappedConstants[a_frameIndex] + offset, &constants, sizeof(constants));
        a_commandList->SetGraphicsRootConstantBufferView(0U,
                                                         m_constants[a_frameIndex]->GetGPUVirtualAddress() + offset);
        a_commandList->DrawIndexedInstanced(36U, 1U, 0U, 0, 0U);
    }
}
} // namespace cue
