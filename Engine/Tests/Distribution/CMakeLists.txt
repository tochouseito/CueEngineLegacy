add_executable(CueDistributionManifestTests ManifestTests.cpp)
target_link_libraries(CueDistributionManifestTests PRIVATE Cue.Distribution)
target_compile_features(CueDistributionManifestTests PRIVATE cxx_std_20)

add_executable(CueDistributionPublisherTests PublisherTests.cpp)
target_link_libraries(CueDistributionPublisherTests PRIVATE Cue.Distribution)
target_compile_features(CueDistributionPublisherTests PRIVATE cxx_std_20)

add_executable(CueDistributionInstallStateTests InstallStateTests.cpp)
target_link_libraries(CueDistributionInstallStateTests PRIVATE Cue.Distribution)
target_compile_features(CueDistributionInstallStateTests PRIVATE cxx_std_20)

add_executable(CueDistributionPublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueDistributionPublicHeaderTest PRIVATE Cue.Distribution)
target_compile_features(CueDistributionPublicHeaderTest PRIVATE cxx_std_20)

set(
    cueDistributionTestTargets
    CueDistributionManifestTests
    CueDistributionPublisherTests
    CueDistributionInstallStateTests
    CueDistributionPublicHeaderTest
)

foreach(testTarget IN LISTS cueDistributionTestTargets)
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

add_custom_target(Cue.Distribution.Tests DEPENDS ${cueDistributionTestTargets})

add_test(NAME Cue.Distribution.Manifest COMMAND CueDistributionManifestTests)
add_test(NAME Cue.Distribution.Publisher COMMAND CueDistributionPublisherTests)
add_test(NAME Cue.Distribution.InstallState COMMAND CueDistributionInstallStateTests)
add_test(NAME Cue.Distribution.PublicHeaders COMMAND CueDistributionPublicHeaderTest)

set(
    cueDistributionTests
    Cue.Distribution.Manifest
    Cue.Distribution.Publisher
    Cue.Distribution.InstallState
    Cue.Distribution.PublicHeaders
)

if(WIN32)
    add_executable(CueDistributionWindowsPublisherTests WindowsPublisherTests.cpp)
    add_executable(CueDistributionWindowsInstallerTests WindowsInstallerTests.cpp)
    add_executable(CueDistributionInstallerProbeHelper InstallerProbeHelper.cpp)
    add_dependencies(
        CueDistributionWindowsInstallerTests
        CueEngineInstallerTool
        CueDistributionInstallerProbeHelper
    )
    add_dependencies(
        Cue.Distribution.Tests
        CueDistributionWindowsPublisherTests
        CueDistributionWindowsInstallerTests
        CueDistributionInstallerProbeHelper
    )
    target_link_libraries(CueDistributionWindowsPublisherTests PRIVATE Cue.Distribution.Windows)
    target_link_libraries(CueDistributionWindowsInstallerTests PRIVATE Cue.Distribution.Windows)
    target_compile_features(CueDistributionWindowsPublisherTests PRIVATE cxx_std_20)
    target_compile_features(CueDistributionWindowsInstallerTests PRIVATE cxx_std_20)
    target_compile_features(CueDistributionInstallerProbeHelper PRIVATE cxx_std_20)
    set_target_properties(
        CueDistributionWindowsPublisherTests
        CueDistributionWindowsInstallerTests
        CueDistributionInstallerProbeHelper
        PROPERTIES
            CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
    if(MSVC)
        target_compile_options(
            CueDistributionWindowsPublisherTests
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
            CueDistributionWindowsInstallerTests
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
            CueDistributionInstallerProbeHelper
            PRIVATE
                /W4
                /WX
                /permissive-
                /Zc:__cplusplus
                /utf-8
        )
        target_link_options(
            CueDistributionWindowsPublisherTests
            PRIVATE
                $<$<CONFIG:Development>:/DEBUG>
        )
        target_link_options(CueDistributionWindowsInstallerTests PRIVATE $<$<CONFIG:Development>:/DEBUG>)
    endif()

    find_program(cueDistributionPowerShell NAMES pwsh REQUIRED)
    add_test(
        NAME Cue.Distribution.RestoreVcpkgContract
        COMMAND
            "${cueDistributionPowerShell}"
            -NoProfile
            -File
            "${CMAKE_CURRENT_SOURCE_DIR}/RestoreVcpkgContractTests.ps1"
            -RepositoryRoot
            "${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME Cue.Distribution.WindowsPublisher COMMAND CueDistributionWindowsPublisherTests)
    add_test(NAME Cue.Distribution.WindowsInstaller COMMAND CueDistributionWindowsInstallerTests)
    add_test(
        NAME Cue.Distribution.WindowsInstallerRecovery
        COMMAND CueDistributionWindowsInstallerTests --registry-recovery
    )
    list(
        APPEND
        cueDistributionTests
        Cue.Distribution.RestoreVcpkgContract
        Cue.Distribution.WindowsPublisher
        Cue.Distribution.WindowsInstaller
        Cue.Distribution.WindowsInstallerRecovery
    )
endif()

set_tests_properties(
    ${cueDistributionTests}
    PROPERTIES
        LABELS Distribution
        TIMEOUT 20
)

if(WIN32)
    set_tests_properties(
        Cue.Distribution.WindowsInstaller
        Cue.Distribution.WindowsInstallerRecovery
        PROPERTIES TIMEOUT 240
    )
endif()
