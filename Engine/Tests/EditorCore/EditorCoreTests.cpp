#include <Cue/EditorCore/EditorController.h>
#include <Cue/EditorCore/EditorIntent.h>
#include <Cue/EditorCore/Error.h>
#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>
#include <Cue/IO/Error.h>
#include <Cue/IO/Filesystem.h>
#include <Cue/Math/Transform.h>
#include <Cue/Project/Descriptor.h>
#include <Cue/Scene/Error.h>
#include <Cue/Scene/Identity.h>
#include <Cue/Scene/SceneDocument.h>
#include <Cue/Schema/Descriptor.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief Test 中の通常 Fatal を Process 失敗へ変換する
    [[noreturn]] void terminate() noexcept override
    {
        std::abort();
    }

    /// @brief Test 中の予期しない Fatal を Process 失敗へ変換する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::abort();
    }
};

/// @brief Editor IntentのObject／Component生成へ重複しないUUID Version 4候補を返す
class TestSceneIdentitySource final : public cue::scene::SceneIdentitySource
{
  public:
    /// @brief Counterを埋め込んだRFC 4122 UUID Version 4候補を返す
    [[nodiscard]] cue::scene::IdentityBytes next_identity() noexcept override
    {
        cue::scene::IdentityBytes bytes{};
        for (std::size_t index = 0U; index < sizeof(m_next); ++index)
        {
            bytes[bytes.size() - 1U - index] = static_cast<std::uint8_t>((m_next >> (index * 8U)) & 0xFFU);
        }
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
        ++m_next;
        return bytes;
    }

  private:
    std::uint64_t m_next = 0x900U;
};

/// @brief Editor Persistence Test用のRoot境界付きMemory Filesystem
class MemoryFilesystemRoot final : public cue::FilesystemRoot
{
  public:
    /// @brief 診断Contextを非所有で保持する空Filesystemを構築する
    explicit MemoryFilesystemRoot(const cue::AssertContext &a_assertContext) noexcept
        : m_assertContext(&a_assertContext)
    {
    }

    /// @brief Memory Filesystemの複製を禁止する
    MemoryFilesystemRoot(const MemoryFilesystemRoot &) = delete;
    /// @brief Memory Filesystemの複製代入を禁止する
    MemoryFilesystemRoot &operator=(const MemoryFilesystemRoot &) = delete;
    /// @brief Memory Filesystemの移動を禁止する
    MemoryFilesystemRoot(MemoryFilesystemRoot &&) = delete;
    /// @brief Memory Filesystemの移動代入を禁止する
    MemoryFilesystemRoot &operator=(MemoryFilesystemRoot &&) = delete;
    /// @brief 所有Byte列を破棄する
    ~MemoryFilesystemRoot() override = default;

    /// @brief Test Double InstanceをMemory Root Identityとして返す
    [[nodiscard]] cue::Result<cue::FilesystemIdentity> root_identity() const noexcept override
    {
        return cue::Result<cue::FilesystemIdentity>::success(make_filesystem_identity(
            0x544553544D454D31ULL, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this))));
    }

    /// @brief Test用File本文を直接設定する
    void set(std::string_view a_path, std::string_view a_text)
    {
        const auto bytes = std::as_bytes(std::span(a_text.data(), a_text.size()));
        m_files[std::string(a_path)] = std::vector<std::byte>(bytes.begin(), bytes.end());
    }

    /// @brief Test用File本文をText Viewで返す
    [[nodiscard]] std::string_view text(std::string_view a_path) const noexcept
    {
        const auto found = m_files.find(std::string(a_path));
        if (found == m_files.end())
        {
            return {};
        }
        return {reinterpret_cast<const char *>(found->second.data()), found->second.size()};
    }

    /// @brief Test用Recovery Backup本文を内部名前空間から返す
    [[nodiscard]] std::string_view recovery_backup_text(std::string_view a_destination) const noexcept
    {
        return text(recovery_backup_key(a_destination));
    }

    /// @brief 指定PathのAtomic Write失敗注入を切り替える
    void fail_write(std::string_view a_path, bool a_fail)
    {
        m_failedPath = a_fail ? std::string(a_path) : std::string{};
    }

    /// @brief 指定PathのRead失敗注入を切り替える
    void fail_read(std::string_view a_path, bool a_fail)
    {
        m_failedReadPath = a_fail ? std::string(a_path) : std::string{};
    }

    /// @brief 指定PathのWrite Lease取得失敗注入を切り替える
    void fail_lease(std::string_view a_path, bool a_fail)
    {
        m_failedLeasePath = a_fail ? std::string(a_path) : std::string{};
    }

    /// @brief 指定Pathの次回Writeを公開済みDurability不明として報告する
    void make_write_uncertain(std::string_view a_path, bool a_uncertain)
    {
        m_uncertainPath = a_uncertain ? std::string(a_path) : std::string{};
    }

    /// @brief 指定Destinationの次回Recovery Backup公開をDurability不明として報告する
    void make_recovery_backup_uncertain(std::string_view a_destination, bool a_uncertain)
    {
        m_uncertainRecoveryBackupPath = a_uncertain ? std::string(a_destination) : std::string{};
    }

    /// @brief 次回Conditional Publish直前に外部変更を注入する
    void mutate_before_publish(std::string_view a_path, std::string_view a_text)
    {
        m_mutationPath = std::string(a_path);
        m_mutationText = std::string(a_text);
    }

    /// @brief 指定回数のFingerprint Read後、次のRead直前に外部変更を注入する
    void mutate_before_read(std::string_view a_path, std::string_view a_text, std::size_t a_matchingReadsToSkip)
    {
        m_preReadMutationPath = std::string(a_path);
        m_preReadMutationText = std::string(a_text);
        m_preReadMutationSkips = a_matchingReadsToSkip;
    }

    /// @brief 指定PathのFile削除失敗注入を切り替える
    void fail_remove(std::string_view a_path, bool a_fail)
    {
        m_failedRemovePath = a_fail ? std::string(a_path) : std::string{};
    }

    /// @brief 指定Pathの次回Publish後Readを破損Dataとして返す
    void fail_next_verification(std::string_view a_path)
    {
        m_verificationFailurePath = std::string(a_path);
    }

    /// @brief Publish後ReadがCandidateを返した直後に外部変更を注入する
    void mutate_after_verification_read(std::string_view a_path, std::string_view a_text)
    {
        m_armPostVerificationMutationPath = std::string(a_path);
        m_armPostVerificationMutationText = std::string(a_text);
    }

    /// @brief Recovery Backup公開成功直後に本文への外部変更を注入する
    void mutate_main_after_backup_write(std::string_view a_destination, std::string_view a_text)
    {
        m_backupWriteMutationPath = std::string(a_destination);
        m_backupWriteMutationText = std::string(a_text);
    }

    /// @brief Memory上のEntry種別を返す
    [[nodiscard]] cue::Result<cue::EntryType> query_entry(const cue::RelativePath &a_path) noexcept override
    {
        return cue::Result<cue::EntryType>::success(
            m_files.contains(std::string(a_path.text())) ? cue::EntryType::RegularFile : cue::EntryType::Missing);
    }

    /// @brief 上限内のMemory Fileを複製して返す
    [[nodiscard]] cue::Result<std::vector<std::byte>> read_file(const cue::RelativePath &a_path,
                                                                std::size_t a_maxBytes) noexcept override
    {
        if (a_path.text() == m_failedReadPath)
        {
            return cue::Result<std::vector<std::byte>>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected read failure"));
        }
        if (a_path.text() == m_preReadMutationPath)
        {
            if (m_preReadMutationSkips == 0U)
            {
                set(m_preReadMutationPath, m_preReadMutationText);
                m_preReadMutationPath.clear();
                m_preReadMutationText.clear();
            }
            else
            {
                --m_preReadMutationSkips;
            }
        }
        if (a_path.text() == m_corruptNextReadPath)
        {
            m_corruptNextReadPath.clear();
            constexpr std::string_view invalid = "invalid verification data";
            const auto bytes = std::as_bytes(std::span(invalid.data(), invalid.size()));
            return cue::Result<std::vector<std::byte>>::success(std::vector<std::byte>(bytes.begin(), bytes.end()));
        }
        const auto found = m_files.find(std::string(a_path.text()));
        if (found == m_files.end())
        {
            return cue::Result<std::vector<std::byte>>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::NotFound, "Memory file was not found"));
        }
        if (found->second.size() > a_maxBytes)
        {
            return cue::Result<std::vector<std::byte>>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::CapacityExceeded, "Memory file exceeds limit"));
        }
        std::vector<std::byte> result(found->second);
        if (a_path.text() == m_postVerificationMutationPath)
        {
            set(m_postVerificationMutationPath, m_postVerificationMutationText);
            m_postVerificationMutationPath.clear();
            m_postVerificationMutationText.clear();
        }
        return cue::Result<std::vector<std::byte>>::success(std::move(result));
    }

    /// @brief Memory TestではDirectory作成を副作用なしで成功させる
    [[nodiscard]] cue::Result<void> create_directories(const cue::RelativePath &) noexcept override
    {
        return cue::Result<void>::success();
    }

    /// @brief 指定PathへByte列を一回で公開する
    [[nodiscard]] cue::Result<void> write_file_atomic(const cue::RelativePath &a_path,
                                                      std::span<const std::byte> a_bytes) noexcept override
    {
        if (a_path.text() == m_failedPath)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected atomic write failure"));
        }
        m_files[std::string(a_path.text())] = std::vector<std::byte>(a_bytes.begin(), a_bytes.end());
        if (a_path.text() == m_armPostVerificationMutationPath)
        {
            m_postVerificationMutationPath = std::move(m_armPostVerificationMutationPath);
            m_postVerificationMutationText = std::move(m_armPostVerificationMutationText);
            m_armPostVerificationMutationPath.clear();
            m_armPostVerificationMutationText.clear();
        }
        if (a_path.text() == m_verificationFailurePath)
        {
            m_corruptNextReadPath = m_verificationFailurePath;
            m_verificationFailurePath.clear();
        }
        if (a_path.text() == m_uncertainPath)
        {
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::DurabilityUnknown,
                                                                 "Injected durability uncertainty"));
        }
        return cue::Result<void>::success();
    }

    /// @brief Recovery Backup公開後の本文競合をTest注入できるようにする
    [[nodiscard]] cue::Result<void> write_recovery_backup_atomic(const cue::RelativePath &a_destination,
                                                                 std::span<const std::byte> a_bytes,
                                                                 const cue::AssertContext &) noexcept override
    {
        m_files[recovery_backup_key(a_destination.text())] = std::vector<std::byte>(a_bytes.begin(), a_bytes.end());
        if (a_destination.text() == m_backupWriteMutationPath)
        {
            set(m_backupWriteMutationPath, m_backupWriteMutationText);
            m_backupWriteMutationPath.clear();
            m_backupWriteMutationText.clear();
        }
        if (a_destination.text() == m_uncertainRecoveryBackupPath)
        {
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::DurabilityUnknown,
                                                                 "Injected backup durability uncertainty"));
        }
        return cue::Result<void>::success();
    }

    /// @brief Memory Test内でDestination所有情報を保持するLeaseを発行する
    [[nodiscard]] cue::Result<cue::FileWriteLease> acquire_file_write_lease(
        const cue::RelativePath &a_path) noexcept override
    {
        if (a_path.text() == m_failedLeasePath)
        {
            return cue::Result<cue::FileWriteLease>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::Busy, "Injected write lease contention"));
        }
        auto state = std::make_unique<MemoryFileWriteLeaseState>(this, std::string(a_path.text()));
        return cue::Result<cue::FileWriteLease>::success(make_file_write_lease(std::move(state)));
    }

    /// @brief Publish直前のFingerprint一致を検査してMemory Fileを更新する
    [[nodiscard]] cue::Result<void> write_file_atomic_if_unchanged(cue::FileWriteLease &a_lease,
                                                                   const cue::RelativePath &a_path,
                                                                   cue::FileFingerprint a_expected,
                                                                   std::size_t a_maximumExpectedBytes,
                                                                   std::span<const std::byte> a_bytes) noexcept override
    {
        auto *state = dynamic_cast<MemoryFileWriteLeaseState *>(file_write_lease_state(a_lease));
        if (state == nullptr || state->owner != this || state->path != a_path.text())
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::PreconditionFailed, "Memory lease mismatch"));
        }
        if (a_path.text() == m_mutationPath)
        {
            set(m_mutationPath, m_mutationText);
            m_mutationPath.clear();
            m_mutationText.clear();
        }
        auto current = cue::fingerprint_file(*this, a_path, a_maximumExpectedBytes, *m_assertContext);
        if (!current || *current.try_value() != a_expected)
        {
            return cue::Result<void>::failure(cue::make_io_error(*m_assertContext, cue::IoError::PreconditionFailed,
                                                                 "Memory file changed before publish"));
        }
        return write_file_atomic(a_path, a_bytes);
    }

    /// @brief Memory Fileを削除し、存在しない場合も成功する
    [[nodiscard]] cue::Result<void> remove_file(const cue::RelativePath &a_path) noexcept override
    {
        if (a_path.text() == m_failedRemovePath)
        {
            return cue::Result<void>::failure(
                cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Injected file removal failure"));
        }
        m_files.erase(std::string(a_path.text()));
        return cue::Result<void>::success();
    }

    /// @brief Scene Persistence対象外のStaging作成を拒否する
    [[nodiscard]] cue::Result<cue::StagingArea> create_staging_area(const cue::RelativePath &) noexcept override
    {
        return cue::Result<cue::StagingArea>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Staging is not used"));
    }

    /// @brief Scene Persistence対象外のStaging公開を拒否する
    [[nodiscard]] cue::Result<void> publish_staging_area(
        cue::StagingArea &&, const cue::RelativePath &,
        const cue::StagingPublishAuthorization * = nullptr) noexcept override
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Staging is not used"));
    }

    /// @brief Scene Persistence対象外のStaging破棄を拒否する
    [[nodiscard]] cue::Result<void> rollback_staging_area(cue::StagingArea &&) noexcept override
    {
        return cue::Result<void>::failure(
            cue::make_io_error(*m_assertContext, cue::IoError::IoFailure, "Staging is not used"));
    }

  private:
    /// @brief 利用者RelativePathでは表現できないTest用Recovery Backup Keyを生成する
    [[nodiscard]] static std::string recovery_backup_key(std::string_view a_destination)
    {
        std::string key(1U, '\x1f');
        key.append("CueBackup:");
        key.append(a_destination);
        return key;
    }

    struct MemoryFileWriteLeaseState final : cue::FileWriteLeaseState
    {
        MemoryFileWriteLeaseState(const MemoryFilesystemRoot *a_owner, std::string a_path) noexcept
            : owner(a_owner), path(std::move(a_path))
        {
        }

        const MemoryFilesystemRoot *owner;
        std::string path;
    };

    std::map<std::string, std::vector<std::byte>> m_files;
    std::string m_failedPath;
    std::string m_failedReadPath;
    std::string m_failedLeasePath;
    std::string m_uncertainPath;
    std::string m_uncertainRecoveryBackupPath;
    std::string m_mutationPath;
    std::string m_mutationText;
    std::string m_preReadMutationPath;
    std::string m_preReadMutationText;
    std::size_t m_preReadMutationSkips = 0U;
    std::string m_failedRemovePath;
    std::string m_verificationFailurePath;
    std::string m_corruptNextReadPath;
    std::string m_armPostVerificationMutationPath;
    std::string m_armPostVerificationMutationText;
    std::string m_postVerificationMutationPath;
    std::string m_postVerificationMutationText;
    std::string m_backupWriteMutationPath;
    std::string m_backupWriteMutationText;
    const cue::AssertContext *m_assertContext;
};

