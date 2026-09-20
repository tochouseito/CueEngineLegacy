#include <Windows.h>

#include <Cue/Editor/ImGui/BuildPresenter.h>
#include <Cue/Editor/ImGui/DebugView.h>
#include <Cue/Editor/ImGui/EditorDockspace.h>
#include <Cue/Editor/ImGui/EditorPresenter.h>
#include <Cue/Editor/ImGui/FilesPresenter.h>
#include <Cue/Editor/ImGui/GameView.h>
#include <Cue/Editor/ImGui/PackagePresenter.h>
#include <Cue/Editor/ImGui/PlayInputRouting.h>
#include <Cue/Editor/ImGui/PlaySessionPresenter.h>
#include <Cue/Editor/ImGui/SessionLog.h>

#include <type_traits>

static_assert(!std::is_copy_constructible_v<cue::editor::BuildPresenter>);
static_assert(!std::is_move_constructible_v<cue::editor::BuildPresenter>);
static_assert(!std::is_copy_constructible_v<cue::editor::EditorPresenter>);
static_assert(!std::is_move_constructible_v<cue::editor::EditorPresenter>);
static_assert(!std::is_copy_constructible_v<cue::editor::FilesPresenter>);
static_assert(!std::is_move_constructible_v<cue::editor::FilesPresenter>);
static_assert(!std::is_copy_constructible_v<cue::editor::PackagePresenter>);
static_assert(!std::is_move_constructible_v<cue::editor::PackagePresenter>);
static_assert(!std::is_copy_constructible_v<cue::editor::PlaySessionPresenter>);
static_assert(!std::is_move_constructible_v<cue::editor::PlaySessionPresenter>);
static_assert(!std::is_copy_constructible_v<cue::editor::EditorSessionLogRouter>);

/// @brief Editor ImGui公開Headerが単独利用できることを検証する
int main()
{
    return 0;
}
