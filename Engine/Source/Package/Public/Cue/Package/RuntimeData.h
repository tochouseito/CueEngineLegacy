#pragma once

#include <Cue/Foundation/Result.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/Scene/Instantiation.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cue
{
class AssertContext;
}

namespace cue::package
{
inline constexpr std::uint32_t k_runtimeProjectDataSchemaVersion = 1U;
inline constexpr std::uint32_t k_runtimeSceneDataSchemaVersion = 1U;
inline constexpr std::uint32_t k_runtimeSceneDataWithRendererSchemaVersion = 2U;
inline constexpr std::size_t k_maximumRuntimeProjectDataBytes = 1024U * 1024U;
inline constexpr std::size_t k_maximumRuntimeSceneDataBytes = 64U * 1024U * 1024U;

/// @brief Package Stagingへ渡す一つの検証済みCanonical Runtime Data File
class RuntimeDataFile final
{
  public:
    /// @brief 無効な空Fileを作らせないため既定構築を禁止する
    RuntimeDataFile() = delete;
    /// @brief 所有Dataを複製する
    RuntimeDataFile(const RuntimeDataFile &) = default;
    /// @brief 所有Dataを複製代入する
    RuntimeDataFile &operator=(const RuntimeDataFile &) = default;
    /// @brief 所有Dataを移動する
    RuntimeDataFile(RuntimeDataFile &&) noexcept = default;
    /// @brief 所有Dataを移動代入する
    RuntimeDataFile &operator=(RuntimeDataFile &&) noexcept = default;
    /// @brief 所有Dataを破棄する
    ~RuntimeDataFile() = default;

    /// @brief Package Root相対の固定Pathを返す
    [[nodiscard]] std::string_view relative_path() const noexcept;
    /// @brief UTF-8、BOMなし、LF終端のCanonical JSON Byte列を返す
    [[nodiscard]] std::string_view bytes() const noexcept;
    /// @brief Byte列の未変換Sizeを返す
    [[nodiscard]] std::uint64_t byte_size() const noexcept;
    /// @brief Byte列のlowercase SHA-256を返す
    [[nodiscard]] std::string_view sha256() const noexcept;

  private:
    friend class MinimalRuntimeDataPublication;
    friend Result<MinimalRuntimeDataPublication> publish_minimal_runtime_data(const ProjectDescriptor &,
                                                                              const scene::SceneSnapshot &,
                                                                              const AssertContext &) noexcept;

    /// @brief 検証済みPath、Byte列、Digestを一つのFileへ束ねる
    RuntimeDataFile(std::string a_relativePath, std::string a_bytes, std::string a_sha256) noexcept;

    std::string m_relativePath;
    std::string m_bytes;
    std::string m_sha256;
};

/// @brief ProjectとStartup Sceneから生成した二つのRuntime Data Fileを一括所有する結果
class MinimalRuntimeDataPublication final
{
  public:
    /// @brief 無効な部分Publicationを作らせないため既定構築を禁止する
    MinimalRuntimeDataPublication() = delete;
    /// @brief 所有Publicationを複製する
    MinimalRuntimeDataPublication(const MinimalRuntimeDataPublication &) = default;
    /// @brief 所有Publicationを複製代入する
    MinimalRuntimeDataPublication &operator=(const MinimalRuntimeDataPublication &) = default;
    /// @brief 所有Publicationを移動する
    MinimalRuntimeDataPublication(MinimalRuntimeDataPublication &&) noexcept = default;
    /// @brief 所有Publicationを移動代入する
    MinimalRuntimeDataPublication &operator=(MinimalRuntimeDataPublication &&) noexcept = default;
    /// @brief 所有Publicationを破棄する
    ~MinimalRuntimeDataPublication() = default;

    /// @brief Runtime Project Data Fileを返す
    [[nodiscard]] const RuntimeDataFile &project_data() const noexcept;
    /// @brief Startup Scene Runtime Data Fileを返す
    [[nodiscard]] const RuntimeDataFile &startup_scene_data() const noexcept;
    /// @brief Runtime Project Dataへ固定したProjectIdを返す
    [[nodiscard]] std::string_view project_id() const noexcept;
    /// @brief 両Dataへ固定したStartup SceneAssetIdを返す
    [[nodiscard]] std::string_view startup_scene_asset_id() const noexcept;

  private:
    friend Result<MinimalRuntimeDataPublication> publish_minimal_runtime_data(const ProjectDescriptor &,
                                                                              const scene::SceneSnapshot &,
                                                                              const AssertContext &) noexcept;

    /// @brief 完全に生成・検証済みの二FileとIdentityを所有する
    MinimalRuntimeDataPublication(RuntimeDataFile a_projectData, RuntimeDataFile a_startupSceneData,
                                  std::string a_projectId, std::string a_startupSceneAssetId) noexcept;

    RuntimeDataFile m_projectData;
    RuntimeDataFile m_startupSceneData;
    std::string m_projectId;
    std::string m_startupSceneAssetId;
};

/// @brief Project Descriptorと不変Scene Snapshotを決定的な最小Runtime Dataへ一方向変換する
///
/// 返却成功までFilesystemへ書き込まず、未知ComponentまたはFieldを検出した場合は部分Outputを返さない。
[[nodiscard]] Result<MinimalRuntimeDataPublication> publish_minimal_runtime_data(
    const ProjectDescriptor &a_descriptor, const scene::SceneSnapshot &a_startupScene,
    const AssertContext &a_assertContext) noexcept;
} // namespace cue::package
