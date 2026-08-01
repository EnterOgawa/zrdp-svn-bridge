/**
 * @file diagnostic_log.h
 * @brief 機密情報を受け付けない構造化診断ログの公開契約を定義する。
 *
 * @details 任意文字列のログAPIを公開せず、固定event名と数値fieldだけを
 *          bounded queueへ渡すことで、接続情報やpayloadの誤記録を防止する。
 */
#pragma once

#include <Windows.h>

#include <cstdint>
#include <initializer_list>
#include <string>

namespace zrdp::common {

/**
 * @brief 診断recordへ固定値として記録するprocessの役割。
 */
enum class DiagnosticRole {
    Client,  ///< 開発PC側のRDP Client。
    Agent,   ///< 支給PC側のSVN Bridge Agent。
};

/**
 * @brief 構造化診断eventのseverity。
 */
enum class DiagnosticLevel {
    Information,  ///< 正常な状態遷移または完了。
    Warning,      ///< 接続継続可能な異常または縮退。
    Error,        ///< 操作失敗または接続不能な異常。
};

/**
 * @brief 診断loggerが受け付ける固定event名。
 *
 * 自由形式messageを意図的に提供しないことで、接続先、domain、ユーザー名、
 * credential、URL、header、payloadが誤ってloggerへ渡ることを防止する。
 */
enum class DiagnosticEvent {
    ProcessStarted,
    ProcessStopping,
    ComInitializationFailed,
    MainWindowCreateStarted,
    MainWindowCreated,
    MainWindowCreateFailed,
    ActiveXCreateStarted,
    ActiveXCreated,
    ActiveXCreateFailed,
    ActiveXControlSelected,
    ParentWindowConfigurationReturned,
    CredSspConfigurationReturned,
    VirtualChannelRegistrationReturned,
    ConnectionHistoryLoadReturned,
    ConnectionHistorySaveReturned,
    ConnectButtonClicked,
    ConnectInputRejected,
    ConnectionSettingsApplyStarted,
    ConnectionSettingsApplyReturned,
    ConnectCallStarted,
    ConnectCallReturned,
    PreLoginConnectionRetryScheduled,
    PreLoginConnectionRetryStarted,
    PreLoginConnectionRetryCompleted,
    PreLoginConnectionRetryExhausted,
    DisconnectButtonClicked,
    DisconnectCallReturned,
    FullScreenCallReturned,
    SmartSizingCallReturned,
    DynamicResolutionCallReturned,
    CloseRequested,
    BridgeSessionStarted,
    BridgeReady,
    BridgeEchoStarted,
    BridgeEchoPassed,
    BridgePeerTimeout,
    BridgeFailed,
    ClientProxyStateChanged,
    ClientProxyStartFailed,
    AgentSessionValidationReturned,
    AgentChannelOpenReturned,
    AgentAllowlistLoadReturned,
    AgentWinsockInitializationReturned,
    AgentStreamRelayEnabled,
    AgentChannelReadFailed,
    AgentChannelWriteFailed,
    EventConnecting,
    EventConnected,
    EventLoginComplete,
    EventDisconnected,
    EventEnterFullScreen,
    EventLeaveFullScreen,
    EventChannelDataReceived,
    EventRequestGoFullScreen,
    EventRequestLeaveFullScreen,
    EventFatalError,
    EventWarning,
    EventConfirmClose,
    EventAutoReconnecting,
    EventAutoReconnected,
    EventAuthenticationWarningDisplayed,
    EventAuthenticationWarningDismissed,
    EventDispatchFailed,
    UnexpectedException,
    LogRecordsDropped,
};

/**
 * @brief 診断recordへ追加できる固定数値field名。
 */
enum class DiagnosticFieldName {
    HResult,
    Code,
    AttemptCount,
    MaximumAttemptCount,
    NetworkAvailable,
    ByteCount,
    RecordCount,
    ControlVersion,
    ExitCode,
    Reason,
    ExtendedReason,
    Enabled,
    DisplayMode,
    BridgeState,
    BridgeError,
    LocallyRequested,
    CredentialPromptCancelled,
    ProxyState,
    ProxyError,
    Port,
};

/**
 * @brief 一つの構造化診断eventへ付加する数値field。
 */
struct DiagnosticField {
    /// 数値の意味と出力keyを決める固定field名。
    DiagnosticFieldName name{DiagnosticFieldName::Code};

    /// signed値もbit patternを保持して格納する64-bit値。
    std::uint64_t value{0U};
};

/**
 * @brief rotation前のcurrent log最大size。
 */
inline constexpr std::uint64_t kDiagnosticLogMaximumBytes =
    1024U * 1024U;

/**
 * @brief current logの隣に保持するbackup世代数。
 */
inline constexpr unsigned int kDiagnosticLogBackupGenerations = 3U;

/**
 * @brief 自由形式textを使わずHRESULT fieldを生成する。
 * @param[in] result 記録するHRESULT。
 * @return 32-bitのHRESULT bit patternを保持した診断field。
 */
[[nodiscard]] constexpr DiagnosticField DiagnosticHResult(
    const HRESULT result) noexcept {
    return DiagnosticField{
        DiagnosticFieldName::HResult,
        static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(result)),
    };
}

/**
 * @brief 自由形式textを使わずsigned code fieldを生成する。
 * @param[in] code 記録するActiveXまたはWin32由来のcode。
 * @return 符号を保持した診断field。
 */
[[nodiscard]] constexpr DiagnosticField DiagnosticCode(
    const long code) noexcept {
    return DiagnosticField{
        DiagnosticFieldName::Code,
        static_cast<std::uint64_t>(
            static_cast<std::int64_t>(code)),
    };
}

/**
 * @brief 固定名を持つunsigned数値fieldを生成する。
 * @param[in] name 診断recordへ出力するfield名。
 * @param[in] value 記録する数値。
 * @return 指定値を保持した診断field。
 */
[[nodiscard]] constexpr DiagnosticField DiagnosticValue(
    const DiagnosticFieldName name,
    const std::uint64_t value) noexcept {
    return DiagnosticField{name, value};
}

/**
 * @brief ClientまたはAgent用のbounded診断logを開始する。
 *
 * @param[in] role 診断recordへ付与するprocessの役割。
 * @return workerとlog fileを利用可能にできた場合はtrue。
 *
 * EXE隣接のlogs directoryを第一候補とし、書込み不能な場合だけ
 * LOCALAPPDATA以下へfallbackする。
 */
[[nodiscard]] bool InitializeDiagnosticLog(DiagnosticRole role) noexcept;

/**
 * @brief file I/Oを待たずに固定名の診断recordをqueueへ投入する。
 * @param[in] level eventのseverity。
 * @param[in] event 自由形式文字列を含まない固定event。
 * @param[in] fields eventへ付加する固定名の数値field群。
 *
 * queue満杯またはlogger停止中はrecordを破棄し、例外を呼出し元へ漏らさない。
 */
void WriteDiagnosticLog(
    DiagnosticLevel level,
    DiagnosticEvent event,
    std::initializer_list<DiagnosticField> fields = {}) noexcept;

/**
 * @brief 受理済みrecordをflushして診断logを停止する。
 *
 * 呼出しthreadでworkerの終了を待つため、UI終了処理以外からは呼び出さない。
 */
void ShutdownDiagnosticLog() noexcept;

/**
 * @brief 選択されたlog pathのcopyを取得する。
 * @return 初期化成功時はlog path、失敗時は空文字列。
 */
[[nodiscard]] std::wstring DiagnosticLogPath() noexcept;

}  // namespace zrdp::common
