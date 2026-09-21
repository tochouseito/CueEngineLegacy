// D3D12 診断機能を Build 設定と Runtime 設定に従って有効化し、GPU 失敗時の根拠を Log へ保存する
// 診断自体の失敗は元の Graphics Error を失わないよう Secondary Context として伝播させる

#include "D3d12Diagnostics.h"

#include "D3d12Error.h"

#include <Cue/Foundation/Assert.h>
#include <Cue/Foundation/Error.h>
#include <Cue/Foundation/Log.h>
#include <Cue/Foundation/Windows/UtfConversion.h>

#include <Windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef CUE_D3D12_DIAGNOSTICS_ALLOWED
#error CUE_D3D12_DIAGNOSTICS_ALLOWED must be provided by the Cue.RHI.D3D12 CMake target
#endif

namespace
{
using cue::d3d12_private::make_error;
using cue::d3d12_private::make_native_error;

constexpr std::int64_t k_invalidConfiguration = 1;
constexpr std::int64_t k_invalidDevice = 2;
constexpr std::int64_t k_infoQueueRollbackFailed = 3;
constexpr std::int64_t k_infoQueueMessageFailed = 4;
constexpr std::int64_t k_deviceRemovedDiagnosticsFailed = 5;
constexpr std::int64_t k_optionalDiagnosticsUnavailable = 6;
constexpr std::int64_t k_diagnosticMessagesDropped = 7;
constexpr std::int64_t k_diagnosticLogFailed = 8;
constexpr std::uint64_t k_maxDiagnosticMessages = 4096;
constexpr std::uint32_t k_maxDredNodes = 4096;

/// @brief Allocation 失敗を追加 Allocation なしで Fatal 終了境界へ渡し、復帰時も Process を停止する
[[noreturn]] void terminate_allocation(const cue::AssertContext &a_context) noexcept
{
    a_context.fatal_handler().terminate("D3D12 diagnostics allocation failed");
    std::abort();
}

/// @brief D3D12 Message Severity を Engine 共通 Log Level へ変換する
[[nodiscard]] cue::LogLevel to_log_level(D3D12_MESSAGE_SEVERITY a_severity) noexcept
{
    switch (a_severity)
    {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
    case D3D12_MESSAGE_SEVERITY_ERROR:
        return cue::LogLevel::Error;
    case D3D12_MESSAGE_SEVERITY_WARNING:
        return cue::LogLevel::Warning;
    case D3D12_MESSAGE_SEVERITY_INFO:
        return cue::LogLevel::Info;
    case D3D12_MESSAGE_SEVERITY_MESSAGE:
    default:
        return cue::LogLevel::Debug;
    }
}

/// @brief D3D12 診断の Info Queue Breaks を診断と Lifecycle の規則に従って更新する
[[nodiscard]] HRESULT rollback_info_queue_breaks(ID3D12InfoQueue &a_infoQueue) noexcept
{
    HRESULT corruptionResult = a_infoQueue.SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
    HRESULT errorResult = a_infoQueue.SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
    return FAILED(corruptionResult) ? corruptionResult : errorResult;
}

/// @brief Log 出力結果を検証し、診断を継続できない場合は終了境界へ移す
[[nodiscard]] cue::Result<void> validate_log_result(cue::LogResult a_result,
                                                    const cue::AssertContext &a_context) noexcept
{
    if (a_result == cue::LogResult::Success)
    {
        return cue::Result<void>::success();
    }

    return cue::Result<void>::failure(
        make_error(a_context, k_diagnosticLogFailed, "Foundation Logger could not record D3D12 diagnostics"));
}

/// @brief D3D12 診断の Fallback を診断出力へ反映し、出力結果を返す
[[nodiscard]] cue::Result<void> log_fallback(const cue::AssertContext &a_context,
                                              std::string_view a_message, HRESULT a_nativeCode) noexcept
{
    cue::Error error = make_native_error(a_context, k_optionalDiagnosticsUnavailable,
                                         "Optional D3D12 diagnostics are unavailable", a_nativeCode);
    cue::LogResult logResult =
        a_context.logger().log(cue::LogLevel::Warning, a_message, std::move(error));

    if (logResult == cue::LogResult::Success)
    {
        return cue::Result<void>::success();
    }

    cue::Error cause = make_native_error(a_context, k_optionalDiagnosticsUnavailable,
                                         "Optional D3D12 diagnostics are unavailable", a_nativeCode);
    cue::ErrorCode code = cue::ErrorCode::create(
        a_context.fatal_handler(), "Cue.RHI.D3D12", k_diagnosticLogFailed);
    cue::Error logError = cue::Error::reclassify(
        a_context.fatal_handler(), std::move(code),
        "Foundation Logger could not record D3D12 diagnostics", std::move(cause));
    return cue::Result<void>::failure(std::move(logError));
}

/// @brief DRED Object 名を利用可能な文字表現から UTF-8 診断名へ変換する
[[nodiscard]] bool try_convert_dred_name(const wchar_t *a_name, std::string &a_storage,
                                         const cue::AssertContext &a_context) noexcept
{
    if (a_name == nullptr)
    {
        return false;
    }

    std::wstring_view name(a_name);

    if (name.empty() || name.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return false;
    }

    const cue::WindowsUtfConversionResult conversion = cue::convert_windows_utf16_to_utf8(
        name, a_storage, a_context.fatal_handler());
    return conversion.status == cue::WindowsUtfConversionStatus::Success;
}

/// @brief DRED 診断で利用可能な Object 名表現を優先順位に従って選択する
[[nodiscard]] std::string_view select_dred_name(
    const char *a_utf8Name, const wchar_t *a_utf16Name, std::string_view a_fallback,
    std::string &a_storage, const cue::AssertContext &a_context) noexcept
{
    if (a_utf8Name != nullptr)
    {
        return a_utf8Name;
    }

    if (try_convert_dred_name(a_utf16Name, a_storage, a_context))
    {
        return a_storage;
    }

    return a_fallback;
}

/// @brief D3D12 診断の DRED Breadcrumbs を診断出力へ反映し、出力結果を返す
[[nodiscard]] cue::Result<void> log_dred_breadcrumbs(
    const D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 &a_breadcrumbs,
    const cue::AssertContext &a_context) noexcept
{
    const D3D12_AUTO_BREADCRUMB_NODE1 *node = a_breadcrumbs.pHeadAutoBreadcrumbNode;
    std::uint32_t nodeCount = 0;

    while (node != nullptr && nodeCount < k_maxDredNodes)
    {
        const std::uint32_t lastBreadcrumb =
            node->pLastBreadcrumbValue != nullptr ? *node->pLastBreadcrumbValue : 0;
        std::int64_t operationValue = -1;

        if (node->pCommandHistory != nullptr && node->BreadcrumbCount > 0)
        {
            const std::uint32_t operationIndex = (std::min)(lastBreadcrumb, node->BreadcrumbCount - 1);
            operationValue = static_cast<std::int64_t>(node->pCommandHistory[operationIndex]);
        }

        std::string commandListNameStorage;
        std::string_view commandListName = select_dred_name(
            node->pCommandListDebugNameA, node->pCommandListDebugNameW,
            "Unnamed D3D12 Command List", commandListNameStorage, a_context);
        cue::ErrorCode code = cue::ErrorCode::create(a_context.fatal_handler(), "D3D12.DRED.Breadcrumb",
                                                     static_cast<std::int64_t>(lastBreadcrumb));
        cue::NativeError operation = cue::NativeError::create(a_context.fatal_handler(),
                                                              "D3D12.BreadcrumbOperation", operationValue);
        cue::Error error = cue::Error::create(a_context.fatal_handler(), std::move(code), commandListName,
                                              std::move(operation));

        std::string commandQueueNameStorage;
        std::string_view commandQueueName = select_dred_name(
            node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW,
            std::string_view(), commandQueueNameStorage, a_context);

        if (!commandQueueName.empty())
        {
            error.add_context(a_context.fatal_handler(), commandQueueName);
        }

        cue::LogResult logResult = a_context.logger().log(
            cue::LogLevel::Error, "D3D12 Device RemovalのBreadcrumb詳細を取得しました", std::move(error));
        cue::Result<void> validationResult = validate_log_result(logResult, a_context);

        if (!validationResult)
        {
            return validationResult;
        }

        node = node->pNext;
        ++nodeCount;
    }

    if (node != nullptr)
    {
        cue::LogResult logResult = a_context.logger().log(
            cue::LogLevel::Warning, "D3D12 Device RemovalのBreadcrumb Node上限を超えたため列挙を停止します");
        return validate_log_result(logResult, a_context);
    }

    return cue::Result<void>::success();
}

/// @brief D3D12 診断の DRED Allocations を診断出力へ反映し、出力結果を返す
[[nodiscard]] cue::Result<void> log_dred_allocations(
    const D3D12_DRED_ALLOCATION_NODE1 *a_head, std::string_view a_domain,
    std::string_view a_message, const cue::AssertContext &a_context) noexcept
{
    const D3D12_DRED_ALLOCATION_NODE1 *node = a_head;
    std::uint32_t nodeCount = 0;

    while (node != nullptr && nodeCount < k_maxDredNodes)
    {
        std::string objectNameStorage;
        std::string_view objectName = select_dred_name(
            node->ObjectNameA, node->ObjectNameW, "Unnamed D3D12 allocation",
            objectNameStorage, a_context);
        cue::ErrorCode code = cue::ErrorCode::create(
            a_context.fatal_handler(), a_domain, static_cast<std::int64_t>(node->AllocationType));
        cue::Error error = cue::Error::create(a_context.fatal_handler(), std::move(code), objectName);
        cue::LogResult logResult =
            a_context.logger().log(cue::LogLevel::Error, a_message, std::move(error));
        cue::Result<void> validationResult = validate_log_result(logResult, a_context);

        if (!validationResult)
        {
            return validationResult;
        }

        node = node->pNext;
        ++nodeCount;
    }

    if (node != nullptr)
    {
        cue::LogResult logResult = a_context.logger().log(
            cue::LogLevel::Warning, "D3D12 Device RemovalのAllocation Node上限を超えたため列挙を停止します");
        return validate_log_result(logResult, a_context);
    }

    return cue::Result<void>::success();
}

} // namespace

