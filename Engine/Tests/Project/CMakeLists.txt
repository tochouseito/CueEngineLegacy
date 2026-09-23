add_executable(CueProjectTests ProjectTests.cpp)
target_link_libraries(CueProjectTests PRIVATE Cue.Project)
target_compile_features(CueProjectTests PRIVATE cxx_std_20)

add_executable(CueProjectPublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueProjectPublicHeaderTest PRIVATE Cue.Project)
target_compile_features(CueProjectPublicHeaderTest PRIVATE cxx_std_20)

add_executable(CueProjectGeneratorTests GeneratorTests.cpp)
target_link_libraries(CueProjectGeneratorTests PRIVATE Cue.Project)
target_compile_features(CueProjectGeneratorTests PRIVATE cxx_std_20)

add_executable(CueProjectRegistryTests RegistryTests.cpp)
target_link_libraries(CueProjectRegistryTests PRIVATE Cue.Project)
target_compile_features(CueProjectRegistryTests PRIVATE cxx_std_20)

add_executable(CueProjectCompatibilityTests CompatibilityTests.cpp)
target_link_libraries(CueProjectCompatibilityTests PRIVATE Cue.Project)
target_compile_features(CueProjectCompatibilityTests PRIVATE cxx_std_20)

set(cueProjectTestTargets CueProjectTests CueProjectPublicHeaderTest CueProjectGeneratorTests CueProjectRegistryTests
                          CueProjectCompatibilityTests)
if(WIN32)
    add_executable(CueProjectGeneratorWindowsTests GeneratorWindowsTests.cpp)
    target_link_libraries(CueProjectGeneratorWindowsTests PRIVATE Cue.Project Cue.IO.Windows)
    target_compile_features(CueProjectGeneratorWindowsTests PRIVATE cxx_std_20)
    target_compile_definitions(
        CueProjectGeneratorWindowsTests
        PRIVATE
            CUE_TEST_CMAKE_COMMAND="${CMAKE_COMMAND}"
            CUE_TEST_ENGINE_ROOT="${CMAKE_SOURCE_DIR}"
            $<$<CONFIG:Debug>:CUE_TEST_BUILD_CONFIGURATION=1>
            $<$<CONFIG:Development>:CUE_TEST_BUILD_CONFIGURATION=2>
            $<$<CONFIG:Release>:CUE_TEST_BUILD_CONFIGURATION=3>
    )

    add_executable(CueProjectM09ProcessTests M09ProcessTests.cpp)
    target_link_libraries(CueProjectM09ProcessTests PRIVATE Cue.Project Cue.IO.Windows)
    target_compile_features(CueProjectM09ProcessTests PRIVATE cxx_std_20)

    list(APPEND cueProjectTestTargets CueProjectGeneratorWindowsTests CueProjectM09ProcessTests)
endif()
foreach(testTarget IN LISTS cueProjectTestTargets)
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

add_custom_target(Cue.Project.Tests DEPENDS ${cueProjectTestTargets})

add_test(NAME Cue.Project.Descriptor COMMAND CueProjectTests)
set_tests_properties(Cue.Project.Descriptor PROPERTIES LABELS Project TIMEOUT 20)

add_test(NAME Cue.Project.PublicHeaders COMMAND CueProjectPublicHeaderTest)
set_tests_properties(Cue.Project.PublicHeaders PROPERTIES LABELS Project TIMEOUT 10)

add_test(NAME Cue.Project.Generator COMMAND CueProjectGeneratorTests)
set_tests_properties(Cue.Project.Generator PROPERTIES LABELS Project TIMEOUT 20)

add_test(NAME Cue.Project.Registry COMMAND CueProjectRegistryTests)
set_tests_properties(Cue.Project.Registry PROPERTIES LABELS Project TIMEOUT 60)

add_test(NAME Cue.Project.Compatibility COMMAND CueProjectCompatibilityTests)
set_tests_properties(Cue.Project.Compatibility PROPERTIES LABELS Project TIMEOUT 20)

if(WIN32)
    add_test(NAME Cue.Project.Generator.Windows COMMAND CueProjectGeneratorWindowsTests)
    set_tests_properties(Cue.Project.Generator.Windows PROPERTIES LABELS Project TIMEOUT 900)

    add_test(NAME Cue.Project.M09.Process COMMAND CueProjectM09ProcessTests)
    set_tests_properties(Cue.Project.M09.Process PROPERTIES LABELS Project TIMEOUT 30)
endif()