/// @brief 条件が偽なら Test Process を失敗終了する
void require(bool a_condition) noexcept
{
    if (!a_condition)
    {
        std::abort();
    }
}

/// @brief Error が指定した診断 Context を含むか判定する
[[nodiscard]] bool has_error_context(const cue::Error &a_error, std::string_view a_expected) noexcept
{
    for (const cue::ErrorContext &context : a_error.contexts())
    {
        if (context.message() == a_expected)
        {
            return true;
        }
    }
    return false;
}

/// @brief 成功 Result から所有 Value を取り出す
template <typename T> T take_value(cue::Result<T> &&a_result) noexcept
{
    require(a_result.has_value());
    return std::move(*a_result.try_value());
}

/// @brief 固定 Identity から Project Descriptor を生成する
cue::ProjectDescriptor make_project_descriptor(const cue::AssertContext &a_assertContext) noexcept
{
    auto projectId = take_value(cue::ProjectId::parse("00000000-0000-4000-8000-000000000001", a_assertContext));
    return take_value(cue::create_blank_project_descriptor(
        projectId, "Editor Core Test", cue::EngineCompatibility{cue::EngineVersion{1U, 0U, 0U}, std::nullopt},
        "00000000-0000-4000-8000-000000000099", a_assertContext));
}

/// @brief 固定 Identity から空 Scene Document を生成する
cue::scene::SceneDocument make_scene_document(std::string_view a_sceneId,
                                              const cue::AssertContext &a_assertContext) noexcept
{
    auto sceneId = take_value(cue::scene::SceneAssetId::parse(a_sceneId, a_assertContext));
    return cue::scene::SceneDocument::create(std::move(sceneId), a_assertContext);
}

/// @brief 固定 Identity から Object ID を生成する
cue::scene::ObjectId make_object_id(std::string_view a_objectId, const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::scene::ObjectId::parse(a_objectId, a_assertContext));
}

/// @brief 固定 Identity から Component Instance ID を生成する
cue::scene::ComponentInstanceId make_component_id(std::string_view a_componentId,
                                                  const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::scene::ComponentInstanceId::parse(a_componentId, a_assertContext));
}

/// @brief Scene Command Test 用 Stable Component Type Identity を生成する
cue::schema::TypeId make_component_type_id(const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::TypeId::parse("10000000-0000-4000-8000-000000000001", a_assertContext));
}

/// @brief Scene Command Test 用 Stable Field Identity を生成する
cue::schema::FieldId make_health_field_id(const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::FieldId::create(1U, a_assertContext));
}

/// @brief Scene Command Test 用 Asset Reference Field Identity を生成する
cue::schema::FieldId make_asset_field_id(const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::FieldId::create(2U, a_assertContext));
}

/// @brief Scene Command Test 用 Schema Version を生成する
cue::schema::SchemaVersion make_component_version(const cue::AssertContext &a_assertContext) noexcept
{
    return take_value(cue::schema::SchemaVersion::create(1U, a_assertContext));
}

/// @brief Health Field を持つ Scene Command Test 用 Schema Registry を構築する
std::unique_ptr<cue::schema::SchemaRegistry> make_component_registry(
    cue::schema::SchemaRegistryIdentitySource &a_identitySource, const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::schema::FieldDescriptor> fields;
    fields.push_back(take_value(
        cue::schema::create_field_descriptor(make_health_field_id(a_assertContext), "health", a_assertContext)));
    fields.push_back(take_value(
        cue::schema::create_field_descriptor(make_asset_field_id(a_assertContext), "asset", a_assertContext)));
    std::vector<cue::schema::FieldId> reserved;
    auto descriptor = take_value(cue::schema::create_type_descriptor(
        make_component_type_id(a_assertContext), "Cue.EditorCore.TestComponent",
        make_component_version(a_assertContext), std::move(fields), std::move(reserved), a_assertContext));
    cue::schema::SchemaRegistryBuilder builder(a_identitySource, a_assertContext);
    require(builder.add_type(std::move(descriptor)).has_value());
    return take_value(builder.seal());
}

/// @brief Health Field の値 Kind を固定する Scene Component Value Registry を構築する
cue::scene::ComponentValueSchemaRegistry make_component_value_registry(
    const cue::schema::SchemaRegistry &a_registry, const cue::AssertContext &a_assertContext) noexcept
{
    std::vector<cue::scene::FieldKindBinding> bindings{
        {make_health_field_id(a_assertContext), cue::scene::FieldValueKind::SignedInteger},
        {make_asset_field_id(a_assertContext), cue::scene::FieldValueKind::AssetReference}};
    std::vector<cue::scene::ComponentValueSchema> schemas;
    schemas.push_back(take_value(cue::scene::create_component_value_schema(
        make_component_type_id(a_assertContext), make_component_version(a_assertContext), std::move(bindings),
        a_registry, a_assertContext)));
    return take_value(
        cue::scene::ComponentValueSchemaRegistry::create(std::move(schemas), a_registry, a_assertContext));
}

/// @brief 指定 Stable Identity と Health 値を持つ既知 Component を生成する
cue::scene::SceneComponent make_health_component(cue::scene::ComponentInstanceId a_componentId, std::int64_t a_health,
                                                 const cue::schema::SchemaRegistry &a_registry,
                                                 const cue::scene::ComponentValueSchemaRegistry &a_valueRegistry,
                                                 const cue::AssertContext &a_assertContext,
                                                 std::string_view a_assetId = "asset://default") noexcept
{
    std::vector<cue::scene::KnownFieldData> fields;
    fields.push_back(take_value(cue::scene::create_known_field(
        make_health_field_id(a_assertContext), cue::scene::FieldValue::signed_integer(a_health),
        cue::scene::FieldValueKind::SignedInteger, a_assertContext)));
    auto assetReference = take_value(cue::scene::AssetReferenceValue::create(a_assetId, a_assertContext));
    fields.push_back(take_value(cue::scene::create_known_field(
        make_asset_field_id(a_assertContext), cue::scene::FieldValue::asset_reference(std::move(assetReference)),
        cue::scene::FieldValueKind::AssetReference, a_assertContext)));
    std::vector<cue::scene::OpaqueFieldData> unknownFields;
    auto component = take_value(cue::scene::create_known_component(
        std::move(a_componentId), make_component_type_id(a_assertContext), make_component_version(a_assertContext),
        std::move(fields), std::move(unknownFields), a_registry, a_valueRegistry, a_assertContext));
    return cue::scene::SceneComponent::known(std::move(component));
}

/// @brief Object 内の指定 Component から Health 値を取得する
std::int64_t component_health(const cue::scene::SceneObject &a_object,
                              const cue::scene::ComponentInstanceId &a_componentId) noexcept
{
    for (const cue::scene::SceneComponent &component : a_object.components())
    {
        if (component.instance_id() != a_componentId)
        {
            continue;
        }
        const cue::scene::KnownComponentData *known = component.try_known();
        require(known != nullptr);
        for (const cue::scene::KnownFieldData &field : known->known_fields())
        {
            if (field.id().value() == 1U)
            {
                const std::int64_t *value = field.value().try_signed_integer();
                require(value != nullptr);
                return *value;
            }
        }
    }
    std::abort();
}

/// @brief Object 内の指定 Component から Asset Reference Token を取得する
std::string_view component_asset_token(const cue::scene::SceneObject &a_object,
                                       const cue::scene::ComponentInstanceId &a_componentId) noexcept
{
    for (const cue::scene::SceneComponent &component : a_object.components())
    {
        if (component.instance_id() != a_componentId)
        {
            continue;
        }
        const cue::scene::KnownComponentData *known = component.try_known();
        require(known != nullptr);
        for (const cue::scene::KnownFieldData &field : known->known_fields())
        {
            if (field.id().value() == 2U)
            {
                const cue::scene::AssetReferenceValue *value = field.value().try_asset_reference();
                require(value != nullptr);
                return value->token();
            }
        }
    }
    std::abort();
}

