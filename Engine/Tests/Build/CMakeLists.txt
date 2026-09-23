add_executable(CueBuildToolchainTests ToolchainTests.cpp)
target_link_libraries(CueBuildToolchainTests PRIVATE Cue.Build)
target_compile_features(CueBuildToolchainTests PRIVATE cxx_std_20)

add_executable(CueBuildPlanTests PlanTests.cpp)
target_link_libraries(CueBuildPlanTests PRIVATE Cue.Build)
target_compile_features(CueBuildPlanTests PRIVATE cxx_std_20)

add_executable(CueBuildCMakeRunnerTests CMakeRunnerTests.cpp)
target_link_libraries(CueBuildCMakeRunnerTests PRIVATE Cue.Build)
target_compile_features(CueBuildCMakeRunnerTests PRIVATE cxx_std_20)

add_executable(CueBuildServiceTests ServiceTests.cpp)
target_link_libraries(CueBuildServiceTests PRIVATE Cue.Build)
target_compile_features(CueBuildServiceTests PRIVATE cxx_std_20)

add_executable(CueBuildDiagnosticBundleTests DiagnosticBundleTests.cpp)
target_link_libraries(CueBuildDiagnosticBundleTests PRIVATE Cue.Build)
target_compile_features(CueBuildDiagnosticBundleTests PRIVATE cxx_std_20)

add_executable(CueBuildPublicHeaderTest CMakeRunnerHeaderCompile.cpp PlanHeaderCompile.cpp PublicHeaderCompile.cpp
                                        ServiceHeaderCompile.cpp)
target_link_libraries(CueBuildPublicHeaderTest PRIVATE Cue.Build)
target_compile_features(CueBuildPublicHeaderTest PRIVATE cxx_std_20)

set(cueBuildTestTargets CueBuildToolchainTests CueBuildPlanTests CueBuildCMakeRunnerTests CueBuildServiceTests
                        CueBuildDiagnosticBundleTests CueBuildPublicHeaderTest)
