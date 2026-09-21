function(cue_configure_runtime_host_library targetName)
    target_compile_features(${targetName} PUBLIC cxx_std_20)
    target_compile_definitions(
        ${targetName}
        PRIVATE
            $<$<CONFIG:Debug>:CUE_RUNTIME_BUILD_CONFIGURATION=1>
            $<$<CONFIG:Development>:CUE_RUNTIME_BUILD_CONFIGURATION=2>
            $<$<CONFIG:Release>:CUE_RUNTIME_BUILD_CONFIGURATION=3>
    )
    set_target_properties(
        ${targetName}
        PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
            CXX_EXTENSIONS OFF
    )
    if(MSVC)
        target_compile_options(
            ${targetName}
            PRIVATE
                /W4
                /WX
                /permissive-
                /Zc:__cplusplus
                /utf-8
                $<$<CONFIG:Development>:/O2>
                $<$<CONFIG:Development>:/Zi>
        )
        target_link_options(${targetName} PRIVATE $<$<CONFIG:Development>:/DEBUG>)
    endif()
endfunction()

add_library(Cue.RuntimeHost.Core STATIC)
target_sources(
    Cue.RuntimeHost.Core
    PRIVATE
        GameModuleQueryProvider.cpp
        RuntimeHostApplication.cpp
        RuntimeHostProcess.cpp
        RuntimeSceneFrame.cpp
        RuntimeSceneFrame.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/RuntimeHost/GameModuleQueryProvider.h
            Public/Cue/RuntimeHost/RuntimeHostApplication.h
            Public/Cue/RuntimeHost/RuntimeHostProcess.h
            Public/Cue/RuntimeHost/RuntimeHostStartup.h
)
target_link_libraries(
    Cue.RuntimeHost.Core
    PUBLIC
        Cue.Foundation
        Cue.GameCore
        Cue.GameModule.Abi
        Cue.Input.Windows
        Cue.Platform.Windows
        Cue.RHI.D3D12.Windows
        Cue.Renderer
        Cue.Runtime
        Cue.Scene
        Cue.Schema
)
target_include_directories(Cue.RuntimeHost.Core PUBLIC Public)
cue_configure_runtime_host_library(Cue.RuntimeHost.Core)

add_library(Cue.RuntimeHost.Static STATIC)
target_sources(
    Cue.RuntimeHost.Static
    PRIVATE
        RuntimePackage.cpp
        RuntimePackage.h
        RuntimePath.h
        StaticGameModuleQueryProvider.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/RuntimeHost/StaticRuntimePackage.h
)
target_link_libraries(
    Cue.RuntimeHost.Static
    PUBLIC
        Cue.RuntimeHost.Core
    PRIVATE
        Cue.IO.Windows
        Cue.Package
        Cue.Renderer
)
target_compile_definitions(Cue.RuntimeHost.Static PRIVATE CUE_RUNTIME_PACKAGE_STATIC=1)
cue_configure_runtime_host_library(Cue.RuntimeHost.Static)

if(NOT DEFINED CUE_RUNTIME_HOST_BUILD_DYNAMIC)
    set(CUE_RUNTIME_HOST_BUILD_DYNAMIC ON)
endif()

if(CUE_RUNTIME_HOST_BUILD_DYNAMIC)
    add_library(Cue.RuntimeHost.Dynamic.Windows STATIC)
    target_sources(
        Cue.RuntimeHost.Dynamic.Windows
        PRIVATE
            RuntimeModuleIdentity.cpp
            RuntimeModuleIdentity.h
            RuntimePackage.cpp
            RuntimePackage.h
            RuntimePath.h
    )
    target_link_libraries(
        Cue.RuntimeHost.Dynamic.Windows
        PUBLIC
            Cue.RuntimeHost.Core
        PRIVATE
            Cue.IO.Windows
            Cue.Package
            Cue.Renderer
    )
    target_compile_definitions(Cue.RuntimeHost.Dynamic.Windows PRIVATE CUE_RUNTIME_PACKAGE_DYNAMIC=1)
    cue_configure_runtime_host_library(Cue.RuntimeHost.Dynamic.Windows)

    add_executable(CueRuntimeHost Main.cpp)
    target_link_libraries(
        CueRuntimeHost
        PRIVATE
            Cue.RuntimeHost.Core
            Cue.RuntimeHost.Dynamic.Windows
    )
    target_compile_features(CueRuntimeHost PRIVATE cxx_std_20)
endif()

if(BUILD_TESTING)
    target_link_libraries(Cue.RuntimeHost.Core PRIVATE Cue.Platform.Windows.TestSupport)
    target_compile_definitions(
        Cue.RuntimeHost.Core
        PRIVATE
            CUE_RUNTIME_RESIZE_SMOKE_SUPPORT=1
            CUE_RUNTIME_SCENE_PIXEL_SMOKE_SUPPORT=1
    )
endif()

target_compile_definitions(
    Cue.RuntimeHost.Core
    PRIVATE
        $<$<CONFIG:Debug>:CUE_RUNTIME_GRAPHICS_DIAGNOSTICS_DEFAULT=1>
        $<$<CONFIG:Development>:CUE_RUNTIME_GRAPHICS_DIAGNOSTICS_DEFAULT=1>
        $<$<CONFIG:Release>:CUE_RUNTIME_GRAPHICS_DIAGNOSTICS_DEFAULT=0>
)

if(CUE_RUNTIME_HOST_BUILD_DYNAMIC)
    set_target_properties(
        CueRuntimeHost
        PROPERTIES
            CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
endif()

if(MSVC AND CUE_RUNTIME_HOST_BUILD_DYNAMIC)
    target_compile_options(
        CueRuntimeHost
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
        CueRuntimeHost
        PRIVATE
            $<$<CONFIG:Development>:/DEBUG>
            /DEPENDENTLOADFLAG:0x800
    )
endif()
