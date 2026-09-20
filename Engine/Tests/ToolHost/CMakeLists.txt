add_executable(CueToolHostSmokeTests ToolHostSmokeTests.cpp)
target_link_libraries(
    CueToolHostSmokeTests
    PRIVATE
        Cue.Foundation
        Cue.ImGui.Core
        Cue.Platform.Windows
        Cue.ToolHost.WindowsD3D12
)
target_compile_features(CueToolHostSmokeTests PRIVATE cxx_std_20)
target_compile_definitions(CueToolHostSmokeTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(
    CueToolHostSmokeTests
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(MSVC)
    target_compile_options(
        CueToolHostSmokeTests
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(CueToolHostSmokeTests PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

add_test(NAME Cue.ToolHost.WindowsD3D12.Smoke COMMAND CueToolHostSmokeTests)
add_test(NAME Cue.ToolHost.WindowsD3D12.WarpSmoke COMMAND CueToolHostSmokeTests --warp)
set_tests_properties(
    Cue.ToolHost.WindowsD3D12.Smoke
    Cue.ToolHost.WindowsD3D12.WarpSmoke
    PROPERTIES
        LABELS "ToolHost;Smoke"
        TIMEOUT 30
        RUN_SERIAL TRUE
)

add_executable(CueToolHostPublicHeaderCompile PublicHeaderCompile.cpp)
target_link_libraries(CueToolHostPublicHeaderCompile PRIVATE Cue.ToolHost.WindowsD3D12)
target_compile_features(CueToolHostPublicHeaderCompile PRIVATE cxx_std_20)

set_target_properties(
    CueToolHostPublicHeaderCompile
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(MSVC)
    target_compile_options(
        CueToolHostPublicHeaderCompile
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(CueToolHostPublicHeaderCompile PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

add_test(NAME Cue.ToolHost.WindowsD3D12.PublicHeader COMMAND CueToolHostPublicHeaderCompile)
set_tests_properties(
    Cue.ToolHost.WindowsD3D12.PublicHeader
    PROPERTIES
        LABELS "ToolHost;PublicHeader"
        TIMEOUT 30
)
