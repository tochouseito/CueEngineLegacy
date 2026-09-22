#include <Cue/Build/Service.h>

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Fatal.h>
#include <Cue/Foundation/Log.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
constexpr cue::BuildWorkspaceCompatibility k_workspaceCompatibility{
    cue::BuildGenerator::VisualStudio2026, cue::BuildArchitecture::X64, {19U, 51U, 0U, 0U}, 1U};

class TestFatalHandler final : public cue::FatalHandler
{
  public:
    /// @brief 想定外の引数なしFatal終了を固有Exit Codeで検出する
    [[noreturn]] void terminate() noexcept override
    {
        std::_Exit(90);
    }
    /// @brief 想定外のMessage付きFatal終了を固有Exit Codeで検出する
    [[noreturn]] void terminate(std::string_view) noexcept override
    {
        std::_Exit(91);
    }
};

enum class RunnerMode : std::uint8_t
{
    BlockUntilCancelled,
    Succeed,
    Fail
};

struct RunnerState final
{
    std::atomic<RunnerMode> mode = RunnerMode::BlockUntilCancelled;
    std::atomic<std::uint32_t> calls = 0U;
    std::atomic<bool> processActive = false;
};

struct InputLeaseState final
{
    std::atomic<std::uint32_t> acquisitions = 0U;
    std::atomic<std::uint32_t> liveLeases = 0U;
};

class TestInputLease final : public cue::BuildInputLease
{
  public:
    /// @brief Test共有状態へ一つのLive Leaseを記録する
    explicit TestInputLease(InputLeaseState &a_state) noexcept : m_state(&a_state)
    {
        m_state->liveLeases.fetch_add(1U, std::memory_order_release);
    }
    /// @brief Build完了時のLease解放をTest共有状態へ記録する
    ~TestInputLease() override
    {
        m_state->liveLeases.fetch_sub(1U, std::memory_order_release);
    }

  private:
    InputLeaseState *m_state;
};

class TestInputLeaseProvider final : public cue::BuildInputLeaseProvider
{
  public:
    /// @brief Test共有状態を借用する
    explicit TestInputLeaseProvider(InputLeaseState &a_state) noexcept : m_state(&a_state)
    {
    }
    /// @brief 各Build開始を記録し、一つのBuild-scoped Leaseを返す
    [[nodiscard]] cue::Result<std::unique_ptr<cue::BuildInputLease>> acquire(
        const cue::BuildPlan &) noexcept override
    {
        m_state->acquisitions.fetch_add(1U, std::memory_order_release);
        try
        {
            std::unique_ptr<cue::BuildInputLease> lease = std::make_unique<TestInputLease>(*m_state);
            return cue::Result<std::unique_ptr<cue::BuildInputLease>>::success(std::move(lease));
        }
        catch (...)
        {
            std::_Exit(92);
        }
    }

  private:
    InputLeaseState *m_state;
};

class ControlledRunner final : public cue::ChildProcessRunner
{
  public:
    /// @brief Testが共有するProcess状態を借用して制御可能Runnerを構築する
    explicit ControlledRunner(RunnerState &a_state) noexcept : m_state(&a_state)
    {
    }

    /// @brief 指定Modeに従う成功、失敗、Cancel結果と順序付きLogを返す
    [[nodiscard]] cue::Result<cue::ChildProcessResult> run(
        const cue::ChildProcessRequest &, const cue::ChildProcessCancellation &a_cancellation) noexcept override
    {
        m_state->processActive.store(true, std::memory_order_release);
        const std::uint32_t call = m_state->calls.fetch_add(1U, std::memory_order_relaxed);
        while (m_state->mode.load(std::memory_order_acquire) == RunnerMode::BlockUntilCancelled &&
               !a_cancellation.is_cancel_requested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_state->processActive.store(false, std::memory_order_release);
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::cancelled({}));
        }
        const std::uint32_t exitCode = m_state->mode.load(std::memory_order_acquire) == RunnerMode::Fail ? 2U : 0U;
        return cue::Result<cue::ChildProcessResult>::success(cue::ChildProcessResult::exited(
            exitCode, {{call, cue::ChildProcessStream::StandardOutput, "log-" + std::to_string(call)}}));
    }

  private:
    RunnerState *m_state;
};

