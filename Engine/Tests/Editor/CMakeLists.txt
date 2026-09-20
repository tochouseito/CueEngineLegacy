add_executable(CueEditorImGuiTests EditorImGuiTests.cpp)
target_link_libraries(CueEditorImGuiTests PRIVATE Cue.Editor.ImGui Cue.ImGui.Core)
target_compile_features(CueEditorImGuiTests PRIVATE cxx_std_20)

add_executable(CueEditorImGuiPublicHeaderTest PublicHeaderCompile.cpp)
target_link_libraries(CueEditorImGuiPublicHeaderTest PRIVATE Cue.Editor.ImGui)
target_compile_features(CueEditorImGuiPublicHeaderTest PRIVATE cxx_std_20)

add_executable(CueEditorFilesImGuiTests FilesImGuiTests.cpp)
target_link_libraries(
    CueEditorFilesImGuiTests
    PRIVATE
        Cue.Editor.ImGui
        Cue.Editor.Windows
        Cue.Foundation.Windows
        Cue.ImGui.Core
        Cue.IO.Windows
        Cue.Project
)
target_compile_features(CueEditorFilesImGuiTests PRIVATE cxx_std_20)

add_executable(CueEditorPlaySessionImGuiTests PlaySessionImGuiTests.cpp)
target_link_libraries(CueEditorPlaySessionImGuiTests PRIVATE Cue.Editor.ImGui Cue.ImGui.Core)
target_compile_features(CueEditorPlaySessionImGuiTests PRIVATE cxx_std_20)

add_executable(CueEditorBuildImGuiTests BuildImGuiTests.cpp)
target_link_libraries(CueEditorBuildImGuiTests PRIVATE Cue.Editor.ImGui Cue.ImGui.Core Cue.IO.Windows)
target_compile_features(CueEditorBuildImGuiTests PRIVATE cxx_std_20)

add_executable(CueEditorGameViewImGuiTests GameViewImGuiTests.cpp)
target_link_libraries(CueEditorGameViewImGuiTests PRIVATE Cue.Editor.ImGui Cue.ImGui.Core)
target_compile_features(CueEditorGameViewImGuiTests PRIVATE cxx_std_20)

add_executable(CueEditorDebugViewImGuiTests DebugViewImGuiTests.cpp)
target_link_libraries(CueEditorDebugViewImGuiTests PRIVATE Cue.Editor.ImGui Cue.ImGui.Core)
target_compile_features(CueEditorDebugViewImGuiTests PRIVATE cxx_std_20)

add_executable(CueEditorPlayInputRoutingTests PlayInputRoutingTests.cpp)
target_link_libraries(CueEditorPlayInputRoutingTests PRIVATE Cue.Editor.ImGui)
target_compile_features(CueEditorPlayInputRoutingTests PRIVATE cxx_std_20)

set(
    cueEditorTestTargets
    CueEditorImGuiTests
    CueEditorImGuiPublicHeaderTest
    CueEditorFilesImGuiTests
    CueEditorPlaySessionImGuiTests
    CueEditorBuildImGuiTests
    CueEditorGameViewImGuiTests
    CueEditorDebugViewImGuiTests
    CueEditorPlayInputRoutingTests
)

foreach(testTarget IN LISTS cueEditorTestTargets)
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

add_custom_target(Cue.Editor.ImGui.Tests DEPENDS ${cueEditorTestTargets})

add_test(NAME Cue.Editor.ImGui.HierarchyInspector COMMAND CueEditorImGuiTests)
add_test(NAME Cue.Editor.ImGui.PublicHeaders COMMAND CueEditorImGuiPublicHeaderTest)
add_test(NAME Cue.Editor.ImGui.Files COMMAND CueEditorFilesImGuiTests)
add_test(NAME Cue.Editor.ImGui.PlaySession COMMAND CueEditorPlaySessionImGuiTests)
add_test(NAME Cue.Editor.ImGui.Build COMMAND CueEditorBuildImGuiTests)
add_test(NAME Cue.Editor.ImGui.GameView COMMAND CueEditorGameViewImGuiTests)
add_test(NAME Cue.Editor.ImGui.DebugView COMMAND CueEditorDebugViewImGuiTests)
add_test(NAME Cue.Editor.ImGui.PlayInputRouting COMMAND CueEditorPlayInputRoutingTests)

set_tests_properties(
    Cue.Editor.ImGui.HierarchyInspector
    Cue.Editor.ImGui.PublicHeaders
    Cue.Editor.ImGui.Files
    Cue.Editor.ImGui.PlaySession
    Cue.Editor.ImGui.Build
    Cue.Editor.ImGui.GameView
    Cue.Editor.ImGui.DebugView
    Cue.Editor.ImGui.PlayInputRouting
    PROPERTIES
        LABELS Editor
        TIMEOUT 10
)
