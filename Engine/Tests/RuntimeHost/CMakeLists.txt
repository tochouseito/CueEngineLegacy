get_target_property(cueRuntimeHostLinks CueRuntimeHost LINK_LIBRARIES)
get_target_property(cueRuntimeHostCoreLinks Cue.RuntimeHost.Core LINK_LIBRARIES)
get_target_property(cueRuntimeHostStaticLinks Cue.RuntimeHost.Static LINK_LIBRARIES)
get_target_property(cueRuntimeHostDynamicLinks Cue.RuntimeHost.Dynamic.Windows LINK_LIBRARIES)

add_library(
    CueRuntimeHostRhiOwnershipCompile
    OBJECT
    BackendOwnershipCompile.cpp
)

target_link_libraries(CueRuntimeHostRhiOwnershipCompile PRIVATE Cue.RHI.D3D12)
target_compile_features(CueRuntimeHostRhiOwnershipCompile PRIVATE cxx_std_20)

add_library(CueRuntimePackageDependencyProbe SHARED RuntimePackageDependencyProbe.cpp)
target_compile_features(CueRuntimePackageDependencyProbe PRIVATE cxx_std_20)
set_target_properties(CueRuntimePackageDependencyProbe PROPERTIES OUTPUT_NAME ProbeDependency)

add_library(CueRuntimePackageProbe SHARED RuntimePackageProbe.cpp)
target_link_libraries(CueRuntimePackageProbe PRIVATE Cue.GameModule.Abi CueRuntimePackageDependencyProbe)
target_compile_features(CueRuntimePackageProbe PRIVATE cxx_std_20)
target_compile_definitions(
    CueRuntimePackageProbe
    PRIVATE
        CUE_GAME_MODULE_BUILD=1
        $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
        $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
        $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
)
set_target_properties(CueRuntimePackageProbe PROPERTIES OUTPUT_NAME CueGameModule)

add_library(CueRuntimePackageStaticProbe STATIC RuntimePackageProbe.cpp)
target_link_libraries(CueRuntimePackageStaticProbe PRIVATE Cue.GameModule.Abi)
target_compile_features(CueRuntimePackageStaticProbe PRIVATE cxx_std_20)
target_compile_definitions(
    CueRuntimePackageStaticProbe
    PRIVATE
        CUE_GAME_MODULE_STATIC=1
        CUE_RUNTIME_PACKAGE_STATIC_PROBE=1
        $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
        $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
        $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
)