/// @brief Project Session と Scene Open の一意性を検証する
void test_workspace_and_open() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    auto controller = cue::editor_core::EditorController::create(make_project_descriptor(assertContext), assertContext);

    auto scene = make_scene_document("00000000-0000-4000-8000-000000000101", assertContext);
    auto locator = take_value(cue::RelativePath::parse("Scenes/Main.cuescene", assertContext));
    const auto documentId = take_value(controller->open_document(std::move(scene), std::move(locator), true));

    require(controller->session().project_descriptor().display_name() == "Editor Core Test");
    require(controller->session().documents().size() == 1U);
    const auto *document = controller->session().find_document(documentId);
    require(document != nullptr);
    require(document->scene_locator().text() == "Scenes/Main.cuescene");
    require(document->has_saved_destination());

    auto duplicateScene = make_scene_document("00000000-0000-4000-8000-000000000101", assertContext);
    auto otherLocator = take_value(cue::RelativePath::parse("Scenes/Other.cuescene", assertContext));
    const auto duplicateSceneResult =
        controller->open_document(std::move(duplicateScene), std::move(otherLocator), true);
    require(!duplicateSceneResult.has_value());
    require(duplicateSceneResult.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::DuplicateScene));
    require(has_error_context(*duplicateSceneResult.try_error(), "ConflictingEditorDocumentId=1"));
    require(has_error_context(*duplicateSceneResult.try_error(),
                              "RequestedSceneAssetId=00000000-0000-4000-8000-000000000101"));

    auto otherScene = make_scene_document("00000000-0000-4000-8000-000000000102", assertContext);
    auto duplicateLocator = take_value(cue::RelativePath::parse("scenes/main.cuescene", assertContext));
    const auto duplicateLocatorResult =
        controller->open_document(std::move(otherScene), std::move(duplicateLocator), true);
    require(!duplicateLocatorResult.has_value());
    require(duplicateLocatorResult.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::DuplicateLocator));
    require(has_error_context(*duplicateLocatorResult.try_error(), "ConflictingEditorDocumentId=1"));
    require(has_error_context(*duplicateLocatorResult.try_error(), "RequestedSceneLocator"));
    require(has_error_context(*duplicateLocatorResult.try_error(), "scenes/main.cuescene"));
}

/// @brief Dirty が Revision 差だけから一貫して決まることを検証する
void test_revision_and_dirty() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    auto controller = cue::editor_core::EditorController::create(make_project_descriptor(assertContext), assertContext);
    auto scene = make_scene_document("00000000-0000-4000-8000-000000000201", assertContext);
    auto locator = take_value(cue::RelativePath::parse("Scenes/Revision.cuescene", assertContext));
    const auto documentId = take_value(controller->open_document(std::move(scene), std::move(locator), true));
    const auto *document = controller->session().find_document(documentId);

    require(document != nullptr);
    require(document->current_state_id().value() == 1U);
    require(document->saved_state_id().value() == 1U);
    require(!document->is_dirty());

    const auto secondState = take_value(controller->record_persistent_change(documentId));
    require(secondState.value() == 2U);
    document = controller->session().find_document(documentId);
    require(document != nullptr);
    require(document->is_dirty());
    require(controller->mark_saved(documentId, secondState).has_value());
    document = controller->session().find_document(documentId);
    require(document != nullptr);
    require(!document->is_dirty());

    const auto thirdState = take_value(controller->record_persistent_change(documentId));
    require(thirdState.value() == 3U);
    document = controller->session().find_document(documentId);
    require(document != nullptr);
    require(controller->mark_saved(documentId, secondState).has_value());
    document = controller->session().find_document(documentId);
    require(document != nullptr);
    require(document->is_dirty());

    const auto missingDocument = controller->mark_saved(cue::editor_core::EditorDocumentId(999U), secondState);
    require(!missingDocument.has_value());
    require(missingDocument.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::DocumentNotFound));
    require(has_error_context(*missingDocument.try_error(), "EditorDocumentId=999"));

    auto secondScene = make_scene_document("00000000-0000-4000-8000-000000000202", assertContext);
    auto secondLocator = take_value(cue::RelativePath::parse("Scenes/SecondRevision.cuescene", assertContext));
    const auto secondDocumentId =
        take_value(controller->open_document(std::move(secondScene), std::move(secondLocator), true));
    const auto secondDocumentState = take_value(controller->record_persistent_change(secondDocumentId));
    require(secondDocumentState.value() == secondState.value());
    require(secondDocumentState.document_id() == secondDocumentId);

    const auto crossDocumentSave = controller->mark_saved(secondDocumentId, secondState);
    require(!crossDocumentSave.has_value());
    require(crossDocumentSave.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidSavedState));
    require(has_error_context(*crossDocumentSave.try_error(), "EditorDocumentId=2"));

    auto secondController =
        cue::editor_core::EditorController::create(make_project_descriptor(assertContext), assertContext);
    auto nextSessionScene = make_scene_document("00000000-0000-4000-8000-000000000203", assertContext);
    auto nextSessionLocator =
        take_value(cue::RelativePath::parse("Scenes/NextSessionRevision.cuescene", assertContext));
    const auto nextSessionDocumentId =
        take_value(secondController->open_document(std::move(nextSessionScene), std::move(nextSessionLocator), true));
    const auto nextSessionState = take_value(secondController->record_persistent_change(nextSessionDocumentId));
    require(nextSessionDocumentId == documentId);
    require(nextSessionState.value() == secondState.value());

    const auto crossSessionSave = secondController->mark_saved(nextSessionDocumentId, secondState);
    require(!crossSessionSave.has_value());
    require(crossSessionSave.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidSavedState));
    require(has_error_context(*crossSessionSave.try_error(), "EditorDocumentId=1"));
}

/// @brief Selection が Stable ObjectId だけを順序付き集合として保持することを検証する
void test_selection_reconciliation() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    auto controller = cue::editor_core::EditorController::create(make_project_descriptor(assertContext), assertContext);
    auto scene = make_scene_document("00000000-0000-4000-8000-000000000301", assertContext);
    const auto first = make_object_id("00000000-0000-4000-8000-000000000311", assertContext);
    const auto second = make_object_id("00000000-0000-4000-8000-000000000312", assertContext);
    const auto removed = make_object_id("00000000-0000-4000-8000-000000000313", assertContext);
    require(scene.add_object(first, "First", true, std::nullopt, cue::math::Transform{}).has_value());
    require(scene.add_object(second, "Second", true, std::nullopt, cue::math::Transform{}).has_value());
    auto locator = take_value(cue::RelativePath::parse("Scenes/Selection.cuescene", assertContext));
    const auto documentId = take_value(controller->open_document(std::move(scene), std::move(locator), true));

    const std::array selection{first, removed, first, second};
    require(controller->set_selection(documentId, selection, &removed).has_value());
    const auto *document = controller->session().find_document(documentId);
    require(document != nullptr);
    require(document->selection().size() == 2U);
    require(document->selection()[0] == first);
    require(document->selection()[1] == second);
    require(document->try_primary_selection() != nullptr);
    require(*document->try_primary_selection() == first);

    const std::array staleSelection{removed};
    require(controller->set_selection(documentId, staleSelection, &removed).has_value());
    document = controller->session().find_document(documentId);
    require(document != nullptr);
    require(document->selection().empty());
    require(document->try_primary_selection() == nullptr);
}

/// @brief Semantic IntentがPresentation非依存でControllerのCommand／Workflowへ変換されることを検証する
void test_editor_intents() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    TestSceneIdentitySource identitySource;
    auto controller = cue::editor_core::EditorController::create(make_project_descriptor(assertContext), assertContext);

    auto scene = make_scene_document("00000000-0000-4000-8000-000000000321", assertContext);
    auto locator = take_value(cue::RelativePath::parse("Scenes/Intent.cuescene", assertContext));
    const auto documentId = take_value(controller->open_document(std::move(scene), std::move(locator), true));

    require(
        controller
            ->execute_intent(documentId, cue::editor_core::AddObjectIntent{std::nullopt, "Root"}, identitySource, {})
            .has_value());
    const auto *document = controller->session().find_document(documentId);
    require(document != nullptr && document->scene_document().object_count() == 1U);
    require(document->selection().size() == 1U && document->try_primary_selection() != nullptr);
    const cue::scene::ObjectId rootId = *document->try_primary_selection();

    require(
        controller
            ->execute_intent(documentId, cue::editor_core::RenameObjectIntent{rootId, "Renamed"}, identitySource, {})
            .has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(rootId)->name() == "Renamed");
    require(controller->execute_intent(documentId, cue::editor_core::UndoIntent{}, identitySource, {}).has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(rootId)->name() == "Root");
    require(controller->execute_intent(documentId, cue::editor_core::RedoIntent{}, identitySource, {}).has_value());
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(rootId)->name() == "Renamed");

    const auto missing = controller->execute_intent(cue::editor_core::EditorDocumentId(documentId.value() + 1U),
                                                    cue::editor_core::UndoIntent{}, identitySource, {});
    require(!missing.has_value());
    require(missing.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::DocumentNotFound));
}

