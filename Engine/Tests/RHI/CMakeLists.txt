function(cue_get_rhi_target_property targetName propertyName outputVariable)
    get_target_property(propertyValue ${targetName} ${propertyName})

    if(propertyValue MATCHES "-NOTFOUND$")
        set(propertyValue "<none>")
    endif()

    set(${outputVariable} "${propertyValue}" PARENT_SCOPE)
endfunction()

function(cue_collect_rhi_target_files targetName outputVariable)
    get_target_property(targetSourceDirectory ${targetName} SOURCE_DIR)
    get_target_property(targetSources ${targetName} SOURCES)
    get_target_property(targetHeaderSets ${targetName} HEADER_SETS)
    get_target_property(targetInterfaceHeaderSets ${targetName} INTERFACE_HEADER_SETS)
    set(targetFiles ${targetSources})

    foreach(headerSetProperty IN ITEMS targetHeaderSets targetInterfaceHeaderSets)
        if(${headerSetProperty} MATCHES "-NOTFOUND$")
            set(${headerSetProperty} "")
        endif()
    endforeach()

    set(allHeaderSets ${targetHeaderSets} ${targetInterfaceHeaderSets})
    list(REMOVE_DUPLICATES allHeaderSets)

    foreach(headerSet IN LISTS allHeaderSets)
        if(headerSet STREQUAL "HEADERS")
            set(headerSetProperty HEADER_SET)
        else()
            set(headerSetProperty "HEADER_SET_${headerSet}")
        endif()

        get_target_property(headerFiles ${targetName} "${headerSetProperty}")

        if(NOT headerFiles MATCHES "-NOTFOUND$")
            list(APPEND targetFiles ${headerFiles})
        endif()
    endforeach()

    set(absoluteTargetFiles "")

    foreach(targetFile IN LISTS targetFiles)
        if(targetFile MATCHES "\\$<")
            message(FATAL_ERROR "${targetName} contains an uninspectable source: ${targetFile}")
        endif()

        get_filename_component(
            absoluteTargetFile
            "${targetFile}"
            ABSOLUTE
            BASE_DIR "${targetSourceDirectory}"
        )
        list(APPEND absoluteTargetFiles "${absoluteTargetFile}")
    endforeach()

    list(REMOVE_DUPLICATES absoluteTargetFiles)
    set(${outputVariable} "${absoluteTargetFiles}" PARENT_SCOPE)
endfunction()

function(cue_verify_rhi_target_shape targetName expectedPublicDirectory)
    foreach(
        forbiddenProperty
        IN ITEMS
            PRECOMPILE_HEADERS
            INTERFACE_PRECOMPILE_HEADERS
            PRECOMPILE_HEADERS_REUSE_FROM
            CXX_MODULE_SETS
            INTERFACE_CXX_MODULE_SETS
    )
        get_target_property(propertyValue ${targetName} ${forbiddenProperty})

        if(NOT propertyValue MATCHES "-NOTFOUND$" AND NOT "${propertyValue}" STREQUAL "")
            message(FATAL_ERROR "${targetName} must not use ${forbiddenProperty}: ${propertyValue}")
        endif()
    endforeach()

    get_target_property(interfaceIncludeDirectories ${targetName} INTERFACE_INCLUDE_DIRECTORIES)

    foreach(interfaceIncludeDirectory IN LISTS interfaceIncludeDirectories)
        string(
            REGEX REPLACE
            "^\\$<BUILD_INTERFACE:(.*)>$"
            "\\1"
            resolvedIncludeDirectory
            "${interfaceIncludeDirectory}"
        )

        if(resolvedIncludeDirectory MATCHES "\\$<")
            message(
                FATAL_ERROR
                "${targetName} contains an uninspectable interface include: ${interfaceIncludeDirectory}"
            )
        endif()

        get_filename_component(resolvedIncludeDirectory "${resolvedIncludeDirectory}" ABSOLUTE)
        get_filename_component(expectedPublicDirectory "${expectedPublicDirectory}" ABSOLUTE)

        if(NOT resolvedIncludeDirectory STREQUAL expectedPublicDirectory)
            message(
                FATAL_ERROR
                "${targetName} exposes an unexpected include directory: ${resolvedIncludeDirectory}"
            )
        endif()
    endforeach()
endfunction()

add_executable(
    CueRhiPublicHeaderTest
    PublicHeaderCompile.cpp
    GraphicsBackendHeaderCompile.cpp
    PresentationContextHeaderCompile.cpp
)

target_link_libraries(CueRhiPublicHeaderTest PRIVATE Cue.RHI)
target_compile_features(CueRhiPublicHeaderTest PRIVATE cxx_std_20)

set(cueRhiTestTargets CueRhiPublicHeaderTest)
set(cueRhiTests Cue.RHI.PublicHeaders)