struct PublisherState final
{
    std::atomic<std::uint32_t> calls = 0U;
    std::atomic<bool> blockUntilCancelled = false;
    std::atomic<bool> active = false;
    std::atomic<bool> failWithNativeError = false;
    std::atomic<bool> timeoutDuringLease = false;
    std::atomic<bool> timeoutDuringPublish = false;
};

class TestPublisher final : public cue::BuildArtifactPublisher
{
  public:
    class TestLease final : public cue::BuildWorkspaceLease
    {
      public:
        /// @brief 検証用Leaseの生存期間だけを表す
        TestLease() noexcept = default;
        /// @brief Native Resourceを持たない検証用Leaseを破棄する
        ~TestLease() override = default;
    };

    /// @brief Publish回数とAssert境界を借用して検証用Publisherを構築する
    TestPublisher(PublisherState &a_state, const cue::AssertContext &a_assertContext) noexcept
        : m_state(&a_state), m_assertContext(&a_assertContext)
    {
    }

    /// @brief 取消前ならBuild Workerへ検証用Exclusive Leaseを返す
    [[nodiscard]] cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>> acquire_build_lease(
        const cue::BuildPlan &, const cue::ChildProcessCancellation &a_cancellation,
        cue::BuildArtifactLockDeadline) noexcept override
    {
        if (m_state->timeoutDuringLease.load(std::memory_order_acquire))
        {
            cue::ErrorCode code =
                cue::ErrorCode::create(m_assertContext->fatal_handler(), "Cue.Build.Publisher",
                                       static_cast<std::int64_t>(cue::BuildArtifactPublisherError::LockWaitTimedOut));
            return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::failure(cue::Error::create(
                m_assertContext->fatal_handler(), std::move(code), "Injected publisher lock timeout"));
        }
        if (a_cancellation.is_cancel_requested())
        {
            return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(std::nullopt);
        }
        return cue::Result<std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>>::success(
            std::optional<std::unique_ptr<cue::BuildWorkspaceLease>>(std::make_unique<TestLease>()));
    }

    /// @brief Cancel、Native Error、成功Artifactを制御可能な検証用Publish結果として返す
    [[nodiscard]] cue::Result<std::optional<cue::BuildArtifactInventory>> publish(
        const cue::BuildPlan &a_plan, const cue::ChildProcessCancellation &a_cancellation,
        std::unique_ptr<cue::BuildWorkspaceLease> a_buildLease, cue::BuildArtifactLockDeadline) noexcept override
    {
        static_cast<void>(a_buildLease);
        m_state->calls.fetch_add(1U, std::memory_order_relaxed);
        if (m_state->timeoutDuringPublish.load(std::memory_order_acquire))
        {
            cue::ErrorCode code = cue::ErrorCode::create(
                m_assertContext->fatal_handler(), "Cue.Build.Publisher",
                static_cast<std::int64_t>(cue::BuildArtifactPublisherError::ModuleProbeTimedOut));
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(cue::Error::create(
                m_assertContext->fatal_handler(), std::move(code), "Injected Game Module probe timeout"));
        }
        if (m_state->blockUntilCancelled.load(std::memory_order_acquire))
        {
            m_state->active.store(true, std::memory_order_release);
            while (!a_cancellation.is_cancel_requested())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            m_state->active.store(false, std::memory_order_release);
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(std::nullopt);
        }
        if (m_state->failWithNativeError.load(std::memory_order_acquire))
        {
            cue::ErrorCode code =
                cue::ErrorCode::create(m_assertContext->fatal_handler(), "Cue.Build.TestPublisher", 1);
            cue::NativeError native = cue::NativeError::create(m_assertContext->fatal_handler(), "Win32", 5);
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(
                cue::Error::create(m_assertContext->fatal_handler(), std::move(code),
                                   "Injected artifact publication failure", std::move(native)));
        }
        auto inventory =
            cue::BuildArtifactInventory::create(a_plan, "11234567-89ab-4cde-8f01-23456789abcd",
                                                {{"CueGameModule.dll", 256U, std::string(64U, 'a')},
                                                 {"CueGameModule.metadata.json", 64U, std::string(64U, 'b')}},
                                                *m_assertContext);
        if (!inventory)
        {
            return cue::Result<std::optional<cue::BuildArtifactInventory>>::failure(std::move(*inventory.try_error()));
        }
        return cue::Result<std::optional<cue::BuildArtifactInventory>>::success(
            std::optional<cue::BuildArtifactInventory>(std::move(*inventory.try_value())));
    }

