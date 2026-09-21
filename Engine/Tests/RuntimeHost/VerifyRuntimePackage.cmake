if(NOT DEFINED TEST_EXECUTABLE OR NOT EXISTS "${TEST_EXECUTABLE}")
    message(FATAL_ERROR "TEST_EXECUTABLE must identify CueRuntimeHost")
endif()
if(NOT DEFINED MODULE_LIBRARY OR NOT EXISTS "${MODULE_LIBRARY}")
    message(FATAL_ERROR "MODULE_LIBRARY must identify the test Game Module")
endif()
if(NOT DEFINED DEPENDENCY_LIBRARY OR NOT EXISTS "${DEPENDENCY_LIBRARY}")
    message(FATAL_ERROR "DEPENDENCY_LIBRARY must identify the App-local dependency probe")
endif()
if(NOT DEFINED TEST_ROOT OR TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "TEST_ROOT is required")
endif()
if(NOT CONFIGURATION MATCHES "^(Debug|Development|Release)$")
    message(FATAL_ERROR "CONFIGURATION is invalid: ${CONFIGURATION}")
endif()
if(NOT DEFINED COMPILER_VERSION OR NOT DEFINED COMPILER_FULL_VERSION OR NOT DEFINED COMPILER_BUILD)
    message(FATAL_ERROR "MSVC version values are required")
endif()

set(projectId "41234567-89ab-4cde-8f01-23456789abcd")
set(sceneId "51234567-89ab-4cde-8f01-23456789abcd")
set(artifactId "61234567-89ab-4cde-8f01-23456789abcd")
string(ASCII 10 runtimeLf)
function(write_runtime_data path content)
    file(CONFIGURE OUTPUT "${path}" CONTENT "${content}" @ONLY NEWLINE_STYLE UNIX)
endfunction()
set(stagingRoot "${TEST_ROOT}/BuiltPackage")
set(packageRoot "${TEST_ROOT}/RelocatedPackage")
set(workingRoot "${TEST_ROOT}/UnrelatedWorkingDirectory")
set(projectPath "${stagingRoot}/Data/CueProject.runtime.json")
set(sceneRelativePath "Data/Scenes/${sceneId}.cueruntime.json")
set(scenePath "${stagingRoot}/${sceneRelativePath}")
set(metadataPath "${stagingRoot}/Game/CueGameModule.metadata.json")

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${stagingRoot}/Data/Scenes" "${stagingRoot}/Game" "${stagingRoot}/Runtime" "${workingRoot}")
file(WRITE "${workingRoot}/CuePackage.json" "{\"schemaVersion\":999}\n")
file(COPY_FILE "${TEST_EXECUTABLE}" "${stagingRoot}/CueRuntimeHost.exe" ONLY_IF_DIFFERENT)
file(COPY_FILE "${MODULE_LIBRARY}" "${stagingRoot}/Game/CueGameModule.dll" ONLY_IF_DIFFERENT)
file(COPY_FILE "${DEPENDENCY_LIBRARY}" "${stagingRoot}/Runtime/ProbeDependency.dll" ONLY_IF_DIFFERENT)

write_runtime_data("${projectPath}"
    "{\"schemaVersion\":1,\"projectId\":\"${projectId}\",\"engineCompatibility\":{\"minimum\":\"1.0.0\",\"maximumExclusive\":\"2.0.0\"},\"requiredCapabilities\":[],\"startupSceneAssetId\":\"${sceneId}\"}${runtimeLf}")
set(canonicalV1Scene
    "{\"schemaVersion\":1,\"sceneAssetId\":\"${sceneId}\",\"objects\":[]}${runtimeLf}")
write_runtime_data("${scenePath}" "${canonicalV1Scene}")

if(CONFIGURATION STREQUAL "Debug")
    set(runtimeLibrary "DebugDll")
    set(iteratorDebugLevel 2)
else()
    set(runtimeLibrary "Dll")
    set(iteratorDebugLevel 0)
endif()
math(EXPR compatibleFullVersion "${COMPILER_FULL_VERSION} + 1")
math(EXPR compatibleBuild "${COMPILER_BUILD} + 1")
file(WRITE "${metadataPath}"
    "{\n    \"schemaVersion\": 1,\n    \"artifactId\": \"${artifactId}\",\n    \"projectId\": \"${projectId}\",\n    \"engineCompatibility\": {\n        \"minimum\": \"1.0.0\",\n        \"maximumExclusive\": \"2.0.0\"\n    },\n    \"abiVersion\": 1,\n    \"configuration\": \"${CONFIGURATION}\",\n    \"architecture\": \"x64\",\n    \"compilerFamily\": \"msvc\",\n    \"msvcToolset\": {\n        \"compilerVersion\": ${COMPILER_VERSION},\n        \"fullVersion\": ${compatibleFullVersion},\n        \"build\": ${compatibleBuild}\n    },\n    \"runtimeLibrary\": \"${runtimeLibrary}\",\n    \"iteratorDebugLevel\": ${iteratorDebugLevel},\n    \"moduleFile\": \"CueGameModule.dll\",\n    \"entrySymbol\": \"cue_game_module_query\"\n}\n")