if(WIN32)
    add_executable(
        CueRhiD3d12PublicHeaderTest
        PublicHeaderCompile.cpp
        D3d12BackendHeaderCompile.cpp
    )
    add_executable(CueRhiD3d12AdapterSelectionProcess D3d12AdapterSelectionProcess.cpp)
    add_executable(CueRhiD3d12BackendProcess D3d12BackendProcess.cpp)
    add_executable(CueRhiD3d12DiagnosticsProcess D3d12DiagnosticsProcess.cpp)
    add_executable(CueRhiD3d12FrameCommandProcess D3d12FrameCommandProcess.cpp)
    add_executable(CueRhiD3d12QueueStateProcess D3d12QueueStateProcess.cpp)
    add_executable(CueRhiD3d12RtvHeapProcess D3d12RtvHeapProcess.cpp)
    add_executable(CueRhiD3d12SceneFrameProcess D3d12SceneFrameProcess.cpp)
    add_executable(CueRhiD3d12SwapChainProcess D3d12SwapChainProcess.cpp)
    add_executable(
        CueRhiD3d12WindowsPublicHeaderTest
        PublicHeaderCompile.cpp
        D3d12WindowsPresentationHeaderCompile.cpp
    )

    target_link_libraries(CueRhiD3d12PublicHeaderTest PRIVATE Cue.RHI.D3D12)
    target_compile_features(CueRhiD3d12PublicHeaderTest PRIVATE cxx_std_20)
    target_link_libraries(CueRhiD3d12AdapterSelectionProcess PRIVATE Cue.RHI.D3D12)
    target_compile_features(CueRhiD3d12AdapterSelectionProcess PRIVATE cxx_std_20)
    target_link_libraries(CueRhiD3d12BackendProcess PRIVATE Cue.RHI.D3D12)
    target_compile_features(CueRhiD3d12BackendProcess PRIVATE cxx_std_20)
    target_link_libraries(CueRhiD3d12DiagnosticsProcess PRIVATE Cue.RHI.D3D12)
    target_compile_features(CueRhiD3d12DiagnosticsProcess PRIVATE cxx_std_20)
    target_link_libraries(CueRhiD3d12FrameCommandProcess PRIVATE Cue.RHI.D3D12)
    target_compile_features(CueRhiD3d12FrameCommandProcess PRIVATE cxx_std_20)
    target_link_libraries(CueRhiD3d12QueueStateProcess PRIVATE Cue.RHI.D3D12)
    target_compile_features(CueRhiD3d12QueueStateProcess PRIVATE cxx_std_20)
    target_link_libraries(CueRhiD3d12RtvHeapProcess PRIVATE Cue.RHI.D3D12)
    target_compile_features(CueRhiD3d12RtvHeapProcess PRIVATE cxx_std_20)
    target_link_libraries(CueRhiD3d12SceneFrameProcess PRIVATE Cue.RHI.D3D12)
    target_compile_features(CueRhiD3d12SceneFrameProcess PRIVATE cxx_std_20)
    target_link_libraries(
        CueRhiD3d12SwapChainProcess
        PRIVATE
            Cue.Platform.Windows
            Cue.RHI.D3D12.Windows
    )
    target_compile_features(CueRhiD3d12SwapChainProcess PRIVATE cxx_std_20)
    target_link_libraries(CueRhiD3d12WindowsPublicHeaderTest PRIVATE Cue.RHI.D3D12.Windows)
    target_compile_features(CueRhiD3d12WindowsPublicHeaderTest PRIVATE cxx_std_20)
    set(
        cueRhiProcessTestTargets
        CueRhiD3d12AdapterSelectionProcess
        CueRhiD3d12BackendProcess
        CueRhiD3d12DiagnosticsProcess
        CueRhiD3d12FrameCommandProcess
        CueRhiD3d12QueueStateProcess
        CueRhiD3d12RtvHeapProcess
        CueRhiD3d12SceneFrameProcess
        CueRhiD3d12SwapChainProcess
    )

    foreach(processTestTarget IN LISTS cueRhiProcessTestTargets)
        target_sources(
            ${processTestTarget}
            PRIVATE
                TestSupport/RhiProcessTestFixture.h
        )
    endforeach()
    list(
        APPEND
        cueRhiTestTargets
        CueRhiD3d12PublicHeaderTest
        CueRhiD3d12AdapterSelectionProcess
        CueRhiD3d12BackendProcess
        CueRhiD3d12DiagnosticsProcess
        CueRhiD3d12FrameCommandProcess
        CueRhiD3d12QueueStateProcess
        CueRhiD3d12RtvHeapProcess
        CueRhiD3d12SceneFrameProcess
        CueRhiD3d12SwapChainProcess
        CueRhiD3d12WindowsPublicHeaderTest
    )
    list(
        APPEND
        cueRhiTests
        Cue.RHI.D3D12.PublicHeaders
        Cue.RHI.D3D12.AdapterSelection.Hardware
        Cue.RHI.D3D12.AdapterSelection.Warp
        Cue.RHI.D3D12.AdapterSelection.Skip
        Cue.RHI.D3D12.AdapterSelection.Mapping
        Cue.RHI.D3D12.AdapterSelection.EmptyName
        Cue.RHI.D3D12.AdapterSelection.FactoryFallback
        Cue.RHI.D3D12.AdapterSelection.FactoryFallbackLogFailure
        Cue.RHI.D3D12.AdapterSelection.LogFailure
        Cue.RHI.D3D12.Backend.Warp
        Cue.RHI.D3D12.Backend.DeviceFailure
        Cue.RHI.D3D12.Backend.ThreadDestruction
        Cue.RHI.D3D12.Backend.InvalidWaitTimeout
        Cue.RHI.D3D12.Backend.OptionalCapabilityFailure
        Cue.RHI.D3D12.Backend.OptionalCapabilityLogFailure
        Cue.RHI.D3D12.Backend.UnknownCapabilityValue
        Cue.RHI.D3D12.Backend.UnknownCapabilityValueLogFailure
        Cue.RHI.D3D12.Backend.LegacyFeatureLevelRuntime
        Cue.RHI.D3D12.Backend.RequiredCapabilityFailure
        Cue.RHI.D3D12.Diagnostics.Disabled
        Cue.RHI.D3D12.Diagnostics.Configured
        Cue.RHI.D3D12.Diagnostics.EmptyDredFallback
        Cue.RHI.D3D12.FrameCommand.Smoke300
        Cue.RHI.D3D12.FrameCommand.InfoQueue300
        Cue.RHI.D3D12.FrameCommand.ClearTwoColors
        Cue.RHI.D3D12.FrameCommand.ClearPresentState
        Cue.RHI.D3D12.FrameCommand.ClearMissingRtv
        Cue.RHI.D3D12.FrameCommand.ClearOutsideRecording
        Cue.RHI.D3D12.FrameCommand.ClearNonCurrentFrame
        Cue.RHI.D3D12.FrameCommand.ClearInvalidRtvHandle
        Cue.RHI.D3D12.FrameCommand.InvalidIndex
        Cue.RHI.D3D12.FrameCommand.TransitionInvalidIndex
        Cue.RHI.D3D12.FrameCommand.TransitionNullResource
        Cue.RHI.D3D12.FrameCommand.TransitionOutsideRecording
        Cue.RHI.D3D12.FrameCommand.TransitionNonCurrentFrame
        Cue.RHI.D3D12.FrameCommand.TransitionUnknownTarget
        Cue.RHI.D3D12.FrameCommand.WaitRecoveryMatrix
        Cue.RHI.D3D12.FrameCommand.BeginFrameTerminalOutcomeMatrix
        Cue.RHI.D3D12.FrameCommand.FrameResetFailedTerminalMatrix
        Cue.RHI.D3D12.FrameCommand.RecordingCloseFailedTerminalMatrix
        Cue.RHI.D3D12.FrameCommand.FenceExhaustionMatrix
        Cue.RHI.D3D12.FrameCommand.FrameSignalOutcomeMatrix
        Cue.RHI.D3D12.FrameCommand.StateOrderBegin
        Cue.RHI.D3D12.FrameCommand.StateOrderClose
        Cue.RHI.D3D12.FrameCommand.StateOrderExecute
        Cue.RHI.D3D12.FrameCommand.StateOrderPresentState
        Cue.RHI.D3D12.FrameCommand.StateOrderResizeSuspend
        Cue.RHI.D3D12.FrameCommand.AllocatorResetFailure
        Cue.RHI.D3D12.FrameCommand.CommandListResetFailure
        Cue.RHI.D3D12.FrameCommand.CommandListCloseFailure
        Cue.RHI.D3D12.FrameCommand.ResizePreparationFailureMatrix
        Cue.RHI.D3D12.QueueState.Smoke
        Cue.RHI.D3D12.QueueState.Exhaustion
        Cue.RHI.D3D12.QueueState.WaitRecoveryMatrix
        Cue.RHI.D3D12.QueueState.WaitSentinelRefreshRemoval
        Cue.RHI.D3D12.QueueState.StaleEventFollowup
        Cue.RHI.D3D12.QueueState.WaitFailedUnavailable
        Cue.RHI.D3D12.QueueState.EventCloseFailure
        Cue.RHI.D3D12.QueueState.EventCreateFailure
        Cue.RHI.D3D12.QueueState.SignalCompletion
        Cue.RHI.D3D12.QueueState.SignalDeviceRemoved
        Cue.RHI.D3D12.QueueState.SignalCompletionDeviceRemovedRace
        Cue.RHI.D3D12.QueueState.SignalUnavailable
        Cue.RHI.D3D12.QueueState.SignalEventCloseFailure
        Cue.RHI.D3D12.QueueState.SignalEventCreateFailure
        Cue.RHI.D3D12.QueueState.TerminalSignalCompletion
        Cue.RHI.D3D12.QueueState.TerminalSignalDeviceRemoved
        Cue.RHI.D3D12.QueueState.TerminalSignalUnavailable
        Cue.RHI.D3D12.QueueState.TerminalCloseFailure
        Cue.RHI.D3D12.RtvHeap.Smoke
        Cue.RHI.D3D12.RtvHeap.InfoQueue
        Cue.RHI.D3D12.RtvHeap.CreationFaults
        Cue.RHI.D3D12.RtvHeap.HandleOverflow
        Cue.RHI.D3D12.RtvHeap.GenerationExhaustion
        Cue.RHI.D3D12.RtvHeap.HeapMove
        Cue.RHI.D3D12.RtvHeap.DoubleRelease
        Cue.RHI.D3D12.RtvHeap.StaleRelease
        Cue.RHI.D3D12.RtvHeap.ForeignRelease
        Cue.RHI.D3D12.RtvHeap.LiveSlotShutdown
        Cue.RHI.D3D12.RtvHeap.OwnerDestructor
        Cue.RHI.D3D12.RtvHeap.ActiveMoveAssignment
        Cue.RHI.D3D12.SceneFrame.WarpPixel
        Cue.RHI.D3D12.SceneFrame.HardwarePixel
        Cue.RHI.D3D12.SceneFrame.Depth.WarpPixel
        Cue.RHI.D3D12.SceneFrame.Depth.HardwarePixel
        Cue.RHI.D3D12.SceneFrame.Depth.Resize.WarpPixel
        Cue.RHI.D3D12.SceneFrame.Depth.Resize.HardwarePixel
        Cue.RHI.D3D12.SceneFrame.BackFaceCull.WarpPixel
        Cue.RHI.D3D12.SceneFrame.BackFaceCull.HardwarePixel
        Cue.RHI.D3D12.SwapChain.SmokeVsync
        Cue.RHI.D3D12.SwapChain.SmokeImmediate
        Cue.RHI.D3D12.SwapChain.InfoQueue
        Cue.RHI.D3D12.SwapChain.TearingMatrix
        Cue.RHI.D3D12.SwapChain.PresentMatrix
        Cue.RHI.D3D12.SwapChain.PresentSignalRecovery
        Cue.RHI.D3D12.SwapChain.BeginFrameUnavailableRetention
        Cue.RHI.D3D12.SwapChain.CloseFrameDeviceRemoved
        Cue.RHI.D3D12.SwapChain.PresentUnavailableRetention
        Cue.RHI.D3D12.SwapChain.SignalUnavailableRetention
        Cue.RHI.D3D12.SwapChain.DirectPresentDeviceRemoved
        Cue.RHI.D3D12.SwapChain.RecoverySignalDeviceRemoved
        Cue.RHI.D3D12.SwapChain.RegularSignalDeviceRemoved
        Cue.RHI.D3D12.SwapChain.SceneBeginFrameUnavailableRetention
        Cue.RHI.D3D12.SwapChain.SceneSignalUnavailableRetention
        Cue.RHI.D3D12.SwapChain.SceneDirectPresentDeviceRemoved
        Cue.RHI.D3D12.SwapChain.CreationFaults
        Cue.RHI.D3D12.SwapChain.LogFailure
        Cue.RHI.D3D12.SwapChain.Validation
        Cue.RHI.D3D12.SwapChain.ProductionOwnership
        Cue.RHI.D3D12.SwapChain.ClearSmoke
        Cue.RHI.D3D12.SwapChain.SceneSmoke
        Cue.RHI.D3D12.SwapChain.SceneDeviceRemoval
        Cue.RHI.D3D12.SwapChain.SceneResize
        Cue.RHI.D3D12.SwapChain.SceneResizeHardware
        Cue.RHI.D3D12.SwapChain.SceneResizeCreationFailure
        Cue.RHI.D3D12.SwapChain.SceneResizePixelWarp
        Cue.RHI.D3D12.SwapChain.SceneResizePixelHardware
        Cue.RHI.D3D12.SwapChain.PresentFrames300
        Cue.RHI.D3D12.SwapChain.ResizeLifecycle
        Cue.RHI.D3D12.SwapChain.ResizeFailure
        Cue.RHI.D3D12.SwapChain.RtvRebuildFailure
        Cue.RHI.D3D12.SwapChain.SceneRtvRebuildFailure
        Cue.RHI.D3D12.SwapChain.TerminalResizeRejection
        Cue.RHI.D3D12.SwapChain.ResizeUnavailableRetention
        Cue.RHI.D3D12.SwapChain.DeviceRemovalLifecycle
        Cue.RHI.D3D12.SwapChain.DeviceRemovalResize
        Cue.RHI.D3D12.SwapChain.DeviceRemovalDredFailure
        Cue.RHI.D3D12.Windows.PublicHeaders
        Cue.RHI.Dependencies
    )