add_executable(CueRuntimeStaticSceneProduct RuntimeStaticSceneProduct.cpp)
target_link_libraries(
    CueRuntimeStaticSceneProduct
    PRIVATE
        CueRuntimePackageStaticProbe
        Cue.RuntimeHost.Static
)
target_compile_features(CueRuntimeStaticSceneProduct PRIVATE cxx_std_20)
target_compile_definitions(CueRuntimeStaticSceneProduct PRIVATE CUE_GAME_MODULE_STATIC=1)
set_target_properties(
    CueRuntimeStaticSceneProduct
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

add_executable(CueRuntimeHostGameModuleQueryProviderTests GameModuleQueryProviderTests.cpp)
target_link_libraries(CueRuntimeHostGameModuleQueryProviderTests PRIVATE Cue.RuntimeHost.Static)
target_compile_features(CueRuntimeHostGameModuleQueryProviderTests PRIVATE cxx_std_20)
target_compile_definitions(
    CueRuntimeHostGameModuleQueryProviderTests
    PRIVATE
        $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
        $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
        $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
)

add_executable(CueRuntimeHostPathTests RuntimePathTests.cpp)
target_include_directories(
    CueRuntimeHostPathTests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/Engine/Source/RuntimeHost"
)
target_compile_features(CueRuntimeHostPathTests PRIVATE cxx_std_20)
set_target_properties(
    CueRuntimeHostPathTests
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

add_executable(
    CueRuntimeHostModuleIdentityTests
    RuntimeModuleIdentityTests.cpp
    "${PROJECT_SOURCE_DIR}/Engine/Source/RuntimeHost/RuntimeModuleIdentity.cpp"
)
target_include_directories(
    CueRuntimeHostModuleIdentityTests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/Engine/Source/RuntimeHost"
)
target_compile_features(CueRuntimeHostModuleIdentityTests PRIVATE cxx_std_20)
set_target_properties(
    CueRuntimeHostModuleIdentityTests
    PROPERTIES
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)

string(REPLACE "." ";" cueRuntimeHostCompilerParts "${CMAKE_CXX_COMPILER_VERSION}")
list(GET cueRuntimeHostCompilerParts 0 cueRuntimeHostCompilerMajor)
list(GET cueRuntimeHostCompilerParts 1 cueRuntimeHostCompilerMinor)
list(GET cueRuntimeHostCompilerParts 2 cueRuntimeHostCompilerPatch)
list(GET cueRuntimeHostCompilerParts 3 cueRuntimeHostCompilerBuild)
math(EXPR cueRuntimeHostCompilerVersion "${cueRuntimeHostCompilerMajor} * 100 + ${cueRuntimeHostCompilerMinor}")
math(EXPR cueRuntimeHostCompilerFullVersion "${cueRuntimeHostCompilerVersion} * 100000 + ${cueRuntimeHostCompilerPatch}")

set_target_properties(
    CueRuntimeHostRhiOwnershipCompile
    PROPERTIES
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        CueRuntimeHostRhiOwnershipCompile
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_compile_options(
        CueRuntimePackageDependencyProbe
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_compile_options(
        CueRuntimePackageProbe
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_compile_options(
        CueRuntimePackageStaticProbe
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_compile_options(
        CueRuntimeStaticSceneProduct
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_compile_options(
        CueRuntimeHostPathTests
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_compile_options(
        CueRuntimeHostGameModuleQueryProviderTests
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_compile_options(
        CueRuntimeHostModuleIdentityTests
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(CueRuntimeHostPathTests PRIVATE $<$<CONFIG:Development>:/DEBUG>)
    target_link_options(CueRuntimeStaticSceneProduct PRIVATE $<$<CONFIG:Development>:/DEBUG>)
    target_link_options(CueRuntimeHostGameModuleQueryProviderTests PRIVATE $<$<CONFIG:Development>:/DEBUG>)
    target_link_options(CueRuntimeHostModuleIdentityTests PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

set(
    cueRuntimeHostDependencyReport
    "${CMAKE_CURRENT_BINARY_DIR}/CueRuntimeHost.Dependencies.txt"
)

configure_file(
    RuntimeHostDependencyReport.txt.in
    "${cueRuntimeHostDependencyReport}"
    @ONLY
)

add_custom_target(
    Cue.RuntimeHost.Tests
    DEPENDS
        CueRhiD3d12AdapterSelectionProcess
        CueRuntimeHost
        CueRuntimeHostGameModuleQueryProviderTests
        CueRuntimePackageDependencyProbe
        CueRuntimePackageProbe
        CueRuntimePackageStaticProbe
        CueRuntimeStaticSceneProduct
        CueRuntimeHostModuleIdentityTests
        CueRuntimeHostPathTests
        CueRuntimeHostRhiOwnershipCompile
        CueRuntimePresentationFrameTests
        CueRuntimeSceneDataTests
)

add_executable(CueRuntimeSceneDataTests RuntimeSceneDataTests.cpp)
target_link_libraries(CueRuntimeSceneDataTests PRIVATE Cue.RuntimeHost.Static Cue.Package Cue.Project)
target_include_directories(CueRuntimeSceneDataTests PRIVATE ${PROJECT_SOURCE_DIR}/Engine/Source/RuntimeHost)
target_compile_features(CueRuntimeSceneDataTests PRIVATE cxx_std_20)
set_target_properties(CueRuntimeSceneDataTests PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(CueRuntimeSceneDataTests PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
endif()
add_test(NAME Cue.RuntimeHost.SceneData COMMAND CueRuntimeSceneDataTests)

add_executable(CueRuntimePresentationFrameTests RuntimeSceneFrameTests.cpp)
target_link_libraries(CueRuntimePresentationFrameTests PRIVATE Cue.RuntimeHost.Core)
target_include_directories(
    CueRuntimePresentationFrameTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/Engine/Source/RuntimeHost
)
target_compile_features(CueRuntimePresentationFrameTests PRIVATE cxx_std_20)
set_target_properties(CueRuntimePresentationFrameTests PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(CueRuntimePresentationFrameTests PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
endif()
add_test(NAME Cue.RuntimeHost.PresentationFrame COMMAND CueRuntimePresentationFrameTests)

add_test(NAME Cue.RuntimeHost.PathUtf8 COMMAND CueRuntimeHostPathTests)
add_test(NAME Cue.RuntimeHost.GameModuleQueryProvider COMMAND CueRuntimeHostGameModuleQueryProviderTests)

add_test(
    NAME Cue.RuntimeHost.ModuleIdentity
    COMMAND
        CueRuntimeHostModuleIdentityTests
        $<TARGET_FILE:CueRuntimePackageDependencyProbe>
        ${CMAKE_CURRENT_BINARY_DIR}/RuntimeModuleIdentity/$<CONFIG>/ProbeDependency.dll
)

add_test(
    NAME Cue.RuntimeHost.Smoke
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyRuntimeApplicationSmoke.cmake
)

add_test(
    NAME Cue.RuntimeHost.Dependencies
    COMMAND
        ${CMAKE_COMMAND}
        -DREPORT_FILE=${cueRuntimeHostDependencyReport}
        -DRUNTIME_HOST_SOURCE_DIR=${PROJECT_SOURCE_DIR}/Engine/Source/RuntimeHost
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyRuntimeHostDependencies.cmake
)

add_test(
    NAME Cue.RuntimeHost.Package.Process
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -DMODULE_LIBRARY=$<TARGET_FILE:CueRuntimePackageProbe>
        -DDEPENDENCY_LIBRARY=$<TARGET_FILE:CueRuntimePackageDependencyProbe>
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/RuntimePackageProcess/$<CONFIG>
        -DCONFIGURATION=$<CONFIG>
        -DCOMPILER_VERSION=${cueRuntimeHostCompilerVersion}
        -DCOMPILER_FULL_VERSION=${cueRuntimeHostCompilerFullVersion}
        -DCOMPILER_BUILD=${cueRuntimeHostCompilerBuild}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyRuntimePackage.cmake
)

add_test(
    NAME Cue.RuntimeHost.StaticScenePackage.Process
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeStaticSceneProduct>
        -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/RuntimeStaticScenePackage/$<CONFIG>
        -DCONFIGURATION=$<CONFIG>
        -DDUMPBIN=${CMAKE_DUMPBIN}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyRuntimeStaticScenePackage.cmake
)

add_test(
    NAME Cue.RuntimeHost.InitializationFailure
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyRuntimeHostFailure.cmake
)

add_test(
    NAME Cue.RuntimeHost.InvalidModeCombination
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyRuntimeHostArguments.cmake
)

add_test(
    NAME Cue.RuntimeHost.D3D12DeviceSmoke.Hardware
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -DADAPTER_PROBE_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12AdapterSelectionProcess>
        -DMODE=hardware
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12DeviceSmoke.cmake
)

add_test(
    NAME Cue.RuntimeHost.D3D12DeviceSmoke.Warp
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -DMODE=warp
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12DeviceSmoke.cmake
)

add_test(
    NAME Cue.RuntimeHost.D3D12PresentationSmoke.Hardware
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -DADAPTER_PROBE_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12AdapterSelectionProcess>
        -DMODE=hardware
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12PresentationSmoke.cmake
)

add_test(
    NAME Cue.RuntimeHost.D3D12PresentationSmoke.Warp
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -DMODE=warp
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12PresentationSmoke.cmake
)

add_test(
    NAME Cue.RuntimeHost.D3D12RenderSmoke.Hardware
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -DADAPTER_PROBE_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12AdapterSelectionProcess>
        -DMODE=hardware
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12RenderSmoke.cmake
)

add_test(
    NAME Cue.RuntimeHost.D3D12RenderSmoke.Warp
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -DMODE=warp
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12RenderSmoke.cmake
)

add_test(
    NAME Cue.RuntimeHost.D3D12ResizeSmoke.Hardware
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -DADAPTER_PROBE_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12AdapterSelectionProcess>
        -DMODE=hardware
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12ResizeSmoke.cmake
)

add_test(
    NAME Cue.RuntimeHost.D3D12ResizeSmoke.Warp
    COMMAND
        ${CMAKE_COMMAND}
        -DTEST_EXECUTABLE=$<TARGET_FILE:CueRuntimeHost>
        -DMODE=warp
        -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12ResizeSmoke.cmake
)

set_tests_properties(
    Cue.RuntimeHost.Smoke
    PROPERTIES
        LABELS "RuntimeHost;Smoke"
        TIMEOUT 10
)

set_tests_properties(
    Cue.RuntimeHost.Dependencies
    Cue.RuntimeHost.GameModuleQueryProvider
    Cue.RuntimeHost.InitializationFailure
    Cue.RuntimeHost.InvalidModeCombination
    Cue.RuntimeHost.ModuleIdentity
    Cue.RuntimeHost.PathUtf8
    Cue.RuntimeHost.PresentationFrame
    PROPERTIES
        LABELS RuntimeHost
        TIMEOUT 10
)

set_tests_properties(
    Cue.RuntimeHost.Package.Process
    Cue.RuntimeHost.StaticScenePackage.Process
    PROPERTIES
        LABELS RuntimeHost
        TIMEOUT 300
)

set_tests_properties(
    Cue.RuntimeHost.D3D12DeviceSmoke.Hardware
    PROPERTIES
        LABELS "RuntimeHost;RHI;Smoke"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

set_tests_properties(
    Cue.RuntimeHost.D3D12DeviceSmoke.Warp
    PROPERTIES
        LABELS "RuntimeHost;RHI;Smoke"
        TIMEOUT 20
)

set_tests_properties(
    Cue.RuntimeHost.D3D12PresentationSmoke.Hardware
    PROPERTIES
        LABELS "RuntimeHost;RHI;Smoke"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

set_tests_properties(
    Cue.RuntimeHost.D3D12PresentationSmoke.Warp
    PROPERTIES
        LABELS "RuntimeHost;RHI;Smoke"
        TIMEOUT 20
)

set_tests_properties(
    Cue.RuntimeHost.D3D12RenderSmoke.Hardware
    PROPERTIES
        LABELS "RuntimeHost;RHI;Smoke"
        SKIP_RETURN_CODE 77
        TIMEOUT 60
)

set_tests_properties(
    Cue.RuntimeHost.D3D12RenderSmoke.Warp
    PROPERTIES
        LABELS "RuntimeHost;RHI;Smoke"
        TIMEOUT 60
)

set_tests_properties(
    Cue.RuntimeHost.D3D12ResizeSmoke.Hardware
    PROPERTIES
        LABELS "RuntimeHost;RHI;Smoke"
        SKIP_RETURN_CODE 77
        TIMEOUT 60
)

set_tests_properties(
    Cue.RuntimeHost.D3D12ResizeSmoke.Warp
    PROPERTIES
        LABELS "RuntimeHost;RHI;Smoke"
        TIMEOUT 60
)