  private:
    PublisherState *m_state;
    const cue::AssertContext *m_assertContext;
};

/// @brief Process起動を行わないTest用のAbsolute Runner設定を返す
[[nodiscard]] cue::CMakeRunnerSettings make_settings()
{
    return {"C:/Tools/cmake.exe", "C:/CueEngine", {}, std::chrono::seconds(5), std::chrono::seconds(5),
            "14.51.36231"};
}

/// @brief Current DirectoryをProject Rootとする検証済みBuild Request入力を返す
[[nodiscard]] cue::BuildRequest make_request(std::string a_operationId, const cue::AssertContext &a_assertContext)
{
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Debug, cue::BuildTarget::GameModule, a_assertContext);
    return {std::filesystem::current_path().generic_string(), *profile.try_value(), std::move(a_operationId),
            k_workspaceCompatibility};
}

/// @brief 非同期WorkerがRunningへ遷移するまで上限付きで待機する
[[nodiscard]] bool wait_until_running(cue::GameBuildService &a_service)
{
    for (std::uint32_t attempt = 0U; attempt < 1000U; ++attempt)
    {
        if (a_service.snapshot().state == cue::GameBuildOperationState::Running)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief 検証用Publisherが取消待機へ入るまで上限付きで待機する
[[nodiscard]] bool wait_until_publisher_active(const PublisherState &a_state)
{
    for (std::uint32_t attempt = 0U; attempt < 1000U; ++attempt)
    {
        if (a_state.active.load(std::memory_order_acquire))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

/// @brief Artifact Modelが必須File、Hash、Pathと決定順を検証するか確認する
[[nodiscard]] bool test_artifact_model(const cue::AssertContext &a_assertContext)
{
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Release, cue::BuildTarget::GameModule, a_assertContext);
    cue::BuildRequest request{std::filesystem::current_path().generic_string(), *profile.try_value(),
                              "01234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    auto plan = cue::create_build_plan(request, a_assertContext);
    auto valid = cue::BuildArtifactInventory::create(*plan.try_value(), "11234567-89ab-4cde-8f01-23456789abcd",
                                                     {{"CueGameModule.metadata.json", 10U, std::string(64U, 'b')},
                                                      {"CueGameModule.dll", 20U, std::string(64U, 'a')}},
                                                     a_assertContext);
    auto traversal = cue::BuildArtifactInventory::create(*plan.try_value(), "21234567-89ab-4cde-8f01-23456789abcd",
                                                         {{"../CueGameModule.dll", 20U, std::string(64U, 'a')},
                                                          {"CueGameModule.metadata.json", 10U, std::string(64U, 'b')}},
                                                         a_assertContext);
    auto missing =
        cue::BuildArtifactInventory::create(*plan.try_value(), "31234567-89ab-4cde-8f01-23456789abcd",
                                            {{"CueGameModule.dll", 20U, std::string(64U, 'a')}}, a_assertContext);
    auto emptyMetadata = cue::BuildArtifactInventory::create(
        *plan.try_value(), "41234567-89ab-4cde-8f01-23456789abcd",
        {{"CueGameModule.dll", 20U, std::string(64U, 'a')}, {"CueGameModule.metadata.json", 0U, std::string(64U, 'b')}},
        a_assertContext);
    auto oversizedFile =
        cue::BuildArtifactInventory::create(*plan.try_value(), "51234567-89ab-4cde-8f01-23456789abcd",
                                            {{"CueGameModule.dll", 9007199254740992ULL, std::string(64U, 'a')},
                                             {"CueGameModule.metadata.json", 10U, std::string(64U, 'b')}},
                                             a_assertContext);
    auto shippingProfile = cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::UnsignedLocal, {}, a_assertContext);
    cue::BuildRequest shippingRequest{std::filesystem::current_path().generic_string(), *shippingProfile.try_value(),
                                      "61234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    auto shippingPlan = cue::create_build_plan(shippingRequest, a_assertContext);
    auto shipping = cue::BuildArtifactInventory::create(
        *shippingPlan.try_value(), "71234567-89ab-4cde-8f01-23456789abcd",
        {{"CueGameProduct.metadata.json", 30U, std::string(64U, 'd'),
          cue::BuildArtifactFilePurpose::RuntimeMetadata},
         {"CueGameProduct.exe", 40U, std::string(64U, 'c'),
          cue::BuildArtifactFilePurpose::DistributionPayload},
         {"CueGameProduct.pdb", 50U, std::string(64U, 'e'),
          cue::BuildArtifactFilePurpose::DevelopmentSymbol}},
        a_assertContext);
    auto shippingUnexpected = cue::BuildArtifactInventory::create(
        *shippingPlan.try_value(), "81234567-89ab-4cde-8f01-23456789abcd",
        {{"CueGameProduct.exe", 40U, std::string(64U, 'c')},
         {"CueGameProduct.metadata.json", 30U, std::string(64U, 'd')},
         {"CueGameModule.dll", 20U, std::string(64U, 'a')}},
        a_assertContext);
    return valid && valid.try_value()->configuration() == cue::BuildConfiguration::Release &&
           valid.try_value()->files()[0].relativePath == "CueGameModule.dll" &&
           valid.try_value()->version_directory().ends_with(valid.try_value()->artifact_id()) && !traversal &&
           !missing && !emptyMetadata && !oversizedFile && shipping &&
           shipping.try_value()->profile().target() == cue::BuildTarget::ShippingProduct &&
           shipping.try_value()->files()[0].purpose == cue::BuildArtifactFilePurpose::DistributionPayload &&
           shipping.try_value()->files()[1].purpose == cue::BuildArtifactFilePurpose::RuntimeMetadata &&
           shipping.try_value()->files()[2].purpose == cue::BuildArtifactFilePurpose::DevelopmentSymbol &&
           !shippingUnexpected;
}

/// @brief Current Manifest Readerが順序と空白を無視しSchema不一致をFail-closedにするか検証する
[[nodiscard]] bool test_current_manifest_validation(const cue::AssertContext &a_assertContext)
{
    auto profile =
        cue::BuildProfile::create(cue::BuildConfiguration::Release, cue::BuildTarget::GameModule, a_assertContext);
    cue::BuildRequest request{std::filesystem::current_path().generic_string(), *profile.try_value(),
                              "01234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    auto plan = cue::create_build_plan(request, a_assertContext);
    auto inventory = cue::BuildArtifactInventory::create(
        *plan.try_value(), "11234567-89ab-4cde-8f01-23456789abcd",
        {{"CueGameModule.metadata.json", 10U, std::string(64U, 'b')},
         {"CueGameModule.dll", 20U, std::string(64U, 'a')}},
        a_assertContext);
    if (!inventory)
    {
        return false;
    }
    const std::string valid =
        "{\n\"files\":[{\"contentHash\":\"" + std::string(64U, 'a') +
        "\",\"path\":\"CueGameModule\\u002edll\",\"hashAlgorithm\":\"sha256\",\"sizeBytes\":20},"
        "{\"sizeBytes\":10,\"hashAlgorithm\":\"sha256\",\"path\":\"CueGameModule.metadata.json\","
        "\"contentHash\":\"" +
        std::string(64U, 'b') +
        "\"}],\n\"configuration\":\"Release\",\"schemaVersion\":1,\"artifactId\":"
        "\"11234567-89ab-4cde-8f01-23456789abcd\"}\n";
    const std::string unknown = valid.substr(0U, valid.size() - 2U) + ",\"unknown\":true}\n";
    const std::string duplicate =
        "{\"schemaVersion\":1,\"schemaVersion\":1,\"configuration\":\"Release\",\"files\":[]}";
    const std::string wrongHash = valid.substr(0U, valid.find(std::string(64U, 'a'))) + std::string(64U, 'c') +
                                  valid.substr(valid.find(std::string(64U, 'a')) + 64U);
    const std::string trailing = valid + "null";
    auto shippingProfile = cue::BuildProfile::create_shipping_product(
        cue::BuildConfiguration::Release, cue::ShippingTrustMode::PublisherSigned, std::string(64U, 'c'),
        a_assertContext);
    cue::BuildRequest shippingRequest{std::filesystem::current_path().generic_string(), *shippingProfile.try_value(),
                                      "61234567-89ab-4cde-8f01-23456789abcd", k_workspaceCompatibility};
    auto shippingPlan = cue::create_build_plan(shippingRequest, a_assertContext);
    auto shippingInventory = cue::BuildArtifactInventory::create(
        *shippingPlan.try_value(), "71234567-89ab-4cde-8f01-23456789abcd",
        {{"CueGameProduct.metadata.json", 30U, std::string(64U, 'd')},
         {"CueGameProduct.exe", 40U, std::string(64U, 'c')}},
        a_assertContext);
    const std::string shippingCurrent =
        "{\"files\":[{\"purpose\":\"DistributionPayload\",\"path\":\"CueGameProduct.exe\","
        "\"sizeBytes\":40,\"contentHash\":\"" + std::string(64U, 'c') +
        "\",\"hashAlgorithm\":\"sha256\"},{\"hashAlgorithm\":\"sha256\","
        "\"contentHash\":\"" + std::string(64U, 'd') +
        "\",\"sizeBytes\":30,\"path\":\"CueGameProduct.metadata.json\","
        "\"purpose\":\"RuntimeMetadata\"}],\"publisherKeyId\":\"" + std::string(64U, 'c') +
        "\",\"target\":\"ShippingProduct\",\"schemaVersion\":2,"
        "\"minimumTrustMode\":\"PublisherSigned\",\"configuration\":\"Release\","
        "\"artifactId\":\"71234567-89ab-4cde-8f01-23456789abcd\"}";
    std::string wrongPublisher = shippingCurrent;
    wrongPublisher.replace(wrongPublisher.find(std::string(64U, 'c'),
                                               wrongPublisher.find("publisherKeyId")),
                           64U, std::string(64U, 'e'));
    std::string wrongPurpose = shippingCurrent;
    wrongPurpose.replace(wrongPurpose.find("DistributionPayload"), std::string_view("DistributionPayload").size(),
                         "DevelopmentSymbol");
    return cue::validate_build_artifact_current_manifest(valid, *inventory.try_value(), a_assertContext) &&
           !cue::validate_build_artifact_current_manifest(unknown, *inventory.try_value(), a_assertContext) &&
           !cue::validate_build_artifact_current_manifest(duplicate, *inventory.try_value(), a_assertContext) &&
           !cue::validate_build_artifact_current_manifest(wrongHash, *inventory.try_value(), a_assertContext) &&
           !cue::validate_build_artifact_current_manifest(trailing, *inventory.try_value(), a_assertContext) &&
           shippingInventory &&
           cue::validate_build_artifact_current_manifest(shippingCurrent, *shippingInventory.try_value(),
                                                         a_assertContext) &&
           !cue::validate_build_artifact_current_manifest(valid, *shippingInventory.try_value(), a_assertContext) &&
           !cue::validate_build_artifact_current_manifest(wrongPublisher, *shippingInventory.try_value(),
                                                          a_assertContext) &&
           !cue::validate_build_artifact_current_manifest(wrongPurpose, *shippingInventory.try_value(),
                                                          a_assertContext);
}

/// @brief 単一Active、Cancel、Retry、Latest成功Artifact保全をHeadless検証する
[[nodiscard]] bool test_service_lifecycle(const cue::AssertContext &a_assertContext)
{
    RunnerState runnerState;
    PublisherState publisherState;
    InputLeaseState inputLeaseState;
    auto created = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(runnerState),
                                                 std::make_unique<TestPublisher>(publisherState, a_assertContext),
                                                 std::make_unique<TestInputLeaseProvider>(inputLeaseState),
                                                 a_assertContext);
    if (!created)
    {
        return false;
    }
    std::unique_ptr<cue::GameBuildService> service = std::move(*created.try_value());
    auto started = service->start(make_request("01234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                                  cue::CMakeConfigureMode::Required);
    if (!started || !wait_until_running(*service) ||
        inputLeaseState.liveLeases.load(std::memory_order_acquire) != 1U ||
        inputLeaseState.acquisitions.load(std::memory_order_acquire) != 1U ||
        service->start(make_request("11234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                       cue::CMakeConfigureMode::Required))
    {
        return false;
    }
    if (!service->request_cancel() || !service->wait_for_completion() ||
        inputLeaseState.liveLeases.load(std::memory_order_acquire) != 0U)
    {
        return false;
    }
    cue::BuildOperationSnapshot cancelled = service->snapshot();
    if (cancelled.state != cue::GameBuildOperationState::Cancelled || cancelled.artifact ||
        cancelled.operationId != "01234567-89ab-4cde-8f01-23456789abcd")
    {
        return false;
    }

    runnerState.mode.store(RunnerMode::Succeed, std::memory_order_release);
    if (!service->retry("21234567-89ab-4cde-8f01-23456789abcd") || !service->wait_for_completion() ||
        inputLeaseState.liveLeases.load(std::memory_order_acquire) != 0U ||
        inputLeaseState.acquisitions.load(std::memory_order_acquire) != 2U)
    {
        return false;
    }
    cue::BuildOperationSnapshot succeeded = service->snapshot();
    if (succeeded.state != cue::GameBuildOperationState::Succeeded || !succeeded.artifact ||
        !succeeded.latestSuccessfulArtifact || succeeded.stages.size() != 2U || succeeded.logs.size() != 2U ||
        publisherState.calls != 1U)
    {
        return false;
    }
    for (const cue::BuildLogSnapshot &log : succeeded.logs)
    {
        if (log.operationId != "21234567-89ab-4cde-8f01-23456789abcd")
        {
            return false;
        }
    }

    publisherState.blockUntilCancelled.store(true, std::memory_order_release);
    if (!service->start(make_request("31234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                        cue::CMakeConfigureMode::Required) ||
        !wait_until_publisher_active(publisherState) ||
        inputLeaseState.liveLeases.load(std::memory_order_acquire) != 1U || !service->request_cancel() ||
        !service->wait_for_completion() || inputLeaseState.liveLeases.load(std::memory_order_acquire) != 0U)
    {
        return false;
    }
    publisherState.blockUntilCancelled.store(false, std::memory_order_release);
    cue::BuildOperationSnapshot publishCancelled = service->snapshot();
    if (publishCancelled.state != cue::GameBuildOperationState::Cancelled || publishCancelled.artifact ||
        !publishCancelled.latestSuccessfulArtifact ||
        publishCancelled.latestSuccessfulArtifact->artifact_id() != succeeded.artifact->artifact_id())
    {
        return false;
    }

    runnerState.mode.store(RunnerMode::Fail, std::memory_order_release);
    if (!service->start(make_request("41234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                        cue::CMakeConfigureMode::Required) ||
        !service->wait_for_completion())
    {
        return false;
    }
    cue::BuildOperationSnapshot failed = service->snapshot();
    return failed.state == cue::GameBuildOperationState::Failed && !failed.artifact &&
           failed.latestSuccessfulArtifact &&
           failed.latestSuccessfulArtifact->artifact_id() == succeeded.artifact->artifact_id() &&
           publisherState.calls == 2U && inputLeaseState.acquisitions.load(std::memory_order_acquire) == 4U &&
           inputLeaseState.liveLeases.load(std::memory_order_acquire) == 0U;
}

/// @brief Publisher由来CauseのNative Error DomainとCodeをSnapshotへ保持するか検証する
[[nodiscard]] bool test_native_error_snapshot(const cue::AssertContext &a_assertContext)
{
    RunnerState runnerState;
    runnerState.mode.store(RunnerMode::Succeed, std::memory_order_release);
    PublisherState publisherState;
    publisherState.failWithNativeError.store(true, std::memory_order_release);
    auto created = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(runnerState),
                                                 std::make_unique<TestPublisher>(publisherState, a_assertContext),
                                                 a_assertContext);
    if (!created)
    {
        return false;
    }
    std::unique_ptr<cue::GameBuildService> service = std::move(*created.try_value());
    if (!service->start(make_request("51234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                        cue::CMakeConfigureMode::Required) ||
        !service->wait_for_completion())
    {
        return false;
    }
    const cue::BuildOperationSnapshot failed = service->snapshot();
    return failed.state == cue::GameBuildOperationState::Failed && failed.diagnostics.size() == 2U &&
           !failed.diagnostics[0].nativeError && failed.diagnostics[1].nativeError &&
           failed.diagnostics[1].nativeError->domain == "Win32" && failed.diagnostics[1].nativeError->code == 5;
}

/// @brief Publisher Lock待機Timeoutを専用診断付きTimedOut状態へ変換するか検証する
[[nodiscard]] bool test_publisher_lock_timeout(const cue::AssertContext &a_assertContext)
{
    RunnerState runnerState;
    PublisherState publisherState;
    publisherState.timeoutDuringLease.store(true, std::memory_order_release);
    auto created = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(runnerState),
                                                 std::make_unique<TestPublisher>(publisherState, a_assertContext),
                                                 a_assertContext);
    if (!created)
    {
        return false;
    }
    std::unique_ptr<cue::GameBuildService> service = std::move(*created.try_value());
    if (!service->start(make_request("71234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                        cue::CMakeConfigureMode::Required) ||
        !service->wait_for_completion())
    {
        return false;
    }
    const cue::BuildOperationSnapshot timedOut = service->snapshot();
    return timedOut.state == cue::GameBuildOperationState::TimedOut && !timedOut.diagnostics.empty() &&
           timedOut.diagnostics.back().domain == "Cue.Build.Publisher" &&
           timedOut.diagnostics.back().code ==
               static_cast<std::int64_t>(cue::BuildArtifactPublisherError::LockWaitTimedOut) &&
           publisherState.calls == 0U;
}

/// @brief Publisher Module Probe Timeoutを専用診断付きTimedOut状態へ変換するか検証する
[[nodiscard]] bool test_publisher_module_probe_timeout(const cue::AssertContext &a_assertContext)
{
    RunnerState runnerState;
    runnerState.mode.store(RunnerMode::Succeed, std::memory_order_release);
    PublisherState publisherState;
    publisherState.timeoutDuringPublish.store(true, std::memory_order_release);
    auto created = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(runnerState),
                                                 std::make_unique<TestPublisher>(publisherState, a_assertContext),
                                                 a_assertContext);
    if (!created)
    {
        return false;
    }
    std::unique_ptr<cue::GameBuildService> service = std::move(*created.try_value());
    if (!service->start(make_request("81234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                        cue::CMakeConfigureMode::Required) ||
        !service->wait_for_completion())
    {
        return false;
    }
    const cue::BuildOperationSnapshot timedOut = service->snapshot();
    return timedOut.state == cue::GameBuildOperationState::TimedOut && !timedOut.diagnostics.empty() &&
           timedOut.diagnostics.back().domain == "Cue.Build.Publisher" &&
           timedOut.diagnostics.back().code ==
               static_cast<std::int64_t>(cue::BuildArtifactPublisherError::ModuleProbeTimedOut) &&
           publisherState.calls == 1U;
}

/// @brief Service破棄が進行中ProcessへCancelを通知して完了を待つか検証する
[[nodiscard]] bool test_shutdown(const cue::AssertContext &a_assertContext)
{
    RunnerState runnerState;
    PublisherState publisherState;
    auto created = cue::GameBuildService::create(make_settings(), std::make_unique<ControlledRunner>(runnerState),
                                                 std::make_unique<TestPublisher>(publisherState, a_assertContext),
                                                 a_assertContext);
    std::unique_ptr<cue::GameBuildService> service = std::move(*created.try_value());
    if (!service->start(make_request("61234567-89ab-4cde-8f01-23456789abcd", a_assertContext),
                        cue::CMakeConfigureMode::Required) ||
        !wait_until_running(*service))
    {
        return false;
    }
    service.reset();
    return !runnerState.processActive.load(std::memory_order_acquire);
}
} // namespace

/// @brief Game Build Service、Artifact、Cancel／Retry、Shutdown契約をHeadless検証する
int main()
{
    TestFatalHandler fatalHandler;
    std::vector<std::unique_ptr<cue::LogSink>> sinks;
    cue::Logger logger(fatalHandler, std::move(sinks));
    cue::AssertContext assertContext(logger, fatalHandler);
    return test_artifact_model(assertContext) && test_current_manifest_validation(assertContext) &&
                   test_service_lifecycle(assertContext) &&
                   test_native_error_snapshot(assertContext) && test_publisher_lock_timeout(assertContext) &&
                   test_publisher_module_probe_timeout(assertContext) && test_shutdown(assertContext)
               ? 0
               : 1;
}