endif()

foreach(testTarget IN LISTS cueRhiTestTargets)
    set_target_properties(
        ${testTarget}
        PROPERTIES
            CXX_EXTENSIONS OFF
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    )

    if(MSVC)
        target_compile_options(
            ${testTarget}
            PRIVATE
                /W4
                /WX
                /permissive-
                /Zc:__cplusplus
                /utf-8
                $<$<CONFIG:Development>:/O2>
                $<$<CONFIG:Development>:/Zi>
        )

        target_link_options(
            ${testTarget}
            PRIVATE
                $<$<CONFIG:Development>:/DEBUG>
        )
    endif()
endforeach()

cue_verify_rhi_target_shape(Cue.RHI "${PROJECT_SOURCE_DIR}/Engine/Source/RHI/Public")

if(WIN32)
    cue_verify_rhi_target_shape(
        Cue.RHI.D3D12
        "${PROJECT_SOURCE_DIR}/Engine/Source/RHI/D3D12/Public"
    )
    cue_verify_rhi_target_shape(
        Cue.RHI.D3D12.Windows
        "${PROJECT_SOURCE_DIR}/Engine/Source/RHI/D3D12/Windows/Public"
    )

    cue_get_rhi_target_property(Cue.RHI LINK_LIBRARIES cueRhiLinks)
    cue_get_rhi_target_property(Cue.RHI INTERFACE_LINK_LIBRARIES cueRhiInterfaceLinks)
    cue_get_rhi_target_property(Cue.RHI MANUALLY_ADDED_DEPENDENCIES cueRhiManualDependencies)
    cue_get_rhi_target_property(Cue.RHI INTERFACE_SOURCES cueRhiInterfaceSources)
    cue_get_rhi_target_property(
        Cue.RHI
        INTERFACE_LINK_LIBRARIES_DIRECT
        cueRhiInterfaceDirectLinks
    )
    cue_get_rhi_target_property(
        Cue.RHI
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
        cueRhiInterfaceSystemIncludes
    )
    cue_get_rhi_target_property(Cue.RHI LINK_OPTIONS cueRhiLinkOptions)
    cue_get_rhi_target_property(Cue.RHI INTERFACE_LINK_OPTIONS cueRhiInterfaceLinkOptions)
    cue_get_rhi_target_property(Cue.RHI LINK_FLAGS cueRhiLinkFlags)
    cue_get_rhi_target_property(Cue.RHI STATIC_LIBRARY_OPTIONS cueRhiStaticLibraryOptions)
    cue_get_rhi_target_property(Cue.RHI.D3D12 LINK_LIBRARIES cueRhiD3d12Links)
    cue_get_rhi_target_property(
        Cue.RHI.D3D12
        INTERFACE_LINK_LIBRARIES
        cueRhiD3d12InterfaceLinks
    )
    cue_get_rhi_target_property(
        Cue.RHI.D3D12
        MANUALLY_ADDED_DEPENDENCIES
        cueRhiD3d12ManualDependencies
    )
    cue_get_rhi_target_property(
        Cue.RHI.D3D12
        INTERFACE_SOURCES
        cueRhiD3d12InterfaceSources
    )
    cue_get_rhi_target_property(
        Cue.RHI.D3D12
        INTERFACE_LINK_LIBRARIES_DIRECT
        cueRhiD3d12InterfaceDirectLinks
    )
    cue_get_rhi_target_property(
        Cue.RHI.D3D12
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
        cueRhiD3d12InterfaceSystemIncludes
    )
    cue_get_rhi_target_property(Cue.RHI.D3D12 LINK_OPTIONS cueRhiD3d12LinkOptions)
    cue_get_rhi_target_property(
        Cue.RHI.D3D12
        INTERFACE_LINK_OPTIONS
        cueRhiD3d12InterfaceLinkOptions
    )
    cue_get_rhi_target_property(Cue.RHI.D3D12 LINK_FLAGS cueRhiD3d12LinkFlags)
    cue_get_rhi_target_property(
        Cue.RHI.D3D12
        STATIC_LIBRARY_OPTIONS
        cueRhiD3d12StaticLibraryOptions
    )
    cue_get_rhi_target_property(
        Cue.RHI.D3D12
        CUE_MINIMUM_WINDOWS_SDK_VERSION
        cueRhiD3d12MinimumWindowsSdkVersion
    )
    cue_get_rhi_target_property(Cue.RHI.D3D12.Windows LINK_LIBRARIES cueRhiD3d12WindowsLinks)
    cue_get_rhi_target_property(
        Cue.RHI.D3D12.Windows
        INTERFACE_LINK_LIBRARIES
        cueRhiD3d12WindowsInterfaceLinks
    )
    cue_get_rhi_target_property(
        Cue.RHI.D3D12.Windows
        INTERFACE_INCLUDE_DIRECTORIES
        cueRhiD3d12WindowsInterfaceIncludes
    )

    cue_collect_rhi_target_files(Cue.RHI cueRhiTargetFiles)
    cue_collect_rhi_target_files(Cue.RHI.D3D12 cueRhiD3d12TargetFiles)
    cue_collect_rhi_target_files(Cue.RHI.D3D12.Windows cueRhiD3d12WindowsTargetFiles)
    set(
        allRhiTargetFiles
        ${cueRhiTargetFiles}
        ${cueRhiD3d12TargetFiles}
        ${cueRhiD3d12WindowsTargetFiles}
    )
    list(REMOVE_DUPLICATES allRhiTargetFiles)
    string(REPLACE ";" "\n" rhiTargetFileContents "${allRhiTargetFiles}")
    set(rhiTargetFileList "${CMAKE_CURRENT_BINARY_DIR}/Cue.RHI.TargetFiles.txt")
    file(GENERATE OUTPUT "${rhiTargetFileList}" CONTENT "${rhiTargetFileContents}\n")

    set(cueRhiDependencyReport "${CMAKE_CURRENT_BINARY_DIR}/Cue.RHI.Dependencies.txt")

    configure_file(
        RHIDependencyReport.txt.in
        "${cueRhiDependencyReport}"
        @ONLY
    )
