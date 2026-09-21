if(NOT CONFIGURATION STREQUAL "Release")
    message("Skipping Release-only Static Runtime Scene Package probe for ${CONFIGURATION}")
    return()
endif()
if(NOT DEFINED TEST_EXECUTABLE OR NOT EXISTS "${TEST_EXECUTABLE}")
    message(FATAL_ERROR "TEST_EXECUTABLE must identify the Static Scene Product")
endif()
if(NOT DEFINED TEST_ROOT OR TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "TEST_ROOT is required")
endif()

set(projectId "41234567-89ab-4cde-8f01-23456789abcd")
set(sceneId "51234567-89ab-4cde-8f01-23456789abcd")
set(packageRoot "${TEST_ROOT}/RelocatedStaticPackage")
set(workingRoot "${TEST_ROOT}/UnrelatedWorkingDirectory")
set(sceneRelativePath "Data/Scenes/${sceneId}.cueruntime.json")
set(projectPath "${packageRoot}/Data/CueProject.runtime.json")
set(scenePath "${packageRoot}/${sceneRelativePath}")
string(ASCII 10 runtimeLf)
function(write_runtime_data path content)
    file(CONFIGURE OUTPUT "${path}" CONTENT "${content}" @ONLY NEWLINE_STYLE UNIX)
endfunction()
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${packageRoot}/Data/Scenes" "${workingRoot}")
file(COPY_FILE "${TEST_EXECUTABLE}" "${packageRoot}/CueGameProduct.exe" ONLY_IF_DIFFERENT)

write_runtime_data("${projectPath}"
    "{\"schemaVersion\":1,\"projectId\":\"${projectId}\",\"engineCompatibility\":{\"minimum\":\"1.0.0\",\"maximumExclusive\":\"2.0.0\"},\"requiredCapabilities\":[],\"startupSceneAssetId\":\"${sceneId}\"}${runtimeLf}")
write_runtime_data("${scenePath}"
    "{\"schemaVersion\":2,\"sceneAssetId\":\"${sceneId}\",\"objects\":[{\"objectId\":\"20000000-0000-4000-8000-000000000001\",\"parentObjectId\":null,\"active\":true,\"transform\":{\"translation\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]},\"components\":[{\"instanceId\":\"30000000-0000-4000-8000-000000000002\",\"typeId\":\"70000000-0000-4000-8000-000000000002\",\"schemaVersion\":1,\"fields\":[{\"fieldId\":1,\"value\":\"cue://engine/mesh/cube\"}]}]},{\"objectId\":\"20000000-0000-4000-8000-000000000002\",\"parentObjectId\":null,\"active\":true,\"transform\":{\"translation\":[0,1,-5],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]},\"components\":[{\"instanceId\":\"30000000-0000-4000-8000-000000000001\",\"typeId\":\"70000000-0000-4000-8000-000000000001\",\"schemaVersion\":1,\"fields\":[{\"fieldId\":1,\"value\":true},{\"fieldId\":2,\"value\":60},{\"fieldId\":3,\"value\":0.1},{\"fieldId\":4,\"value\":1000}]}]}]}${runtimeLf}")

file(SIZE "${packageRoot}/CueGameProduct.exe" executableSize)
file(SHA256 "${packageRoot}/CueGameProduct.exe" executableHash)
file(SIZE "${projectPath}" projectSize)
file(SHA256 "${projectPath}" projectHash)
file(SIZE "${scenePath}" sceneSize)
file(SHA256 "${scenePath}" sceneHash)
file(WRITE "${packageRoot}/CuePackage.json"
    "{\"schemaVersion\":2,\"projectId\":\"${projectId}\",\"engineVersion\":\"1.0.0\",\"architecture\":\"x64\",\"configuration\":\"Release\",\"executionModel\":\"monolithic\",\"startupScene\":{\"sceneAssetId\":\"${sceneId}\",\"runtimeDataPath\":\"${sceneRelativePath}\"},\"applicationExecutable\":\"CueGameProduct.exe\",\"trust\":{\"mode\":\"UnsignedLocal\",\"publisherKeyId\":null,\"manifestSignaturePath\":null},\"files\":[{\"role\":\"applicationExecutable\",\"path\":\"CueGameProduct.exe\",\"sizeBytes\":${executableSize},\"sha256\":\"${executableHash}\"},{\"role\":\"projectRuntimeData\",\"path\":\"Data/CueProject.runtime.json\",\"sizeBytes\":${projectSize},\"sha256\":\"${projectHash}\"},{\"role\":\"startupSceneRuntimeData\",\"path\":\"${sceneRelativePath}\",\"sizeBytes\":${sceneSize},\"sha256\":\"${sceneHash}\"}]}\n")