set(paths
    "CueRuntimeHost.exe"
    "Data/CueProject.runtime.json"
    "${sceneRelativePath}"
    "Game/CueGameModule.dll"
    "Game/CueGameModule.metadata.json"
    "Runtime/ProbeDependency.dll"
)
set(roles
    "runtimeHost"
    "projectRuntimeData"
    "startupSceneRuntimeData"
    "gameModule"
    "gameModuleMetadata"
    "runtimeDependency"
)
function(write_package_manifest packageDirectory projectSizeOverride metadataSizeOverride)
    if(ARGC GREATER 3)
        set(moduleSizeOverride "${ARGV3}")
    else()
        set(moduleSizeOverride "")
    endif()
    set(filesJson "")
    list(LENGTH paths fileCount)
    math(EXPR lastFileIndex "${fileCount} - 1")
    foreach(index RANGE 0 ${lastFileIndex})
        list(GET paths ${index} relativePath)
        list(GET roles ${index} role)
        set(absolutePath "${packageDirectory}/${relativePath}")
        file(SIZE "${absolutePath}" sizeBytes)
        if(role STREQUAL "projectRuntimeData" AND NOT projectSizeOverride STREQUAL "")
            set(sizeBytes "${projectSizeOverride}")
        elseif(role STREQUAL "gameModuleMetadata" AND NOT metadataSizeOverride STREQUAL "")
            set(sizeBytes "${metadataSizeOverride}")
        elseif(role STREQUAL "gameModule" AND NOT moduleSizeOverride STREQUAL "")
            set(sizeBytes "${moduleSizeOverride}")
        endif()
        file(SHA256 "${absolutePath}" sha256)
        if(NOT filesJson STREQUAL "")
            string(APPEND filesJson ",")
        endif()
        string(APPEND filesJson
            "{\"role\":\"${role}\",\"path\":\"${relativePath}\",\"sizeBytes\":${sizeBytes},\"sha256\":\"${sha256}\"}")
    endforeach()
    file(WRITE "${packageDirectory}/CuePackage.json"
        "{\"schemaVersion\":1,\"projectId\":\"${projectId}\",\"engineVersion\":\"1.0.0\",\"configuration\":\"${CONFIGURATION}\",\"startupScene\":{\"sceneAssetId\":\"${sceneId}\",\"runtimeDataPath\":\"${sceneRelativePath}\"},\"files\":[${filesJson}]}\n")
endfunction()

write_package_manifest("${stagingRoot}" "" "")

file(RENAME "${stagingRoot}" "${packageRoot}")
# KnownDLLではないLoad-time Importを偽装し、Application Directory探索の回帰を検出する
file(WRITE "${packageRoot}/d3d12.dll" "unverified app-local system dependency")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE packageResult
    OUTPUT_VARIABLE packageOutput
    ERROR_VARIABLE packageError
    TIMEOUT 15
)
set(combinedOutput "${packageOutput}\n${packageError}")
if(NOT packageResult EQUAL 0)
    message(FATAL_ERROR "Relocated Runtime Package exited with ${packageResult}\n${combinedOutput}")
endif()
foreach(requiredMessage IN ITEMS
    "D3D12 Render Loop ready:"
    "Runtime Application Session started: Generation=1, WorldId="
    "Runtime Presentation Frame: Mode=DiagnosticClear, CubeCount=0"
    "Runtime Application Session stopped: Reason=WindowClosed, FrameCount=1"
    "D3D12 Render Loop completed: FrameCount=1"
    "D3D12 Render Loop shutdown completed"
)
    string(FIND "${combinedOutput}" "${requiredMessage}" messagePosition)
    if(messagePosition EQUAL -1)
        message(FATAL_ERROR "Relocated Runtime Package output is missing: ${requiredMessage}\n${combinedOutput}")
    endif()
endforeach()

# v2 Componentを接続したStandalone CompositionのCamera選択と再起動を実Processで検証する
set(scenePackagePath "${packageRoot}/${sceneRelativePath}")
set(cameraComponent
    "{\"instanceId\":\"30000000-0000-4000-8000-000000000001\",\"typeId\":\"70000000-0000-4000-8000-000000000001\",\"schemaVersion\":1,\"fields\":[{\"fieldId\":1,\"value\":true},{\"fieldId\":2,\"value\":60},{\"fieldId\":3,\"value\":0.1},{\"fieldId\":4,\"value\":1000}]}"
)
set(secondCameraComponent
    "{\"instanceId\":\"30000000-0000-4000-8000-000000000003\",\"typeId\":\"70000000-0000-4000-8000-000000000001\",\"schemaVersion\":1,\"fields\":[{\"fieldId\":1,\"value\":true},{\"fieldId\":2,\"value\":60},{\"fieldId\":3,\"value\":0.1},{\"fieldId\":4,\"value\":1000}]}"
)
set(meshComponent
    "{\"instanceId\":\"30000000-0000-4000-8000-000000000002\",\"typeId\":\"70000000-0000-4000-8000-000000000002\",\"schemaVersion\":1,\"fields\":[{\"fieldId\":1,\"value\":\"cue://engine/mesh/cube\"}]}"
)
set(objectPrefix
    "{\"objectId\":\"20000000-0000-4000-8000-000000000001\",\"parentObjectId\":null,\"active\":true,\"transform\":{\"translation\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]},\"components\":["
)
set(secondObject
    "{\"objectId\":\"20000000-0000-4000-8000-000000000002\",\"parentObjectId\":null,\"active\":true,\"transform\":{\"translation\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]},\"components\":[${secondCameraComponent}]}"
)
set(missingCameraScene
    "{\"schemaVersion\":2,\"sceneAssetId\":\"${sceneId}\",\"objects\":[${objectPrefix}${meshComponent}]}]}${runtimeLf}"
)
set(readyCameraScene
    "{\"schemaVersion\":2,\"sceneAssetId\":\"${sceneId}\",\"objects\":[${objectPrefix}${cameraComponent},${meshComponent}]}]}${runtimeLf}"
)
set(multipleCameraScene
    "{\"schemaVersion\":2,\"sceneAssetId\":\"${sceneId}\",\"objects\":[${objectPrefix}${cameraComponent},${meshComponent}]},${secondObject}]}${runtimeLf}"
)
function(verify_render_snapshot_scene sceneBytes expectedStatus expectedPresentationMode)
    write_runtime_data("${scenePackagePath}" "${sceneBytes}")
    write_package_manifest("${packageRoot}" "" "")
    execute_process(
        COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
        WORKING_DIRECTORY "${workingRoot}"
        RESULT_VARIABLE sceneResult
        OUTPUT_VARIABLE sceneOutput
        ERROR_VARIABLE sceneError
        TIMEOUT 15
    )
    set(sceneCombined "${sceneOutput}\n${sceneError}")
    string(FIND "${sceneCombined}" "Runtime Render Snapshot: MainCamera=${expectedStatus}, MeshCount=1"
        snapshotPosition)
    string(FIND "${sceneCombined}"
        "Runtime Presentation Frame: Mode=${expectedPresentationMode}, CubeCount=1"
        presentationPosition)
    string(FIND "${sceneCombined}" "Runtime Application Session stopped: Reason=WindowClosed, FrameCount=1"
        stopPosition)
    if(NOT sceneResult EQUAL 0 OR snapshotPosition EQUAL -1 OR presentationPosition EQUAL -1 OR stopPosition EQUAL -1)
        message(FATAL_ERROR "Runtime Scene v2 composition failed for ${expectedStatus}\n${sceneCombined}")
    endif()
