add_library(Cue.ProjectHub.Windows STATIC)

target_sources(
    Cue.ProjectHub.Windows
    PRIVATE
        Private/WindowsEditorProcess.cpp
        Private/WindowsProjectHubPlatform.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/ProjectHub/Windows/WindowsProjectHubPlatform.h
)

target_link_libraries(
    Cue.ProjectHub.Windows
    PUBLIC
        Cue.ProjectHub
    PRIVATE
        Cue.Distribution.Windows
        Cue.Foundation.Windows
        Cue.IO.Windows
        bcrypt
        shell32
)
target_compile_features(Cue.ProjectHub.Windows PUBLIC cxx_std_20)
target_include_directories(Cue.ProjectHub.Windows PUBLIC Public)
target_compile_definitions(Cue.ProjectHub.Windows PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(
    Cue.ProjectHub.Windows
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.ProjectHub.Windows
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.ProjectHub.Windows PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