namespace cue
{
/// @brief DRED 診断で利用可能な Object 名表現を優先順位に従って選択する
std::string_view select_d3d12_dred_name(
    const char *a_utf8Name, const wchar_t *a_utf16Name, std::string_view a_fallback,
    std::string &a_storage, const AssertContext &a_assertContext) noexcept
{
    return select_dred_name(a_utf8Name, a_utf16Name, a_fallback, a_storage, a_assertContext);
}

/// @brief Build 設定と実行 Mode から D3D12 診断を有効化できるか判定する
bool are_d3d12_diagnostics_allowed() noexcept
{
    return CUE_D3D12_DIAGNOSTICS_ALLOWED != 0;
}

Result<D3d12DiagnosticsStatus> configure_d3d12_pre_device_diagnostics(
    const D3d12BackendDescriptor &a_descriptor, const AssertContext &a_assertContext) noexcept
{
    D3d12DiagnosticsStatus status = {};

    if (!are_d3d12_diagnostics_allowed())
    {
        if (a_descriptor.validationMode != D3d12ValidationMode::Disabled || a_descriptor.isDredEnabled)
        {
            return Result<D3d12DiagnosticsStatus>::failure(make_error(
                a_assertContext, k_invalidConfiguration, "D3D12 diagnostics are forbidden in Release builds"));
        }

        return Result<D3d12DiagnosticsStatus>::success(std::move(status));
    }

    if (a_descriptor.isDredEnabled)
    {
        Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
        HRESULT dredResult = D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings));