endfunction()
verify_render_snapshot_scene("${missingCameraScene}" "Missing" "DiagnosticClear")
verify_render_snapshot_scene("${readyCameraScene}" "Ready" "Scene")
verify_render_snapshot_scene("${multipleCameraScene}" "Multiple" "DiagnosticClear")
verify_render_snapshot_scene("${readyCameraScene}" "Ready" "Scene")
write_runtime_data("${scenePackagePath}" "${readyCameraScene}")
write_package_manifest("${packageRoot}" "" "")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env CUE_RUNTIME_PACKAGE_PROBE_MODE=system-start-failure
        "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE failedStartResult
    OUTPUT_VARIABLE failedStartOutput
    ERROR_VARIABLE failedStartError
    TIMEOUT 15
)
set(failedStartCombined "${failedStartOutput}\n${failedStartError}")
string(FIND "${failedStartCombined}" "Runtime Host failed to start Runtime Application" failedStartPosition)
if(NOT failedStartResult EQUAL 14 OR failedStartPosition EQUAL -1)
    message(FATAL_ERROR "Renderer and Game Module start failure did not rollback safely\n${failedStartCombined}")
endif()
verify_render_snapshot_scene("${readyCameraScene}" "Ready" "Scene")
write_runtime_data("${scenePackagePath}" "${canonicalV1Scene}")
write_package_manifest("${packageRoot}" "" "")

# v2 Reader拒否と共通Manifest Size／Hash境界をGame Module接続前に検証する
set(invalidV2Scene
    "{\"schemaVersion\":2,\"sceneAssetId\":\"${sceneId}\",\"objects\":[{\"objectId\":\"20000000-0000-4000-8000-000000000001\",\"parentObjectId\":null,\"active\":true,\"transform\":{\"translation\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]},\"components\":[{\"instanceId\":\"30000000-0000-4000-8000-000000000001\",\"typeId\":\"40000000-0000-4000-8000-000000000001\",\"schemaVersion\":1,\"fields\":[{\"fieldId\":1,\"value\":\"cue://engine/mesh/cube\"}]}]}]}${runtimeLf}")
write_runtime_data("${scenePackagePath}" "${invalidV2Scene}")
write_package_manifest("${packageRoot}" "" "")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE invalidV2Result
    OUTPUT_VARIABLE invalidV2Output
    ERROR_VARIABLE invalidV2Error
    TIMEOUT 15
)
set(invalidV2Combined "${invalidV2Output}\n${invalidV2Error}")
string(FIND "${invalidV2Combined}" "unsupported component type" invalidV2Position)
if(invalidV2Result EQUAL 0 OR invalidV2Position EQUAL -1)
    message(FATAL_ERROR "Unknown Runtime Scene v2 component was not rejected before Game Module startup\n${invalidV2Combined}")
endif()
file(APPEND "${scenePackagePath}" "x")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE tamperedV2Result
    OUTPUT_VARIABLE tamperedV2Output
    ERROR_VARIABLE tamperedV2Error
    TIMEOUT 15
)
set(tamperedV2Combined "${tamperedV2Output}\n${tamperedV2Error}")
string(FIND "${tamperedV2Combined}" "Package file size or SHA-256 differs" tamperedV2Position)
if(tamperedV2Result EQUAL 0 OR tamperedV2Position EQUAL -1)
    message(FATAL_ERROR "Runtime Scene v2 size or hash tamper was not rejected\n${tamperedV2Combined}")
endif()
write_runtime_data("${scenePackagePath}" "${canonicalV1Scene}")
write_package_manifest("${packageRoot}" "" "")

