# CueEngine Rebuild の正式なビルド入口を定義する
cmake_minimum_required(VERSION 4.2.0)

project(CueEngine LANGUAGES C CXX)

include(CTest)

if(CMAKE_CONFIGURATION_TYPES)
    set(
        CMAKE_CONFIGURATION_TYPES
        "Debug;Development;Release"
        CACHE STRING
        "Supported CueEngine build configurations"
        FORCE
    )
endif()

add_subdirectory(Engine/Source/Foundation)
add_subdirectory(Engine/Source/GameModule)
add_subdirectory(Engine/Source/Build)
add_subdirectory(Engine/Source/Schema)
add_subdirectory(Engine/Source/GameCore)
add_subdirectory(Engine/Source/IO)
add_subdirectory(Engine/Source/Project)
add_subdirectory(Engine/Source/ProjectFiles)
add_subdirectory(Engine/Source/ImGui)
add_subdirectory(Engine/Source/ProjectHub)
add_subdirectory(Engine/Source/Math)
add_subdirectory(Engine/Source/EngineAssets)
add_subdirectory(Engine/Source/Scene)
add_subdirectory(Engine/Source/Package)
add_subdirectory(Engine/Source/Distribution)
add_subdirectory(Engine/Source/Platform)
add_subdirectory(Engine/Source/Input)
add_subdirectory(Engine/Source/Runtime)
add_subdirectory(Engine/Source/Renderer)
add_subdirectory(Engine/Source/EditorCore)
add_subdirectory(Engine/Source/Editor/ImGui)
add_subdirectory(Engine/Source/RHI)

if(WIN32)
    add_subdirectory(Engine/Source/Distribution/Windows)
    add_subdirectory(Engine/Source/DistributionTools)
    add_subdirectory(Engine/Source/Build/Windows)
    add_subdirectory(Engine/Source/Editor/Windows)
    add_subdirectory(Engine/Source/ToolHost/WindowsD3D12)
    add_subdirectory(Engine/Source/EditorTool)
    add_subdirectory(Engine/Source/ProjectHubTool)
    add_subdirectory(Engine/Source/RuntimeHost)
endif()

add_executable(CueBuildProbe)

target_sources(
    CueBuildProbe
    PRIVATE
        Engine/Source/CueBuildProbe/Main.cpp
)

target_compile_features(CueBuildProbe PRIVATE cxx_std_20)

set_target_properties(
    CueBuildProbe
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(MSVC)
    target_compile_options(
        CueBuildProbe
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )

    target_link_options(
        CueBuildProbe
        PRIVATE
            $<$<CONFIG:Development>:/DEBUG>
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(Tools/GameCoreBenchmark)
    add_subdirectory(Engine/Tests/Foundation)
    add_subdirectory(Engine/Tests/Build)
    add_subdirectory(Engine/Tests/GameModule)
    add_subdirectory(Engine/Tests/Schema)
    add_subdirectory(Engine/Tests/GameCore)
    if(WIN32)
        add_subdirectory(Engine/Tests/IO)
    endif()
    add_subdirectory(Engine/Tests/Project)
    add_subdirectory(Engine/Tests/ProjectFiles)
    add_subdirectory(Engine/Tests/ImGui)
    add_subdirectory(Engine/Tests/ProjectHub)
    add_subdirectory(Engine/Tests/Math)
    add_subdirectory(Engine/Tests/EngineAssets)
    add_subdirectory(Engine/Tests/Scene)
    add_subdirectory(Engine/Tests/Package)
    add_subdirectory(Engine/Tests/Distribution)
    add_subdirectory(Engine/Tests/Runtime)
    add_subdirectory(Engine/Tests/Renderer)
    add_subdirectory(Engine/Tests/EditorCore)
    add_subdirectory(Engine/Tests/Editor)
    add_subdirectory(Engine/Tests/Platform)
    add_subdirectory(Engine/Tests/Input)
    add_subdirectory(Engine/Tests/RHI)

    if(WIN32)
        add_subdirectory(Engine/Tests/EditorWorkflow)
        add_subdirectory(Engine/Tests/ToolHost)
        add_subdirectory(Engine/Tests/RuntimeHost)
    endif()

    add_test(NAME CueBuildProbe.Smoke COMMAND CueBuildProbe)

    set_tests_properties(
        CueBuildProbe.Smoke
        PROPERTIES
            LABELS Smoke
            TIMEOUT 10
    )
endif()
