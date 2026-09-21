add_library(Cue.EditorCore STATIC)

target_sources(
    Cue.EditorCore
    PRIVATE
        Private/EditorController.cpp
        Private/EditorDocument.cpp
        Private/EditorIntent.cpp
        Private/EditorPlaySessionController.cpp
        Private/Error.cpp
        Private/FilesWorkspace.cpp
        Private/Persistence.cpp
        Private/SceneCommand.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/EditorCore/EditorController.h
            Public/Cue/EditorCore/EditorDocument.h
            Public/Cue/EditorCore/EditorIntent.h
            Public/Cue/EditorCore/EditorPlaySessionController.h
            Public/Cue/EditorCore/Error.h
            Public/Cue/EditorCore/FilesWorkspace.h
            Public/Cue/EditorCore/Persistence.h
            Public/Cue/EditorCore/SceneCommand.h
)

target_compile_features(Cue.EditorCore PUBLIC cxx_std_20)
target_link_libraries(
    Cue.EditorCore
    PUBLIC Cue.Foundation Cue.IO Cue.Project Cue.ProjectFiles Cue.Scene Cue.Schema Cue.Input Cue.Runtime
    PRIVATE Cue.EngineAssets Cue.Renderer
)
target_include_directories(Cue.EditorCore PUBLIC Public)

set_target_properties(
    Cue.EditorCore
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.EditorCore
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.EditorCore PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

get_target_property(cueEditorCoreLinks Cue.EditorCore LINK_LIBRARIES)
get_target_property(cueEditorCoreInterfaceLinks Cue.EditorCore INTERFACE_LINK_LIBRARIES)

set(
    cueEditorCoreExpectedLinks
    Cue.Foundation
    Cue.IO
    Cue.Project
    Cue.ProjectFiles
    Cue.Scene
    Cue.Schema
    Cue.Input
    Cue.Runtime
    Cue.EngineAssets
    Cue.Renderer
)

if(NOT "${cueEditorCoreLinks}" STREQUAL "${cueEditorCoreExpectedLinks}")
    message(
        FATAL_ERROR
        "Cue.EditorCore has unexpected direct dependencies: "
        "${cueEditorCoreLinks}"
    )
endif()

set(
    cueEditorCoreExpectedInterfaceLinks
    Cue.Foundation
    Cue.IO
    Cue.Project
    Cue.ProjectFiles
    Cue.Scene
    Cue.Schema
    Cue.Input
    Cue.Runtime
    "$<LINK_ONLY:Cue.EngineAssets>"
    "$<LINK_ONLY:Cue.Renderer>"
)

if(NOT "${cueEditorCoreInterfaceLinks}" STREQUAL "${cueEditorCoreExpectedInterfaceLinks}")
    message(
        FATAL_ERROR
        "Cue.EditorCore has unexpected interface dependencies: "
        "${cueEditorCoreInterfaceLinks}"
    )
endif()