set(unicodePackageRoot "${TEST_ROOT}/RelocatedPackage-日本語-😀")
file(RENAME "${packageRoot}" "${unicodePackageRoot}")
execute_process(
    COMMAND "${unicodePackageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE unicodePackageResult
    OUTPUT_VARIABLE unicodePackageOutput
    ERROR_VARIABLE unicodePackageError
    TIMEOUT 15
)
if(NOT unicodePackageResult EQUAL 0)
    message(FATAL_ERROR
        "Runtime Package failed from a non-ACP Unicode path\n${unicodePackageOutput}\n${unicodePackageError}")
endif()
file(RENAME "${unicodePackageRoot}" "${packageRoot}")

set(longPathSegment "0123456789abcdef0123456789abcdef0123456789abcdef")
set(longPackageParent
    "${TEST_ROOT}/LongPath/${longPathSegment}/${longPathSegment}/${longPathSegment}/${longPathSegment}")
set(longPackageRoot "${longPackageParent}/RelocatedPackage")
set(longRuntimeExecutable "${longPackageRoot}/CueRuntimeHost.exe")
string(LENGTH "${longRuntimeExecutable}" longRuntimeExecutableLength)
if(longRuntimeExecutableLength LESS_EQUAL 260)
    message(FATAL_ERROR "Long-path Runtime Package fixture did not exceed MAX_PATH")
endif()
file(MAKE_DIRECTORY "${longPackageParent}")
file(RENAME "${packageRoot}" "${longPackageRoot}")
execute_process(
    COMMAND "${longRuntimeExecutable}" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE longPathPackageResult
    OUTPUT_VARIABLE longPathPackageOutput
    ERROR_VARIABLE longPathPackageError
    TIMEOUT 15
)
if(NOT longPathPackageResult EQUAL 0)
    message(FATAL_ERROR
        "Runtime Package failed from a path longer than MAX_PATH\n${longPathPackageOutput}\n${longPathPackageError}")
endif()
file(RENAME "${longPackageRoot}" "${packageRoot}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "CUE_RUNTIME_PACKAGE_PROBE_MODE=reserved-api-tail"
        "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE reservedApiResult
    OUTPUT_VARIABLE reservedApiOutput
    ERROR_VARIABLE reservedApiError
    TIMEOUT 15
)
set(reservedApiCombined "${reservedApiOutput}\n${reservedApiError}")
string(FIND "${reservedApiCombined}" "Game Module API identity or lifecycle is incompatible"
    reservedApiMessagePosition)
string(FIND "${reservedApiCombined}" "Error: Cue.Package/5" reservedApiDomainPosition)
if(reservedApiResult EQUAL 0 OR reservedApiMessagePosition EQUAL -1 OR reservedApiDomainPosition EQUAL -1)
    message(FATAL_ERROR "Non-zero Game Module API reserved tail was accepted\n${reservedApiCombined}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "CUE_RUNTIME_PACKAGE_PROBE_MODE=reserved-query-output"
        "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE reservedQueryResult
    OUTPUT_VARIABLE reservedQueryOutput
    ERROR_VARIABLE reservedQueryError
    TIMEOUT 15
)
set(reservedQueryCombined "${reservedQueryOutput}\n${reservedQueryError}")
string(FIND "${reservedQueryCombined}" "Game Module rejected the RuntimeHost ABI" reservedQueryMessagePosition)
string(FIND "${reservedQueryCombined}" "Error: Cue.Package/5" reservedQueryDomainPosition)
if(reservedQueryResult EQUAL 0 OR reservedQueryMessagePosition EQUAL -1 OR reservedQueryDomainPosition EQUAL -1)
    message(FATAL_ERROR "Non-zero Game Module Query Output reserved field was accepted\n${reservedQueryCombined}")
endif()

foreach(queryOutputMode IN ITEMS query-output-size query-output-version)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "CUE_RUNTIME_PACKAGE_PROBE_MODE=${queryOutputMode}"
            "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
        WORKING_DIRECTORY "${workingRoot}"
        RESULT_VARIABLE invalidQueryOutputResult
        OUTPUT_VARIABLE invalidQueryOutputOutput
        ERROR_VARIABLE invalidQueryOutputError
        TIMEOUT 15
    )
    set(invalidQueryOutputCombined "${invalidQueryOutputOutput}\n${invalidQueryOutputError}")
    string(FIND "${invalidQueryOutputCombined}" "Game Module rejected the RuntimeHost ABI"
        invalidQueryOutputMessagePosition)
    if(invalidQueryOutputResult EQUAL 0 OR invalidQueryOutputMessagePosition EQUAL -1)
        message(FATAL_ERROR
            "Invalid Game Module Query Output ${queryOutputMode} was accepted\n${invalidQueryOutputCombined}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "CUE_RUNTIME_PACKAGE_PROBE_MODE=create-module-failure"
        "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE createModuleFailureResult
    OUTPUT_VARIABLE createModuleFailureOutput
    ERROR_VARIABLE createModuleFailureError
    TIMEOUT 15
)
set(createModuleFailureCombined "${createModuleFailureOutput}\n${createModuleFailureError}")
string(FIND "${createModuleFailureCombined}" "Probe Project Scope initialization was rejected"
    createModuleFailureMessagePosition)
string(FIND "${createModuleFailureCombined}" "diagnostic code 5" createModuleFailureCodePosition)
if(createModuleFailureResult EQUAL 0 OR createModuleFailureMessagePosition EQUAL -1 OR
   createModuleFailureCodePosition EQUAL -1)
    message(FATAL_ERROR
        "Game Module Project Scope diagnostic was not preserved\n${createModuleFailureCombined}")
