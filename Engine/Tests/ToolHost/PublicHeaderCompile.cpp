#include <Cue/ToolHost/WindowsD3D12/ToolHost.h>

/// @brief Tool Host Public HeaderがPortable Input型を公開依存だけで使用できることを検証する
int main()
{
    cue::FrameInputSnapshot snapshot;
    const cue::tool_host::ToolHostInputFrameView input{{}, snapshot, {}};
    return input.events.empty() && input.snapshot.has_focus() ? 0 : 1;
}
