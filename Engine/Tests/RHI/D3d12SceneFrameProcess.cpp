#include "TestSupport/RhiProcessTestFixture.h"

#include <Cue/RHI/D3D12/TestSupport/D3d12ScenePassProbe.h>

/// @brief 固定Cube PassのWARP画素をProcess分離下で検証する
int main()
{
    cue::test::RhiProcessTestFixture fixture;
    return cue::verify_d3d12_scene_pixel_for_probe(fixture.assert_context()) ? 0 : 1;
}
