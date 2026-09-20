add_library(Cue.ToolHost.WindowsD3D12 STATIC)

target_sources(
    Cue.ToolHost.WindowsD3D12
    PRIVATE
        Private/ToolHost.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/ToolHost/WindowsD3D12/ToolHost.h
)

target_link_libraries(
    Cue.ToolHost.WindowsD3D12
    PUBLIC
        Cue.Foundation
        Cue.Input
        Cue.Platform
        Cue.Renderer
    PRIVATE
        Cue.ImGui.Core
        Cue.Input.Windows
        Cue.Platform.Windows
        d3d12
        d3dcompiler
        dxgi
        dxguid
)

get_target_property(cueToolHostWindowsD3d12Links Cue.ToolHost.WindowsD3D12 LINK_LIBRARIES)
set(
    cueToolHostWindowsD3d12ExpectedLinks
    Cue.Foundation
    Cue.Input
    Cue.Platform
    Cue.Renderer
    Cue.ImGui.Core
    Cue.Input.Windows
    Cue.Platform.Windows
    d3d12
    d3dcompiler
    dxgi
    dxguid
)
if(NOT "${cueToolHostWindowsD3d12Links}" STREQUAL "${cueToolHostWindowsD3d12ExpectedLinks}")
    message(
        FATAL_ERROR
        "Cue.ToolHost.WindowsD3D12 direct dependencies changed: ${cueToolHostWindowsD3d12Links}"
    )
endif()

get_target_property(cueToolHostWindowsD3d12InterfaceLinks Cue.ToolHost.WindowsD3D12 INTERFACE_LINK_LIBRARIES)
set(
    cueToolHostWindowsD3d12ExpectedInterfaceLinks
    Cue.Foundation
    Cue.Input
    Cue.Platform
    Cue.Renderer
    "$<LINK_ONLY:Cue.ImGui.Core>"
    "$<LINK_ONLY:Cue.Input.Windows>"
    "$<LINK_ONLY:Cue.Platform.Windows>"
    "$<LINK_ONLY:d3d12>"
    "$<LINK_ONLY:d3dcompiler>"
    "$<LINK_ONLY:dxgi>"
    "$<LINK_ONLY:dxguid>"
)
if(NOT "${cueToolHostWindowsD3d12InterfaceLinks}" STREQUAL "${cueToolHostWindowsD3d12ExpectedInterfaceLinks}")
    message(
        FATAL_ERROR
        "Cue.ToolHost.WindowsD3D12 public dependencies changed: ${cueToolHostWindowsD3d12InterfaceLinks}"
    )
endif()
target_compile_features(Cue.ToolHost.WindowsD3D12 PUBLIC cxx_std_20)
target_include_directories(Cue.ToolHost.WindowsD3D12 PUBLIC Public)
target_compile_definitions(Cue.ToolHost.WindowsD3D12 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(
    Cue.ToolHost.WindowsD3D12
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.ToolHost.WindowsD3D12
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.ToolHost.WindowsD3D12 PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