/// @brief Scene 編集 Command の Stable ID、完全 Rollback、Subtree 操作を検証する
void test_scene_commands() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource registryIdentitySource;
    auto registry = make_component_registry(registryIdentitySource, assertContext);
    auto valueRegistry = make_component_value_registry(*registry, assertContext);
    auto controller = cue::editor_core::EditorController::create(make_project_descriptor(assertContext), assertContext);

    const auto opaqueId = make_component_id("00000000-0000-4000-8000-000000000523", assertContext);
    const auto opaqueCopyId = make_component_id("00000000-0000-4000-8000-000000000524", assertContext);
    auto futureVersion = take_value(cue::schema::SchemaVersion::create(2U, assertContext));
    auto opaqueData = take_value(cue::scene::OpaqueComponentData::create(
        opaqueId, make_component_type_id(assertContext), futureVersion, "{\"future\":true}", *registry, assertContext));
    const auto opaqueDuplicate =
        cue::scene::SceneComponent::opaque(std::move(opaqueData)).duplicate_with_identity(opaqueCopyId, assertContext);
    require(!opaqueDuplicate.has_value());
    require(opaqueDuplicate.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::UnsupportedComponentOperation));

    auto scene = make_scene_document("00000000-0000-4000-8000-000000000501", assertContext);
    const auto sceneAssetId = scene.scene_asset_id();
    const auto rootId = make_object_id("00000000-0000-4000-8000-000000000511", assertContext);
    const auto childId = make_object_id("00000000-0000-4000-8000-000000000512", assertContext);
    const auto grandchildId = make_object_id("00000000-0000-4000-8000-000000000513", assertContext);
    const auto componentId = make_component_id("00000000-0000-4000-8000-000000000521", assertContext);
    const auto secondComponentId = make_component_id("00000000-0000-4000-8000-000000000525", assertContext);
    require(scene.add_object(rootId, "Root", true, std::nullopt, cue::math::Transform{}).has_value());
    require(scene.add_object(childId, "Child", true, rootId, cue::math::Transform{}).has_value());
    require(
        scene.add_component(childId, make_health_component(componentId, 100, *registry, valueRegistry, assertContext))
            .has_value());
    require(scene
                .add_component(childId,
                               make_health_component(secondComponentId, 50, *registry, valueRegistry, assertContext))
                .has_value());

    auto checkpoint = scene.create_checkpoint();
    require(scene.remove_component(childId, componentId).has_value());
    require(scene.restore_checkpoint(std::move(checkpoint)).has_value());
    require(component_health(*scene.find_object(childId), componentId) == 100);

    auto locator = take_value(cue::RelativePath::parse("Scenes/Commands.cuescene", assertContext));
    const auto documentId = take_value(controller->open_document(std::move(scene), std::move(locator), true));

    auto state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId,
        cue::editor_core::AddObjectCommand{grandchildId, "Grandchild", true, childId, cue::math::Transform{}}}));
    require(state.value() == 2U);
    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "Renamed Root"}}));
    require(state.value() == 3U);
    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::ReparentObjectCommand{grandchildId, std::nullopt}}));
    require(state.value() == 4U);
    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::ReparentObjectCommand{grandchildId, childId}}));
    require(state.value() == 5U);

    const auto cycle = controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::ReparentObjectCommand{rootId, childId}});
    require(!cycle.has_value());
    require(has_error_context(*cycle.try_error(), "EditorDocumentId=1"));
    require(has_error_context(*cycle.try_error(), "ObjectId=00000000-0000-4000-8000-000000000511"));
    const auto *document = controller->session().find_document(documentId);
    require(document != nullptr);
    require(document->current_state_id().value() == 5U);
    require(document->scene_document().find_object(rootId)->try_parent_id() == nullptr);

    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId,
        cue::editor_core::EditFieldCommand{childId, componentId, make_health_field_id(assertContext),
                                           cue::scene::FieldValue::signed_integer(250)}}));
    require(state.value() == 6U);
    document = controller->session().find_document(documentId);
    require(component_health(*document->scene_document().find_object(childId), componentId) == 250);

    const auto fieldMismatch = controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId,
        cue::editor_core::EditFieldCommand{childId, componentId, make_health_field_id(assertContext),
                                           cue::scene::FieldValue::boolean(true)}});
    require(!fieldMismatch.has_value());
    document = controller->session().find_document(documentId);
    require(document->current_state_id().value() == 6U);
    require(component_health(*document->scene_document().find_object(childId), componentId) == 250);

    auto movedAssetReference = take_value(cue::scene::AssetReferenceValue::create("asset://moved", assertContext));
    const auto consumedAssetValue = cue::scene::FieldValue::asset_reference(std::move(movedAssetReference));
    (void)consumedAssetValue;
    auto invalidAssetValue = cue::scene::FieldValue::asset_reference(std::move(movedAssetReference));
    require(!cue::scene::is_valid_field_value(invalidAssetValue));
    const auto invalidAsset = controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId,
        cue::editor_core::EditFieldCommand{childId, componentId, make_asset_field_id(assertContext),
                                           std::move(invalidAssetValue)}});
    require(!invalidAsset.has_value());
    require(invalidAsset.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::scene::SceneError::FieldTypeMismatch));
    document = controller->session().find_document(documentId);
    require(document->current_state_id().value() == 6U);
    require(component_asset_token(*document->scene_document().find_object(childId), componentId) == "asset://default");

    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::EditTransformCommand{childId, cue::math::Transform{}}}));
    require(state.value() == 6U);

    const auto rootComponentId = make_component_id("00000000-0000-4000-8000-000000000522", assertContext);
    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId,
        cue::editor_core::AddComponentCommand{
            rootId, make_health_component(rootComponentId, 10, *registry, valueRegistry, assertContext)}}));
    require(state.value() == 7U);
    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::RemoveComponentCommand{rootId, rootComponentId}}));
    require(state.value() == 8U);

    const auto repeatedChildCopyId = make_object_id("00000000-0000-4000-8000-000000000535", assertContext);
    const auto repeatedGrandchildCopyId = make_object_id("00000000-0000-4000-8000-000000000536", assertContext);
    const auto repeatedComponentCopyId = make_component_id("00000000-0000-4000-8000-000000000537", assertContext);
    std::vector<cue::editor_core::DuplicateObjectTarget> repeatedTargets;
    repeatedTargets.push_back(
        {childId, repeatedChildCopyId, "Child Copy", {repeatedComponentCopyId, repeatedComponentCopyId}});
    repeatedTargets.push_back({grandchildId, repeatedGrandchildCopyId, "Grandchild Copy", {}});
    const auto repeatedDuplicate = controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::DuplicateObjectCommand{childId, std::move(repeatedTargets)}});
    require(!repeatedDuplicate.has_value());
    document = controller->session().find_document(documentId);
    require(document->current_state_id().value() == 8U);
    require(document->scene_document().find_object(repeatedChildCopyId) == nullptr);

    const auto failedChildCopyId = make_object_id("00000000-0000-4000-8000-000000000531", assertContext);
    const auto failedGrandchildCopyId = make_object_id("00000000-0000-4000-8000-000000000532", assertContext);
    const auto failedComponentCopyId = make_component_id("00000000-0000-4000-8000-000000000533", assertContext);
    const auto failedSecondComponentCopyId = make_component_id("00000000-0000-4000-8000-000000000534", assertContext);
    std::vector<cue::editor_core::DuplicateObjectTarget> failedTargets;
    failedTargets.push_back(
        {childId, failedChildCopyId, "Child Copy", {failedComponentCopyId, failedSecondComponentCopyId}});
    failedTargets.push_back({grandchildId, failedGrandchildCopyId, "", {}});
    const auto failedDuplicate = controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::DuplicateObjectCommand{childId, std::move(failedTargets)}});
    require(!failedDuplicate.has_value());
    document = controller->session().find_document(documentId);
    require(document->current_state_id().value() == 8U);
    require(document->scene_document().find_object(failedChildCopyId) == nullptr);
    require(document->scene_document().find_object(failedGrandchildCopyId) == nullptr);
    require(component_health(*document->scene_document().find_object(childId), componentId) == 250);

    const auto childCopyId = make_object_id("00000000-0000-4000-8000-000000000541", assertContext);
    const auto grandchildCopyId = make_object_id("00000000-0000-4000-8000-000000000542", assertContext);
    const auto componentCopyId = make_component_id("00000000-0000-4000-8000-000000000543", assertContext);
    const auto secondComponentCopyId = make_component_id("00000000-0000-4000-8000-000000000544", assertContext);
    std::vector<cue::editor_core::DuplicateObjectTarget> targets;
    targets.push_back({childId, childCopyId, "Child Copy", {componentCopyId, secondComponentCopyId}});
    targets.push_back({grandchildId, grandchildCopyId, "Grandchild Copy", {}});
    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::DuplicateObjectCommand{childId, std::move(targets)}}));
    require(state.value() == 9U);
    document = controller->session().find_document(documentId);
    const auto *childCopy = document->scene_document().find_object(childCopyId);
    const auto *grandchildCopy = document->scene_document().find_object(grandchildCopyId);
    require(childCopy != nullptr);
    require(grandchildCopy != nullptr);
    require(component_health(*childCopy, componentCopyId) == 250);
    require(component_health(*childCopy, secondComponentCopyId) == 50);
    require(grandchildCopy->try_parent_id() != nullptr && *grandchildCopy->try_parent_id() == childCopyId);

    auto otherSceneId =
        take_value(cue::scene::SceneAssetId::parse("00000000-0000-4000-8000-000000000599", assertContext));
    const auto crossDocument = controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, otherSceneId, cue::editor_core::RenameObjectCommand{rootId, "Wrong Scene"}});
    require(!crossDocument.has_value());
    require(crossDocument.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::SceneMismatch));
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(rootId)->name() == "Renamed Root");
    require(document->current_state_id().value() == 9U);

    const std::array selection{childId, grandchildId};
    require(controller->set_selection(documentId, selection, &childId).has_value());
    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::DeleteObjectCommand{childId}}));
    require(state.value() == 10U);
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(childId) == nullptr);
    require(document->scene_document().find_object(grandchildId) == nullptr);
    require(document->selection().empty());
    require(document->scene_document().find_object(childCopyId) != nullptr);
}

/// @brief Transaction単位のUndo／Redo、分岐破棄、連続編集復元を検証する
void test_transaction_history() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource registryIdentitySource;
    auto registry = make_component_registry(registryIdentitySource, assertContext);
    auto valueRegistry = make_component_value_registry(*registry, assertContext);
    auto controller = cue::editor_core::EditorController::create(make_project_descriptor(assertContext), assertContext);

    auto scene = make_scene_document("00000000-0000-4000-8000-000000000601", assertContext);
    const auto sceneAssetId = scene.scene_asset_id();
    const auto rootId = make_object_id("00000000-0000-4000-8000-000000000611", assertContext);
    const auto childId = make_object_id("00000000-0000-4000-8000-000000000612", assertContext);
    const auto grandchildId = make_object_id("00000000-0000-4000-8000-000000000613", assertContext);
    const auto componentId = make_component_id("00000000-0000-4000-8000-000000000621", assertContext);
    require(scene.add_object(rootId, "Root", true, std::nullopt, cue::math::Transform{}).has_value());
    auto locator = take_value(cue::RelativePath::parse("Scenes/History.cuescene", assertContext));
    const auto documentId = take_value(controller->open_document(std::move(scene), std::move(locator), true));

    cue::editor_core::EditorTransaction failedTransaction{"Fail Together", {}};
    failedTransaction.commands.push_back(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "Partial Name"}});
    failedTransaction.commands.push_back(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId,
        cue::editor_core::AddObjectCommand{rootId, "Duplicate", true, std::nullopt, cue::math::Transform{}}});
    require(!controller->execute_transaction(std::move(failedTransaction)).has_value());
    const auto *document = controller->session().find_document(documentId);
    require(document != nullptr && document->current_state_id().value() == 1U);
    require(document->scene_document().find_object(rootId)->name() == "Root");
    require(document->history_entry_count() == 0U);

    cue::editor_core::EditorTransaction addTransaction{"Add Character", {}};
    addTransaction.commands.push_back(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId,
        cue::editor_core::AddObjectCommand{childId, "Child", true, rootId, cue::math::Transform{}}});
    addTransaction.commands.push_back(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId,
        cue::editor_core::AddComponentCommand{
            childId, make_health_component(componentId, 77, *registry, valueRegistry, assertContext)}});
    addTransaction.commands.push_back(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId,
        cue::editor_core::AddObjectCommand{grandchildId, "Grandchild", false, childId, cue::math::Transform{}}});
    auto state = take_value(controller->execute_transaction(std::move(addTransaction)));
    require(state.value() == 2U);
    document = controller->session().find_document(documentId);
    require(document != nullptr && document->history_entry_count() == 1U && document->history_byte_size() > 0U);
    require(document->history_byte_size() <= cue::editor_core::EditorDocument::maximum_history_bytes());
    require(document->undo_label() == "Add Character");
    require(component_health(*document->scene_document().find_object(childId), componentId) == 77);
    require(document->scene_document().find_object(grandchildId)->try_parent_id() != nullptr);

    state = take_value(controller->undo(documentId));
    require(state.value() == 1U);
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(childId) == nullptr);
    require(!document->is_dirty() && document->can_redo());
    require(document->redo_label() == "Add Character");
    state = take_value(controller->redo(documentId));
    require(state.value() == 2U);
    document = controller->session().find_document(documentId);
    require(component_health(*document->scene_document().find_object(childId), componentId) == 77);
    require(document->is_dirty());

    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::DeleteObjectCommand{childId}}));
    require(state.value() == 3U);
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(childId) == nullptr);
    state = take_value(controller->undo(documentId));
    require(state.value() == 2U);
    document = controller->session().find_document(documentId);
    require(component_health(*document->scene_document().find_object(childId), componentId) == 77);
    const auto *restoredGrandchild = document->scene_document().find_object(grandchildId);
    require(restoredGrandchild != nullptr && restoredGrandchild->try_parent_id() != nullptr &&
            *restoredGrandchild->try_parent_id() == childId);
    require(take_value(controller->redo(documentId)).value() == 3U);
    require(take_value(controller->undo(documentId)).value() == 2U);

    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::RemoveComponentCommand{childId, componentId}}));
    require(state.value() == 4U);
    document = controller->session().find_document(documentId);
    require(document->scene_document().find_object(childId)->components().empty());
    require(take_value(controller->undo(documentId)).value() == 2U);
    document = controller->session().find_document(documentId);
    require(component_health(*document->scene_document().find_object(childId), componentId) == 77);

    state = take_value(controller->execute_command(cue::editor_core::SceneCommandRequest{
        documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "Branch Root"}}));
    require(state.value() == 5U);
    document = controller->session().find_document(documentId);
    require(!document->can_redo());

    for (std::size_t index = 0U; index < 100U; ++index)
    {
        const std::string_view name = index % 2U == 0U ? "Loop A" : "Loop B";
        require(controller
                    ->execute_command(cue::editor_core::SceneCommandRequest{
                        documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, std::string(name)}})
                    .has_value());
    }
    document = controller->session().find_document(documentId);
    require(document->current_state_id().value() == 105U);
    for (std::size_t index = 0U; index < 100U; ++index)
    {
        require(controller->undo(documentId).has_value());
    }
    document = controller->session().find_document(documentId);
    require(document->current_state_id().value() == 5U);
    require(document->scene_document().find_object(rootId)->name() == "Branch Root");
    for (std::size_t index = 0U; index < 100U; ++index)
    {
        require(controller->redo(documentId).has_value());
    }
    document = controller->session().find_document(documentId);
    require(document->current_state_id().value() == 105U);
    require(document->scene_document().find_object(rootId)->name() == "Loop B");
    require(document->scene_document().validate().has_value());

    for (std::size_t index = 0U; index < 160U; ++index)
    {
        const std::string_view name = index % 2U == 0U ? "Limit A" : "Limit B";
        require(controller
                    ->execute_command(cue::editor_core::SceneCommandRequest{
                        documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, std::string(name)}})
                    .has_value());
    }
    document = controller->session().find_document(documentId);
    require(document->history_entry_count() == cue::editor_core::EditorDocument::maximum_history_entries());
    require(document->history_byte_size() <= cue::editor_core::EditorDocument::maximum_history_bytes());

    require(controller->undo(documentId).has_value());
    document = controller->session().find_document(documentId);
    require(document->can_undo() && document->can_redo());
    require(controller->record_persistent_change(documentId).has_value());
    document = controller->session().find_document(documentId);
    require(!document->can_undo() && !document->can_redo());
    require(document->history_entry_count() == 0U && document->history_byte_size() == 0U);

    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "History Restart"}})
                .has_value());
    document = controller->session().find_document(documentId);
    require(document->can_undo() && !document->can_redo());
    require(controller->record_persistent_change(documentId).has_value());
    document = controller->session().find_document(documentId);
    require(!document->can_undo() && !document->can_redo());
}

