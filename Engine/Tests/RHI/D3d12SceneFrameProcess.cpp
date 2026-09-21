#include "TestSupport/RhiProcessTestFixture.h"

#include <Cue/RHI/D3D12/TestSupport/D3d12ScenePassProbe.h>

#include <string_view>

/// @brief 固定Cube Passの色、Depth、裏面Cull画素をGPU種別ごとにProcess分離下で検証する
int main(int a_argc, char **a_argv)
{
    if (a_argc != 3)
    {
        return 2;
    }
    const std::string_view pixelCaseName = a_argv[1];
    const std::string_view adapterName = a_argv[2];
    cue::D3d12ScenePixelCase pixelCase = cue::D3d12ScenePixelCase::Basic;
    if (pixelCaseName == "Depth")
    {
        pixelCase = cue::D3d12ScenePixelCase::Depth;
    }
    else if (pixelCaseName == "Cull")
    {
        pixelCase = cue::D3d12ScenePixelCase::BackFaceCull;
    }
    else if (pixelCaseName != "Basic")
    {
        return 2;
    }
    if (adapterName != "Warp" && adapterName != "Hardware")
    {
        return 2;
    }
    const cue::D3d12SceneProbeAdapter adapter =
        adapterName == "Warp" ? cue::D3d12SceneProbeAdapter::Warp : cue::D3d12SceneProbeAdapter::Hardware;
    cue::test::RhiProcessTestFixture fixture;
    if (pixelCase == cue::D3d12ScenePixelCase::Depth)
    {
        const cue::D3d12SceneProbeResult farReference = cue::verify_d3d12_scene_pixel_for_probe(
            fixture.assert_context(), cue::D3d12ScenePixelCase::DepthFarReference, adapter);
        if (farReference != cue::D3d12SceneProbeResult::Passed)
        {
            return farReference == cue::D3d12SceneProbeResult::HardwareUnavailable ? 77 : 1;
        }
        const cue::D3d12SceneProbeResult reverseOrder = cue::verify_d3d12_scene_pixel_for_probe(
            fixture.assert_context(), cue::D3d12ScenePixelCase::DepthReverseOrder, adapter);
        if (reverseOrder != cue::D3d12SceneProbeResult::Passed)
        {
            return reverseOrder == cue::D3d12SceneProbeResult::HardwareUnavailable ? 77 : 1;
        }
    }
    const cue::D3d12SceneProbeResult result =
        cue::verify_d3d12_scene_pixel_for_probe(fixture.assert_context(), pixelCase, adapter);
    return result == cue::D3d12SceneProbeResult::Passed                ? 0
           : result == cue::D3d12SceneProbeResult::HardwareUnavailable ? 77
                                                                       : 1;
}
