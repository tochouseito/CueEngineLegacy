add_library(Cue.Editor.ImGui STATIC)

target_sources(
    Cue.Editor.ImGui
    PRIVATE
        Private/BuildPresenter.cpp
        Private/EditorDockspace.cpp
        Private/EditorPresenter.cpp
        Private/FilesPresenter.cpp
        Private/DebugView.cpp
        Private/GameView.cpp
        Private/PackagePresenter.cpp
        Private/PlayInputRouting.cpp
        Private/PlaySessionPresenter.cpp
        Private/SessionLog.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Editor/ImGui/BuildPresenter.h
            Public/Cue/Editor/ImGui/EditorDockspace.h
            Public/Cue/Editor/ImGui/EditorPresenter.h
            Public/Cue/Editor/ImGui/FilesPresenter.h
            Public/Cue/Editor/ImGui/DebugView.h
            Public/Cue/Editor/ImGui/GameView.h
            Public/Cue/Editor/ImGui/PackagePresenter.h
            Public/Cue/Editor/ImGui/PlayInputRouting.h
            Public/Cue/Editor/ImGui/PlaySessionPresenter.h
            Public/Cue/Editor/ImGui/SessionLog.h
)

target_link_libraries(
    Cue.Editor.ImGui
    PUBLIC
        Cue.Build
        Cue.EditorCore
        Cue.Input
        Cue.Package
    PRIVATE
        Cue.ImGui.Core
)
target_compile_features(Cue.Editor.ImGui PUBLIC cxx_std_20)
target_include_directories(Cue.Editor.ImGui PUBLIC Public)

set_target_properties(
    Cue.Editor.ImGui
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Editor.ImGui
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Editor.ImGui PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
