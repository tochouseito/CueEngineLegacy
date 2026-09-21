add_library(Cue.EngineAssets STATIC)

target_sources(
    Cue.EngineAssets
    PRIVATE
        Private/BuiltInAssetCatalog.cpp
        Private/BuiltInAuthoringMeshCatalog.cpp
        Private/CubeMesh.cpp
        Private/Error.cpp
        Private/PlaneMesh.cpp
        Private/SphereMesh.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS Public
        FILES
            Public/Cue/EngineAssets/BuiltInAssetCatalog.h
            Public/Cue/EngineAssets/BuiltInMesh.h
            Public/Cue/EngineAssets/Error.h
)

target_compile_features(Cue.EngineAssets PUBLIC cxx_std_20)
target_link_libraries(Cue.EngineAssets PUBLIC Cue.Foundation Cue.Math)
target_include_directories(Cue.EngineAssets PUBLIC Public)

set_target_properties(
    Cue.EngineAssets
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
        CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(
        Cue.EngineAssets
        PRIVATE
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
            $<$<CONFIG:Development>:/O2>
            $<$<CONFIG:Development>:/Zi>
    )
    target_link_options(Cue.EngineAssets PRIVATE $<$<CONFIG:Development>:/DEBUG>)
endif()

get_target_property(cueEngineAssetsLinks Cue.EngineAssets LINK_LIBRARIES)
get_target_property(cueEngineAssetsInterfaceLinks Cue.EngineAssets INTERFACE_LINK_LIBRARIES)

if(NOT "${cueEngineAssetsLinks}" STREQUAL "Cue.Foundation;Cue.Math")
    message(FATAL_ERROR "Cue.EngineAssets must directly link only Cue.Foundation and Cue.Math: ${cueEngineAssetsLinks}")
endif()

if(NOT "${cueEngineAssetsInterfaceLinks}" STREQUAL "Cue.Foundation;Cue.Math")
    message(FATAL_ERROR
        "Cue.EngineAssets must expose only Cue.Foundation and Cue.Math: ${cueEngineAssetsInterfaceLinks}"
    )
endif()