/// @brief Save失敗、競合、Reload、Save All、Recovery、再Openを一連で検証する
void test_scene_persistence_workflow() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource registryIdentitySource;
    auto registry = make_component_registry(registryIdentitySource, assertContext);
    auto valueRegistry = make_component_value_registry(*registry, assertContext);
    cue::scene::SceneMigrationRegistry sceneMigrations;
    cue::scene::ComponentMigrationRegistry componentMigrations;
    MemoryFilesystemRoot sourceAssets(assertContext);
    MemoryFilesystemRoot savedRoot(assertContext);

    const auto rootId = make_object_id("00000000-0000-4000-8000-000000000711", assertContext);
    auto initialScene = make_scene_document("00000000-0000-4000-8000-000000000701", assertContext);
    const auto sceneAssetId = initialScene.scene_asset_id();
    require(initialScene.add_object(rootId, "Initial", true, std::nullopt, cue::math::Transform{}).has_value());
    const auto initialJson = take_value(cue::scene::serialize_scene_document(initialScene, assertContext));
    sourceAssets.set("Scenes/Main.cuescene", initialJson);

    cue::editor_core::ScenePersistenceServices services(sourceAssets, savedRoot, *registry, valueRegistry,
                                                        sceneMigrations, componentMigrations);
    auto controller =
        cue::editor_core::EditorController::create(make_project_descriptor(assertContext), services, assertContext);
    const auto documentId = take_value(controller->open_document_from_storage(
        take_value(cue::RelativePath::parse("Scenes/Main.cuescene", assertContext))));
    const auto *document = controller->session().find_document(documentId);
    require(document != nullptr && !document->is_dirty() && !document->has_recovery_candidate());

    TestSceneIdentitySource identitySource;
    std::vector<cue::editor_core::EditorPrimitiveTemplate> primitiveTemplates;
    primitiveTemplates.push_back(cue::editor_core::EditorPrimitiveTemplate{
        "Cube",
        "cue://engine/mesh/cube",
        make_health_component(make_component_id("00000000-0000-4000-8000-000000000713", assertContext), 25, *registry,
                              valueRegistry, assertContext, "cue://engine/mesh/cube"),
        {},
        true});
    require(controller
                ->execute_intent(documentId, cue::editor_core::CreatePrimitiveIntent{std::nullopt, 0U}, identitySource,
                                 {}, primitiveTemplates)
                .has_value());
    document = controller->session().find_document(documentId);
    require(document != nullptr && document->is_dirty() && document->scene_document().object_count() == 2U &&
            document->selection().size() == 1U);
    const cue::scene::ObjectId primitiveId = document->selection()[0];
    const cue::scene::SceneObject *primitive = document->scene_document().find_object(primitiveId);
    require(primitive != nullptr && primitive->name() == "Cube" && primitive->components().size() == 1U);
    const cue::scene::ComponentInstanceId primitiveComponentId = primitive->components()[0].instance_id();
    require(component_asset_token(*primitive, primitiveComponentId) == "cue://engine/mesh/cube");
    require(controller->undo(documentId).has_value());
    document = controller->session().find_document(documentId);
    require(document != nullptr && document->scene_document().find_object(primitiveId) == nullptr);
    require(controller->redo(documentId).has_value());
    document = controller->session().find_document(documentId);
    primitive = document->scene_document().find_object(primitiveId);
    require(primitive != nullptr && primitive->components().size() == 1U &&
            primitive->components()[0].instance_id() == primitiveComponentId);
    require(component_asset_token(*primitive, primitiveComponentId) == "cue://engine/mesh/cube");
    auto primitiveSave = controller->save_document(documentId);
    require(primitiveSave.has_value() && primitiveSave.try_value()->status() == cue::scene::SceneSaveStatus::Committed);
    require(controller->reload_document(documentId).has_value());
    document = controller->session().find_document(documentId);
    primitive = document != nullptr ? document->scene_document().find_object(primitiveId) : nullptr;
    require(primitive != nullptr && primitive->transform().translation() == cue::math::Vector3{} &&
            primitive->transform().scale() == cue::math::Vector3{1.0F, 1.0F, 1.0F} &&
            primitive->components().size() == 1U &&
            component_asset_token(*primitive, primitiveComponentId) == "cue://engine/mesh/cube");

    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "Edited"}})
                .has_value());
    const std::string originalFile(sourceAssets.text("Scenes/Main.cuescene"));
    sourceAssets.fail_write("Scenes/Main.cuescene", true);
    auto failedSave = controller->save_document(documentId);
    require(failedSave.has_value());
    require(failedSave.try_value()->status() == cue::scene::SceneSaveStatus::NotPublished);
    document = controller->session().find_document(documentId);
    require(document != nullptr && document->is_dirty());
    require(sourceAssets.text("Scenes/Main.cuescene") == originalFile);

    sourceAssets.fail_write("Scenes/Main.cuescene", false);
    auto committedSave = controller->save_document(documentId);
    require(committedSave.has_value());
    require(committedSave.try_value()->status() == cue::scene::SceneSaveStatus::Committed);
    document = controller->session().find_document(documentId);
    require(document != nullptr && !document->is_dirty());

    const std::string committedBeforeBackupRace(sourceAssets.text("Scenes/Main.cuescene"));
    const std::string backupBeforeBackupRace(sourceAssets.recovery_backup_text("Scenes/Main.cuescene"));
    auto backupRaceScene = make_scene_document("00000000-0000-4000-8000-000000000701", assertContext);
    require(backupRaceScene.add_object(rootId, "External Before Backup", true, std::nullopt, cue::math::Transform{})
                .has_value());
    const auto backupRaceJson = take_value(cue::scene::serialize_scene_document(backupRaceScene, assertContext));
    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "Backup Race"}})
                .has_value());
    sourceAssets.mutate_before_read("Scenes/Main.cuescene", backupRaceJson, 2U);
    const auto backupRaceSave = controller->save_document(documentId);
    require(!backupRaceSave.has_value());
    require(backupRaceSave.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::ExternalConflict));
    require(sourceAssets.recovery_backup_text("Scenes/Main.cuescene") == backupBeforeBackupRace);
    sourceAssets.set("Scenes/Main.cuescene", committedBeforeBackupRace);
    require(take_value(controller->poll_external_change(documentId)) == cue::editor_core::ExternalChangeState::None);
    auto recoveredBackupRaceSave = controller->save_document(documentId);
    require(recoveredBackupRaceSave.has_value() &&
            recoveredBackupRaceSave.try_value()->status() == cue::scene::SceneSaveStatus::Committed);

    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "Local Conflict"}})
                .has_value());
    auto externalScene = make_scene_document("00000000-0000-4000-8000-000000000701", assertContext);
    require(externalScene.add_object(rootId, "External", true, std::nullopt, cue::math::Transform{}).has_value());
    const auto externalJson = take_value(cue::scene::serialize_scene_document(externalScene, assertContext));
    sourceAssets.set("Scenes/Main.cuescene", externalJson);
    const auto conflictingSave = controller->save_document(documentId);
    require(!conflictingSave.has_value());
    require(conflictingSave.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::ExternalConflict));
    require(sourceAssets.text("Scenes/Main.cuescene") == externalJson);
    require(take_value(controller->poll_external_change(documentId)) ==
            cue::editor_core::ExternalChangeState::Modified);

    sourceAssets.set("Scenes/Main.cuescene", "invalid scene data");
    const auto failedReload = controller->reload_document(documentId);
    require(!failedReload.has_value());
    document = controller->session().find_document(documentId);
    require(document != nullptr && document->is_dirty() && document->history_entry_count() == 3U);
    require(document->scene_document().find_object(rootId)->name() == "Local Conflict");
    sourceAssets.set("Scenes/Main.cuescene", externalJson);

    const auto reloadedState = take_value(controller->reload_document(documentId));
    document = controller->session().find_document(documentId);
    require(document != nullptr && !document->is_dirty() && document->history_entry_count() == 0U);
    require(document->current_state_id() == reloadedState && document->saved_state_id() == reloadedState);
    require(document->scene_document().find_object(rootId)->name() == "External");
    require(document->external_change_state() == cue::editor_core::ExternalChangeState::None);

    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "Save As Content"}})
                .has_value());
    sourceAssets.fail_write("Scenes/Renamed.cuescene", true);
    auto failedSaveAs = controller->save_document_as(
        documentId, take_value(cue::RelativePath::parse("Scenes/Renamed.cuescene", assertContext)));
    require(failedSaveAs.has_value());
    require(failedSaveAs.try_value()->status() == cue::scene::SceneSaveStatus::NotPublished);
    document = controller->session().find_document(documentId);
    require(document != nullptr && document->is_dirty());
    require(document->scene_locator().text() == "Scenes/Main.cuescene");
    sourceAssets.fail_write("Scenes/Renamed.cuescene", false);
    sourceAssets.mutate_before_publish("Scenes/Renamed.cuescene", externalJson);
    auto racedSaveAs = controller->save_document_as(
        documentId, take_value(cue::RelativePath::parse("Scenes/Renamed.cuescene", assertContext)));
    require(!racedSaveAs.has_value());
    require(racedSaveAs.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::ExternalConflict));
    document = controller->session().find_document(documentId);
    require(document != nullptr && document->is_dirty());
    require(document->scene_locator().text() == "Scenes/Main.cuescene");
    require(document->external_change_state() == cue::editor_core::ExternalChangeState::None);
    require(sourceAssets.text("Scenes/Renamed.cuescene") == externalJson);
    auto originalLocatorSave = controller->save_document(documentId);
    require(originalLocatorSave.has_value() &&
            originalLocatorSave.try_value()->status() == cue::scene::SceneSaveStatus::Committed);
    const std::string savedOriginalLocator(sourceAssets.text("Scenes/Main.cuescene"));
    auto protectedSaveAs = controller->save_document_as_new(
        documentId, take_value(cue::RelativePath::parse("Scenes/Renamed.cuescene", assertContext)));
    require(!protectedSaveAs.has_value());
    require(protectedSaveAs.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::ExternalConflict));
    require(sourceAssets.text("Scenes/Renamed.cuescene") == externalJson);
    auto saveAs = controller->save_document_as(
        documentId, take_value(cue::RelativePath::parse("Scenes/Renamed.cuescene", assertContext)));
    require(saveAs.has_value() && saveAs.try_value()->status() == cue::scene::SceneSaveStatus::Committed);
    document = controller->session().find_document(documentId);
    require(document != nullptr && !document->is_dirty());
    require(document->scene_locator().text() == "Scenes/Renamed.cuescene");
    require(sourceAssets.text("Scenes/Main.cuescene") == savedOriginalLocator);

    const auto secondRootId = make_object_id("00000000-0000-4000-8000-000000000712", assertContext);
    auto secondScene = make_scene_document("00000000-0000-4000-8000-000000000702", assertContext);
    const auto secondSceneAssetId = secondScene.scene_asset_id();
    require(secondScene.add_object(secondRootId, "Second", true, std::nullopt, cue::math::Transform{}).has_value());
    const auto secondOriginalJson = take_value(cue::scene::serialize_scene_document(secondScene, assertContext));
    sourceAssets.set("Scenes/Second.cuescene", secondOriginalJson);
    const auto secondDocumentId = take_value(controller->open_document_from_storage(
        take_value(cue::RelativePath::parse("Scenes/Second.cuescene", assertContext))));
    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "Batch First"}})
                .has_value());
    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    secondDocumentId, secondSceneAssetId,
                    cue::editor_core::RenameObjectCommand{secondRootId, "Batch Second"}})
                .has_value());
    const auto allStatuses = take_value(controller->save_all_documents());
    require(allStatuses.size() == 2U);
    require(allStatuses[0] == cue::scene::SceneSaveStatus::Committed &&
            allStatuses[1] == cue::scene::SceneSaveStatus::Committed);
    require(!controller->session().find_document(documentId)->is_dirty());
    require(!controller->session().find_document(secondDocumentId)->is_dirty());

    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    secondDocumentId, secondSceneAssetId,
                    cue::editor_core::RenameObjectCommand{secondRootId, "Uncertain Publish"}})
                .has_value());
    const auto uncertainCandidateJson = take_value(cue::scene::serialize_scene_document(
        controller->session().find_document(secondDocumentId)->scene_document(), assertContext));
    sourceAssets.make_write_uncertain("Scenes/Second.cuescene", true);
    auto uncertainSave = controller->save_document(secondDocumentId);
    require(uncertainSave.has_value());
    require(uncertainSave.try_value()->status() == cue::scene::SceneSaveStatus::PublishedButDurabilityUnknown);
    const auto *secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && secondDocument->is_dirty());
    require(secondDocument->persistence_state() == cue::editor_core::DocumentPersistenceState::SaveUncertain);
    require(secondDocument->requires_close_decision());
    sourceAssets.make_write_uncertain("Scenes/Second.cuescene", false);
    require(take_value(controller->request_close(secondDocumentId)) ==
            cue::editor_core::DocumentCloseState::AwaitingDecision);
    require(take_value(controller->respond_to_close(secondDocumentId, cue::editor_core::CloseDecision::Save)) ==
            cue::editor_core::DocumentCloseState::SaveRequested);
    sourceAssets.fail_lease("Scenes/Second.cuescene", true);
    require(!controller->retry_uncertain_save(secondDocumentId).has_value());
    sourceAssets.fail_lease("Scenes/Second.cuescene", false);
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr);
    require(secondDocument->close_state() == cue::editor_core::DocumentCloseState::AwaitingDecision);
    require(secondDocument->persistence_state() == cue::editor_core::DocumentPersistenceState::SaveUncertain);
    require(take_value(controller->respond_to_close(secondDocumentId, cue::editor_core::CloseDecision::Cancel)) ==
            cue::editor_core::DocumentCloseState::Open);
    require(!controller->reload_document(secondDocumentId).has_value());
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && secondDocument->is_dirty());
    require(secondDocument->persistence_state() == cue::editor_core::DocumentPersistenceState::SaveUncertain);
    sourceAssets.mutate_after_verification_read("Scenes/Second.cuescene", secondOriginalJson);
    const auto racedRetry = controller->retry_uncertain_save(secondDocumentId);
    require(!racedRetry.has_value());
    require(racedRetry.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::ExternalConflict));
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && secondDocument->is_dirty());
    require(secondDocument->persistence_state() == cue::editor_core::DocumentPersistenceState::SaveUncertain);
    require(sourceAssets.text("Scenes/Second.cuescene") == secondOriginalJson);
    sourceAssets.set("Scenes/Second.cuescene", uncertainCandidateJson);
    require(take_value(controller->retry_uncertain_save(secondDocumentId)) == cue::scene::SceneSaveStatus::Committed);
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && !secondDocument->is_dirty());
    require(secondDocument->persistence_state() == cue::editor_core::DocumentPersistenceState::Idle);

    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    secondDocumentId, secondSceneAssetId,
                    cue::editor_core::RenameObjectCommand{secondRootId, "Uncertain Save As"}})
                .has_value());
    sourceAssets.make_write_uncertain("Scenes/Second-Renamed.cuescene", true);
    auto uncertainSaveAs = controller->save_document_as(
        secondDocumentId, take_value(cue::RelativePath::parse("Scenes/Second-Renamed.cuescene", assertContext)));
    require(uncertainSaveAs.has_value());
    require(uncertainSaveAs.try_value()->status() == cue::scene::SceneSaveStatus::PublishedButDurabilityUnknown);
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && secondDocument->is_dirty());
    require(secondDocument->scene_locator().text() == "Scenes/Second.cuescene");
    require(!controller->reload_document(secondDocumentId).has_value());
    sourceAssets.make_write_uncertain("Scenes/Second-Renamed.cuescene", false);
    sourceAssets.set("Scenes/Second-Renamed.cuescene", secondOriginalJson);
    const auto conflictingSaveAsRetry = controller->retry_uncertain_save(secondDocumentId);
    require(!conflictingSaveAsRetry.has_value());
    require(conflictingSaveAsRetry.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::ExternalConflict));
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && secondDocument->is_dirty());
    require(secondDocument->scene_locator().text() == "Scenes/Second.cuescene");
    require(secondDocument->external_change_state() == cue::editor_core::ExternalChangeState::None);
    require(controller->discard_uncertain_save(secondDocumentId).has_value());
    auto originalAfterSaveAsRetry = controller->save_document(secondDocumentId);
    require(originalAfterSaveAsRetry.has_value() &&
            originalAfterSaveAsRetry.try_value()->status() == cue::scene::SceneSaveStatus::Committed);
    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    secondDocumentId, secondSceneAssetId,
                    cue::editor_core::RenameObjectCommand{secondRootId, "Uncertain Save As Retry"}})
                .has_value());
    sourceAssets.make_write_uncertain("Scenes/Second-Renamed.cuescene", true);
    auto retriedUncertainSaveAs = controller->save_document_as(
        secondDocumentId, take_value(cue::RelativePath::parse("Scenes/Second-Renamed.cuescene", assertContext)));
    require(retriedUncertainSaveAs.has_value());
    require(retriedUncertainSaveAs.try_value()->status() == cue::scene::SceneSaveStatus::PublishedButDurabilityUnknown);
    sourceAssets.make_write_uncertain("Scenes/Second-Renamed.cuescene", false);
    require(take_value(controller->retry_uncertain_save(secondDocumentId)) == cue::scene::SceneSaveStatus::Committed);
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && !secondDocument->is_dirty());
    require(secondDocument->scene_locator().text() == "Scenes/Second-Renamed.cuescene");

    const std::string backupRetrySource(sourceAssets.text("Scenes/Second-Renamed.cuescene"));
    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    secondDocumentId, secondSceneAssetId,
                    cue::editor_core::RenameObjectCommand{secondRootId, "Backup Durability Retry"}})
                .has_value());
    sourceAssets.make_recovery_backup_uncertain("Scenes/Second-Renamed.cuescene", true);
    auto backupUncertainSave = controller->save_document(secondDocumentId);
    require(backupUncertainSave.has_value());
    require(backupUncertainSave.try_value()->status() ==
            cue::scene::SceneSaveStatus::PublishedButBackupDurabilityUnknown);
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && secondDocument->is_dirty());
    require(secondDocument->persistence_state() == cue::editor_core::DocumentPersistenceState::SaveUncertain);
    const std::string backupRetryCandidate(sourceAssets.text("Scenes/Second-Renamed.cuescene"));
    sourceAssets.make_recovery_backup_uncertain("Scenes/Second-Renamed.cuescene", false);
    sourceAssets.mutate_main_after_backup_write("Scenes/Second-Renamed.cuescene", secondOriginalJson);
    const auto conflictingBackupRetry = controller->retry_uncertain_save(secondDocumentId);
    require(!conflictingBackupRetry.has_value());
    require(conflictingBackupRetry.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::ExternalConflict));
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && secondDocument->is_dirty());
    require(secondDocument->persistence_state() == cue::editor_core::DocumentPersistenceState::SaveUncertain);
    sourceAssets.set("Scenes/Second-Renamed.cuescene", backupRetryCandidate);
    sourceAssets.fail_write("Scenes/Second-Renamed.cuescene", true);
    require(take_value(controller->retry_uncertain_save(secondDocumentId)) == cue::scene::SceneSaveStatus::Committed);
    sourceAssets.fail_write("Scenes/Second-Renamed.cuescene", false);
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && !secondDocument->is_dirty());
    require(secondDocument->persistence_state() == cue::editor_core::DocumentPersistenceState::Idle);
    require(sourceAssets.recovery_backup_text("Scenes/Second-Renamed.cuescene") == backupRetrySource);

    const std::string mainThenBackupSource(sourceAssets.text("Scenes/Second-Renamed.cuescene"));
    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    secondDocumentId, secondSceneAssetId,
                    cue::editor_core::RenameObjectCommand{secondRootId, "Main Then Backup Durability Retry"}})
                .has_value());
    sourceAssets.make_write_uncertain("Scenes/Second-Renamed.cuescene", true);
    auto mainUncertainSave = controller->save_document(secondDocumentId);
    require(mainUncertainSave.has_value());
    require(mainUncertainSave.try_value()->status() == cue::scene::SceneSaveStatus::PublishedButDurabilityUnknown);
    sourceAssets.make_write_uncertain("Scenes/Second-Renamed.cuescene", false);
    sourceAssets.make_recovery_backup_uncertain("Scenes/Second-Renamed.cuescene", true);
    require(take_value(controller->request_close(secondDocumentId)) ==
            cue::editor_core::DocumentCloseState::AwaitingDecision);
    require(take_value(controller->respond_to_close(secondDocumentId, cue::editor_core::CloseDecision::Save)) ==
            cue::editor_core::DocumentCloseState::SaveRequested);
    require(take_value(controller->retry_uncertain_save(secondDocumentId)) ==
            cue::scene::SceneSaveStatus::PublishedButBackupDurabilityUnknown);
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr);
    require(secondDocument->close_state() == cue::editor_core::DocumentCloseState::AwaitingDecision);
    require(take_value(controller->respond_to_close(secondDocumentId, cue::editor_core::CloseDecision::Cancel)) ==
            cue::editor_core::DocumentCloseState::Open);
    require(sourceAssets.recovery_backup_text("Scenes/Second-Renamed.cuescene") == mainThenBackupSource);
    sourceAssets.make_recovery_backup_uncertain("Scenes/Second-Renamed.cuescene", false);
    sourceAssets.fail_write("Scenes/Second-Renamed.cuescene", true);
    require(take_value(controller->retry_uncertain_save(secondDocumentId)) == cue::scene::SceneSaveStatus::Committed);
    sourceAssets.fail_write("Scenes/Second-Renamed.cuescene", false);
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && !secondDocument->is_dirty());
    require(secondDocument->persistence_state() == cue::editor_core::DocumentPersistenceState::Idle);
    require(sourceAssets.recovery_backup_text("Scenes/Second-Renamed.cuescene") == mainThenBackupSource);

    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    secondDocumentId, secondSceneAssetId,
                    cue::editor_core::RenameObjectCommand{secondRootId, "Discarded Uncertain Record"}})
                .has_value());
    sourceAssets.make_write_uncertain("Scenes/Second-Renamed.cuescene", true);
    auto discardedUncertainSave = controller->save_document(secondDocumentId);
    require(discardedUncertainSave.has_value());
    require(discardedUncertainSave.try_value()->status() == cue::scene::SceneSaveStatus::PublishedButDurabilityUnknown);
    sourceAssets.make_write_uncertain("Scenes/Second-Renamed.cuescene", false);
    require(controller->discard_uncertain_save(secondDocumentId).has_value());
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && secondDocument->is_dirty());
    require(secondDocument->persistence_state() == cue::editor_core::DocumentPersistenceState::Idle);
    require(!controller->discard_uncertain_save(secondDocumentId).has_value());
    require(controller->reload_document(secondDocumentId).has_value());

    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    secondDocumentId, secondSceneAssetId,
                    cue::editor_core::RenameObjectCommand{secondRootId, "Verification Retry"}})
                .has_value());
    sourceAssets.fail_next_verification("Scenes/Second-Renamed.cuescene");
    auto verificationFailedSave = controller->save_document(secondDocumentId);
    require(verificationFailedSave.has_value());
    require(verificationFailedSave.try_value()->status() ==
            cue::scene::SceneSaveStatus::PublishedButVerificationFailed);
    require(!controller->reload_document(secondDocumentId).has_value());
    require(take_value(controller->retry_uncertain_save(secondDocumentId)) == cue::scene::SceneSaveStatus::Committed);
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && !secondDocument->is_dirty());

    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    secondDocumentId, secondSceneAssetId,
                    cue::editor_core::RenameObjectCommand{secondRootId, "Post Commit Race"}})
                .has_value());
    sourceAssets.mutate_after_verification_read("Scenes/Second-Renamed.cuescene", secondOriginalJson);
    const auto postCommitRace = controller->save_document(secondDocumentId);
    require(!postCommitRace.has_value());
    require(postCommitRace.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::ExternalConflict));
    secondDocument = controller->session().find_document(secondDocumentId);
    require(secondDocument != nullptr && secondDocument->is_dirty());
    require(secondDocument->external_change_state() == cue::editor_core::ExternalChangeState::Modified);
    require(sourceAssets.text("Scenes/Second-Renamed.cuescene") == secondOriginalJson);

    auto unsavedRecoveryScene = make_scene_document("00000000-0000-4000-8000-000000000703", assertContext);
    const auto unsavedRecoveryRoot = make_object_id("00000000-0000-4000-8000-000000000713", assertContext);
    require(unsavedRecoveryScene
                .add_object(unsavedRecoveryRoot, "Unsaved Recovery", true, std::nullopt, cue::math::Transform{})
                .has_value());
    const auto unsavedRecoveryId = take_value(controller->open_document(
        std::move(unsavedRecoveryScene),
        take_value(cue::RelativePath::parse("Scenes/Unsaved-Recovery.cuescene", assertContext)), false));
    const auto *unsavedRecoveryDocument = controller->session().find_document(unsavedRecoveryId);
    require(unsavedRecoveryDocument != nullptr && !unsavedRecoveryDocument->is_dirty() &&
            !unsavedRecoveryDocument->has_saved_destination());
    require(controller->autosave_recovery(unsavedRecoveryId).has_value());
    unsavedRecoveryDocument = controller->session().find_document(unsavedRecoveryId);
    require(unsavedRecoveryDocument != nullptr && unsavedRecoveryDocument->has_recovery_candidate());
    require(!savedRoot.text("Editor/Recovery/00000000-0000-4000-8000-000000000703.cuerecovery").empty());
    require(savedRoot.text("Editor/Recovery/Registry.cueindex").find("00000000-0000-4000-8000-000000000703") !=
            std::string_view::npos);

    cue::editor_core::ScenePersistenceServices unsavedRestartServices(sourceAssets, savedRoot, *registry, valueRegistry,
                                                                      sceneMigrations, componentMigrations);
    auto unsavedRestart = cue::editor_core::EditorController::create(make_project_descriptor(assertContext),
                                                                     unsavedRestartServices, assertContext);
    const auto recoveryCandidates = take_value(unsavedRestart->list_recovery_candidates());
    bool foundUnsavedRecovery = false;
    for (const cue::editor_core::RecoveryCandidateInspection &candidate : recoveryCandidates)
    {
        foundUnsavedRecovery =
            foundUnsavedRecovery || (candidate.scene_id() == "00000000-0000-4000-8000-000000000703" &&
                                     candidate.try_metadata() != nullptr && candidate.try_error() == nullptr);
    }
    require(foundUnsavedRecovery);
    sourceAssets.set("Scenes/Unsaved-Recovery.cuescene", "unreadable source");
    sourceAssets.fail_read("Scenes/Unsaved-Recovery.cuescene", true);
    const auto restartedUnsavedId =
        take_value(unsavedRestart->open_document_from_recovery("00000000-0000-4000-8000-000000000703"));
    sourceAssets.fail_read("Scenes/Unsaved-Recovery.cuescene", false);
    const auto *restartedUnsavedDocument = unsavedRestart->session().find_document(restartedUnsavedId);
    require(restartedUnsavedDocument != nullptr && restartedUnsavedDocument->is_dirty() &&
            !restartedUnsavedDocument->has_saved_destination() && restartedUnsavedDocument->has_recovery_candidate());
    require(restartedUnsavedDocument->external_change_state() == cue::editor_core::ExternalChangeState::Unknown);
    require(restartedUnsavedDocument->scene_document().find_object(unsavedRecoveryRoot) != nullptr);
    require(restartedUnsavedDocument->scene_document().find_object(unsavedRecoveryRoot)->name() == "Unsaved Recovery");

    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    documentId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "Recovered Content"}})
                .has_value());
    document = controller->session().find_document(documentId);
    const auto recoverySourceState = document->current_state_id().value();
    const auto canonicalBeforeRecovery = std::string(sourceAssets.text("Scenes/Renamed.cuescene"));
    savedRoot.fail_write("Editor/Recovery/00000000-0000-4000-8000-000000000701.cuerecovery", true);
    require(!controller->autosave_recovery(documentId).has_value());
    document = controller->session().find_document(documentId);
    require(document->is_dirty() && !document->has_recovery_candidate());
    require(sourceAssets.text("Scenes/Renamed.cuescene") == canonicalBeforeRecovery);
    savedRoot.fail_write("Editor/Recovery/00000000-0000-4000-8000-000000000701.cuerecovery", false);
    require(controller->autosave_recovery(documentId).has_value());
    document = controller->session().find_document(documentId);
    require(document->is_dirty() && document->has_recovery_candidate());
    require(sourceAssets.text("Scenes/Renamed.cuescene") == canonicalBeforeRecovery);
    require(!savedRoot.text("Editor/Recovery/00000000-0000-4000-8000-000000000701.cuerecovery").empty());
    savedRoot.set("Editor/Recovery/00000000-0000-4000-8000-000000000703.cuerecovery", "invalid recovery");

    cue::editor_core::ScenePersistenceServices restartServices(sourceAssets, savedRoot, *registry, valueRegistry,
                                                               sceneMigrations, componentMigrations);
    auto restarted = cue::editor_core::EditorController::create(make_project_descriptor(assertContext), restartServices,
                                                                assertContext);
    const auto restartedCandidates = take_value(restarted->list_recovery_candidates());
    bool foundValidRecovery = false;
    bool foundInvalidRecovery = false;
    for (const cue::editor_core::RecoveryCandidateInspection &candidate : restartedCandidates)
    {
        foundValidRecovery =
            foundValidRecovery || (candidate.scene_id() == "00000000-0000-4000-8000-000000000701" &&
                                   candidate.try_metadata() != nullptr && candidate.try_error() == nullptr);
        foundInvalidRecovery =
            foundInvalidRecovery || (candidate.scene_id() == "00000000-0000-4000-8000-000000000703" &&
                                     candidate.try_metadata() == nullptr && candidate.try_error() != nullptr);
    }
    require(foundValidRecovery && foundInvalidRecovery);
    const auto restartedId = take_value(restarted->open_document_from_storage(
        take_value(cue::RelativePath::parse("Scenes/Renamed.cuescene", assertContext))));
    const auto *restartedDocument = restarted->session().find_document(restartedId);
    require(restartedDocument != nullptr && restartedDocument->has_recovery_candidate());
    require(restartedDocument->scene_document().find_object(rootId)->name() == "Batch First");
    const auto recoveryMetadata = take_value(restarted->inspect_recovery(restartedId));
    require(recoveryMetadata.format_version() == 1U);
    require(recoveryMetadata.source_state_value() == recoverySourceState);
    require(recoveryMetadata.source_locator().text() == "Scenes/Renamed.cuescene");

    require(restarted
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    restartedId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "Pending Recover"}})
                .has_value());
    sourceAssets.make_write_uncertain("Scenes/Renamed.cuescene", true);
    const auto pendingRecoverySave = restarted->save_document(restartedId);
    require(pendingRecoverySave.has_value());
    require(pendingRecoverySave.try_value()->status() == cue::scene::SceneSaveStatus::PublishedButDurabilityUnknown);
    sourceAssets.make_write_uncertain("Scenes/Renamed.cuescene", false);
    require(!restarted->recover_document(restartedId).has_value());
    restartedDocument = restarted->session().find_document(restartedId);
    require(restartedDocument != nullptr);
    require(restartedDocument->persistence_state() == cue::editor_core::DocumentPersistenceState::SaveUncertain);
    require(restarted->discard_uncertain_save(restartedId).has_value());
    sourceAssets.set("Scenes/Renamed.cuescene", canonicalBeforeRecovery);
    require(restarted->reload_document(restartedId).has_value());

    auto recoveryConflictScene = make_scene_document("00000000-0000-4000-8000-000000000701", assertContext);
    require(
        recoveryConflictScene.add_object(rootId, "External Before Recover", true, std::nullopt, cue::math::Transform{})
            .has_value());
    const auto recoveryConflictJson =
        take_value(cue::scene::serialize_scene_document(recoveryConflictScene, assertContext));
    sourceAssets.set("Scenes/Renamed.cuescene", recoveryConflictJson);
    require(restarted->recover_document(restartedId).has_value());
    restartedDocument = restarted->session().find_document(restartedId);
    require(restartedDocument->is_dirty() && restartedDocument->history_entry_count() == 0U);
    require(restartedDocument->scene_document().find_object(rootId)->name() == "Recovered Content");
    require(restartedDocument->external_change_state() == cue::editor_core::ExternalChangeState::Modified);
    const auto recoveryConflictSave = restarted->save_document(restartedId);
    require(!recoveryConflictSave.has_value());
    require(sourceAssets.text("Scenes/Renamed.cuescene") == recoveryConflictJson);

    sourceAssets.set("Scenes/Renamed.cuescene", canonicalBeforeRecovery);
    require(restarted->reload_document(restartedId).has_value());
    require(restarted->recover_document(restartedId).has_value());
    auto recoveredSave = restarted->save_document(restartedId);
    require(recoveredSave.has_value() && recoveredSave.try_value()->status() == cue::scene::SceneSaveStatus::Committed);

    cue::editor_core::ScenePersistenceServices verificationServices(sourceAssets, savedRoot, *registry, valueRegistry,
                                                                    sceneMigrations, componentMigrations);
    auto verification = cue::editor_core::EditorController::create(make_project_descriptor(assertContext),
                                                                   verificationServices, assertContext);
    const auto verificationId = take_value(verification->open_document_from_storage(
        take_value(cue::RelativePath::parse("Scenes/Renamed.cuescene", assertContext))));
    const auto *verifiedDocument = verification->session().find_document(verificationId);
    require(verifiedDocument != nullptr && !verifiedDocument->is_dirty());
    require(verifiedDocument->scene_document().find_object(rootId)->name() == "Recovered Content");
    require(verification->ignore_recovery(verificationId).has_value());
    verifiedDocument = verification->session().find_document(verificationId);
    require(verifiedDocument != nullptr && !verifiedDocument->has_recovery_candidate());
    constexpr std::string_view recoveryPath = "Editor/Recovery/00000000-0000-4000-8000-000000000701.cuerecovery";
    const std::string validRecovery(savedRoot.text(recoveryPath));
    require(!validRecovery.empty());
    std::string unsupportedRecovery = validRecovery;
    require(unsupportedRecovery.starts_with("CueRecovery\n1\n"));
    unsupportedRecovery[12] = '2';
    savedRoot.set(recoveryPath, unsupportedRecovery);
    const auto unsupportedInspect = verification->inspect_recovery(verificationId);
    require(!unsupportedInspect.has_value());
    require(unsupportedInspect.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::UnsupportedRecovery));
    require(savedRoot.text(recoveryPath) == unsupportedRecovery);
    savedRoot.set(recoveryPath, validRecovery);
    require(verification->inspect_recovery(verificationId).has_value());
    savedRoot.fail_remove(recoveryPath, true);
    require(!verification->discard_recovery(verificationId).has_value());
    verifiedDocument = verification->session().find_document(verificationId);
    require(verifiedDocument != nullptr && verifiedDocument->has_recovery_candidate());
    require(savedRoot.text(recoveryPath) == validRecovery);
    savedRoot.fail_remove(recoveryPath, false);
    require(verification->discard_recovery(verificationId).has_value());
    require(savedRoot.text(recoveryPath).empty());

    require(verification
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    verificationId, sceneAssetId, cue::editor_core::RenameObjectCommand{rootId, "Close Saved"}})
                .has_value());
    require(take_value(verification->request_close(verificationId)) ==
            cue::editor_core::DocumentCloseState::AwaitingDecision);
    require(take_value(verification->respond_to_close(verificationId, cue::editor_core::CloseDecision::Save)) ==
            cue::editor_core::DocumentCloseState::SaveRequested);
    sourceAssets.fail_lease("Scenes/Renamed.cuescene", true);
    const auto failedCloseSave = verification->save_document(verificationId);
    require(!failedCloseSave.has_value());
    verifiedDocument = verification->session().find_document(verificationId);
    require(verifiedDocument != nullptr);
    require(verifiedDocument->close_state() == cue::editor_core::DocumentCloseState::AwaitingDecision);
    sourceAssets.fail_lease("Scenes/Renamed.cuescene", false);
    require(take_value(verification->respond_to_close(verificationId, cue::editor_core::CloseDecision::Save)) ==
            cue::editor_core::DocumentCloseState::SaveRequested);
    auto closeSave = verification->save_document(verificationId);
    require(closeSave.has_value() && closeSave.try_value()->status() == cue::scene::SceneSaveStatus::Committed);
    require(verification->session().find_document(verificationId) == nullptr);
}

