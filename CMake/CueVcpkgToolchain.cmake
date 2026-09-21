# CueEngineが検証済みvcpkg Toolchainだけを使用する

if(NOT DEFINED CUE_VCPKG_ROOT OR CUE_VCPKG_ROOT STREQUAL "")
    set(CUE_VCPKG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/.tools/vcpkg")
endif()
cmake_path(NORMAL_PATH CUE_VCPKG_ROOT)
set(CUE_VCPKG_ROOT "${CUE_VCPKG_ROOT}" CACHE PATH "Validated external vcpkg tool root" FORCE)

if(NOT IS_ABSOLUTE "${CUE_VCPKG_ROOT}")
    message(FATAL_ERROR "CUE_VCPKG_ROOT must be an absolute path.")
endif()

if(NOT DEFINED VCPKG_INSTALLED_DIR OR VCPKG_INSTALLED_DIR STREQUAL "")
    set(VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/vcpkg_installed")
endif()
cmake_path(NORMAL_PATH VCPKG_INSTALLED_DIR)

if(NOT IS_ABSOLUTE "${VCPKG_INSTALLED_DIR}")
    message(FATAL_ERROR "VCPKG_INSTALLED_DIR must be an absolute path.")
endif()

if(DEFINED CUE_ENGINE_INSTALLED_VERSION_ROOT AND NOT CUE_ENGINE_INSTALLED_VERSION_ROOT STREQUAL "")
    if(NOT IS_ABSOLUTE "${CUE_ENGINE_INSTALLED_VERSION_ROOT}")
        message(FATAL_ERROR "CUE_ENGINE_INSTALLED_VERSION_ROOT must be an absolute path.")
    endif()
    cmake_path(NORMAL_PATH CUE_ENGINE_INSTALLED_VERSION_ROOT)
    cmake_path(IS_PREFIX CUE_ENGINE_INSTALLED_VERSION_ROOT "${CUE_VCPKG_ROOT}" NORMALIZE cueToolInVersion)
    cmake_path(IS_PREFIX CUE_ENGINE_INSTALLED_VERSION_ROOT "${VCPKG_INSTALLED_DIR}" NORMALIZE cueInstallInVersion)
    if(cueToolInVersion OR cueInstallInVersion)
        message(FATAL_ERROR "vcpkg Tool and Install roots must remain outside the immutable installed version.")
    endif()
endif()

set(CUE_VCPKG_TOOLCHAIN "${CUE_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
if(NOT EXISTS "${CUE_VCPKG_TOOLCHAIN}")
    message(
        FATAL_ERROR
        "Pinned vcpkg toolchain is missing. Run Tools/Dependencies/RestoreVcpkg.ps1 first."
    )
endif()

set(VCPKG_INSTALLED_DIR "${VCPKG_INSTALLED_DIR}" CACHE PATH "Validated external vcpkg install root" FORCE)
set(VCPKG_MANIFEST_INSTALL OFF CACHE BOOL "Disable implicit dependency restore" FORCE)
list(
    APPEND
    CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    CUE_VCPKG_ROOT
    VCPKG_INSTALLED_DIR
    CUE_ENGINE_INSTALLED_VERSION_ROOT
)
include("${CUE_VCPKG_TOOLCHAIN}")