endif()

foreach(ignoredSinkFailureMode IN ITEMS
    ignored-schema-sink-failure
    ignored-component-sink-failure
    ignored-system-sink-failure
)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "CUE_RUNTIME_PACKAGE_PROBE_MODE=${ignoredSinkFailureMode}"
            "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
        WORKING_DIRECTORY "${workingRoot}"
        RESULT_VARIABLE ignoredSinkFailureResult
        OUTPUT_VARIABLE ignoredSinkFailureOutput
        ERROR_VARIABLE ignoredSinkFailureError
        TIMEOUT 15
    )
    set(ignoredSinkFailureCombined "${ignoredSinkFailureOutput}\n${ignoredSinkFailureError}")
    string(FIND "${ignoredSinkFailureCombined}" "Game Module registration failed"
        ignoredSinkFailureMessagePosition)
    if(ignoredSinkFailureResult EQUAL 0 OR ignoredSinkFailureMessagePosition EQUAL -1)
        message(FATAL_ERROR
            "Ignored Sink failure ${ignoredSinkFailureMode} was accepted\n${ignoredSinkFailureCombined}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "CUE_RUNTIME_PACKAGE_PROBE_MODE=invalid-system-id"
        "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE invalidSystemIdResult
    OUTPUT_VARIABLE invalidSystemIdOutput
    ERROR_VARIABLE invalidSystemIdError
    TIMEOUT 15
)
set(invalidSystemIdCombined "${invalidSystemIdOutput}\n${invalidSystemIdError}")
string(FIND "${invalidSystemIdCombined}" "Game Module registration failed" invalidSystemIdMessagePosition)
if(invalidSystemIdResult EQUAL 0 OR invalidSystemIdMessagePosition EQUAL -1)
    message(FATAL_ERROR "Invalid UTF-8 Runtime System ID was accepted\n${invalidSystemIdCombined}")
endif()

file(COPY_FILE "${packageRoot}/CueRuntimeHost.exe" "${packageRoot}/RenamedRuntimeHost.exe" ONLY_IF_DIFFERENT)
execute_process(
    COMMAND "${packageRoot}/RenamedRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE renamedHostResult
    OUTPUT_VARIABLE renamedHostOutput
    ERROR_VARIABLE renamedHostError
    TIMEOUT 15
)
set(renamedHostCombined "${renamedHostOutput}\n${renamedHostError}")
string(FIND "${renamedHostCombined}" "Running RuntimeHost executable does not match the Manifest role"
    renamedHostMessagePosition)
if(renamedHostResult EQUAL 0 OR renamedHostMessagePosition EQUAL -1)
    message(FATAL_ERROR "Renamed RuntimeHost was not rejected by Manifest identity\n${renamedHostCombined}")
endif()
file(REMOVE "${packageRoot}/RenamedRuntimeHost.exe")

math(EXPR oversizedProjectBytes "1024 * 1024 + 1")
write_package_manifest("${packageRoot}" "${oversizedProjectBytes}" "")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE oversizedProjectResult
    OUTPUT_VARIABLE oversizedProjectOutput
    ERROR_VARIABLE oversizedProjectError
    TIMEOUT 15
)
set(oversizedProjectCombined "${oversizedProjectOutput}\n${oversizedProjectError}")
string(FIND "${oversizedProjectCombined}" "Runtime Data role exceeds its Package startup size limit"
    oversizedProjectMessagePosition)
if(oversizedProjectResult EQUAL 0 OR oversizedProjectMessagePosition EQUAL -1)
    message(FATAL_ERROR
        "Oversized Project Runtime Data declaration was not rejected before read\n${oversizedProjectCombined}")
endif()
write_package_manifest("${packageRoot}" "" "")

math(EXPR oversizedMetadataBytes "64 * 1024 + 1")
write_package_manifest("${packageRoot}" "" "${oversizedMetadataBytes}")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE oversizedMetadataResult
    OUTPUT_VARIABLE oversizedMetadataOutput
    ERROR_VARIABLE oversizedMetadataError
    TIMEOUT 15
)
set(oversizedMetadataCombined "${oversizedMetadataOutput}\n${oversizedMetadataError}")
string(FIND "${oversizedMetadataCombined}" "Game Module Metadata role exceeds its Package startup size limit"
    oversizedMetadataMessagePosition)
if(oversizedMetadataResult EQUAL 0 OR oversizedMetadataMessagePosition EQUAL -1)
    message(FATAL_ERROR
        "Oversized Game Module Metadata declaration was not rejected before read\n${oversizedMetadataCombined}")
endif()
write_package_manifest("${packageRoot}" "" "")

math(EXPR oversizedRuntimePeBytes "128 * 1024 * 1024 + 1")
write_package_manifest("${packageRoot}" "" "" "${oversizedRuntimePeBytes}")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE oversizedRuntimePeResult
    OUTPUT_VARIABLE oversizedRuntimePeOutput
    ERROR_VARIABLE oversizedRuntimePeError
    TIMEOUT 15
)
set(oversizedRuntimePeCombined "${oversizedRuntimePeOutput}\n${oversizedRuntimePeError}")
string(FIND "${oversizedRuntimePeCombined}" "Runtime PE image inventory exceeds the RuntimeHost memory contract"
    oversizedRuntimePeMessagePosition)
if(oversizedRuntimePeResult EQUAL 0 OR oversizedRuntimePeMessagePosition EQUAL -1)
    message(FATAL_ERROR
        "Oversized Runtime PE declaration was not rejected before read\n${oversizedRuntimePeCombined}")