endif()

add_custom_target(Cue.RHI.Tests DEPENDS ${cueRhiTestTargets})

add_test(NAME Cue.RHI.PublicHeaders COMMAND CueRhiPublicHeaderTest)

if(WIN32)
    add_test(NAME Cue.RHI.D3D12.PublicHeaders COMMAND CueRhiD3d12PublicHeaderTest)
    add_test(NAME Cue.RHI.D3D12.Windows.PublicHeaders COMMAND CueRhiD3d12WindowsPublicHeaderTest)
    add_test(
        NAME Cue.RHI.D3D12.AdapterSelection.Hardware
        COMMAND CueRhiD3d12AdapterSelectionProcess Hardware
    )
    add_test(
        NAME Cue.RHI.D3D12.AdapterSelection.Warp
        COMMAND CueRhiD3d12AdapterSelectionProcess Warp
    )
    add_test(
        NAME Cue.RHI.D3D12.AdapterSelection.Skip
        COMMAND CueRhiD3d12AdapterSelectionProcess Skip
    )
    add_test(
        NAME Cue.RHI.D3D12.AdapterSelection.Mapping
        COMMAND CueRhiD3d12AdapterSelectionProcess Mapping
    )
    add_test(
        NAME Cue.RHI.D3D12.AdapterSelection.EmptyName
        COMMAND CueRhiD3d12AdapterSelectionProcess EmptyName
    )
    add_test(
        NAME Cue.RHI.D3D12.AdapterSelection.FactoryFallback
        COMMAND CueRhiD3d12AdapterSelectionProcess FactoryFallback
    )
    add_test(
        NAME Cue.RHI.D3D12.AdapterSelection.FactoryFallbackLogFailure
        COMMAND CueRhiD3d12AdapterSelectionProcess FactoryFallbackLogFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.AdapterSelection.LogFailure
        COMMAND CueRhiD3d12AdapterSelectionProcess LogFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.Backend.Warp
        COMMAND CueRhiD3d12BackendProcess Warp
    )
    add_test(
        NAME Cue.RHI.D3D12.Backend.OptionalCapabilityFailure
        COMMAND CueRhiD3d12BackendProcess OptionalCapabilityFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.Backend.OptionalCapabilityLogFailure
        COMMAND CueRhiD3d12BackendProcess OptionalCapabilityLogFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.Backend.UnknownCapabilityValue
        COMMAND CueRhiD3d12BackendProcess UnknownCapabilityValue
    )
    add_test(
        NAME Cue.RHI.D3D12.Backend.UnknownCapabilityValueLogFailure
        COMMAND CueRhiD3d12BackendProcess UnknownCapabilityValueLogFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.Backend.LegacyFeatureLevelRuntime
        COMMAND CueRhiD3d12BackendProcess LegacyFeatureLevelRuntime
    )
    add_test(
        NAME Cue.RHI.D3D12.Backend.RequiredCapabilityFailure
        COMMAND CueRhiD3d12BackendProcess RequiredCapabilityFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.Backend.DeviceFailure
        COMMAND CueRhiD3d12BackendProcess DeviceFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.Backend.ThreadDestruction
        COMMAND
            ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12BackendProcess>
            -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12BackendThreadDestruction.cmake
    )
    add_test(
        NAME Cue.RHI.D3D12.Diagnostics.Disabled
        COMMAND CueRhiD3d12DiagnosticsProcess Disabled
    )
    add_test(
        NAME Cue.RHI.D3D12.Diagnostics.Configured
        COMMAND CueRhiD3d12DiagnosticsProcess Configured
    )
    add_test(
        NAME Cue.RHI.D3D12.Diagnostics.EmptyDredFallback
        COMMAND CueRhiD3d12DiagnosticsProcess EmptyDredFallback
    )
    add_test(
        NAME Cue.RHI.D3D12.Backend.InvalidWaitTimeout
        COMMAND CueRhiD3d12BackendProcess InvalidWaitTimeout
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.Smoke300
        COMMAND CueRhiD3d12FrameCommandProcess Smoke300
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.InfoQueue300
        COMMAND CueRhiD3d12FrameCommandProcess InfoQueue300
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.ClearTwoColors
        COMMAND CueRhiD3d12FrameCommandProcess ClearTwoColors
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.ClearPresentState
        COMMAND CueRhiD3d12FrameCommandProcess ClearPresentState
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.ClearMissingRtv
        COMMAND CueRhiD3d12FrameCommandProcess ClearMissingRtv
    )
    foreach(clearViolationMode IN ITEMS ClearOutsideRecording ClearNonCurrentFrame ClearInvalidRtvHandle)
        add_test(
            NAME Cue.RHI.D3D12.FrameCommand.${clearViolationMode}
            COMMAND
                ${CMAKE_COMMAND}
                -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12FrameCommandProcess>
                -DTEST_MODE=${clearViolationMode}
                -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12FrameCommandStateOrder.cmake
        )
    endforeach()
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.InvalidIndex
        COMMAND
            ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12FrameCommandProcess>
            -DTEST_MODE=InvalidIndex
            -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12FrameCommandStateOrder.cmake
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.TransitionInvalidIndex
        COMMAND
            ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12FrameCommandProcess>
            -DTEST_MODE=TransitionInvalidIndex
            -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12FrameCommandStateOrder.cmake
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.TransitionNullResource
        COMMAND CueRhiD3d12FrameCommandProcess TransitionNullResource
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.TransitionOutsideRecording
        COMMAND
            ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12FrameCommandProcess>
            -DTEST_MODE=TransitionOutsideRecording
            -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12FrameCommandStateOrder.cmake
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.TransitionNonCurrentFrame
        COMMAND
            ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12FrameCommandProcess>
            -DTEST_MODE=TransitionNonCurrentFrame
            -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12FrameCommandStateOrder.cmake
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.TransitionUnknownTarget
        COMMAND CueRhiD3d12FrameCommandProcess TransitionUnknownTarget
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.StateOrderBegin
        COMMAND
            ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12FrameCommandProcess>
            -DTEST_MODE=StateOrderBegin
            -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12FrameCommandStateOrder.cmake
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.WaitRecoveryMatrix
        COMMAND CueRhiD3d12FrameCommandProcess WaitRecoveryMatrix
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.BeginFrameTerminalOutcomeMatrix
        COMMAND CueRhiD3d12FrameCommandProcess BeginFrameTerminalOutcomeMatrix
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.FrameResetFailedTerminalMatrix
        COMMAND CueRhiD3d12FrameCommandProcess FrameResetFailedTerminalMatrix
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.RecordingCloseFailedTerminalMatrix
        COMMAND CueRhiD3d12FrameCommandProcess RecordingCloseFailedTerminalMatrix
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.FenceExhaustionMatrix
        COMMAND CueRhiD3d12FrameCommandProcess FenceExhaustionMatrix
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.FrameSignalOutcomeMatrix
        COMMAND CueRhiD3d12FrameCommandProcess FrameSignalOutcomeMatrix
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.StateOrderClose
        COMMAND
            ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12FrameCommandProcess>
            -DTEST_MODE=StateOrderClose
            -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12FrameCommandStateOrder.cmake
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.StateOrderExecute
        COMMAND
            ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12FrameCommandProcess>
            -DTEST_MODE=StateOrderExecute
            -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12FrameCommandStateOrder.cmake
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.StateOrderPresentState
        COMMAND
            ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12FrameCommandProcess>
            -DTEST_MODE=StateOrderPresentState
            -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12FrameCommandStateOrder.cmake
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.AllocatorResetFailure
        COMMAND CueRhiD3d12FrameCommandProcess AllocatorResetFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.CommandListResetFailure
        COMMAND CueRhiD3d12FrameCommandProcess CommandListResetFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.CommandListCloseFailure
        COMMAND CueRhiD3d12FrameCommandProcess CommandListCloseFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.StateOrderResizeSuspend
        COMMAND ${CMAKE_COMMAND}
            -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12FrameCommandProcess>
            -DTEST_MODE=StateOrderResizeSuspend
            -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12FrameCommandStateOrder.cmake
    )
    add_test(
        NAME Cue.RHI.D3D12.FrameCommand.ResizePreparationFailureMatrix
        COMMAND CueRhiD3d12FrameCommandProcess ResizePreparationFailureMatrix
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.Smoke
        COMMAND CueRhiD3d12QueueStateProcess Smoke
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.Exhaustion
        COMMAND CueRhiD3d12QueueStateProcess Exhaustion
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.WaitRecoveryMatrix
        COMMAND CueRhiD3d12QueueStateProcess WaitRecoveryMatrix
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.WaitSentinelRefreshRemoval
        COMMAND CueRhiD3d12QueueStateProcess WaitSentinelRefreshRemoval
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.WaitFailedUnavailable
        COMMAND CueRhiD3d12QueueStateProcess WaitFailedUnavailable
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.StaleEventFollowup
        COMMAND CueRhiD3d12QueueStateProcess StaleEventFollowup
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.EventCloseFailure
        COMMAND CueRhiD3d12QueueStateProcess EventCloseFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.EventCreateFailure
        COMMAND CueRhiD3d12QueueStateProcess EventCreateFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.SignalCompletion
        COMMAND CueRhiD3d12QueueStateProcess SignalCompletion
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.SignalDeviceRemoved
        COMMAND CueRhiD3d12QueueStateProcess SignalDeviceRemoved
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.SignalCompletionDeviceRemovedRace
        COMMAND CueRhiD3d12QueueStateProcess SignalCompletionDeviceRemovedRace
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.SignalUnavailable
        COMMAND CueRhiD3d12QueueStateProcess SignalUnavailable
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.SignalEventCloseFailure
        COMMAND CueRhiD3d12QueueStateProcess SignalEventCloseFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.SignalEventCreateFailure
        COMMAND CueRhiD3d12QueueStateProcess SignalEventCreateFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.TerminalSignalCompletion
        COMMAND CueRhiD3d12QueueStateProcess TerminalSignalCompletion
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.TerminalSignalDeviceRemoved
        COMMAND CueRhiD3d12QueueStateProcess TerminalSignalDeviceRemoved
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.TerminalSignalUnavailable
        COMMAND CueRhiD3d12QueueStateProcess TerminalSignalUnavailable
    )
    add_test(
        NAME Cue.RHI.D3D12.QueueState.TerminalCloseFailure
        COMMAND CueRhiD3d12QueueStateProcess TerminalCloseFailure
    )
    add_test(
        NAME Cue.RHI.D3D12.RtvHeap.Smoke
        COMMAND CueRhiD3d12RtvHeapProcess Smoke
    )
    add_test(
        NAME Cue.RHI.D3D12.RtvHeap.InfoQueue
        COMMAND CueRhiD3d12RtvHeapProcess InfoQueue
    )
    add_test(
        NAME Cue.RHI.D3D12.RtvHeap.CreationFaults
        COMMAND CueRhiD3d12RtvHeapProcess CreationFaults
    )
    add_test(
        NAME Cue.RHI.D3D12.RtvHeap.HandleOverflow
        COMMAND CueRhiD3d12RtvHeapProcess HandleOverflow
    )
    add_test(
        NAME Cue.RHI.D3D12.RtvHeap.GenerationExhaustion
        COMMAND CueRhiD3d12RtvHeapProcess GenerationExhaustion
    )
    add_test(
        NAME Cue.RHI.D3D12.RtvHeap.HeapMove
        COMMAND CueRhiD3d12RtvHeapProcess HeapMove
    )

    foreach(violationMode IN ITEMS DoubleRelease StaleRelease ForeignRelease LiveSlotShutdown)
        add_test(
            NAME Cue.RHI.D3D12.RtvHeap.${violationMode}
            COMMAND
                ${CMAKE_COMMAND}
                -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12RtvHeapProcess>
                -DTEST_MODE=${violationMode}
                -DEXPECTED_EXIT_CODE=$<IF:$<CONFIG:Release>,0,90>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12RtvHeapViolation.cmake
        )
    endforeach()

    foreach(ownerViolationMode IN ITEMS OwnerDestructor ActiveMoveAssignment)
        add_test(
            NAME Cue.RHI.D3D12.RtvHeap.${ownerViolationMode}
            COMMAND
                ${CMAKE_COMMAND}
                -DTEST_EXECUTABLE=$<TARGET_FILE:CueRhiD3d12RtvHeapProcess>
                -DTEST_MODE=${ownerViolationMode}
                -DEXPECTED_EXIT_CODE=91
                -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyD3d12RtvHeapViolation.cmake
        )
    endforeach()
    add_test(NAME Cue.RHI.D3D12.SceneFrame.WarpPixel COMMAND CueRhiD3d12SceneFrameProcess Basic Warp)
    add_test(NAME Cue.RHI.D3D12.SceneFrame.HardwarePixel COMMAND CueRhiD3d12SceneFrameProcess Basic Hardware)
    add_test(NAME Cue.RHI.D3D12.SceneFrame.Depth.WarpPixel COMMAND CueRhiD3d12SceneFrameProcess Depth Warp)
    add_test(NAME Cue.RHI.D3D12.SceneFrame.Depth.HardwarePixel COMMAND CueRhiD3d12SceneFrameProcess Depth Hardware)
    add_test(NAME Cue.RHI.D3D12.SceneFrame.Depth.Resize.WarpPixel COMMAND CueRhiD3d12SceneFrameProcess DepthResize Warp)
    add_test(NAME Cue.RHI.D3D12.SceneFrame.Depth.Resize.HardwarePixel COMMAND CueRhiD3d12SceneFrameProcess DepthResize Hardware)
    add_test(NAME Cue.RHI.D3D12.SceneFrame.BackFaceCull.WarpPixel COMMAND CueRhiD3d12SceneFrameProcess Cull Warp)
    add_test(NAME Cue.RHI.D3D12.SceneFrame.BackFaceCull.HardwarePixel COMMAND CueRhiD3d12SceneFrameProcess Cull Hardware)
    set_tests_properties(
        Cue.RHI.D3D12.SceneFrame.HardwarePixel
        Cue.RHI.D3D12.SceneFrame.Depth.HardwarePixel
        Cue.RHI.D3D12.SceneFrame.Depth.Resize.HardwarePixel
        Cue.RHI.D3D12.SceneFrame.BackFaceCull.HardwarePixel
        PROPERTIES SKIP_RETURN_CODE 77
    )
    foreach(
        swapChainMode
        IN ITEMS SmokeVsync SmokeImmediate InfoQueue TearingMatrix PresentMatrix PresentSignalRecovery BeginFrameUnavailableRetention CloseFrameDeviceRemoved PresentUnavailableRetention SignalUnavailableRetention DirectPresentDeviceRemoved RecoverySignalDeviceRemoved RegularSignalDeviceRemoved SceneBeginFrameUnavailableRetention SceneSignalUnavailableRetention SceneDirectPresentDeviceRemoved CreationFaults LogFailure Validation ProductionOwnership ClearSmoke SceneSmoke SceneDeviceRemoval SceneResize SceneResizeHardware SceneResizeCreationFailure SceneResizePixelWarp SceneResizePixelHardware PresentFrames300 ResizeLifecycle ResizeFailure RtvRebuildFailure SceneRtvRebuildFailure TerminalResizeRejection ResizeUnavailableRetention DeviceRemovalLifecycle DeviceRemovalResize DeviceRemovalDredFailure
    )
        add_test(
            NAME Cue.RHI.D3D12.SwapChain.${swapChainMode}
            COMMAND CueRhiD3d12SwapChainProcess ${swapChainMode}
        )
    endforeach()
    set_tests_properties(
        Cue.RHI.D3D12.SwapChain.SceneResizePixelHardware
        Cue.RHI.D3D12.SwapChain.SceneResizeHardware
        PROPERTIES SKIP_RETURN_CODE 77
    )
    set_tests_properties(
        Cue.RHI.D3D12.SwapChain.DirectPresentDeviceRemoved
        Cue.RHI.D3D12.SwapChain.RecoverySignalDeviceRemoved
        Cue.RHI.D3D12.SwapChain.RegularSignalDeviceRemoved
        Cue.RHI.D3D12.SwapChain.SceneDirectPresentDeviceRemoved
        Cue.RHI.D3D12.SwapChain.SceneDeviceRemoval
        Cue.RHI.D3D12.SwapChain.DeviceRemovalLifecycle
        Cue.RHI.D3D12.SwapChain.DeviceRemovalDredFailure
        PROPERTIES SKIP_RETURN_CODE 77
    )
    add_test(
        NAME Cue.RHI.Dependencies
        COMMAND
            ${CMAKE_COMMAND}
            -DREPORT_FILE=${cueRhiDependencyReport}
            -DRHI_SOURCE_DIR=${PROJECT_SOURCE_DIR}/Engine/Source/RHI
            -DRHI_TARGET_FILE_LIST=${rhiTargetFileList}
            -DWINDOWS_SDK_VERSION=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/VerifyRHIDependencies.cmake
    )
endif()

if(WIN32)
    set_tests_properties(
        Cue.RHI.D3D12.AdapterSelection.Hardware
        Cue.RHI.D3D12.FrameCommand.InfoQueue300
        Cue.RHI.D3D12.RtvHeap.InfoQueue
        Cue.RHI.D3D12.SwapChain.InfoQueue
        PROPERTIES
            SKIP_RETURN_CODE 77
    )
endif()

set_tests_properties(
    ${cueRhiTests}
    PROPERTIES
        LABELS RHI
        TIMEOUT 10
)

if(WIN32)
    set_tests_properties(
        Cue.RHI.D3D12.SwapChain.PresentFrames300
        PROPERTIES TIMEOUT 30
    )
endif()

if(WIN32)
    set_tests_properties(
        Cue.RHI.D3D12.FrameCommand.Smoke300
        Cue.RHI.D3D12.FrameCommand.InfoQueue300
        PROPERTIES
            TIMEOUT 30
    )
endif()
