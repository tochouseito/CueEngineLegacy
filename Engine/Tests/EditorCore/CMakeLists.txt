add_executable(CueEditorCoreTests EditorCoreTests.cpp)
target_link_libraries(CueEditorCoreTests PRIVATE Cue.EditorCore Cue.EngineAssets Cue.Renderer)
target_compile_features(CueEditorCoreTests PRIVATE cxx_std_20)

add_executable(CueEditorCorePublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueEditorCorePublicHeaderTest PRIVATE Cue.EditorCore)
target_compile_features(CueEditorCorePublicHeaderTest PRIVATE cxx_std_20)

add_executable(CueEditorCoreFilesWorkspaceTests FilesWorkspaceTests.cpp)
target_link_libraries(CueEditorCoreFilesWorkspaceTests PRIVATE Cue.EditorCore Cue.IO.Windows)
target_compile_features(CueEditorCoreFilesWorkspaceTests PRIVATE cxx_std_20)

add_executable(CueEditorCorePlaySessionTests EditorPlaySessionControllerTests.cpp)
target_link_libraries(CueEditorCorePlaySessionTests PRIVATE Cue.EditorCore)
target_compile_features(CueEditorCorePlaySessionTests PRIVATE cxx_std_20)

set(
    cueEditorCoreTestTargets
    CueEditorCoreTests
    CueEditorCorePublicHeaderTest
    CueEditorCoreFilesWorkspaceTests
    CueEditorCorePlaySessionTests
)

foreach(testTarget IN LISTS cueEditorCoreTestTargets)
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

add_custom_target(Cue.EditorCore.Tests DEPENDS ${cueEditorCoreTestTargets})

add_test(NAME Cue.EditorCore.DocumentState COMMAND CueEditorCoreTests)
add_test(NAME Cue.EditorCore.PublicHeaders COMMAND CueEditorCorePublicHeaderTest)
add_test(NAME Cue.EditorCore.FilesWorkspace COMMAND CueEditorCoreFilesWorkspaceTests)
add_test(NAME Cue.EditorCore.PlaySession COMMAND CueEditorCorePlaySessionTests)

set_tests_properties(
    Cue.EditorCore.DocumentState
    Cue.EditorCore.PublicHeaders
    Cue.EditorCore.FilesWorkspace
    Cue.EditorCore.PlaySession
    PROPERTIES
        LABELS EditorCore
        TIMEOUT 10
)