endif()
write_package_manifest("${packageRoot}" "" "")

set(packageMetadataPath "${packageRoot}/Game/CueGameModule.metadata.json")
file(READ "${packageMetadataPath}" validMetadata)
string(REPLACE "\"artifactId\": \"${artifactId}\"" "\"artifactId\": \"runtime-package-probe\""
    invalidMetadata "${validMetadata}")
file(WRITE "${packageMetadataPath}" "${invalidMetadata}")
write_package_manifest("${packageRoot}" "" "")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE invalidArtifactIdResult
    OUTPUT_VARIABLE invalidArtifactIdOutput
    ERROR_VARIABLE invalidArtifactIdError
    TIMEOUT 15
)
set(invalidArtifactIdCombined "${invalidArtifactIdOutput}\n${invalidArtifactIdError}")
string(FIND "${invalidArtifactIdCombined}" "Game Module Metadata header is invalid"
    invalidArtifactIdMessagePosition)
if(invalidArtifactIdResult EQUAL 0 OR invalidArtifactIdMessagePosition EQUAL -1)
    message(FATAL_ERROR "Non-canonical Metadata artifactId was accepted\n${invalidArtifactIdCombined}")
endif()
file(WRITE "${packageMetadataPath}" "${validMetadata}")
write_package_manifest("${packageRoot}" "" "")

function(assert_invalid_metadata metadata caseName)
    file(WRITE "${packageMetadataPath}" "${metadata}")
    write_package_manifest("${packageRoot}" "" "")
    execute_process(
        COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
        WORKING_DIRECTORY "${workingRoot}"
        RESULT_VARIABLE invalidShapeResult
        OUTPUT_VARIABLE invalidShapeOutput
        ERROR_VARIABLE invalidShapeError
        TIMEOUT 15
    )
    set(invalidShapeCombined "${invalidShapeOutput}\n${invalidShapeError}")
    string(FIND "${invalidShapeCombined}" "Game Module Metadata body is invalid"
        invalidShapeMessagePosition)
    if(invalidShapeResult EQUAL 0 OR invalidShapeMessagePosition EQUAL -1)
        message(FATAL_ERROR
            "Invalid Metadata shape was accepted (${caseName})\n${invalidShapeCombined}")
    endif()
endfunction()

string(REPLACE
    "    \"entrySymbol\": \"cue_game_module_query\""
    "    \"unknownMember\": 0,\n    \"entrySymbol\": \"cue_game_module_query\""
    unknownMemberMetadata "${validMetadata}")
assert_invalid_metadata("${unknownMemberMetadata}" "unknown top-level member")

string(REPLACE
    "        \"compilerVersion\": ${COMPILER_VERSION},"
    "        \"compilerVersion\": ${COMPILER_VERSION},\n        \"compilerVersion\": ${COMPILER_VERSION},"
    duplicateMemberMetadata "${validMetadata}")
assert_invalid_metadata("${duplicateMemberMetadata}" "duplicate nested member")

string(REPLACE
    "    \"configuration\": \"${CONFIGURATION}\",\n"
    ""
    missingMemberMetadata "${validMetadata}")
assert_invalid_metadata("${missingMemberMetadata}" "missing top-level member")

file(WRITE "${packageMetadataPath}" "${validMetadata}")
write_package_manifest("${packageRoot}" "" "")

file(WRITE "${packageMetadataPath}"
    "{\"entrySymbol\":\"cue_game_module_query\",\"moduleFile\":\"CueGameModule.dll\",\"iteratorDebugLevel\":${iteratorDebugLevel},\"runtimeLibrary\":\"${runtimeLibrary}\",\"msvcToolset\":{\"build\":${compatibleBuild},\"fullVersion\":${compatibleFullVersion},\"compilerVersion\":${COMPILER_VERSION}},\"compilerFamily\":\"msvc\",\"architecture\":\"x64\",\"configuration\":\"${CONFIGURATION}\",\"abiVersion\":1,\"engineCompatibility\":{\"maximumExclusive\":\"2.0.0\",\"minimum\":\"1.0.0\"},\"projectId\":\"${projectId}\",\"artifactId\":\"${artifactId}\",\"schemaVersion\":1}\n")
write_package_manifest("${packageRoot}" "" "")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE reorderedMetadataResult
    OUTPUT_VARIABLE reorderedMetadataOutput
    ERROR_VARIABLE reorderedMetadataError
    TIMEOUT 15
)
if(NOT reorderedMetadataResult EQUAL 0)
    message(FATAL_ERROR
        "Order-independent Game Module Metadata was rejected\n${reorderedMetadataOutput}\n${reorderedMetadataError}")
endif()
file(WRITE "${packageMetadataPath}" "${validMetadata}")
write_package_manifest("${packageRoot}" "" "")

