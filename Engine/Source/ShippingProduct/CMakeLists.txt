# Generated ProjectからRelease Monolithic Productに必要なEngine Targetだけを公開する
if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Cue Shipping Product initially supports Windows x64 with MSVC only")
endif()

# Shipping Link Closure内の全Engine SourceをCFGとStack Security Check付きでCompileする。
add_compile_options(/GS /guard:cf)

function(cue_add_shipping_engine_module relativePath)
    string(REPLACE "/" "-" binaryName "${relativePath}")
    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../${relativePath}"
        "${CMAKE_CURRENT_BINARY_DIR}/Engine/${binaryName}"
        EXCLUDE_FROM_ALL
    )
endfunction()

cue_add_shipping_engine_module(Foundation)
cue_add_shipping_engine_module(GameModule)
cue_add_shipping_engine_module(Schema)
cue_add_shipping_engine_module(Math)
cue_add_shipping_engine_module(EngineAssets)
cue_add_shipping_engine_module(GameCore)
cue_add_shipping_engine_module(IO)
cue_add_shipping_engine_module(Project)
cue_add_shipping_engine_module(Scene)
cue_add_shipping_engine_module(Platform)
cue_add_shipping_engine_module(Build)
cue_add_shipping_engine_module(Input)
cue_add_shipping_engine_module(Runtime)
cue_add_shipping_engine_module(Renderer)
cue_add_shipping_engine_module(RHI)
cue_add_shipping_engine_module(Package)

set(CUE_RUNTIME_HOST_BUILD_DYNAMIC OFF)
cue_add_shipping_engine_module(RuntimeHost)

# Add the Engine-owned monolithic product entry around one project Static Game Module.
function(cue_add_shipping_product targetName gameModuleTarget projectId)
    if(NOT CUE_GAME_CONFIGURATION STREQUAL "Release")
        message(FATAL_ERROR "Cue Shipping Product target is Release-only")
    endif()
    if(NOT TARGET "${gameModuleTarget}")
        message(FATAL_ERROR "Cue Shipping Product Game Module target does not exist: ${gameModuleTarget}")
    endif()
    string(REGEX MATCH "(^|,)version=([^,]+)" cueShippingToolsetMatch "${CMAKE_GENERATOR_TOOLSET}")
    set(cueShippingToolsetVersion "${CMAKE_MATCH_2}")
    file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" cueShippingCompilerPath)
    string(REGEX MATCH "/VC/Tools/MSVC/([^/]+)/bin/" cueShippingCompilerToolsetMatch "${cueShippingCompilerPath}")
    set(cueShippingCompilerToolsetVersion "${CMAKE_MATCH_1}")
    if(cueShippingCompilerToolsetMatch STREQUAL "" OR cueShippingCompilerToolsetVersion STREQUAL "")
        message(FATAL_ERROR "Cue Shipping Product could not identify the selected Visual Studio toolset version")
    endif()
    if(NOT cueShippingToolsetMatch STREQUAL "" AND
       NOT cueShippingToolsetVersion STREQUAL cueShippingCompilerToolsetVersion)
        message(FATAL_ERROR "Cue Shipping Product Visual Studio toolset selection is inconsistent")
    endif()

    add_executable(
        "${targetName}"
        EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StaticProductMain.cpp"
    )
    set_property(TARGET "${targetName}" PROPERTY VS_GLOBAL_VCToolsVersion "${cueShippingCompilerToolsetVersion}")
    target_link_libraries("${targetName}" PRIVATE "${gameModuleTarget}" Cue.RuntimeHost.Static)
    target_compile_features("${targetName}" PRIVATE cxx_std_20)
    target_compile_definitions(
        "${targetName}"
        PRIVATE
            "CUE_GAME_PRODUCT_PROJECT_ID=\"${projectId}\""
            CUE_GAME_PRODUCT_MINIMUM_TRUST_MODE=1
            "CUE_GAME_PRODUCT_PUBLISHER_KEY_ID=\"\""
    )
    set_target_properties(
        "${targetName}"
        PROPERTIES
            CXX_EXTENSIONS OFF
            MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
            OUTPUT_NAME "CueGameProduct"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
            PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )
    target_compile_options(
        "${targetName}"
        PRIVATE
            /GS
            /guard:cf
            /W4
            /WX
            /permissive-
            /Zc:__cplusplus
            /utf-8
    )
    target_compile_options("${gameModuleTarget}" PRIVATE /GS /guard:cf)
    target_link_options(
        "${targetName}"
        PRIVATE
            /DYNAMICBASE
            /HIGHENTROPYVA
            /NXCOMPAT
            /GUARD:CF
            /CETCOMPAT
            /DEPENDENTLOADFLAG:0x800
    )
endfunction()
