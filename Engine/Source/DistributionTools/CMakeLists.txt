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

# Workerは自身の証跡検証前にApplication-local MSVC RuntimeをLoadしてはならない。
# 通常Targetの動的Runtime契約を変えず、Workerが使うFirst-party依存閉包だけを静的Runtimeで再構築する。
function(cue_resolve_target_paths output target)
    get_target_property(targetSourceDirectory ${target} SOURCE_DIR)
    set(resolvedPaths)
    foreach(path IN LISTS ARGN)
        if(IS_ABSOLUTE "${path}" OR path MATCHES "^\\$<")
            list(APPEND resolvedPaths "${path}")
        else()
            get_filename_component(resolvedPath "${path}" ABSOLUTE BASE_DIR "${targetSourceDirectory}")
            list(APPEND resolvedPaths "${resolvedPath}")
        endif()
    endforeach()
    set(${output} "${resolvedPaths}" PARENT_SCOPE)
endfunction()

function(cue_add_static_runtime_clone target sourceTarget)
    get_target_property(targetSources ${sourceTarget} SOURCES)
    cue_resolve_target_paths(resolvedSources ${sourceTarget} ${targetSources})
    add_library(${target} STATIC ${resolvedSources})

    get_target_property(targetIncludes ${sourceTarget} INCLUDE_DIRECTORIES)
    if(targetIncludes)
        cue_resolve_target_paths(resolvedIncludes ${sourceTarget} ${targetIncludes})
        target_include_directories(${target} PRIVATE ${resolvedIncludes})
    endif()
    get_target_property(targetInterfaceIncludes ${sourceTarget} INTERFACE_INCLUDE_DIRECTORIES)
    if(targetInterfaceIncludes)
        cue_resolve_target_paths(resolvedInterfaceIncludes ${sourceTarget} ${targetInterfaceIncludes})
        target_include_directories(${target} PUBLIC ${resolvedInterfaceIncludes})
    endif()

    foreach(property IN ITEMS COMPILE_DEFINITIONS COMPILE_FEATURES COMPILE_OPTIONS)
        get_target_property(values ${sourceTarget} ${property})
        if(values)
            if(property STREQUAL "COMPILE_DEFINITIONS")
                target_compile_definitions(${target} PRIVATE ${values})
            elseif(property STREQUAL "COMPILE_FEATURES")
                target_compile_features(${target} PRIVATE ${values})
            else()
                target_compile_options(${target} PRIVATE ${values})
            endif()
        endif()
    endforeach()
    foreach(property IN ITEMS INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_FEATURES INTERFACE_COMPILE_OPTIONS)
        get_target_property(values ${sourceTarget} ${property})
        if(values)
            if(property STREQUAL "INTERFACE_COMPILE_DEFINITIONS")
                target_compile_definitions(${target} PUBLIC ${values})
            elseif(property STREQUAL "INTERFACE_COMPILE_FEATURES")
                target_compile_features(${target} PUBLIC ${values})
            else()
                target_compile_options(${target} PUBLIC ${values})
            endif()
        endif()
    endforeach()

    set_target_properties(
        ${target}
        PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>/InstallWorker"
            CXX_EXTENSIONS OFF
            MSVC_RUNTIME_LIBRARY "MultiThreaded"
    )
endfunction()

cue_add_distribution_tool(CueEngineBootstrap BootstrapMain.cpp)
cue_add_distribution_tool(CueEngineInstallerTool InstallerMain.cpp)
cue_add_distribution_tool(CueEngineInstallWorker InstallWorkerMain.cpp)
cue_add_distribution_tool(CueEngineDistributionPublisherTool PublisherMain.cpp)

set_property(TARGET CueEngineBootstrap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

cue_add_static_runtime_clone(Cue.InstallWorker.Foundation Cue.Foundation)
cue_add_static_runtime_clone(Cue.InstallWorker.Foundation.Windows Cue.Foundation.Windows)
cue_add_static_runtime_clone(Cue.InstallWorker.IO Cue.IO)
cue_add_static_runtime_clone(Cue.InstallWorker.IO.Windows Cue.IO.Windows)
cue_add_static_runtime_clone(Cue.InstallWorker.Platform Cue.Platform)
cue_add_static_runtime_clone(Cue.InstallWorker.Platform.Windows Cue.Platform.Windows)
cue_add_static_runtime_clone(Cue.InstallWorker.Distribution Cue.Distribution)
cue_add_static_runtime_clone(Cue.InstallWorker.Distribution.Windows Cue.Distribution.Windows)
# WorkerはInstall APIを呼ばず、Toolchain診断のRegistry／Version依存をImport閉包へ含めない。
target_compile_definitions(
    Cue.InstallWorker.Distribution.Windows
    PRIVATE
        CUE_INSTALL_WORKER_STATIC_RUNTIME=1
)

target_link_libraries(Cue.InstallWorker.Foundation.Windows PUBLIC Cue.InstallWorker.Foundation)
target_link_libraries(Cue.InstallWorker.IO PUBLIC Cue.InstallWorker.Foundation)
target_link_libraries(
    Cue.InstallWorker.IO.Windows
    PUBLIC
        Cue.InstallWorker.IO
    PRIVATE
        Cue.InstallWorker.Foundation.Windows
        bcrypt
        Ole32
        Shell32
)
target_link_libraries(Cue.InstallWorker.Platform PUBLIC Cue.InstallWorker.Foundation)
target_link_libraries(
    Cue.InstallWorker.Platform.Windows
    PUBLIC
        Cue.InstallWorker.Platform
    PRIVATE
        Cue.InstallWorker.Foundation.Windows
        Ole32
        Shell32
        User32
)
target_link_libraries(Cue.InstallWorker.Distribution PUBLIC Cue.InstallWorker.Foundation)
target_link_libraries(
    Cue.InstallWorker.Distribution.Windows
    PUBLIC
        Cue.InstallWorker.Distribution
        Cue.InstallWorker.Platform
    PRIVATE
        Cue.InstallWorker.IO.Windows
        Cue.InstallWorker.Platform.Windows
        ole32
)

set_property(
    TARGET CueEngineInstallWorker
    PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

target_link_libraries(CueEngineDistributionPublisherTool PRIVATE Cue.Distribution.Windows Cue.Foundation)
target_link_libraries(CueEngineInstallerTool PRIVATE Cue.Distribution.Windows Cue.Foundation)
target_link_libraries(CueEngineInstallWorker PRIVATE Cue.InstallWorker.Distribution.Windows Cue.InstallWorker.Foundation)
target_include_directories(
    CueEngineDistributionPublisherTool
    PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/Generated"
)