execute_process(
    COMMAND "${packageRoot}/CueGameProduct.exe" --package-scene-smoke-test warp
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE productResult
    OUTPUT_VARIABLE productOutput
    ERROR_VARIABLE productError
    TIMEOUT 20
)
set(combinedOutput "${productOutput}\n${productError}")
foreach(requiredMessage IN ITEMS
    "Runtime Render Snapshot: MainCamera=Ready, MeshCount=1"
    "Runtime Presentation Frame: Mode=Scene, CubeCount=1"
    "Runtime Package Scene Pixel Probe: Passed"
)
    string(FIND "${combinedOutput}" "${requiredMessage}" messagePosition)
    if(messagePosition EQUAL -1)
        message(FATAL_ERROR "Static Runtime Scene output is missing: ${requiredMessage}\n${combinedOutput}")
    endif()
endforeach()
if(NOT productResult EQUAL 0)
    message(FATAL_ERROR "Static Runtime Scene Product exited with ${productResult}\n${combinedOutput}")
endif()

# Runtime Data改ざんはGame Module接続前のInventory検証で拒否する
file(APPEND "${scenePath}" "x")
execute_process(
    COMMAND "${packageRoot}/CueGameProduct.exe" --package-scene-smoke-test warp
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE tamperedResult
    OUTPUT_VARIABLE tamperedOutput
    ERROR_VARIABLE tamperedError
    TIMEOUT 20
)
set(tamperedCombined "${tamperedOutput}\n${tamperedError}")
string(FIND "${tamperedCombined}" "Package file size or SHA-256 differs" tamperedPosition)
if(tamperedResult EQUAL 0 OR tamperedPosition EQUAL -1)
    message(FATAL_ERROR "Static Runtime Scene tamper was not rejected\n${tamperedCombined}")
endif()

if(DEFINED DUMPBIN AND EXISTS "${DUMPBIN}")
    execute_process(
        COMMAND "${DUMPBIN}" /DEPENDENTS "${packageRoot}/CueGameProduct.exe"
        RESULT_VARIABLE dumpbinResult
        OUTPUT_VARIABLE dumpbinOutput
        ERROR_VARIABLE dumpbinError
    )
    if(NOT dumpbinResult EQUAL 0)
        message(FATAL_ERROR "dumpbin failed for Static Product\n${dumpbinOutput}\n${dumpbinError}")
    endif()

    string(REGEX MATCHALL "[A-Za-z0-9_.-]+\\.[Dd][Ll][Ll]" importedLibraries "${dumpbinOutput}")
    list(TRANSFORM importedLibraries TOLOWER)
    list(REMOVE_DUPLICATES importedLibraries)
    set(allowedLibraries
        "api-ms-win-crt-heap-l1-1-0.dll"
        "api-ms-win-crt-locale-l1-1-0.dll"
        "api-ms-win-crt-math-l1-1-0.dll"
        "api-ms-win-crt-runtime-l1-1-0.dll"
        "api-ms-win-crt-stdio-l1-1-0.dll"
        "api-ms-win-crt-string-l1-1-0.dll"
        "bcrypt.dll"
        "d3d12.dll"
        "dxgi.dll"
        "kernel32.dll"
        "msvcp140.dll"
        "ucrtbase.dll"
        "user32.dll"
        "vcruntime140.dll"
        "vcruntime140_1.dll"
    )
    if(NOT importedLibraries)
        message(FATAL_ERROR "dumpbin did not report any Static Product imports\n${dumpbinOutput}")
    endif()
    foreach(importedLibrary IN LISTS importedLibraries)
        list(FIND allowedLibraries "${importedLibrary}" allowedLibraryIndex)
        if(allowedLibraryIndex EQUAL -1)
            message(FATAL_ERROR
                "Static Product imports unapproved dependency ${importedLibrary}\n${dumpbinOutput}")
        endif()
    endforeach()
endif()