        if (SUCCEEDED(dredResult))
        {
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->SetWatsonDumpEnablement(D3D12_DRED_ENABLEMENT_FORCED_OFF);
            status.isDredEnabled = true;
        }
        else
        {
            Result<void> fallbackResult = log_fallback(
                a_assertContext, "D3D12 DREDを利用できないため診断なしで続行します", dredResult);

            if (!fallbackResult)
            {
                return Result<D3d12DiagnosticsStatus>::failure(std::move(*fallbackResult.try_error()));
            }
        }
    }

    if (a_descriptor.validationMode == D3d12ValidationMode::Disabled)
    {
        return Result<D3d12DiagnosticsStatus>::success(std::move(status));
    }

    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    HRESULT debugResult = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));

    if (FAILED(debugResult))
    {
        Result<void> fallbackResult = log_fallback(
            a_assertContext, "D3D12 Debug Layerを利用できないため診断なしで続行します", debugResult);

        if (!fallbackResult)
        {
            return Result<D3d12DiagnosticsStatus>::failure(std::move(*fallbackResult.try_error()));
        }

        return Result<D3d12DiagnosticsStatus>::success(std::move(status));
    }

    debugController->EnableDebugLayer();
    status.isDebugLayerEnabled = true;

    if (a_descriptor.validationMode == D3d12ValidationMode::GpuBased)
    {
        Microsoft::WRL::ComPtr<ID3D12Debug1> gpuValidationController;
        HRESULT gpuValidationResult = debugController.As(&gpuValidationController);

        if (FAILED(gpuValidationResult))
        {
            Result<void> fallbackResult = log_fallback(
                a_assertContext, "D3D12 GPU Based Validationを利用できないためStandardで続行します",
                gpuValidationResult);

            if (!fallbackResult)
            {
                return Result<D3d12DiagnosticsStatus>::failure(std::move(*fallbackResult.try_error()));
            }
        }
        else
        {
            gpuValidationController->SetEnableGPUBasedValidation(TRUE);
        }
    }

    return Result<D3d12DiagnosticsStatus>::success(std::move(status));
}