/// @brief Package入力がActive DocumentではなくDescriptorの保存済みStartup Sceneから作られることを検証する
void test_saved_startup_scene_snapshot() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    cue::schema::SchemaRegistryIdentitySource registryIdentitySource;
    auto registry = make_component_registry(registryIdentitySource, assertContext);
    auto valueRegistry = make_component_value_registry(*registry, assertContext);
    cue::scene::SceneMigrationRegistry sceneMigrations;
    cue::scene::ComponentMigrationRegistry componentMigrations;
    MemoryFilesystemRoot sourceAssets(assertContext);
    MemoryFilesystemRoot savedRoot(assertContext);

    const auto objectId = make_object_id("00000000-0000-4000-8000-000000000799", assertContext);
    auto startupScene = make_scene_document("00000000-0000-4000-8000-000000000099", assertContext);
    require(startupScene.add_object(objectId, "Descriptor Startup", true, std::nullopt, cue::math::Transform{})
                .has_value());
    sourceAssets.set("Scenes/Default.cuescene",
                     take_value(cue::scene::serialize_scene_document(startupScene, assertContext)));

    cue::editor_core::ScenePersistenceServices services(sourceAssets, savedRoot, *registry, valueRegistry,
                                                        sceneMigrations, componentMigrations);
    auto controller =
        cue::editor_core::EditorController::create(make_project_descriptor(assertContext), services, assertContext);
    const auto documentId = take_value(controller->open_document_from_storage(
        take_value(cue::RelativePath::parse("Scenes/Default.cuescene", assertContext))));
    require(controller
                ->execute_command(cue::editor_core::SceneCommandRequest{
                    documentId, startupScene.scene_asset_id(),
                    cue::editor_core::RenameObjectCommand{objectId, "Save As Document"}})
                .has_value());
    auto dirtySnapshot = controller->load_saved_startup_scene_snapshot();
    require(!dirtySnapshot.has_value());
    require(dirtySnapshot.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidSavedState));
    const auto saveAs = controller->save_document_as(
        documentId, take_value(cue::RelativePath::parse("Scenes/Alternate.cuescene", assertContext)));
    require(saveAs.has_value() && saveAs.try_value()->status() == cue::scene::SceneSaveStatus::Committed);

    auto descriptorSnapshot = controller->load_saved_startup_scene_snapshot();
    require(descriptorSnapshot.has_value() && descriptorSnapshot.try_value()->objects().size() == 1U);
    require(descriptorSnapshot.try_value()->objects()[0].name() == "Descriptor Startup");

    auto externalScene = make_scene_document("00000000-0000-4000-8000-000000000099", assertContext);
    require(
        externalScene.add_object(objectId, "Externally Saved", true, std::nullopt, cue::math::Transform{}).has_value());
    sourceAssets.set("Scenes/Default.cuescene",
                     take_value(cue::scene::serialize_scene_document(externalScene, assertContext)));
    auto externalSnapshot = controller->load_saved_startup_scene_snapshot();
    require(externalSnapshot.has_value() && externalSnapshot.try_value()->objects().size() == 1U);
    require(externalSnapshot.try_value()->objects()[0].name() == "Externally Saved");

    auto mismatchedScene = make_scene_document("00000000-0000-4000-8000-000000000098", assertContext);
    sourceAssets.set("Scenes/Default.cuescene",
                     take_value(cue::scene::serialize_scene_document(mismatchedScene, assertContext)));
    auto mismatchedSnapshot = controller->load_saved_startup_scene_snapshot();
    require(!mismatchedSnapshot.has_value());
    require(mismatchedSnapshot.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::SceneMismatch));
}

