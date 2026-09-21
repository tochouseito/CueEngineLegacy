cmake_minimum_required(VERSION 4.2.0)

include("${CMAKE_CURRENT_LIST_DIR}/../CMake/CueDependencyVerification.cmake")

if(NOT EXISTS "${REPORT_FILE}")
    message(FATAL_ERROR "RHI dependency report does not exist: ${REPORT_FILE}")
endif()

file(STRINGS "${REPORT_FILE}" dependencyReportLines)

foreach(
    requiredLine
    IN ITEMS
        "Cue.RHI LINK_LIBRARIES: Cue.Foundation"
        "Cue.RHI INTERFACE_LINK_LIBRARIES: Cue.Foundation"
        "Cue.RHI MANUALLY_ADDED_DEPENDENCIES: <none>"
        "Cue.RHI INTERFACE_SOURCES: <none>"
        "Cue.RHI INTERFACE_LINK_LIBRARIES_DIRECT: <none>"
        "Cue.RHI INTERFACE_SYSTEM_INCLUDE_DIRECTORIES: <none>"
        "Cue.RHI LINK_OPTIONS: $<$<CONFIG:Development>:/DEBUG>"
        "Cue.RHI INTERFACE_LINK_OPTIONS: <none>"
        "Cue.RHI LINK_FLAGS: <none>"
        "Cue.RHI STATIC_LIBRARY_OPTIONS: <none>"
        "Cue.RHI.D3D12 LINK_LIBRARIES: Cue.RHI;Cue.Foundation.Windows;Cue.EngineAssets;D3D12;DXGI;DXGUID"
        "Cue.RHI.D3D12 INTERFACE_LINK_LIBRARIES: Cue.RHI;$<LINK_ONLY:Cue.Foundation.Windows>;$<LINK_ONLY:Cue.EngineAssets>;$<LINK_ONLY:D3D12>;$<LINK_ONLY:DXGI>;$<LINK_ONLY:DXGUID>"
        "Cue.RHI.D3D12 MANUALLY_ADDED_DEPENDENCIES: CueD3d12SceneShaders"
        "Cue.RHI.D3D12 INTERFACE_SOURCES: <none>"
        "Cue.RHI.D3D12 INTERFACE_LINK_LIBRARIES_DIRECT: <none>"
        "Cue.RHI.D3D12 INTERFACE_SYSTEM_INCLUDE_DIRECTORIES: <none>"
        "Cue.RHI.D3D12 LINK_OPTIONS: $<$<CONFIG:Development>:/DEBUG>"
        "Cue.RHI.D3D12 INTERFACE_LINK_OPTIONS: <none>"
        "Cue.RHI.D3D12 LINK_FLAGS: <none>"
        "Cue.RHI.D3D12 STATIC_LIBRARY_OPTIONS: <none>"
        "Cue.RHI.D3D12.Windows LINK_LIBRARIES: Cue.RHI.D3D12;Cue.Platform.Windows"
        "Cue.RHI.D3D12.Windows INTERFACE_LINK_LIBRARIES: Cue.RHI.D3D12;$<LINK_ONLY:Cue.Platform.Windows>"
        "Minimum Windows SDK: 10.0.26100.0"
        "Allowed graph: Cue.RHI.D3D12->Cue.RHI+Cue.Foundation.Windows+Cue.EngineAssets->Cue.Foundation"
        "Allowed Windows adapter graph: Cue.RHI.D3D12.Windows->Cue.RHI.D3D12+Cue.Platform.Windows"
        "Private links are LINK_ONLY: Cue.Foundation.Windows;Cue.EngineAssets;D3D12;DXGI;DXGUID"
        "Windows and D3D12 native types in public headers: forbidden"
        "Target Source and Header File Sets: scanned"
        "Precompiled Headers and CXX Modules: forbidden"
        "Interface include roots: target Public directories only"
        "Library-injecting link options and compiler pragmas: forbidden"
)
    cue_require_report_line(
        dependencyReportLines
        "${requiredLine}"
        "RHI dependency report is missing an exact line: "
    )
endforeach()

file(READ "${REPORT_FILE}" dependencyReport)
string(
    REGEX MATCH
    "Selected Windows SDK: ([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)"
    selectedWindowsSdkLine
    "${dependencyReport}"
)

if(NOT selectedWindowsSdkLine)
    message(FATAL_ERROR "RHI dependency report does not contain a selected Windows SDK")
endif()

set(selectedWindowsSdkVersion "${CMAKE_MATCH_1}")

if(selectedWindowsSdkVersion VERSION_LESS "10.0.26100.0")
    message(FATAL_ERROR "Selected Windows SDK is below the minimum: ${selectedWindowsSdkVersion}")
endif()

if(NOT EXISTS "${RHI_TARGET_FILE_LIST}")
    message(FATAL_ERROR "RHI Target file list does not exist: ${RHI_TARGET_FILE_LIST}")
endif()

file(STRINGS "${RHI_TARGET_FILE_LIST}" rhiTargetFiles)
file(
    GLOB_RECURSE
    rhiSources
    LIST_DIRECTORIES FALSE
    "${RHI_SOURCE_DIR}/*.c"
    "${RHI_SOURCE_DIR}/*.cc"
    "${RHI_SOURCE_DIR}/*.cpp"
    "${RHI_SOURCE_DIR}/*.cxx"
    "${RHI_SOURCE_DIR}/*.h"
    "${RHI_SOURCE_DIR}/*.hh"
    "${RHI_SOURCE_DIR}/*.hpp"
    "${RHI_SOURCE_DIR}/*.hxx"
    "${RHI_SOURCE_DIR}/*.inl"
    "${RHI_SOURCE_DIR}/*.ixx"
)
list(APPEND rhiSources ${rhiTargetFiles})
list(REMOVE_DUPLICATES rhiSources)