if(WIN32)
    add_library(CueBuildArtifactProbe SHARED ArtifactProbe.cpp)
    target_link_libraries(CueBuildArtifactProbe PRIVATE Cue.GameModule.Abi)
    target_compile_features(CueBuildArtifactProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildArtifactProbe
        PRIVATE
            CUE_GAME_MODULE_BUILD=1
            $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
            $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
            $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
    )

    add_library(CueBuildInvalidArtifactProbe SHARED ArtifactProbe.cpp)
    target_link_libraries(CueBuildInvalidArtifactProbe PRIVATE Cue.GameModule.Abi)
    target_compile_features(CueBuildInvalidArtifactProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildInvalidArtifactProbe
        PRIVATE
            CUE_GAME_MODULE_BUILD=1
            CUE_TEST_INVALID_API=1
            $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
            $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
            $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
    )

    add_library(CueBuildCrashingArtifactProbe SHARED ArtifactProbe.cpp)
    target_link_libraries(CueBuildCrashingArtifactProbe PRIVATE Cue.GameModule.Abi)
    target_compile_features(CueBuildCrashingArtifactProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildCrashingArtifactProbe
        PRIVATE
            CUE_GAME_MODULE_BUILD=1
            CUE_TEST_CRASH_QUERY=1
            $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
            $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
            $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
    )

    add_library(CueBuildHangingArtifactProbe SHARED ArtifactProbe.cpp)
    target_link_libraries(CueBuildHangingArtifactProbe PRIVATE Cue.GameModule.Abi)
    target_compile_features(CueBuildHangingArtifactProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildHangingArtifactProbe
        PRIVATE
            CUE_GAME_MODULE_BUILD=1
            CUE_TEST_HANG_QUERY=1
            $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
            $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
            $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
    )

    add_library(CueBuildZeroExitArtifactProbe SHARED ArtifactProbe.cpp)
    target_link_libraries(CueBuildZeroExitArtifactProbe PRIVATE Cue.GameModule.Abi)
    target_compile_features(CueBuildZeroExitArtifactProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildZeroExitArtifactProbe
        PRIVATE
            CUE_GAME_MODULE_BUILD=1
            CUE_TEST_EXIT_ZERO_QUERY=1
            $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
            $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
            $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
    )

    add_executable(CueBuildCMakeRunnerProbe CMakeRunnerProbe.cpp)
    target_compile_features(CueBuildCMakeRunnerProbe PRIVATE cxx_std_20)

    function(cue_configure_shipping_security_test_product targetName)
        set_target_properties(
            "${targetName}"
            PROPERTIES
                MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
                MSVC_RUNTIME_CHECKS ""
        )
        target_compile_options("${targetName}" PRIVATE /GS /guard:cf /O2 /Ob2 /utf-8)
        target_compile_definitions("${targetName}" PRIVATE NDEBUG)
        target_link_options(
            "${targetName}"
            PRIVATE
                /DYNAMICBASE
                /HIGHENTROPYVA
                /NXCOMPAT
                /GUARD:CF
                /CETCOMPAT
                /DEPENDENTLOADFLAG:0x800
                /INCREMENTAL:NO
                /OPT:REF
                /OPT:ICF
        )
    endfunction()

    add_executable(CueBuildShippingProductProbe ShippingProductProbe.cpp)
    target_compile_features(CueBuildShippingProductProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildShippingProductProbe
        PRIVATE
            CUE_TEST_PRODUCT_PROJECT_ID="41234567-89ab-4cde-8f01-23456789abcd"
            CUE_TEST_PRODUCT_CONFIGURATION="Release"
    )
    cue_configure_shipping_security_test_product(CueBuildShippingProductProbe)

    add_executable(CueBuildD3D12OrdinalShippingProductProbe ShippingProductProbe.cpp)
    target_link_libraries(CueBuildD3D12OrdinalShippingProductProbe PRIVATE d3d12)
    target_compile_features(CueBuildD3D12OrdinalShippingProductProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildD3D12OrdinalShippingProductProbe
        PRIVATE
            CUE_TEST_PRODUCT_PROJECT_ID="41234567-89ab-4cde-8f01-23456789abcd"
            CUE_TEST_PRODUCT_CONFIGURATION="Release"
            CUE_TEST_PRODUCT_D3D12_ORDINAL_IMPORT=1
    )
    cue_configure_shipping_security_test_product(CueBuildD3D12OrdinalShippingProductProbe)

    add_executable(CueBuildWrongProjectShippingProductProbe ShippingProductProbe.cpp)
    target_compile_features(CueBuildWrongProjectShippingProductProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildWrongProjectShippingProductProbe
        PRIVATE
            CUE_TEST_PRODUCT_PROJECT_ID="51234567-89ab-4cde-8f01-23456789abcd"
            CUE_TEST_PRODUCT_CONFIGURATION="Release"
    )
    cue_configure_shipping_security_test_product(CueBuildWrongProjectShippingProductProbe)

    add_executable(CueBuildWrongConfigurationShippingProductProbe ShippingProductProbe.cpp)
    target_compile_features(CueBuildWrongConfigurationShippingProductProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildWrongConfigurationShippingProductProbe
        PRIVATE
            CUE_TEST_PRODUCT_PROJECT_ID="41234567-89ab-4cde-8f01-23456789abcd"
            CUE_TEST_PRODUCT_CONFIGURATION="Development"
    )
    cue_configure_shipping_security_test_product(CueBuildWrongConfigurationShippingProductProbe)

    add_executable(CueBuildExtraFileShippingProductProbe ShippingProductProbe.cpp)
    target_compile_features(CueBuildExtraFileShippingProductProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildExtraFileShippingProductProbe
        PRIVATE
            CUE_TEST_PRODUCT_PROJECT_ID="41234567-89ab-4cde-8f01-23456789abcd"
            CUE_TEST_PRODUCT_CONFIGURATION="Release"
            CUE_TEST_PRODUCT_EXTRA_FILE=1
    )
    cue_configure_shipping_security_test_product(CueBuildExtraFileShippingProductProbe)

    add_executable(CueBuildUnhardenedShippingProductProbe ShippingProductProbe.cpp)
    target_compile_features(CueBuildUnhardenedShippingProductProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildUnhardenedShippingProductProbe
        PRIVATE
            CUE_TEST_PRODUCT_PROJECT_ID="41234567-89ab-4cde-8f01-23456789abcd"
            CUE_TEST_PRODUCT_CONFIGURATION="Release"
    )

    add_executable(CueBuildLoaderShippingProductProbe ShippingProductProbe.cpp)
    target_compile_features(CueBuildLoaderShippingProductProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildLoaderShippingProductProbe
        PRIVATE
            CUE_TEST_PRODUCT_PROJECT_ID="41234567-89ab-4cde-8f01-23456789abcd"
            CUE_TEST_PRODUCT_CONFIGURATION="Release"
            CUE_TEST_PRODUCT_LOADER_IMPORT=1
    )
    cue_configure_shipping_security_test_product(CueBuildLoaderShippingProductProbe)

    add_executable(CueBuildPackagedLoaderShippingProductProbe ShippingProductProbe.cpp)
    target_compile_features(CueBuildPackagedLoaderShippingProductProbe PRIVATE cxx_std_20)
    target_compile_definitions(
        CueBuildPackagedLoaderShippingProductProbe
        PRIVATE
            CUE_TEST_PRODUCT_PROJECT_ID="41234567-89ab-4cde-8f01-23456789abcd"
            CUE_TEST_PRODUCT_CONFIGURATION="Release"
            CUE_TEST_PRODUCT_PACKAGED_LOADER_IMPORT=1
    )
    cue_configure_shipping_security_test_product(CueBuildPackagedLoaderShippingProductProbe)

    add_executable(CueBuildWindowsProductSecurityTests WindowsProductSecurityTests.cpp)
    target_link_libraries(CueBuildWindowsProductSecurityTests PRIVATE Cue.Build.Windows)
    target_compile_features(CueBuildWindowsProductSecurityTests PRIVATE cxx_std_20)
    target_include_directories(
        CueBuildWindowsProductSecurityTests
        PRIVATE
            "${CMAKE_SOURCE_DIR}/Engine/Source/Build/Windows/Private"
    )
    add_dependencies(
        CueBuildWindowsProductSecurityTests
        CueBuildShippingProductProbe
        CueBuildD3D12OrdinalShippingProductProbe
        CueBuildUnhardenedShippingProductProbe
        CueBuildLoaderShippingProductProbe
        CueBuildPackagedLoaderShippingProductProbe
    )

    add_executable(CueBuildCMakeRunnerProcessTests CMakeRunnerProcessTests.cpp)
    target_link_libraries(CueBuildCMakeRunnerProcessTests PRIVATE Cue.Build Cue.Platform.Windows)
    target_compile_features(CueBuildCMakeRunnerProcessTests PRIVATE cxx_std_20)

    add_executable(CueBuildWindowsToolchainTests WindowsToolchainTests.cpp)
    target_link_libraries(CueBuildWindowsToolchainTests PRIVATE Cue.Build.Windows)
    target_compile_features(CueBuildWindowsToolchainTests PRIVATE cxx_std_20)
    target_include_directories(
        CueBuildWindowsToolchainTests
        PRIVATE
            "${CMAKE_SOURCE_DIR}/Engine/Source/Build/Windows/Private"
    )

    add_executable(CueBuildWindowsPublicHeaderTest WindowsPublicHeaderCompile.cpp)
    target_link_libraries(CueBuildWindowsPublicHeaderTest PRIVATE Cue.Build.Windows)
    target_compile_features(CueBuildWindowsPublicHeaderTest PRIVATE cxx_std_20)

    get_filename_component(cueTestMsvcArchitectureRoot "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(cueTestMsvcHostRoot "${cueTestMsvcArchitectureRoot}" DIRECTORY)
    get_filename_component(cueTestMsvcBinRoot "${cueTestMsvcHostRoot}" DIRECTORY)
    get_filename_component(cueTestMsvcToolsetRoot "${cueTestMsvcBinRoot}" DIRECTORY)
    get_filename_component(cueTestMsvcToolsetVersion "${cueTestMsvcToolsetRoot}" NAME)

    add_executable(CueBuildWindowsArtifactPublisherTests WindowsArtifactPublisherTests.cpp)
    target_link_libraries(CueBuildWindowsArtifactPublisherTests PRIVATE Cue.Build.Windows Cue.Project)
    target_compile_features(CueBuildWindowsArtifactPublisherTests PRIVATE cxx_std_20)
    target_include_directories(
        CueBuildWindowsArtifactPublisherTests
        PRIVATE
            "${CMAKE_SOURCE_DIR}/Engine/Source/Build/Windows/Private"
    )
    target_compile_definitions(
        CueBuildWindowsArtifactPublisherTests
        PRIVATE
            $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
            $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
            $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
            CUE_TEST_CMAKE_COMMAND="${CMAKE_COMMAND}"
            CUE_TEST_CMAKE_VERSION="${CMAKE_VERSION}"
            CUE_TEST_CMAKE_GENERATOR="${CMAKE_GENERATOR}"
            CUE_TEST_CMAKE_GENERATOR_INSTANCE="${CMAKE_GENERATOR_INSTANCE}"
            CUE_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
            CUE_TEST_ENGINE_ROOT="${CMAKE_SOURCE_DIR}"
            CUE_TEST_PLATFORM_TOOLSET="${CMAKE_VS_PLATFORM_TOOLSET}"
            CUE_TEST_MSVC_TOOLSET_VERSION="${cueTestMsvcToolsetVersion}"
            CUE_TEST_WINDOWS_SDK_VERSION="${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
    )
    add_dependencies(
        CueBuildWindowsArtifactPublisherTests
        CueBuildArtifactProbe
        CueBuildInvalidArtifactProbe
        CueBuildCrashingArtifactProbe
        CueBuildHangingArtifactProbe
        CueBuildZeroExitArtifactProbe
        CueBuildShippingProductProbe
        CueBuildWrongProjectShippingProductProbe
        CueBuildWrongConfigurationShippingProductProbe
        CueBuildExtraFileShippingProductProbe
    )
    list(
        APPEND
        cueBuildTestTargets
        CueBuildCMakeRunnerProbe
        CueBuildShippingProductProbe
        CueBuildWrongProjectShippingProductProbe
        CueBuildWrongConfigurationShippingProductProbe
        CueBuildExtraFileShippingProductProbe
        CueBuildUnhardenedShippingProductProbe
        CueBuildLoaderShippingProductProbe
        CueBuildPackagedLoaderShippingProductProbe
        CueBuildWindowsProductSecurityTests
        CueBuildCMakeRunnerProcessTests
        CueBuildWindowsToolchainTests
        CueBuildWindowsPublicHeaderTest
        CueBuildArtifactProbe
        CueBuildInvalidArtifactProbe
        CueBuildCrashingArtifactProbe
        CueBuildHangingArtifactProbe
        CueBuildZeroExitArtifactProbe
        CueBuildWindowsArtifactPublisherTests
    )
endif()

foreach(testTarget IN LISTS cueBuildTestTargets)
    set_target_properties(
        ${testTarget}
        PROPERTIES
            CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
    if(MSVC)
        target_compile_options(
            ${testTarget}
            PRIVATE
                /W4
                /WX
                /permissive-
                /Zc:__cplusplus
                /utf-8
                $<$<CONFIG:Development>:/O2>
                $<$<CONFIG:Development>:/Zi>
        )
        target_link_options(${testTarget} PRIVATE $<$<CONFIG:Development>:/DEBUG>)
    endif()
endforeach()

add_custom_target(Cue.Build.Tests DEPENDS ${cueBuildTestTargets})

add_test(NAME Cue.Build.Toolchain COMMAND CueBuildToolchainTests)
set_tests_properties(Cue.Build.Toolchain PROPERTIES LABELS Build TIMEOUT 20)

add_test(NAME Cue.Build.Plan COMMAND CueBuildPlanTests $<TARGET_FILE_DIR:CueBuildPlanTests>)
set_tests_properties(Cue.Build.Plan PROPERTIES LABELS Build TIMEOUT 20)

add_test(NAME Cue.Build.CMakeRunner COMMAND CueBuildCMakeRunnerTests)
set_tests_properties(Cue.Build.CMakeRunner PROPERTIES LABELS Build TIMEOUT 20)

add_test(NAME Cue.Build.Service COMMAND CueBuildServiceTests)
set_tests_properties(Cue.Build.Service PROPERTIES LABELS Build TIMEOUT 30)

add_test(NAME Cue.Build.DiagnosticBundle COMMAND CueBuildDiagnosticBundleTests $<TARGET_FILE_DIR:CueBuildDiagnosticBundleTests>)
set_tests_properties(Cue.Build.DiagnosticBundle PROPERTIES LABELS Build TIMEOUT 30)

add_test(NAME Cue.Build.PublicHeaders COMMAND CueBuildPublicHeaderTest)
set_tests_properties(Cue.Build.PublicHeaders PROPERTIES LABELS Build TIMEOUT 10)

if(WIN32)
    add_test(NAME Cue.Build.CMakeRunner.Process
             COMMAND CueBuildCMakeRunnerProcessTests $<TARGET_FILE:CueBuildCMakeRunnerProbe>)
    set_tests_properties(Cue.Build.CMakeRunner.Process PROPERTIES LABELS "Build;Windows" TIMEOUT 30)

    add_test(NAME Cue.Build.Windows.Toolchain COMMAND CueBuildWindowsToolchainTests)
    set_tests_properties(Cue.Build.Windows.Toolchain PROPERTIES LABELS "Build;Windows" TIMEOUT 20)

    add_test(NAME Cue.Build.Windows.PublicHeaders COMMAND CueBuildWindowsPublicHeaderTest)
    set_tests_properties(Cue.Build.Windows.PublicHeaders PROPERTIES LABELS "Build;Windows" TIMEOUT 10)

    add_test(NAME Cue.Build.Windows.ArtifactPublisher
             COMMAND CueBuildWindowsArtifactPublisherTests $<TARGET_FILE:CueBuildArtifactProbe>
                     $<TARGET_FILE:CueBuildInvalidArtifactProbe> $<TARGET_FILE:CueBuildCrashingArtifactProbe>
                     $<TARGET_FILE:CueBuildHangingArtifactProbe> $<TARGET_FILE:CueBuildZeroExitArtifactProbe>
                     $<TARGET_FILE:CueBuildShippingProductProbe>
                     $<TARGET_FILE:CueBuildWrongProjectShippingProductProbe>
                     $<TARGET_FILE:CueBuildWrongConfigurationShippingProductProbe>
                     $<TARGET_FILE:CueBuildExtraFileShippingProductProbe>)
    set_tests_properties(Cue.Build.Windows.ArtifactPublisher PROPERTIES LABELS "Build;Windows" TIMEOUT 60)

    add_test(NAME Cue.Build.Windows.ProductSecurity
             COMMAND CueBuildWindowsProductSecurityTests $<TARGET_FILE:CueBuildShippingProductProbe>
                     $<TARGET_FILE:CueBuildD3D12OrdinalShippingProductProbe>
                     $<TARGET_FILE:CueBuildUnhardenedShippingProductProbe>
                     $<TARGET_FILE:CueBuildLoaderShippingProductProbe>
                     $<TARGET_FILE:CueBuildPackagedLoaderShippingProductProbe>)
    set_tests_properties(Cue.Build.Windows.ProductSecurity PROPERTIES LABELS "Build;Windows;Security" TIMEOUT 30)
endif()