// InfoQueue は Device 生成後に取得し、開発中に見逃せない Severity だけ Debugger 停止へ接続する
// 途中設定に失敗した場合は既に有効化した Break 条件を戻し、部分適用状態を残さない
Result<void> configure_d3d12_info_queue(ID3D12Device *a_device, D3d12DiagnosticsStatus &a_status,
                                        const AssertContext &a_assertContext) noexcept
{
    a_status.isInfoQueueEnabled = false;

    if (!a_status.isDebugLayerEnabled)
    {
        return Result<void>::success();
    }

    if (a_device == nullptr)
    {
        return Result<void>::failure(
            make_error(a_assertContext, k_invalidDevice, "D3D12 Device is required for InfoQueue configuration"));
    }

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    HRESULT queryResult = a_device->QueryInterface(IID_PPV_ARGS(&infoQueue));

    if (FAILED(queryResult))
    {
        return log_fallback(
            a_assertContext, "D3D12 InfoQueueを利用できないため診断なしで続行します", queryResult);
    }

    infoQueue->ClearStorageFilter();
    infoQueue->ClearRetrievalFilter();

    for (D3D12_MESSAGE_SEVERITY severity : {D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR})
    {
        HRESULT breakResult = infoQueue->SetBreakOnSeverity(severity, TRUE);

        if (FAILED(breakResult))
        {
            HRESULT rollbackResult = rollback_info_queue_breaks(*infoQueue.Get());

            if (FAILED(rollbackResult))
            {
                return Result<void>::failure(make_native_error(
                    a_assertContext, k_infoQueueRollbackFailed,
                    "D3D12 InfoQueue Break policy could not be rolled back", rollbackResult));
            }

            return log_fallback(
                a_assertContext, "D3D12 InfoQueueのBreak Policyを設定できないため診断なしで続行します",
                breakResult);
        }
    }

    HRESULT warningBreakResult = infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);

    if (FAILED(warningBreakResult))
    {
        HRESULT rollbackResult = rollback_info_queue_breaks(*infoQueue.Get());

        if (FAILED(rollbackResult))
        {
            return Result<void>::failure(make_native_error(
                a_assertContext, k_infoQueueRollbackFailed,
                "D3D12 InfoQueue Break policy could not be rolled back", rollbackResult));
        }

        return log_fallback(
            a_assertContext, "D3D12 InfoQueueのWarning Policyを設定できないため診断なしで続行します",
            warningBreakResult);
    }

    a_status.isInfoQueueEnabled = true;
    return Result<void>::success();
}

Result<void> report_d3d12_live_device_objects(
    ID3D12Device *a_device, const D3d12DiagnosticsStatus &a_status,
    const AssertContext &a_assertContext) noexcept
{
    if (!a_status.isDebugLayerEnabled || !are_d3d12_diagnostics_allowed())
    {
        return Result<void>::success();
    }

    if (a_device == nullptr)
    {
        return Result<void>::failure(
            make_error(a_assertContext, k_invalidDevice,
                       "D3D12 Device is required for live object diagnostics"));
    }

    Microsoft::WRL::ComPtr<ID3D12DebugDevice1> debugDevice;
    HRESULT queryResult = a_device->QueryInterface(IID_PPV_ARGS(&debugDevice));

    if (FAILED(queryResult))
    {
        return log_fallback(
            a_assertContext, "D3D12 Live Object診断Interfaceを取得できません", queryResult);
    }

    HRESULT reportResult = debugDevice->ReportLiveDeviceObjects(
        D3D12_RLDO_SUMMARY | D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);

    if (FAILED(reportResult))
    {
        return log_fallback(
            a_assertContext, "D3D12 Live Object診断を実行できません", reportResult);
    }

    return Result<void>::success();
}

