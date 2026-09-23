#pragma once

#include <Cue/Build/Toolchain.h>

namespace cue
{
class AssertContext;

/// @brief このCueEngine Binaryが対応するWindows Toolchain要件を返す
///
/// AssertContextは呼出中だけ借用し、返却値は全要件を所有する。独立した呼出しは並行可能。Allocation失敗はFatalHandlerへ渡す。
[[nodiscard]] BuildEnvironmentRequirements current_windows_build_requirements(
    const AssertContext &a_assertContext) noexcept;

/// @brief Current DirectoryやPATHに依存せずEngine Build MetadataからWindows Tool候補を検出する
///
/// Tool executableは原則起動せず、存在、File Version、Binary種別を検査する。Gitだけは`git --version`を実行して
/// Git for Windows Identityを検査する。Windows SDKはEngine Build時に記録したRoot、Version、Header、x64 Libraryを検査する。
/// AssertContextは呼出中だけ借用し、返却値は全Dataを所有する。
/// 独立した呼出しは並行可能でProject Fileを変更しない。Allocation失敗はFatalHandlerへ渡す。
[[nodiscard]] BuildEnvironmentInventory discover_current_windows_build_environment(
    const AssertContext &a_assertContext) noexcept;

/// @brief 現在のWindows HostをEngine Build要件と照合した実行前Reportを返す
///
/// AssertContextは呼出中だけ借用し、返却値は全Dataを所有する。独立した呼出しは並行可能。Allocation失敗はFatalHandlerへ渡す。
[[nodiscard]] BuildEnvironmentReport validate_current_windows_build_environment(
    const AssertContext &a_assertContext) noexcept;
} // namespace cue
