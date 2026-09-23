add_library(Cue.Distribution.Windows STATIC)

target_sources(
    Cue.Distribution.Windows
    PRIVATE
        Private/Installer.cpp
        Private/SourceSdkPublisher.cpp
        Private/WindowsDeveloperPrerequisites.cpp
        Private/WindowsDeveloperPrerequisites.h
        Private/WindowsDirectoryAncestryLock.cpp
        Private/WindowsDirectoryAncestryLock.h
        Private/WindowsStablePath.cpp
        Private/WindowsStablePath.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Distribution/Windows/Installer.h
            Public/Cue/Distribution/Windows/SourceSdkPublisher.h
)

target_link_libraries(
    Cue.Distribution.Windows
    PUBLIC
        Cue.Distribution
        Cue.Platform
    PRIVATE
        Cue.IO.Windows
        Cue.Platform.Windows
        advapi32
        ole32
        version
)
target_compile_features(Cue.Distribution.Windows PUBLIC cxx_std_20)
target_include_directories(Cue.Distribution.Windows PUBLIC Public)
target_compile_definitions(Cue.Distribution.Windows PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(
    Cue.Distribution.Windows
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Distribution.Windows
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Distribution.Windows PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()
