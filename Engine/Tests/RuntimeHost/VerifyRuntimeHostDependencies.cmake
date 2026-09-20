cmake_minimum_required(VERSION 4.2.0)

include("${CMAKE_CURRENT_LIST_DIR}/../CMake/CueDependencyVerification.cmake")

if(NOT EXISTS "${REPORT_FILE}")
    message(FATAL_ERROR "Runtime Host dependency report does not exist: ${REPORT_FILE}")
endif()

file(STRINGS "${REPORT_FILE}" dependencyReportLines)

foreach(
    requiredLine
    IN ITEMS
        "CueRuntimeHost LINK_LIBRARIES: Cue.RuntimeHost.Core;Cue.RuntimeHost.Dynamic.Windows"
        "Cue.RuntimeHost.Core LINK_LIBRARIES: Cue.Foundation;Cue.GameCore;Cue.GameModule.Abi;Cue.Input.Windows;Cue.Platform.Windows;Cue.RHI.D3D12.Windows;Cue.Runtime;Cue.Scene;Cue.Schema;Cue.Platform.Windows.TestSupport"
        "Cue.RuntimeHost.Static LINK_LIBRARIES: Cue.RuntimeHost.Core;Cue.IO.Windows;Cue.Package;Cue.Renderer"
        "Cue.RuntimeHost.Dynamic.Windows LINK_LIBRARIES: Cue.RuntimeHost.Core;Cue.IO.Windows;Cue.Package;Cue.Renderer"
        "Core must not link: Cue.RuntimeHost.Dynamic.Windows;Cue.IO.Windows;Cue.Package"
        "Static package loader dependencies: Cue.RuntimeHost.Core;Cue.IO.Windows;Cue.Package;Cue.Renderer"
        "Process implementation target: Cue.RuntimeHost.Core"
        "Testing-only Core dependency: Cue.Platform.Windows.TestSupport"
        "Forbidden source dependencies: D3D12NativeTypes;Editor;ProjectFiles;ECS"
        "Renderer source dependency allowed only in RuntimePackage.cpp"
)
    cue_require_report_line(
        dependencyReportLines
        "${requiredLine}"
        "Runtime Host dependency report is missing an exact line: "
    )
endforeach()

foreach(
    loaderFreeSource
    IN ITEMS
        "${RUNTIME_HOST_SOURCE_DIR}/GameModuleQueryProvider.cpp"
        "${RUNTIME_HOST_SOURCE_DIR}/RuntimeHostApplication.cpp"
        "${RUNTIME_HOST_SOURCE_DIR}/RuntimeHostProcess.cpp"
        "${RUNTIME_HOST_SOURCE_DIR}/StaticGameModuleQueryProvider.cpp"
)
    file(READ "${loaderFreeSource}" loaderFreeContents)
    string(REGEX MATCH "LoadLibraryExW|GetProcAddress|FreeLibrary" loaderReference "${loaderFreeContents}")
    if(loaderReference)
        message(FATAL_ERROR "Runtime Host Core or Static source references a Dynamic Loader API: ${loaderFreeSource}")
    endif()
endforeach()

file(
    GLOB_RECURSE
    runtimeHostSources
    LIST_DIRECTORIES FALSE
    "${RUNTIME_HOST_SOURCE_DIR}/*.c"
    "${RUNTIME_HOST_SOURCE_DIR}/*.cc"
    "${RUNTIME_HOST_SOURCE_DIR}/*.cpp"
    "${RUNTIME_HOST_SOURCE_DIR}/*.cxx"
    "${RUNTIME_HOST_SOURCE_DIR}/*.h"
    "${RUNTIME_HOST_SOURCE_DIR}/*.hh"
    "${RUNTIME_HOST_SOURCE_DIR}/*.hpp"
    "${RUNTIME_HOST_SOURCE_DIR}/*.hxx"
)

foreach(runtimeHostSource IN LISTS runtimeHostSources)
    file(READ "${runtimeHostSource}" sourceContents)
    set(forbiddenPattern "WideCharToMultiByte|MultiByteToWideChar|d3d12\\.h|dxgi[0-9_]*\\.h|ID3D12|IDXGI|D3D12_|DXGI_|DirectX|Editor|ProjectFiles|Cue/ECS")
    get_filename_component(runtimeHostName "${runtimeHostSource}" NAME)
    if(NOT runtimeHostName STREQUAL "RuntimePackage.cpp")
        string(APPEND forbiddenPattern "|Renderer")
    endif()
    string(
        REGEX MATCH
        "${forbiddenPattern}"
        forbiddenDependency
        "${sourceContents}"
    )

    if(forbiddenDependency)
        message(FATAL_ERROR "Runtime Host source contains a forbidden dependency: ${runtimeHostSource}")
    endif()
endforeach()

message(STATUS "CueRuntimeHost dependency direction: passed")