Result<void> log_d3d12_messages_at_quiescent_point(
    ID3D12Device *a_device, const D3d12DiagnosticsStatus &a_status, std::string_view a_context,
    const AssertContext &a_assertContext) noexcept
{
    if (!a_status.isInfoQueueEnabled)
    {
        return Result<void>::success();
    }

    if (a_device == nullptr)
    {
        return Result<void>::failure(
            make_error(a_assertContext, k_invalidDevice, "D3D12 Device is required for InfoQueue logging"));
    }

    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue;
    HRESULT queryResult = a_device->QueryInterface(IID_PPV_ARGS(&infoQueue));

    if (FAILED(queryResult))
    {
        return Result<void>::failure(make_native_error(a_assertContext, k_infoQueueMessageFailed,
                                                       "D3D12 InfoQueue is unavailable", queryResult));
    }

    std::uint64_t messageIndex = 0;
    std::uint64_t storedMessageCount = 0;

    do
    {
        while (messageIndex < k_maxDiagnosticMessages)
        {
            std::uint64_t availableMessageCount =
                infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();

            if (messageIndex >= availableMessageCount)
            {
                std::uint64_t confirmedMessageCount =
                    infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();

                if (messageIndex >= confirmedMessageCount)
                {
                    break;
                }

                continue;
            }

            SIZE_T messageSize = 0;
            HRESULT sizeResult = infoQueue->GetMessage(messageIndex, nullptr, &messageSize);

            if (FAILED(sizeResult))
            {
                return Result<void>::failure(make_native_error(
                    a_assertContext, k_infoQueueMessageFailed,
                    "D3D12 InfoQueue message size could not be read", sizeResult));
            }

            try
            {
                std::vector<std::byte> messageStorage(messageSize);
                D3D12_MESSAGE *message = reinterpret_cast<D3D12_MESSAGE *>(messageStorage.data());
                HRESULT messageResult = infoQueue->GetMessage(messageIndex, message, &messageSize);

                if (FAILED(messageResult))
                {
                    return Result<void>::failure(make_native_error(
                        a_assertContext, k_infoQueueMessageFailed,
                        "D3D12 InfoQueue message could not be read", messageResult));
                }

                std::string_view description = message->pDescription != nullptr
                                                   ? std::string_view(message->pDescription)
                                                   : std::string_view("D3D12 message description is unavailable");
                ErrorCode code = ErrorCode::create(a_assertContext.fatal_handler(), "D3D12.Message",
                                                   static_cast<std::int64_t>(message->ID));
                Error error = Error::create(a_assertContext.fatal_handler(), std::move(code), description);
                error.add_context(a_assertContext.fatal_handler(), a_context);
                LogResult logResult = a_assertContext.logger().log(
                    to_log_level(message->Severity), "D3D12診断メッセージを取得しました", std::move(error));
                Result<void> validationResult = validate_log_result(logResult, a_assertContext);

                if (!validationResult)
                {
                    return validationResult;
                }

                ++messageIndex;
            }
            catch (...)
            {
                terminate_allocation(a_assertContext);
            }
        }

        storedMessageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    } while (messageIndex < (std::min)(storedMessageCount, k_maxDiagnosticMessages));

    if (storedMessageCount > messageIndex)
    {
        ErrorCode code = ErrorCode::create(a_assertContext.fatal_handler(), "D3D12.Message",
                                           k_diagnosticMessagesDropped);
        NativeError droppedCount = NativeError::create(
            a_assertContext.fatal_handler(), "D3D12.DroppedMessageCount",
            static_cast<std::int64_t>(storedMessageCount - messageIndex));
        Error error = Error::create(a_assertContext.fatal_handler(), std::move(code),
                                    "D3D12 diagnostic message limit was exceeded", std::move(droppedCount));
        error.add_context(a_assertContext.fatal_handler(), a_context);
        LogResult logResult = a_assertContext.logger().log(
            LogLevel::Warning, "D3D12診断メッセージの上限超過分を破棄します", std::move(error));
        Result<void> validationResult = validate_log_result(logResult, a_assertContext);

        if (!validationResult)
        {
            return validationResult;
        }
    }

    // 呼出側が D3D12 API 停止を保証するため、最終件数確認から Clear まで新規 Message が追加されない
    infoQueue->ClearStoredMessages();
    return Result<void>::success();
}