foreach(toolsetField IN ITEMS compilerVersion fullVersion build)
    if(toolsetField STREQUAL "compilerVersion")
        set(validToolsetValue "${COMPILER_VERSION}")
    elseif(toolsetField STREQUAL "fullVersion")
        set(validToolsetValue "${compatibleFullVersion}")
    else()
        set(validToolsetValue "${compatibleBuild}")
    endif()
    string(REPLACE "\"${toolsetField}\": ${validToolsetValue}"
        "\"${toolsetField}\": 9007199254740992" outOfRangeMetadata "${validMetadata}")
    file(WRITE "${packageMetadataPath}" "${outOfRangeMetadata}")
    write_package_manifest("${packageRoot}" "" "")
    execute_process(
        COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
        WORKING_DIRECTORY "${workingRoot}"
        RESULT_VARIABLE outOfRangeToolsetResult
        OUTPUT_VARIABLE outOfRangeToolsetOutput
        ERROR_VARIABLE outOfRangeToolsetError
        TIMEOUT 15
    )
    set(outOfRangeToolsetCombined "${outOfRangeToolsetOutput}\n${outOfRangeToolsetError}")
    string(FIND "${outOfRangeToolsetCombined}" "Game Module Metadata body is invalid"
        outOfRangeToolsetMessagePosition)
    if(outOfRangeToolsetResult EQUAL 0 OR outOfRangeToolsetMessagePosition EQUAL -1)
        message(FATAL_ERROR
            "Out-of-range Metadata ${toolsetField} was accepted\n${outOfRangeToolsetCombined}")
    endif()
endforeach()
file(WRITE "${packageMetadataPath}" "${validMetadata}")
write_package_manifest("${packageRoot}" "" "")

set(packageScenePath "${packageRoot}/${sceneRelativePath}")
set(packageProjectPath "${packageRoot}/Data/CueProject.runtime.json")
file(READ "${packageProjectPath}" validProject)
file(READ "${packageScenePath}" validScene)
string(REGEX REPLACE "[\r\n]+$" "" projectWithoutFinalLf "${validProject}")
file(WRITE "${packageProjectPath}" "${projectWithoutFinalLf}")
write_package_manifest("${packageRoot}" "" "")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE nonCanonicalProjectResult
    OUTPUT_VARIABLE nonCanonicalProjectOutput
    ERROR_VARIABLE nonCanonicalProjectError
    TIMEOUT 15
)
set(nonCanonicalProjectCombined "${nonCanonicalProjectOutput}\n${nonCanonicalProjectError}")
string(FIND "${nonCanonicalProjectCombined}" "Runtime Project Data is not the canonical Publisher representation"
    nonCanonicalProjectMessagePosition)
if(nonCanonicalProjectResult EQUAL 0 OR nonCanonicalProjectMessagePosition EQUAL -1)
    message(FATAL_ERROR "Runtime Project Data without final LF was accepted\n${nonCanonicalProjectCombined}")
endif()
write_runtime_data("${packageProjectPath}" "${validProject}")

string(REPLACE "{\"schemaVersion\"" "{ \"schemaVersion\"" sceneWithWhitespace "${validScene}")
write_runtime_data("${packageScenePath}" "${sceneWithWhitespace}")
write_package_manifest("${packageRoot}" "" "")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE nonCanonicalWhitespaceResult
    OUTPUT_VARIABLE nonCanonicalWhitespaceOutput
    ERROR_VARIABLE nonCanonicalWhitespaceError
    TIMEOUT 15
)
set(nonCanonicalWhitespaceCombined "${nonCanonicalWhitespaceOutput}\n${nonCanonicalWhitespaceError}")
string(FIND "${nonCanonicalWhitespaceCombined}" "Runtime Scene Data is not the canonical Publisher representation"
    nonCanonicalWhitespaceMessagePosition)
if(nonCanonicalWhitespaceResult EQUAL 0 OR nonCanonicalWhitespaceMessagePosition EQUAL -1)
    message(FATAL_ERROR "Runtime Scene Data with non-canonical whitespace was accepted\n${nonCanonicalWhitespaceCombined}")
endif()
write_runtime_data("${packageScenePath}" "${validScene}")

write_runtime_data("${packageScenePath}"
    "{\"schemaVersion\":1,\"sceneAssetId\":\"${sceneId}\",\"objects\":[{\"objectId\":\"61234567-89ab-4cde-8f01-23456789abcd\",\"parentObjectId\":null,\"active\":true,\"transform\":{\"translation\":[0,0,0],\"rotation\":[0,0,0,2],\"scale\":[1,1,1]},\"components\":[]}]}${runtimeLf}")
write_package_manifest("${packageRoot}" "" "")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE nonUnitRotationResult
    OUTPUT_VARIABLE nonUnitRotationOutput
    ERROR_VARIABLE nonUnitRotationError
    TIMEOUT 15
)
set(nonUnitRotationCombined "${nonUnitRotationOutput}\n${nonUnitRotationError}")
string(FIND "${nonUnitRotationCombined}" "Runtime Scene rotation is not a supported unit Quaternion"
    nonUnitRotationMessagePosition)
if(nonUnitRotationResult EQUAL 0 OR nonUnitRotationMessagePosition EQUAL -1)
    message(FATAL_ERROR "Non-unit Runtime Scene Quaternion was not classified as unsupported\n${nonUnitRotationCombined}")
endif()
write_runtime_data("${packageScenePath}" "${validScene}")

foreach(invalidNumber IN ITEMS "01" ".5" "1." "1e")
    write_runtime_data("${packageScenePath}"
        "{\"schemaVersion\":1,\"sceneAssetId\":\"${sceneId}\",\"objects\":[{\"objectId\":\"61234567-89ab-4cde-8f01-23456789abcd\",\"parentObjectId\":null,\"active\":true,\"transform\":{\"translation\":[${invalidNumber},0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]},\"components\":[]}]}\n")
    write_package_manifest("${packageRoot}" "" "")
    execute_process(
        COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
        WORKING_DIRECTORY "${workingRoot}"
        RESULT_VARIABLE invalidNumberResult
        OUTPUT_VARIABLE invalidNumberOutput
        ERROR_VARIABLE invalidNumberError
        TIMEOUT 15
    )
    set(invalidNumberCombined "${invalidNumberOutput}\n${invalidNumberError}")
    string(FIND "${invalidNumberCombined}" "Runtime Scene object values are invalid" invalidNumberMessagePosition)
    if(invalidNumberResult EQUAL 0 OR invalidNumberMessagePosition EQUAL -1)
        message(FATAL_ERROR "Invalid JSON number ${invalidNumber} was accepted\n${invalidNumberCombined}")
    endif()
