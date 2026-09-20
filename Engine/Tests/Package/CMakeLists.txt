add_executable(CuePackageRuntimeDataTests RuntimeDataTests.cpp)
target_link_libraries(CuePackageRuntimeDataTests PRIVATE Cue.Package Cue.Renderer Cue.Runtime Cue.Schema)
target_compile_features(CuePackageRuntimeDataTests PRIVATE cxx_std_20)
target_include_directories(
    CuePackageRuntimeDataTests
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Package/Private"
)

add_executable(CuePackagePublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CuePackagePublicHeaderTest PRIVATE Cue.Package)
target_compile_features(CuePackagePublicHeaderTest PRIVATE cxx_std_20)

add_executable(CuePackageManifestTests ManifestTests.cpp)
target_link_libraries(CuePackageManifestTests PRIVATE Cue.Package)
target_compile_features(CuePackageManifestTests PRIVATE cxx_std_20)

set(cuePackageTestTargets
    CuePackageRuntimeDataTests
    CuePackageManifestTests
    CuePackagePublicHeaderTest
)

if(WIN32)
    add_executable(CuePackagePublisherTests PublisherTests.cpp)
    target_link_libraries(CuePackagePublisherTests PRIVATE Cue.Package Cue.IO.Windows)
    target_compile_features(CuePackagePublisherTests PRIVATE cxx_std_20)
    target_include_directories(
        CuePackagePublisherTests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Package/Private"
    )

    add_executable(CuePackageWorkflowTests WorkflowTests.cpp)
    target_link_libraries(CuePackageWorkflowTests PRIVATE Cue.Package Cue.IO.Windows Cue.Schema)
    target_compile_features(CuePackageWorkflowTests PRIVATE cxx_std_20)

    list(APPEND cuePackageTestTargets CuePackagePublisherTests CuePackageWorkflowTests)
endif()

foreach(testTarget IN LISTS cuePackageTestTargets)
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

add_custom_target(Cue.Package.Tests DEPENDS ${cuePackageTestTargets})

add_test(NAME Cue.Package.RuntimeData COMMAND CuePackageRuntimeDataTests)
add_test(NAME Cue.Package.Manifest COMMAND CuePackageManifestTests "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME Cue.Package.PublicHeaders COMMAND CuePackagePublicHeaderTest)

set_tests_properties(
    Cue.Package.RuntimeData
    Cue.Package.Manifest
    Cue.Package.PublicHeaders
    PROPERTIES
        LABELS Package
        TIMEOUT 10
)

if(WIN32)
    add_test(NAME Cue.Package.Publisher COMMAND CuePackagePublisherTests "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME Cue.Package.Workflow COMMAND CuePackageWorkflowTests)
    set_tests_properties(Cue.Package.Publisher Cue.Package.Workflow PROPERTIES LABELS Package TIMEOUT 10)
endif()
