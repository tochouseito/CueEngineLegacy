add_executable(CueDistributionManifestTests ManifestTests.cpp)
target_link_libraries(CueDistributionManifestTests PRIVATE Cue.Distribution)
target_compile_features(CueDistributionManifestTests PRIVATE cxx_std_20)

add_executable(CueDistributionPublisherTests PublisherTests.cpp)
target_link_libraries(CueDistributionPublisherTests PRIVATE Cue.Distribution)
target_compile_features(CueDistributionPublisherTests PRIVATE cxx_std_20)

add_executable(CueDistributionPublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueDistributionPublicHeaderTest PRIVATE Cue.Distribution)
target_compile_features(CueDistributionPublicHeaderTest PRIVATE cxx_std_20)

set(
    cueDistributionTestTargets
    CueDistributionManifestTests
    CueDistributionPublisherTests
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
add_test(NAME Cue.Distribution.PublicHeaders COMMAND CueDistributionPublicHeaderTest)

set_tests_properties(
    Cue.Distribution.Manifest
    Cue.Distribution.Publisher
    Cue.Distribution.PublicHeaders
    PROPERTIES
        LABELS Distribution
        TIMEOUT 20
)