if(NOT DEFINED WINDOWS_SDK_VERSION OR "${WINDOWS_SDK_VERSION}" STREQUAL "")
    message(FATAL_ERROR "Selected Windows SDK version is not available")
endif()

cmake_host_system_information(
    RESULT windowsSdkRoot
    QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
    VALUE KitsRoot10
    VIEW HOST
    ERROR_VARIABLE windowsSdkRegistryError
)

if(NOT "${windowsSdkRegistryError}" STREQUAL "" OR NOT IS_DIRECTORY "${windowsSdkRoot}")
    message(FATAL_ERROR "Windows SDK root could not be resolved: ${windowsSdkRegistryError}")
endif()

set(windowsSdkIncludeRoot "${windowsSdkRoot}/Include/${WINDOWS_SDK_VERSION}")
set(
    windowsSdkIncludeDirectories
    "${windowsSdkIncludeRoot}/um"
    "${windowsSdkIncludeRoot}/shared"
    "${windowsSdkIncludeRoot}/winrt"
    "${windowsSdkIncludeRoot}/cppwinrt"
)

foreach(includeDirectory IN LISTS windowsSdkIncludeDirectories)
    if(NOT IS_DIRECTORY "${includeDirectory}")
        message(FATAL_ERROR "Windows SDK include directory does not exist: ${includeDirectory}")
    endif()
endforeach()

set(hasD3d12PrivateInclude FALSE)

foreach(rhiSource IN LISTS rhiSources)
    if(NOT EXISTS "${rhiSource}" OR IS_DIRECTORY "${rhiSource}")
        message(FATAL_ERROR "RHI Target source does not exist: ${rhiSource}")
    endif()

    file(RELATIVE_PATH relativeSource "${RHI_SOURCE_DIR}" "${rhiSource}")

    if(relativeSource MATCHES "^\\.\\.(/|$)")
        message(FATAL_ERROR "RHI Target source escaped its source tree: ${rhiSource}")
    endif()

    file(READ "${rhiSource}" sourceContents)
    string(TOLOWER "${sourceContents}" sourceContentsLower)
    string(
        REGEX MATCHALL
        "(^|\n)[ \t]*#[ \t]*include[^\r\n]*"
        allIncludeDirectives
        "${sourceContents}"
    )

    foreach(includeDirective IN LISTS allIncludeDirectives)
        string(STRIP "${includeDirective}" includeDirective)

        if(NOT includeDirective MATCHES "^#[ \t]*include[ \t]*[<\"][^>\"]+[>\"]")
            message(
                FATAL_ERROR
                "RHI source contains a non-literal include directive: ${relativeSource}: ${includeDirective}"
            )
        endif()

        string(
            REGEX REPLACE
            "^#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"].*$"
            "\\1"
            includedHeader
            "${includeDirective}"
        )
        set(isWindowsSdkInclude FALSE)

        foreach(includeDirectory IN LISTS windowsSdkIncludeDirectories)
            if(EXISTS "${includeDirectory}/${includedHeader}")
                set(isWindowsSdkInclude TRUE)
                break()
            endif()
        endforeach()

        if(isWindowsSdkInclude)
            if(NOT relativeSource MATCHES "^D3D12/Private/")
                message(
                    FATAL_ERROR
                    "Windows SDK include escaped the D3D12 private boundary: ${relativeSource}: ${includedHeader}"
                )
            endif()

            set(hasD3d12PrivateInclude TRUE)
        endif()
    endforeach()

    if(relativeSource MATCHES "(^|/)Public/")
        string(
            REGEX MATCH
            "(^|[^A-Za-z0-9_])(HWND|HANDLE|HINSTANCE|HRESULT|LUID|GUID|IUnknown|ID3D12[A-Za-z0-9_]*|IDXGI[A-Za-z0-9_]*|D3D_[A-Za-z0-9_]*|D3D12_[A-Za-z0-9_]*|DXGI_[A-Za-z0-9_]*)([^A-Za-z0-9_]|$)|Microsoft::WRL"
            exposedNativeType
            "${sourceContents}"
        )

        if(exposedNativeType)
            message(FATAL_ERROR "Public RHI header exposes a Windows or D3D12 type: ${relativeSource}")
        endif()
    endif()

    string(
        REGEX MATCH
        "(^|\n)[ \t]*#[ \t]*pragma[ \t]+comment[ \t]*\\("
        pragmaLink
        "${sourceContentsLower}"
    )

    if(pragmaLink)
        message(FATAL_ERROR "RHI source contains a compiler pragma comment: ${relativeSource}")
    endif()

    string(REGEX MATCH "(__pragma|_pragma)[ \t\r\n]*\\(" compilerPragma "${sourceContentsLower}")

    if(compilerPragma)
        message(FATAL_ERROR "RHI source contains a compiler pragma: ${relativeSource}")
    endif()
endforeach()

if(NOT hasD3d12PrivateInclude)
    message(FATAL_ERROR "Cue.RHI.D3D12 does not exercise a private Windows SDK include")
endif()

message(STATUS "Cue.RHI dependency direction: passed")
message(STATUS "Cue.RHI.D3D12 native boundary: passed")
