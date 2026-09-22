add_library(Cue.Distribution STATIC)

target_sources(
    Cue.Distribution
    PRIVATE
        Private/Error.cpp
        Private/Identity.cpp
        Private/InstallState.cpp
        Private/Manifest.cpp
        Private/Publisher.cpp
        Private/Sha256.cpp
        Private/Sha256.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Distribution/Error.h
            Public/Cue/Distribution/Identity.h
            Public/Cue/Distribution/InstallState.h
            Public/Cue/Distribution/Manifest.h
            Public/Cue/Distribution/Publisher.h
)

target_compile_features(Cue.Distribution PUBLIC cxx_std_20)
target_link_libraries(Cue.Distribution PUBLIC Cue.Foundation)
target_include_directories(Cue.Distribution PUBLIC Public)

set_target_properties(
    Cue.Distribution
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Distribution
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Distribution PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

get_target_property(cueDistributionLinks Cue.Distribution LINK_LIBRARIES)
get_target_property(cueDistributionInterfaceLinks Cue.Distribution INTERFACE_LINK_LIBRARIES)

if(NOT "${cueDistributionLinks}" STREQUAL "Cue.Foundation")
    message(FATAL_ERROR "Cue.Distribution has unexpected direct dependencies: ${cueDistributionLinks}")
endif()

if(NOT "${cueDistributionInterfaceLinks}" STREQUAL "Cue.Foundation")
    message(FATAL_ERROR "Cue.Distribution must expose only Cue.Foundation: ${cueDistributionInterfaceLinks}")
endif()