// DRED が有効な場合は、Native Object 解放後に失われる Breadcrumb と Page Fault 情報の収集を試行する
Result<void> collect_d3d12_device_removed_diagnostics(ID3D12Device *a_device,
                                                       const D3d12DiagnosticsStatus &a_status,
                                                       bool &a_isDredCollectionInterfaceAvailable,
                                                       const AssertContext &a_assertContext) noexcept
{
    a_isDredCollectionInterfaceAvailable = false;
    if (a_device == nullptr)
    {
        return Result<void>::failure(
            make_error(a_assertContext, k_invalidDevice, "D3D12 Device is required for removal diagnostics"));
    }

    if (!a_status.isDredEnabled || !are_d3d12_diagnostics_allowed())
    {
        return Result<void>::success();
    }

    Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    HRESULT queryResult = a_device->QueryInterface(IID_PPV_ARGS(&dred));

    if (FAILED(queryResult))
    {
        return log_fallback(
            a_assertContext, "D3D12 Device RemovalのDRED Interfaceを取得できません", queryResult);
    }

    a_isDredCollectionInterfaceAvailable = true;

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
    HRESULT breadcrumbsResult = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);

    if (FAILED(breadcrumbsResult))
    {
        Result<void> fallbackResult = log_fallback(
            a_assertContext, "D3D12 Device RemovalのAuto Breadcrumbを取得できません", breadcrumbsResult);

        if (!fallbackResult)
        {
            return fallbackResult;
        }
    }
    else
    {
        if (breadcrumbs.pHeadAutoBreadcrumbNode == nullptr)
        {
            LogResult logResult = a_assertContext.logger().log(
                LogLevel::Error, "D3D12 Device RemovalのAuto Breadcrumbは空です");
            Result<void> validationResult = validate_log_result(logResult, a_assertContext);

            if (!validationResult)
            {
                return validationResult;
            }
        }
        else
        {
            Result<void> breadcrumbLogResult = log_dred_breadcrumbs(breadcrumbs, a_assertContext);

            if (!breadcrumbLogResult)
            {
                return breadcrumbLogResult;
            }
        }
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault = {};
    HRESULT pageFaultResult = dred->GetPageFaultAllocationOutput1(&pageFault);

    if (FAILED(pageFaultResult))
    {
        Result<void> fallbackResult = log_fallback(
            a_assertContext, "D3D12 Device RemovalのPage Fault情報を取得できません", pageFaultResult);

        if (!fallbackResult)
        {
            return fallbackResult;
        }
    }
    else
    {
        NativeError pageFaultAddress = NativeError::create(
            a_assertContext.fatal_handler(), "D3D12.GpuVirtualAddress",
            static_cast<std::int64_t>(pageFault.PageFaultVA));
        ErrorCode code = ErrorCode::create(a_assertContext.fatal_handler(), "Cue.RHI.D3D12",
                                           k_deviceRemovedDiagnosticsFailed);
        Error pageFaultError = Error::create(a_assertContext.fatal_handler(), std::move(code),
                                             "D3D12 Device Removal page fault data", std::move(pageFaultAddress));
        LogResult pageFaultLogResult = a_assertContext.logger().log(
            LogLevel::Error, "D3D12 Device RemovalのPage Fault情報を取得しました", std::move(pageFaultError));
        Result<void> validationResult = validate_log_result(pageFaultLogResult, a_assertContext);

        if (!validationResult)
        {
            return validationResult;
        }

        Result<void> existingAllocationResult = log_dred_allocations(
            pageFault.pHeadExistingAllocationNode, "D3D12.DRED.ExistingAllocation",
            "D3D12 Device Removalの既存Allocation詳細を取得しました", a_assertContext);

        if (!existingAllocationResult)
        {
            return existingAllocationResult;
        }

        Result<void> freedAllocationResult = log_dred_allocations(
            pageFault.pHeadRecentFreedAllocationNode, "D3D12.DRED.RecentFreedAllocation",
            "D3D12 Device Removalの解放済みAllocation詳細を取得しました", a_assertContext);

        if (!freedAllocationResult)
        {
            return freedAllocationResult;
        }
    }

    return Result<void>::success();
}
} // namespace cue
