add_library(Cue.Package STATIC)

target_sources(
    Cue.Package
    PRIVATE
        Private/Error.cpp
        Private/Manifest.cpp
        Private/ManifestVerification.h
        Private/PeDependency.cpp
        Private/Publisher.cpp
        Private/PublisherPath.h
        Private/RuntimeData.cpp
        Private/Sha256.cpp
        Private/Sha256.h
        Private/Workflow.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/Package/Error.h
            Public/Cue/Package/Manifest.h
            Public/Cue/Package/Publisher.h
            Public/Cue/Package/RuntimeData.h
            Public/Cue/Package/Workflow.h
)

target_compile_features(Cue.Package PUBLIC cxx_std_20)
target_link_libraries(Cue.Package PUBLIC Cue.Build Cue.Foundation Cue.IO Cue.Project Cue.Scene PRIVATE Cue.Renderer)
if(WIN32)
    target_link_libraries(Cue.Package PRIVATE Cue.IO.Windows)
    target_compile_definitions(Cue.Package PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_include_directories(Cue.Package PUBLIC Public)

set_target_properties(
    Cue.Package
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.Package
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.Package PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

get_target_property(cuePackageLinks Cue.Package LINK_LIBRARIES)
get_target_property(cuePackageInterfaceLinks Cue.Package INTERFACE_LINK_LIBRARIES)

set(cuePackageExpectedLinks Cue.Build Cue.Foundation Cue.IO Cue.Project Cue.Scene Cue.Renderer)
set(cuePackageExpectedInterfaceLinks Cue.Build Cue.Foundation Cue.IO Cue.Project Cue.Scene "$<LINK_ONLY:Cue.Renderer>")
if(WIN32)
    list(APPEND cuePackageExpectedLinks Cue.IO.Windows)
    list(APPEND cuePackageExpectedInterfaceLinks "$<LINK_ONLY:Cue.IO.Windows>")
endif()

if(NOT "${cuePackageLinks}" STREQUAL "${cuePackageExpectedLinks}")
    message(FATAL_ERROR "Cue.Package has unexpected direct dependencies: ${cuePackageLinks}")
endif()

if(NOT "${cuePackageInterfaceLinks}" STREQUAL "${cuePackageExpectedInterfaceLinks}")
    message(FATAL_ERROR "Cue.Package must expose only its declared Runtime Data dependencies: ${cuePackageInterfaceLinks}")
endif()