endforeach()
write_runtime_data("${packageScenePath}"
    "{\"schemaVersion\":1,\"sceneAssetId\":\"${sceneId}\",\"objects\":[{\"objectId\":\"71234567-89ab-4cde-8f01-23456789abcd\",\"parentObjectId\":null,\"active\":true,\"transform\":{\"translation\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]},\"components\":[]},{\"objectId\":\"61234567-89ab-4cde-8f01-23456789abcd\",\"parentObjectId\":null,\"active\":true,\"transform\":{\"translation\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]},\"components\":[]}]}\n")
write_package_manifest("${packageRoot}" "" "")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE nonCanonicalSceneResult
    OUTPUT_VARIABLE nonCanonicalSceneOutput
    ERROR_VARIABLE nonCanonicalSceneError
    TIMEOUT 15
)
set(nonCanonicalSceneCombined "${nonCanonicalSceneOutput}\n${nonCanonicalSceneError}")
string(FIND "${nonCanonicalSceneCombined}" "Runtime Scene object order is not canonical"
    nonCanonicalSceneMessagePosition)
if(nonCanonicalSceneResult EQUAL 0 OR nonCanonicalSceneMessagePosition EQUAL -1)
    message(FATAL_ERROR "Non-canonical Runtime Scene object order was accepted\n${nonCanonicalSceneCombined}")
endif()
write_runtime_data("${packageScenePath}" "${validScene}")
write_package_manifest("${packageRoot}" "" "")

set(runtimeSceneObjects "")
foreach(objectIndex RANGE 0 4096)
    set(objectIdSuffix "000000000000${objectIndex}")
    string(LENGTH "${objectIdSuffix}" objectIdSuffixLength)
    math(EXPR objectIdSuffixOffset "${objectIdSuffixLength} - 12")
    string(SUBSTRING "${objectIdSuffix}" ${objectIdSuffixOffset} 12 objectIdSuffix)
    if(NOT runtimeSceneObjects STREQUAL "")
        string(APPEND runtimeSceneObjects ",")
    endif()
    string(APPEND runtimeSceneObjects
        "{\"objectId\":\"00000000-0000-4000-8000-${objectIdSuffix}\",\"parentObjectId\":null,\"active\":true,\"transform\":{\"translation\":[0,0,0],\"rotation\":[0,0,0,1],\"scale\":[1,1,1]},\"components\":[]}")
endforeach()
write_runtime_data("${packageScenePath}"
    "{\"schemaVersion\":1,\"sceneAssetId\":\"${sceneId}\",\"objects\":[${runtimeSceneObjects}]}${runtimeLf}")
write_package_manifest("${packageRoot}" "" "")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE authoringLimitRuntimeSceneResult
    OUTPUT_VARIABLE authoringLimitRuntimeSceneOutput
    ERROR_VARIABLE authoringLimitRuntimeSceneError
    TIMEOUT 30
)
if(NOT authoringLimitRuntimeSceneResult EQUAL 0)
    message(FATAL_ERROR
        "Runtime Scene above the Authoring object limit was rejected\n${authoringLimitRuntimeSceneOutput}\n${authoringLimitRuntimeSceneError}")
endif()
write_runtime_data("${packageScenePath}" "${validScene}")
write_package_manifest("${packageRoot}" "" "")

file(MAKE_DIRECTORY "${packageRoot}/Runtime")
file(WRITE "${packageRoot}/Runtime/Unlisted.dll" "unlisted")
execute_process(
    COMMAND "${packageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE unlistedRuntimeResult
    OUTPUT_VARIABLE unlistedRuntimeOutput
    ERROR_VARIABLE unlistedRuntimeError
    TIMEOUT 15
)
if(unlistedRuntimeResult EQUAL 0)
    message(FATAL_ERROR
        "Manifest-external Runtime dependency was accepted\n${unlistedRuntimeOutput}\n${unlistedRuntimeError}")
endif()
file(REMOVE "${packageRoot}/Runtime/Unlisted.dll")

set(tamperedPackageRoot "${TEST_ROOT}/TamperedPackage")
file(COPY "${packageRoot}/" DESTINATION "${tamperedPackageRoot}")
file(APPEND "${tamperedPackageRoot}/Data/CueProject.runtime.json" "tampered")
execute_process(
    COMMAND "${tamperedPackageRoot}/CueRuntimeHost.exe" --package-smoke-test
    WORKING_DIRECTORY "${workingRoot}"
    RESULT_VARIABLE tamperedResult
    OUTPUT_VARIABLE tamperedOutput
    ERROR_VARIABLE tamperedError
    TIMEOUT 15
)
if(tamperedResult EQUAL 0)
    message(FATAL_ERROR "Tampered Runtime Data was accepted\n${tamperedOutput}\n${tamperedError}")
endif()

message(STATUS "Relocated Runtime Package discovery, lifecycle, and tamper rejection: passed")
