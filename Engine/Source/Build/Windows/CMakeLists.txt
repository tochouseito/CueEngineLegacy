if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio ([0-9]+) ")
    message(FATAL_ERROR "Cue.Build.Windows requires a Visual Studio generator")
endif()
set(cueBuildVisualStudioMajor "${CMAKE_MATCH_1}")

function(cue_escape_cpp_string output input)
    string(REPLACE "\\" "\\\\" escaped "${input}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

cue_escape_cpp_string(cueBuildCMakeCommand "${CMAKE_COMMAND}")
cue_escape_cpp_string(cueBuildCMakeVersion "${CMAKE_VERSION}")
cue_escape_cpp_string(cueBuildCMakeGenerator "${CMAKE_GENERATOR}")
cue_escape_cpp_string(cueBuildCMakeMinimumVersion "${CMAKE_MINIMUM_REQUIRED_VERSION}")

find_package(Git REQUIRED)
cue_escape_cpp_string(cueBuildGitCommand "${GIT_EXECUTABLE}")

file(TO_CMAKE_PATH "${CMAKE_VS_MSBUILD_COMMAND}" cueBuildVisualStudioMsbuildValue)
if(
    NOT cueBuildVisualStudioMsbuildValue
        MATCHES "^(.*)/MSBuild/Current/Bin(/amd64)?/MSBuild\\.exe$"
)
    message(FATAL_ERROR "The selected Visual Studio root could not be recorded")
endif()
set(cueBuildVisualStudioRootValue "${CMAKE_MATCH_1}")
cue_escape_cpp_string(cueBuildVisualStudioRoot "${cueBuildVisualStudioRootValue}")
cue_escape_cpp_string(cueBuildVisualStudioMsbuild "${cueBuildVisualStudioMsbuildValue}")
cue_escape_cpp_string(cueBuildMsvcCompiler "${CMAKE_CXX_COMPILER}")
cue_escape_cpp_string(cueBuildPlatformToolset "${CMAKE_VS_PLATFORM_TOOLSET}")
cue_escape_cpp_string(cueBuildWindowsSdkVersion "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

get_filename_component(cueBuildMsvcArchitectureRoot "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(cueBuildMsvcHostRoot "${cueBuildMsvcArchitectureRoot}" DIRECTORY)
get_filename_component(cueBuildMsvcBinRoot "${cueBuildMsvcHostRoot}" DIRECTORY)
get_filename_component(cueBuildMsvcToolsetRoot "${cueBuildMsvcBinRoot}" DIRECTORY)
get_filename_component(cueBuildMsvcToolsetVersionValue "${cueBuildMsvcToolsetRoot}" NAME)
if(NOT cueBuildMsvcToolsetVersionValue MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?$")
    message(FATAL_ERROR "The selected MSVC minor toolset version could not be recorded")
endif()
cue_escape_cpp_string(cueBuildMsvcToolsetVersion "${cueBuildMsvcToolsetVersionValue}")

if(DEFINED ENV{CMAKE_WINDOWS_KITS_10_DIR} AND NOT "$ENV{CMAKE_WINDOWS_KITS_10_DIR}" STREQUAL "")
    set(cueBuildWindowsSdkRootValue "$ENV{CMAKE_WINDOWS_KITS_10_DIR}")
else()
    cmake_host_system_information(
        RESULT cueBuildWindowsSdkRootValue
        QUERY WINDOWS_REGISTRY
        "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
        VALUE KitsRoot10
        VIEW BOTH
    )
endif()

if(cueBuildWindowsSdkRootValue STREQUAL "")
    message(FATAL_ERROR "The selected Windows SDK root could not be recorded")
endif()

cue_escape_cpp_string(cueBuildWindowsSdkRoot "${cueBuildWindowsSdkRootValue}")
cue_escape_cpp_string(cueBuildEngineSourceRoot "${CMAKE_SOURCE_DIR}")
cue_escape_cpp_string(cueBuildEngineBinaryRoot "${CMAKE_BINARY_DIR}")

configure_file(
    Private/EngineBuildMetadata.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/Generated/EngineBuildMetadata.h"
    @ONLY
)

add_executable(CueGameModuleProbe Private/GameModuleProbe.cpp)
target_link_libraries(CueGameModuleProbe PRIVATE Cue.GameModule.Abi)
target_compile_features(CueGameModuleProbe PRIVATE cxx_std_20)
target_compile_definitions(CueGameModuleProbe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(
    CueGameModuleProbe
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

if(MSVC)
    target_compile_options(
        CueGameModuleProbe
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(CueGameModuleProbe PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

add_library(Cue.Build.Windows STATIC)
add_dependencies(Cue.Build.Windows CueGameModuleProbe)

target_sources(
    Cue.Build.Windows
    PRIVATE
        Private/WindowsArtifactPublisher.cpp
        Private/WindowsProductSecurity.cpp
        Private/WindowsProductSecurityInternal.h
        Private/WindowsToolchain.cpp
        Private/WindowsToolchainInternal.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Build/Windows/WindowsArtifactPublisher.h
            Public/Cue/Build/Windows/WindowsProductSecurity.h
            Public/Cue/Build/Windows/WindowsToolchain.h
)

target_link_libraries(
    Cue.Build.Windows
    PUBLIC
        Cue.Build
    PRIVATE
        Cue.Foundation.Windows
        Cue.GameModule.Abi
        Cue.Platform.Windows
        Cue.Project
        bcrypt
        crypt32
        version
        wintrust
)
target_compile_features(Cue.Build.Windows PUBLIC cxx_std_20)
target_include_directories(
    Cue.Build.Windows
    PUBLIC
        Public
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/Generated"
)
target_compile_definitions(Cue.Build.Windows PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(
    Cue.Build.Windows
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Build.Windows
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Build.Windows PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
