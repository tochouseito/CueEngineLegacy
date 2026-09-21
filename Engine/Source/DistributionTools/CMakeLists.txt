if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio ([0-9]+) ")
    message(FATAL_ERROR "CueEngine Distribution Tools require a Visual Studio generator")
endif()

function(cue_distribution_escape_cpp_string output input)
    string(REPLACE "\\" "\\\\" escaped "${input}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

find_package(Git REQUIRED)
find_program(cueDistributionPowerShell NAMES pwsh REQUIRED)

get_filename_component(cueDistributionCompilerArchitectureRoot "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(cueDistributionCompilerHostRoot "${cueDistributionCompilerArchitectureRoot}" DIRECTORY)
get_filename_component(cueDistributionCompilerBinRoot "${cueDistributionCompilerHostRoot}" DIRECTORY)
get_filename_component(cueDistributionCompilerToolsetRoot "${cueDistributionCompilerBinRoot}" DIRECTORY)
get_filename_component(cueDistributionToolsetVersionValue "${cueDistributionCompilerToolsetRoot}" NAME)
if(NOT cueDistributionToolsetVersionValue MATCHES "^[0-9]+\\.[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?$")
    message(FATAL_ERROR "The Distribution Publisher MSVC toolset version could not be recorded")
endif()

cue_distribution_escape_cpp_string(cueDistributionRepositoryRoot "${CMAKE_SOURCE_DIR}")
cue_distribution_escape_cpp_string(cueDistributionCMakeCommand "${CMAKE_COMMAND}")
cue_distribution_escape_cpp_string(cueDistributionCMakeVersion "${CMAKE_VERSION}")
cue_distribution_escape_cpp_string(cueDistributionCMakeGenerator "${CMAKE_GENERATOR}")
cue_distribution_escape_cpp_string(cueDistributionGitCommand "${GIT_EXECUTABLE}")
cue_distribution_escape_cpp_string(cueDistributionPowerShellCommand "${cueDistributionPowerShell}")
cue_distribution_escape_cpp_string(cueDistributionCompilerVersion "${CMAKE_CXX_COMPILER_VERSION}")
cue_distribution_escape_cpp_string(cueDistributionToolsetVersion "${cueDistributionToolsetVersionValue}")
cue_distribution_escape_cpp_string(cueDistributionWindowsSdkVersion "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

configure_file(
    Private/DistributionBuildMetadata.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/Generated/DistributionBuildMetadata.h"
    @ONLY
)

function(cue_add_distribution_tool target source)
    add_executable(${target} ${source})
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    set_target_properties(
        ${target}
        PROPERTIES
            CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
    if(MSVC)
        target_compile_options(
            ${target}
            PRIVATE
                /W4
                /WX
                /permissive-
                /Zc:__cplusplus
                /utf-8
                $<$<CONFIG:Development>:/O2>
                $<$<CONFIG:Development>:/Zi>
        )
        target_link_options(${target} PRIVATE $<$<CONFIG:Development>:/DEBUG>)
    endif()
endfunction()

cue_add_distribution_tool(CueEngineBootstrap BootstrapMain.cpp)
cue_add_distribution_tool(CueEngineInstallerTool InstallerMain.cpp)
cue_add_distribution_tool(CueEngineInstallWorker InstallWorkerMain.cpp)
cue_add_distribution_tool(CueEngineDistributionPublisherTool PublisherMain.cpp)

set_property(TARGET CueEngineBootstrap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

target_link_libraries(CueEngineDistributionPublisherTool PRIVATE Cue.Distribution.Windows Cue.Foundation)
target_include_directories(
    CueEngineDistributionPublisherTool
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/Generated"
)