/// @brief 外部変更と Close 判断の状態遷移を検証する
void test_external_change_and_close() noexcept
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    auto controller = cue::editor_core::EditorController::create(make_project_descriptor(assertContext), assertContext);

    auto cleanScene = make_scene_document("00000000-0000-4000-8000-000000000401", assertContext);
    auto cleanLocator = take_value(cue::RelativePath::parse("Scenes/Clean.cuescene", assertContext));
    const auto cleanId = take_value(controller->open_document(std::move(cleanScene), std::move(cleanLocator), true));
    require(take_value(controller->request_close(cleanId)) == cue::editor_core::DocumentCloseState::Closed);
    require(controller->session().find_document(cleanId) == nullptr);

    auto unsavedScene = make_scene_document("00000000-0000-4000-8000-000000000402", assertContext);
    auto unsavedLocator = take_value(cue::RelativePath::parse("Scenes/Unsaved.cuescene", assertContext));
    const auto unsavedId =
        take_value(controller->open_document(std::move(unsavedScene), std::move(unsavedLocator), false));
    const auto *unsavedDocument = controller->session().find_document(unsavedId);
    require(unsavedDocument != nullptr);
    const auto unsavedState = unsavedDocument->current_state_id();
    require(take_value(controller->request_close(unsavedId)) == cue::editor_core::DocumentCloseState::AwaitingDecision);
    require(take_value(controller->respond_to_close(unsavedId, cue::editor_core::CloseDecision::Cancel)) ==
            cue::editor_core::DocumentCloseState::Open);
    require(take_value(controller->request_close(unsavedId)) == cue::editor_core::DocumentCloseState::AwaitingDecision);
    require(take_value(controller->respond_to_close(unsavedId, cue::editor_core::CloseDecision::Save)) ==
            cue::editor_core::DocumentCloseState::SaveRequested);
    require(controller->mark_saved(unsavedId, unsavedState).has_value());
    require(controller->session().find_document(unsavedId) == nullptr);

    auto advancedScene = make_scene_document("00000000-0000-4000-8000-000000000405", assertContext);
    auto advancedLocator = take_value(cue::RelativePath::parse("Scenes/Advanced.cuescene", assertContext));
    const auto advancedId =
        take_value(controller->open_document(std::move(advancedScene), std::move(advancedLocator), true));
    const auto *advancedDocument = controller->session().find_document(advancedId);
    require(advancedDocument != nullptr);
    const auto initialAdvancedState = advancedDocument->current_state_id();
    require(controller->record_persistent_change(advancedId).has_value());
    require(take_value(controller->request_close(advancedId)) ==
            cue::editor_core::DocumentCloseState::AwaitingDecision);
    require(take_value(controller->respond_to_close(advancedId, cue::editor_core::CloseDecision::Save)) ==
            cue::editor_core::DocumentCloseState::SaveRequested);
    require(controller->mark_saved(advancedId, initialAdvancedState).has_value());
    advancedDocument = controller->session().find_document(advancedId);
    require(advancedDocument != nullptr);
    require(advancedDocument->is_dirty());
    require(advancedDocument->close_state() == cue::editor_core::DocumentCloseState::AwaitingDecision);

    auto failedSaveScene = make_scene_document("00000000-0000-4000-8000-000000000406", assertContext);
    auto failedSaveLocator = take_value(cue::RelativePath::parse("Scenes/FailedSave.cuescene", assertContext));
    const auto failedSaveId =
        take_value(controller->open_document(std::move(failedSaveScene), std::move(failedSaveLocator), false));
    require(take_value(controller->request_close(failedSaveId)) ==
            cue::editor_core::DocumentCloseState::AwaitingDecision);
    require(take_value(controller->respond_to_close(failedSaveId, cue::editor_core::CloseDecision::Save)) ==
            cue::editor_core::DocumentCloseState::SaveRequested);
    require(take_value(controller->report_save_failure(failedSaveId)) ==
            cue::editor_core::DocumentCloseState::AwaitingDecision);
    require(take_value(controller->respond_to_close(failedSaveId, cue::editor_core::CloseDecision::Cancel)) ==
            cue::editor_core::DocumentCloseState::Open);

    auto discardScene = make_scene_document("00000000-0000-4000-8000-000000000403", assertContext);
    auto discardLocator = take_value(cue::RelativePath::parse("Scenes/Discard.cuescene", assertContext));
    const auto discardId =
        take_value(controller->open_document(std::move(discardScene), std::move(discardLocator), false));
    require(take_value(controller->request_close(discardId)) == cue::editor_core::DocumentCloseState::AwaitingDecision);
    require(take_value(controller->respond_to_close(discardId, cue::editor_core::CloseDecision::Discard)) ==
            cue::editor_core::DocumentCloseState::Closed);
    require(controller->session().find_document(discardId) == nullptr);

    auto changedScene = make_scene_document("00000000-0000-4000-8000-000000000404", assertContext);
    auto changedLocator = take_value(cue::RelativePath::parse("Scenes/Changed.cuescene", assertContext));
    const auto changedId =
        take_value(controller->open_document(std::move(changedScene), std::move(changedLocator), true));
    require(
        controller->set_external_change_state(changedId, cue::editor_core::ExternalChangeState::Modified).has_value());
    require(take_value(controller->request_close(changedId)) == cue::editor_core::DocumentCloseState::AwaitingDecision);
    const auto conflictingSave = controller->respond_to_close(changedId, cue::editor_core::CloseDecision::Save);
    require(!conflictingSave.has_value());
    require(conflictingSave.try_error()->code().value() ==
            static_cast<std::int64_t>(cue::editor_core::EditorCoreError::InvalidCloseTransition));
    const auto *changedDocument = controller->session().find_document(changedId);
    require(changedDocument != nullptr);
    require(changedDocument->close_state() == cue::editor_core::DocumentCloseState::AwaitingDecision);
}
} // namespace

int main()
{
    test_workspace_and_open();
    test_revision_and_dirty();
    test_selection_reconciliation();
    test_editor_intents();
    test_scene_commands();
    test_transaction_history();
    test_scene_persistence_workflow();
    test_saved_startup_scene_snapshot();
    test_external_change_and_close();
    return 0;
}
