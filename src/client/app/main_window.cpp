/**
 * @file main_window.cpp
 * @brief Win32 UI、RDP ActiveX、接続履歴をCOM STA上で連携する。
 *
 * 等倍表示とSmart Sizingではremote desktop寸法を維持する。利用者が明示した
 * 動的解像度modeだけは、debounce後にremote desktop寸法を表示領域へ追従させる。
 */

#include "client/app/main_window.h"

#include "client/app/dynamic_resolution_retry.h"
#include "client/app/dynamic_resolution_target.h"
#include "client/app/pre_login_connection_retry.h"
#include "client/app/rdp_window_sizing.h"
#include "client/app/rdp_user_identity.h"
#include "client/app/resource.h"
#include "client/app/session_disconnect_policy.h"
#include "common/diagnostic_log.h"

#include <CommCtrl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace zrdp::client::app {
namespace {

// UI固有メッセージと、96 DPIを基準とする初期レイアウト値。
constexpr wchar_t kMainWindowClassName[] = L"ZRdpSvnBridge.MainWindow";
constexpr UINT kMessageCloseReady = WM_APP + 1U;
constexpr UINT kMessageSetFullScreen = WM_APP + 2U;
constexpr UINT kMessageShowDisconnectError = WM_APP + 3U;
constexpr UINT kMessageBridgeSend = WM_APP + 5U;
constexpr UINT kMessageProxyWorker = WM_APP + 6U;
constexpr UINT kMessageAutoConnect = WM_APP + 7U;
constexpr UINT_PTR kDynamicResolutionTimerId = 1U;
constexpr UINT_PTR kBridgeHandshakeTimerId = 2U;
constexpr UINT_PTR kBridgeEchoTimerId = 3U;
constexpr UINT_PTR kBridgeHealthTimerId = 4U;
constexpr UINT_PTR kPreLoginConnectionRetryTimerId = 5U;
constexpr UINT kDynamicResolutionDelayMilliseconds = 250U;
constexpr UINT kBridgeHandshakeRetryMilliseconds = 1000U;
constexpr UINT kBridgeEchoTimeoutMilliseconds = 15000U;
constexpr UINT kBridgeHealthIntervalMilliseconds = 5000U;
constexpr UINT kBridgeHealthTimeoutMilliseconds = 15000U;
constexpr std::size_t kMaximumBridgeChunksPerDispatch = 8U;
constexpr std::size_t kMaximumProxyFramesPerDispatch = 32U;
constexpr int kInitialClientWidth = 1280;
constexpr int kInitialClientHeight = 800;
constexpr int kMinimumWindowWidth = 900;
constexpr int kMinimumWindowHeight = 520;
constexpr int kSessionWindowCascadeOffset = 28;
constexpr int kDisplayModeToolbarMargin = 4;
constexpr int kDisplayModeToolbarGap = 4;
constexpr int kDisplayModeControlHeight = 23;
constexpr int kDisplayModeToolbarHeight =
    kDisplayModeToolbarMargin * 2 +
    kDisplayModeControlHeight;

/**
 * @brief Proxy workerからUI messageだけを予約するnon-blocking callback。
 * @param[in] context MainWindowのHWND。
 * @return messageを予約できた場合はtrue。
 */
[[nodiscard]] bool PostProxyWorkerMessage(
    void* const context) noexcept {
    const HWND window = static_cast<HWND>(context);
    return window != nullptr &&
        PostMessageW(
            window,
            kMessageProxyWorker,
            0U,
            0L) != FALSE;
}

// ---------------------------------------------------------------------------
// 接続状態と入力値の補助処理
// ---------------------------------------------------------------------------

/**
 * @brief Windows 標準 RDP の全画面接続と同じ初期デスクトップ寸法を取得する。
 *
 * @param[in] window 接続を開始する ZRdp のトップレベルウィンドウ。
 * @param[out] width ウィンドウが存在するモニターの幅。
 * @param[out] height ウィンドウが存在するモニターの高さ。
 * @return 寸法を取得できた場合は S_OK、それ以外は原因を表す HRESULT。
 *
 * RDP は接続開始時にクライアントからデスクトップ寸法を必ず要求する。
 * ウィンドウ表示領域ではなくモニター全体を基準にすることで、ZRdp の
 * ウィンドウ変更が接続先アプリケーションのレイアウトへ波及しない。
 */
[[nodiscard]] HRESULT ResolveInitialDesktopSize(
    const HWND window,
    long& width,
    long& height) noexcept {
    const HMONITOR monitor =
        MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    // 無効なモニター情報から接続先のデスクトップ寸法を決定してはならない。
    if (monitor == nullptr) {
        return E_FAIL;
    }

    MONITORINFO monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (!GetMonitorInfoW(monitor, &monitor_info)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    const long monitor_width =
        monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
    const long monitor_height =
        monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
    // 空の矩形は RDP の初期デスクトップ寸法として利用できない。
    if (monitor_width <= 0L || monitor_height <= 0L) {
        return E_UNEXPECTED;
    }

    width = monitor_width;
    height = monitor_height;
    return S_OK;
}

/**
 * @brief 現在のActiveX host表示領域を動的解像度のdesktop寸法として取得する。
 * @param[in] rdp_window RdpControlHostが所有する有効なchild window。
 * @param[out] width child windowのclient幅。
 * @param[out] height child windowのclient高さ。
 * @return 正の寸法を取得できた場合はS_OK、それ以外は原因HRESULT。
 */
[[nodiscard]] HRESULT ResolveRdpDisplayAreaSize(
    const HWND rdp_window,
    long& width,
    long& height) noexcept {
    // 別windowの寸法や破棄済みHWNDをremote sessionへ適用しない。
    if (!IsWindow(rdp_window)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }

    RECT bounds{};
    if (!GetClientRect(rdp_window, &bounds)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    const long client_width = bounds.right - bounds.left;
    const long client_height = bounds.bottom - bounds.top;
    // 最小化や生成途中の空領域はremote desktop寸法として利用しない。
    if (client_width <= 0L || client_height <= 0L) {
        return E_INVALIDARG;
    }

    width = client_width;
    height = client_height;
    return S_OK;
}

/**
 * @brief 接続操作を排他的に扱う必要がある状態か判定する。
 * @param state 現在のRDP接続状態。
 * @return 新規接続入力を無効化すべき場合はtrue。
 */
[[nodiscard]] bool IsConnectionActive(
    const rdp::RdpConnectionState state) noexcept {
    return state == rdp::RdpConnectionState::Connecting ||
        state == rdp::RdpConnectionState::Connected ||
        state == rdp::RdpConnectionState::LoggedIn ||
        state == rdp::RdpConnectionState::Reconnecting ||
        state == rdp::RdpConnectionState::Disconnecting ||
        state == rdp::RdpConnectionState::Failed;
}

/**
 * @brief ActiveXの全画面切替を要求できる状態か判定する。
 * @param state 現在のRDP接続状態。
 * @return 画面セッションが成立している場合はtrue。
 */
[[nodiscard]] bool CanUseFullScreen(
    const rdp::RdpConnectionState state) noexcept {
    return state == rdp::RdpConnectionState::Connected ||
        state == rdp::RdpConnectionState::LoggedIn ||
        state == rdp::RdpConnectionState::Reconnecting;
}

/**
 * @brief 成立済みsessionへ表示modeを切替可能な状態か判定する。
 * @param[in] state 現在のRDP接続状態。
 * @return ActiveXへ実行時設定を適用できる場合はtrue。
 */
[[nodiscard]] bool CanChangeDisplayMode(
    const rdp::RdpConnectionState state) noexcept {
    return rdp::CanUpdateSessionDisplaySettings(state);
}

/**
 * @brief session titleに表示する短いRDP状態resourceを選択する。
 * @param[in] state 現在のRDP接続状態。
 * @return 対応するstring resource ID。
 */
[[nodiscard]] UINT RdpTitleStateResource(
    const rdp::RdpConnectionState state) noexcept {
    switch (state) {
        case rdp::RdpConnectionState::NotInitialized:
            return IDS_TITLE_RDP_NOT_CONNECTED;
        case rdp::RdpConnectionState::Disconnected:
            return IDS_TITLE_RDP_NOT_CONNECTED;
        case rdp::RdpConnectionState::Connecting:
            return IDS_TITLE_RDP_CONNECTING;
        case rdp::RdpConnectionState::Connected:
            return IDS_TITLE_RDP_CONNECTED;
        case rdp::RdpConnectionState::LoggedIn:
            return IDS_TITLE_RDP_CONNECTED;
        case rdp::RdpConnectionState::Reconnecting:
            return IDS_TITLE_RDP_RECONNECTING;
        case rdp::RdpConnectionState::Disconnecting:
            return IDS_TITLE_RDP_DISCONNECTING;
        case rdp::RdpConnectionState::Failed:
            return IDS_TITLE_RDP_FAILED;
    }

    return IDS_TITLE_RDP_FAILED;
}

/**
 * @brief session titleに表示する短いAgent状態resourceを選択する。
 * @param[in] state 現在のBridge接続状態。
 * @return 対応するstring resource ID。
 */
[[nodiscard]] UINT AgentTitleStateResource(
    const bridge::ClientBridgeState state) noexcept {
    switch (state) {
        case bridge::ClientBridgeState::Stopped:
            return IDS_TITLE_AGENT_STOPPED;
        case bridge::ClientBridgeState::WaitingForAgent:
            return IDS_TITLE_AGENT_WAITING;
        case bridge::ClientBridgeState::Ready:
            return IDS_TITLE_AGENT_CONNECTED;
        case bridge::ClientBridgeState::EchoRunning:
            return IDS_TITLE_AGENT_ECHO_RUNNING;
        case bridge::ClientBridgeState::EchoPassed:
            return IDS_TITLE_AGENT_CONNECTED;
        case bridge::ClientBridgeState::Failed:
            return IDS_TITLE_AGENT_FAILED;
        case bridge::ClientBridgeState::Incompatible:
            return IDS_TITLE_AGENT_INCOMPATIBLE;
    }

    return IDS_TITLE_AGENT_FAILED;
}

/**
 * @brief session titleに表示する短いProxy状態resourceを選択する。
 * @param[in] snapshot 現在のProxy worker状態。
 * @return 対応するstring resource ID。
 */
[[nodiscard]] UINT ProxyTitleStateResource(
    const proxy::ClientProxyWorkerSnapshot& snapshot) noexcept {
    // errorを保持するworkerを見かけ上の待受中または利用可として表示しない。
    if (snapshot.error !=
            proxy::ClientProxyWorkerError::None ||
        snapshot.state ==
            proxy::ClientProxyWorkerState::Failed) {
        return IDS_TITLE_PROXY_FAILED;
    }

    switch (snapshot.state) {
        case proxy::ClientProxyWorkerState::Stopped:
            return IDS_TITLE_PROXY_STOPPED;
        case proxy::ClientProxyWorkerState::Starting:
            return IDS_TITLE_PROXY_STARTING;
        case proxy::ClientProxyWorkerState::Listening:
            return snapshot.bridge_ready
                ? IDS_TITLE_PROXY_READY
                : IDS_TITLE_PROXY_LISTENING;
        case proxy::ClientProxyWorkerState::Failed:
            return IDS_TITLE_PROXY_FAILED;
    }

    return IDS_TITLE_PROXY_FAILED;
}

/**
 * @brief UI入力の前後にある空白文字を除いた所有文字列を返す。
 * @param value 編集する入力文字列。
 * @return 前後空白を除いた文字列。空白だけの場合は空文字列。
 */
[[nodiscard]] std::wstring TrimCopy(std::wstring value) {
    const auto not_space = [](const wchar_t character) {
        return std::iswspace(static_cast<wint_t>(character)) == 0;
    };

    const auto first = std::find_if(value.begin(), value.end(), not_space);
    const auto last = std::find_if(value.rbegin(), value.rend(), not_space).base();
    // 空白だけの入力では不正な接続先文字列を生成しない。
    if (first >= last) {
        return {};
    }

    return std::wstring(first, last);
}

}  // namespace

// ---------------------------------------------------------------------------
// メインウィンドウの生成とメッセージ処理
// ---------------------------------------------------------------------------

MainWindow::MainWindow(
    const HINSTANCE instance,
    ISessionWindowObserver& observer,
    SessionLaunchSettings settings)
    : instance_(instance),
      observer_(observer),
      launch_settings_(std::move(settings)),
      rdp_host_(*this),
      display_mode_(
          launch_settings_.connection.display_mode) {}

MainWindow::~MainWindow() {
    // application側の生成失敗rollbackでもtop-level Windowを残さない。
    if (window_ != nullptr &&
        IsWindow(window_)) {
        DestroyWindow(window_);
    }

    // COM STAが終了する前に、UIへ通知し得るワーカーを先に停止する。
    CancelDynamicResolutionUpdate();
    StopBridgeSession();
    client_proxy_worker_.Stop();
    rdp_host_.Destroy();
}

bool MainWindow::Create(const int show_command) {
    // クラス登録に失敗した場合は、部分的なウィンドウ生成へ進まない。
    if (!RegisterWindowClass()) {
        create_error_ = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }

    RECT initial_bounds{
        0,
        0,
        kInitialClientWidth,
        kInitialClientHeight,
    };
    const UINT dpi = GetDpiForSystem();
    // 指定したクライアント領域が得られない場合は不正な初期寸法を使わない。
    if (!AdjustWindowRectExForDpi(
            &initial_bounds,
            WS_OVERLAPPEDWINDOW,
            FALSE,
            WS_EX_CONTROLPARENT,
            dpi)) {
        create_error_ = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }

    const int window_width = initial_bounds.right - initial_bounds.left;
    const int window_height = initial_bounds.bottom - initial_bounds.top;
    const int screen_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int screen_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int screen_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int screen_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int centered_x =
        screen_x +
        std::max(0, (screen_width - window_width) / 2);
    const int centered_y =
        screen_y +
        std::max(0, (screen_height - window_height) / 2);
    const int cascade_step =
        MulDiv(
            kSessionWindowCascadeOffset,
            static_cast<int>(dpi),
            USER_DEFAULT_SCREEN_DPI);
    const int cascade_offset =
        cascade_step *
        static_cast<int>(
            launch_settings_.proxy_slot);
    const int maximum_x =
        screen_x +
        std::max(0, screen_width - window_width);
    const int maximum_y =
        screen_y +
        std::max(0, screen_height - window_height);
    // 追加sessionを完全重畳させず、virtual screen外へ出ない範囲で斜めに配置する。
    const int window_x =
        std::clamp(
            centered_x + cascade_offset,
            screen_x,
            maximum_x);
    const int window_y =
        std::clamp(
            centered_y + cascade_offset,
            screen_y,
            maximum_y);

    const std::wstring title = LoadStringResource(IDS_APP_TITLE);
    window_ = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kMainWindowClassName,
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        window_x,
        window_y,
        window_width,
        window_height,
        nullptr,
        nullptr,
        instance_,
        this);
    // WM_CREATE中の詳細エラーを保持している場合は、汎用LastErrorで上書きしない。
    if (window_ == nullptr) {
        if (SUCCEEDED(create_error_)) {
            create_error_ = HRESULT_FROM_WIN32(GetLastError());
        }

        return false;
    }

    ShowWindow(window_, show_command);
    UpdateWindow(window_);
    return true;
}

bool MainWindow::ProcessDialogMessage(
    MSG& message) const noexcept {
    // 他sessionまたはmanager宛てmessageをこのWindowのTab処理へ取り込まない。
    if (window_ == nullptr ||
        (message.hwnd != window_ &&
         !IsChild(window_, message.hwnd))) {
        return false;
    }

    return IsDialogMessageW(window_, &message) != FALSE;
}

HWND MainWindow::window() const noexcept {
    return window_;
}

// ---------------------------------------------------------------------------
// RDPイベントとWin32コールバック境界
// ---------------------------------------------------------------------------

void MainWindow::OnRdpEvent(rdp::RdpEvent event) {
    // COM callbackから所有コピー化されたイベントだけをUI状態へ反映する。
    switch (event.kind) {
        case rdp::RdpEventKind::Connecting:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::EventConnecting);
            SetStatus(IDS_STATUS_CONNECTING);
            UpdateUiForState(rdp::RdpConnectionState::Connecting);
            break;
        case rdp::RdpEventKind::Connected:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::EventConnected);
            SetStatus(IDS_STATUS_CONNECTED);
            UpdateUiForState(rdp::RdpConnectionState::Connected);
            break;
        case rdp::RdpEventKind::LoginComplete:
            // 一回以上の有限再試行後に成功した場合だけ固定数値eventで識別する。
            if (pre_login_connection_retries_scheduled_ > 0U) {
                common::WriteDiagnosticLog(
                    common::DiagnosticLevel::Information,
                    common::DiagnosticEvent::
                        PreLoginConnectionRetryCompleted,
                    {
                        common::DiagnosticValue(
                            common::DiagnosticFieldName::AttemptCount,
                            pre_login_connection_retries_scheduled_),
                        common::DiagnosticValue(
                            common::DiagnosticFieldName::Port,
                            launch_settings_.proxy_port),
                    });
            }
            CancelPreLoginConnectionRetry();
            login_completed_ = true;
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::EventLoginComplete);
            SetStatus(IDS_STATUS_CONNECTED);
            UpdateUiForState(rdp::RdpConnectionState::LoggedIn);
            RememberSuccessfulConnection();
            ScheduleDynamicResolutionUpdate();
            StartBridgeSession();
            break;
        case rdp::RdpEventKind::AutoReconnected:
            // 再接続成功は、ログイン済みsessionへ復帰したことを明示的に保持する。
            login_completed_ = true;
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::EventAutoReconnected);
            SetStatus(IDS_STATUS_CONNECTED);
            UpdateUiForState(rdp::RdpConnectionState::LoggedIn);
            ScheduleDynamicResolutionUpdate();
            StartBridgeSession();
            break;
        case rdp::RdpEventKind::Disconnected: {
            CancelDynamicResolutionUpdate();
            StopBridgeSession();
            const bool security_warning_cancelled =
                IsSecurityWarningCancellation(
                    login_completed_,
                    event.locally_requested_disconnect,
                    event.credential_prompt_cancelled,
                    authentication_dialog_display_count_,
                    event.code,
                    event.extended_disconnect_reason);
            const PreLoginConnectionRetryDecision retry_decision =
                SelectPreLoginConnectionRetry(
                    login_completed_,
                    event.locally_requested_disconnect,
                    event.credential_prompt_cancelled,
                    authentication_dialog_was_displayed_,
                    closing_,
                    event.code,
                    event.extended_disconnect_reason,
                    pre_login_connection_retries_scheduled_);
            // Microsoft dialog終了後の遅延focus処理を切断済みControlへ残さない。
            authentication_dialog_active_ = false;
            const bool close_after_external_disconnect =
                ShouldCloseWindowAfterExternalDisconnect(
                    login_completed_,
                    event.locally_requested_disconnect,
                    event.credential_prompt_cancelled);
            login_completed_ = false;
            const bool user_initiated_disconnect =
                event.locally_requested_disconnect ||
                event.credential_prompt_cancelled ||
                security_warning_cancelled;
            common::WriteDiagnosticLog(
                user_initiated_disconnect
                    ? common::DiagnosticLevel::Information
                    : retry_decision.action ==
                            PreLoginConnectionRetryAction::Schedule
                        ? common::DiagnosticLevel::Warning
                        : common::DiagnosticLevel::Error,
                common::DiagnosticEvent::EventDisconnected,
                {
                    common::DiagnosticCode(event.code),
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::ExtendedReason,
                        static_cast<std::uint32_t>(
                            event.extended_disconnect_reason)),
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::LocallyRequested,
                        event.locally_requested_disconnect ? 1U : 0U),
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::
                            CredentialPromptCancelled,
                        event.credential_prompt_cancelled ? 1U : 0U),
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::
                            SecurityWarningCancelled,
                        security_warning_cancelled ? 1U : 0U),
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::Port,
                        launch_settings_.proxy_port),
                });
            // 成立済みsessionの最終外部切断では、診断をlogだけに残してWindowを閉じる。
            if (close_after_external_disconnect) {
                pending_disconnect_error_.clear();
                closing_ = true;
                PostMessageW(window_, kMessageCloseReady, 0U, 0L);
                break;
            }

            // Microsoft警告で接続を拒否済みなら、重複errorを出さず未成立Windowを閉じる。
            if (security_warning_cancelled) {
                pending_disconnect_error_.clear();
                closing_ = true;
                PostMessageW(window_, kMessageCloseReady, 0U, 0L);
                break;
            }

            // 完全一致した有限回の失敗はdialogを出さず、callback外のtimerへ渡す。
            if (retry_decision.action ==
                    PreLoginConnectionRetryAction::Schedule &&
                SchedulePreLoginConnectionRetry(
                    event,
                    retry_decision.delay_milliseconds)) {
                break;
            }

            // 有限上限へ到達後は原因を隠さず、従来のerror表示へ戻して必ず停止する。
            if (retry_decision.action ==
                PreLoginConnectionRetryAction::Exhausted) {
                common::WriteDiagnosticLog(
                    common::DiagnosticLevel::Error,
                    common::DiagnosticEvent::
                        PreLoginConnectionRetryExhausted,
                    {
                        common::DiagnosticValue(
                            common::DiagnosticFieldName::AttemptCount,
                            pre_login_connection_retries_scheduled_),
                        common::DiagnosticValue(
                            common::DiagnosticFieldName::
                                MaximumAttemptCount,
                            kMaximumPreLoginConnectionRetries),
                        common::DiagnosticValue(
                            common::DiagnosticFieldName::Reason,
                            static_cast<std::uint32_t>(event.code)),
                        common::DiagnosticValue(
                            common::DiagnosticFieldName::ExtendedReason,
                            static_cast<std::uint32_t>(
                                event.extended_disconnect_reason)),
                        common::DiagnosticValue(
                            common::DiagnosticFieldName::Port,
                            launch_settings_.proxy_port),
                    });
            }

            // credential UIの取消は追加診断を見せず通常の未接続表示へ戻す。
            if (event.credential_prompt_cancelled) {
                SetStatus(IDS_STATUS_DISCONNECTED);
            } else {
                SetDisconnectedStatus(
                    event.code,
                    event.extended_disconnect_reason);
            }
            UpdateUiForState(rdp::RdpConnectionState::Disconnected);
            UpdateFullScreenButton(false);
            QueueDisconnectError(event);
            // 終了待機中はActiveX切断完了後にだけトップレベルウィンドウを破棄する。
            if (closing_) {
                PostMessageW(window_, kMessageCloseReady, 0U, 0L);
            }
            break;
        }
        case rdp::RdpEventKind::EnterFullScreen:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::EventEnterFullScreen);
            UpdateFullScreenButton(true);
            // 動的解像度だけは全画面monitorへ追従し、ほかのmodeは寸法を維持する。
            ScheduleDynamicResolutionUpdate();
            break;
        case rdp::RdpEventKind::LeaveFullScreen:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::EventLeaveFullScreen);
            UpdateFullScreenButton(false);
            ScheduleDynamicResolutionUpdate();
            break;
        case rdp::RdpEventKind::ChannelDataReceived: {
            // 登録済みでも名称が一致しないcallbackをESVNBRG dataとして解釈しない。
            if (event.channel_name !=
                protocol::kStaticVirtualChannelNameWide) {
                break;
            }

            const bridge::ClientBridgeSnapshot before =
                client_bridge_.snapshot();
            static_cast<void>(
                client_bridge_.OnChannelData(
                    event.channel_payload));
            const bridge::ClientBridgeSnapshot processed =
                client_bridge_.snapshot();
            // HELLO_ACKをworker session開始へ反映してから同callbackのstream Frameを渡す。
            if (!SynchronizeProxyBridgeSession(
                    before.state,
                    processed.state) ||
                !DrainInboundStreamFramesToProxy()) {
                client_bridge_.OnStreamRelayFailed();
                client_proxy_worker_.EnqueueBridgeStop();
            }
            UpdateBridgeHandshakeTimer();
            // 完全なPONG確認時だけ期限を延長し、無関係なfragmentでは延長しない。
            if (processed.state !=
                    bridge::ClientBridgeState::EchoRunning ||
                processed.echo_completed_bytes !=
                    before.echo_completed_bytes) {
                UpdateBridgeEchoTimer();
            }

            bridge::ClientBridgeSnapshot after =
                client_bridge_.snapshot();
            // handshake／Echo／health応答の境界だけで周期timerを更新する。
            if (before.state != after.state ||
                before.health_check_outstanding !=
                    after.health_check_outstanding) {
                UpdateBridgeHealthTimer();
                after = client_bridge_.snapshot();
            }
            LogBridgeTransition(before.state, after);
            UpdateBridgeUi();
            // PONG処理が次PINGをqueueしてもCOM callback中には送信しない。
            QueueBridgeSendMessage();
            break;
        }
        case rdp::RdpEventKind::RequestGoFullScreen:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::EventRequestGoFullScreen);
            PostMessageW(window_, kMessageSetFullScreen, TRUE, 0L);
            break;
        case rdp::RdpEventKind::RequestLeaveFullScreen:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::EventRequestLeaveFullScreen);
            PostMessageW(window_, kMessageSetFullScreen, FALSE, 0L);
            break;
        case rdp::RdpEventKind::FatalError:
            authentication_dialog_active_ = false;
            CancelPreLoginConnectionRetry();
            CancelDynamicResolutionUpdate();
            StopBridgeSession();
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Error,
                common::DiagnosticEvent::EventFatalError,
                {common::DiagnosticCode(event.code)});
            SetStatusWithCode(IDS_STATUS_FATAL_FORMAT, event.code);
            UpdateUiForState(rdp::RdpConnectionState::Failed);
            break;
        case rdp::RdpEventKind::Warning:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Warning,
                common::DiagnosticEvent::EventWarning,
                {common::DiagnosticCode(event.code)});
            SetStatusWithCode(IDS_STATUS_WARNING_FORMAT, event.code);
            break;
        case rdp::RdpEventKind::ConfirmClose:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::EventConfirmClose,
                {
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::Port,
                        launch_settings_.proxy_port),
                });
            PostMessageW(window_, kMessageCloseReady, 0U, 0L);
            break;
        case rdp::RdpEventKind::AutoReconnecting:
            CancelDynamicResolutionUpdate();
            StopBridgeSession();
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Warning,
                common::DiagnosticEvent::EventAutoReconnecting,
                {
                    common::DiagnosticCode(event.code),
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::AttemptCount,
                        static_cast<std::uint64_t>(
                            event.attempt_count)),
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::MaximumAttemptCount,
                        static_cast<std::uint64_t>(
                            event.max_attempt_count)),
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::NetworkAvailable,
                        event.network_available ? 1U : 0U),
                });
            SetStatus(IDS_STATUS_RECONNECTING);
            UpdateUiForState(rdp::RdpConnectionState::Reconnecting);
            break;
        case rdp::RdpEventKind::AuthenticationWarningDisplayed:
            // eventからdismissedまでActiveX childへfocusを奪い返さない。
            authentication_dialog_active_ = true;
            authentication_dialog_was_displayed_ = true;
            // certificate UIの到達判定に必要な二段階だけを有限状態として保持する。
            if (authentication_dialog_display_count_ <
                kSecurityWarningCancellationMinimumDisplayCount) {
                ++authentication_dialog_display_count_;
            }
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Warning,
                common::DiagnosticEvent::EventAuthenticationWarningDisplayed);
            SetStatus(IDS_STATUS_AUTH_WARNING);
            break;
        case rdp::RdpEventKind::AuthenticationWarningDismissed:
            authentication_dialog_active_ = false;
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::EventAuthenticationWarningDismissed);
            // 警告解除後は現在の接続段階に対応する状態表示へ戻す。
            if (CanUseFullScreen(rdp_host_.state())) {
                SetStatus(IDS_STATUS_CONNECTED);
            } else {
                SetStatus(IDS_STATUS_CONNECTING);
            }
            break;
    }
}

LRESULT CALLBACK MainWindow::WindowProcedure(
    HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) noexcept {
    MainWindow* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    // 最初の生成メッセージで、CreateWindowExWから渡された所有者を関連付ける。
    if (message == WM_NCCREATE) {
        const auto* create_data = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<MainWindow*>(create_data->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
    }

    // 所有者がまだ関連付いていないメッセージは既定処理へ委ねる。
    if (self == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    try {
        const LRESULT result = self->HandleMessage(message, wparam, lparam);
        // 破棄後のHWND再利用による誤dispatchを防ぐため関連付けを解除する。
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->window_ = nullptr;
        }

        return result;
    } catch (...) {
        // C++例外をUSER32 callback境界から漏らさず、固定イベントだけを記録する。
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Error,
            common::DiagnosticEvent::UnexpectedException);
        std::array<wchar_t, 512> error_text{};
        LoadStringW(
            self->instance_,
            IDS_UNEXPECTED_ERROR,
            error_text.data(),
            static_cast<int>(error_text.size()));
        MessageBoxW(
            window,
            error_text.data(),
            L"ZRdp SVN Bridge",
            MB_OK | MB_ICONERROR);
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

LRESULT MainWindow::HandleMessage(
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    // ActiveXを所有するUIスレッド上でのみ、各メッセージに対応する状態を変更する。
    switch (message) {
        case WM_CREATE: {
            // 必須UIが一つでも欠ける場合は、曖昧な接続操作を許可せず生成を中止する。
            if (!CreateControls()) {
                create_error_ = HRESULT_FROM_WIN32(GetLastError());
                return -1;
            }

            RECT client_bounds{};
            GetClientRect(window_, &client_bounds);
            LayoutControls(client_bounds.right, client_bounds.bottom);

            const RECT rdp_bounds =
                RdpBounds(client_bounds.right, client_bounds.bottom);
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::ActiveXCreateStarted);
            create_error_ = rdp_host_.Create(window_, rdp_bounds);
            // ActiveX生成失敗時はRDP操作可能な画面として公開しない。
            if (FAILED(create_error_)) {
                common::WriteDiagnosticLog(
                    common::DiagnosticLevel::Error,
                    common::DiagnosticEvent::ActiveXCreateFailed,
                    {common::DiagnosticHResult(create_error_)});
                ShowError(IDS_ACTIVE_X_CREATE_FAILED, create_error_);
                return -1;
            }

            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::ActiveXCreated);
            bool proxy_started = false;
            // 不正なclient.iniではbase portへ黙ってfallbackせずProxyだけを停止する。
            if (launch_settings_.
                    proxy_configuration_error ==
                proxy::ClientProxyConfigurationError::None) {
                proxy::ClientProxyWorkerSettings
                    proxy_settings{};
                proxy_settings.port =
                    launch_settings_.proxy_port;
                proxy_settings.load_client_ini = false;
                proxy_started =
                    client_proxy_worker_.Start(
                        proxy_settings,
                        &PostProxyWorkerMessage,
                        window_);
            }

            // Proxy開始失敗でもRDP画面を利用可能なまま維持し専用状態欄へ表示する。
            if (!proxy_started) {
                common::WriteDiagnosticLog(
                    common::DiagnosticLevel::Error,
                    common::DiagnosticEvent::
                        ClientProxyStartFailed);
            }
            SetInitialStatus();
            UpdateBridgeUi();
            UpdateWindowTitle();
            // Window表示後に接続状態を描画してからMicrosoft認証UIを開始する。
            static_cast<void>(
                PostMessageW(
                    window_,
                    kMessageAutoConnect,
                    0U,
                    0L));
            return 0;
        }
        case WM_ENTERSIZEMOVE: {
            RECT sizing_reference{};
            manual_sizing_reference_width_ = 0;
            manual_sizing_reference_height_ = 0;
            // 一回のdrag中に補正後寸法を基準へ戻さず、利用者の主操作軸を安定させる。
            if (GetWindowRect(
                    window_,
                    &sizing_reference)) {
                const std::int64_t width =
                    static_cast<std::int64_t>(
                        sizing_reference.right) -
                    static_cast<std::int64_t>(
                        sizing_reference.left);
                const std::int64_t height =
                    static_cast<std::int64_t>(
                        sizing_reference.bottom) -
                    static_cast<std::int64_t>(
                        sizing_reference.top);
                // RECT差分をintへ安全に保持できる場合だけ角操作の基準にする。
                if (width > 0 &&
                    height > 0 &&
                    width <=
                        std::numeric_limits<int>::max() &&
                    height <=
                        std::numeric_limits<int>::max()) {
                    manual_sizing_reference_width_ =
                        static_cast<int>(width);
                    manual_sizing_reference_height_ =
                        static_cast<int>(height);
                }
            }
            return 0;
        }
        case WM_EXITSIZEMOVE:
            manual_sizing_reference_width_ = 0;
            manual_sizing_reference_height_ = 0;
            return 0;
        case WM_SIZING: {
            auto* proposed_bounds =
                reinterpret_cast<RECT*>(lparam);
            // USER32の手動変更候補だけを補正し、最大化やスナップへ介入しない。
            if (proposed_bounds != nullptr &&
                ConstrainManualSizing(
                    static_cast<UINT>(wparam),
                    *proposed_bounds)) {
                return TRUE;
            }

            break;
        }
        case WM_SIZE: {
            // 最小化中は0に近い表示領域をActiveXへ適用しない。
            if (wparam != SIZE_MINIMIZED) {
                const int width = LOWORD(lparam);
                const int height = HIWORD(lparam);
                LayoutControls(width, height);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested_bounds =
                reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(
                window_,
                nullptr,
                suggested_bounds->left,
                suggested_bounds->top,
                suggested_bounds->right - suggested_bounds->left,
                suggested_bounds->bottom - suggested_bounds->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* min_max_info = reinterpret_cast<MINMAXINFO*>(lparam);
            min_max_info->ptMinTrackSize.x = Scale(kMinimumWindowWidth);
            min_max_info->ptMinTrackSize.y = Scale(kMinimumWindowHeight);
            return 0;
        }
        case WM_COMMAND: {
            const WORD identifier = LOWORD(wparam);
            const WORD notification = HIWORD(wparam);
            // button通知だけを接続操作と排他的な表示mode選択として扱う。
            if (notification == BN_CLICKED) {
                switch (identifier) {
                    case IDC_CONNECT:
                        OnConnect();
                        return 0;
                    case IDC_DISCONNECT:
                        OnDisconnect();
                        return 0;
                    case IDC_FULL_SCREEN:
                        OnFullScreen();
                        return 0;
                    case IDC_BRIDGE_ECHO_TEST:
                        OnBridgeEchoTest();
                        return 0;
                    case IDC_DISPLAY_MODE_NATIVE:
                        OnDisplayModeChanged(
                            rdp::RdpDisplayMode::NativeScroll);
                        return 0;
                    case IDC_DISPLAY_MODE_SMART_SIZING:
                        OnDisplayModeChanged(
                            rdp::RdpDisplayMode::SmartSizing);
                        return 0;
                    case IDC_DISPLAY_MODE_DYNAMIC_RESOLUTION:
                        OnDisplayModeChanged(
                            rdp::RdpDisplayMode::DynamicResolution);
                        return 0;
                    default:
                        break;
                }
            }

            break;
        }
        case WM_SETFOCUS:
            // Microsoft認証dialogのmodal focusをZRdp childへ移し直さない。
            if (authentication_dialog_active_) {
                return 0;
            }

            // 接続中はキー入力をRDPへ、未接続時は接続先入力へ渡す。
            if (IsConnectionActive(rdp_host_.state())) {
                rdp_host_.Focus();
            } else if (server_combo_ != nullptr) {
                SetFocus(server_combo_);
            }
            return 0;
        case WM_TIMER:
            // 取消後にqueueへ残った同IDのmessageでは新しいRDP接続を開始しない。
            if (wparam == kPreLoginConnectionRetryTimerId) {
                if (!pre_login_connection_retry_timer_active_) {
                    return 0;
                }

                CancelPreLoginConnectionRetryTimer();
                // 終了中または別操作が接続状態を変えた場合はstale retryを破棄する。
                if (closing_ ||
                    rdp_host_.state() !=
                        rdp::RdpConnectionState::Disconnected) {
                    return 0;
                }

                common::WriteDiagnosticLog(
                    common::DiagnosticLevel::Information,
                    common::DiagnosticEvent::
                        PreLoginConnectionRetryStarted,
                    {
                        common::DiagnosticValue(
                            common::DiagnosticFieldName::AttemptCount,
                            pre_login_connection_retries_scheduled_),
                        common::DiagnosticValue(
                            common::DiagnosticFieldName::Port,
                            launch_settings_.proxy_port),
                    });
                BeginConnectionAttempt();
                return 0;
            }

            // Agent未起動でもRDPを維持し、HELLOだけを有限頻度で再送する。
            if (wparam == kBridgeHandshakeTimerId) {
                const bridge::ClientBridgeState before =
                    client_bridge_.snapshot().state;
                static_cast<void>(
                    client_bridge_.QueueHelloRetry());
                const bridge::ClientBridgeSnapshot after =
                    client_bridge_.snapshot();
                LogBridgeTransition(before, after);
                UpdateBridgeHandshakeTimer();
                UpdateBridgeUi();
                QueueBridgeSendMessage();
                return 0;
            }

            // 一件のPINGが期限内に応答されない場合もRDP画面を切断しない。
            if (wparam == kBridgeEchoTimerId) {
                const bridge::ClientBridgeState before =
                    client_bridge_.snapshot().state;
                const std::uint64_t now =
                    GetTickCount64();
                // 以前のtimer messageが新しいPING期限より先に届いた場合は無視する。
                if (before ==
                        bridge::ClientBridgeState::EchoRunning &&
                    bridge_echo_deadline_tick_ > now) {
                    const std::uint64_t remaining =
                        bridge_echo_deadline_tick_ - now;
                    const UINT delay =
                        static_cast<UINT>(
                            std::min<std::uint64_t>(
                                remaining,
                                std::numeric_limits<UINT>::max()));
                    // 再設定不能時は無期限待機へfallbackせずBridgeだけを停止する。
                    const bool timer_rearmed =
                        SetTimer(
                            window_,
                            kBridgeEchoTimerId,
                            delay,
                            nullptr) != 0U;
                    if (!timer_rearmed) {
                        client_bridge_.OnEchoTimeout();
                    }

                    const bridge::ClientBridgeSnapshot after =
                        client_bridge_.snapshot();
                    LogBridgeTransition(before, after);
                    // 再設定失敗時だけ停止済みtimer stateを明示的に破棄する。
                    if (!timer_rearmed) {
                        UpdateBridgeEchoTimer();
                        UpdateBridgeHealthTimer();
                    }
                    UpdateBridgeUi();
                    return 0;
                }

                bridge_echo_deadline_tick_ = 0U;
                client_bridge_.OnEchoTimeout();
                const bridge::ClientBridgeSnapshot after =
                    client_bridge_.snapshot();
                LogBridgeTransition(before, after);
                UpdateBridgeHandshakeTimer();
                UpdateBridgeEchoTimer();
                UpdateBridgeHealthTimer();
                UpdateBridgeUi();
                return 0;
            }

            // 成立済みBridgeだけを周期PINGで確認し、Agent異常終了後は再HELLOする。
            if (wparam == kBridgeHealthTimerId) {
                const bridge::ClientBridgeSnapshot before =
                    client_bridge_.snapshot();
                const bool health_state =
                    before.state == bridge::ClientBridgeState::Ready ||
                    before.state ==
                        bridge::ClientBridgeState::EchoPassed;
                // 取消済みtimer messageを停止済みまたはEcho中sessionへ適用しない。
                if (!health_state) {
                    UpdateBridgeHealthTimer();
                    return 0;
                }

                const std::uint64_t now = GetTickCount64();
                if (!before.health_check_outstanding) {
                    if (client_bridge_.QueueHealthCheck()) {
                        bridge_health_sent_tick_ = now;
                        QueueBridgeSendMessage();
                    }
                } else if (bridge_health_sent_tick_ == 0U ||
                    now - bridge_health_sent_tick_ >=
                        kBridgeHealthTimeoutMilliseconds) {
                    common::WriteDiagnosticLog(
                        common::DiagnosticLevel::Warning,
                        common::DiagnosticEvent::BridgePeerTimeout);
                    static_cast<void>(
                        client_bridge_.OnHealthCheckTimeout());
                    bridge_health_sent_tick_ = 0U;
                }

                const bridge::ClientBridgeSnapshot after =
                    client_bridge_.snapshot();
                static_cast<void>(
                    SynchronizeProxyBridgeSession(
                        before.state,
                        after.state));
                LogBridgeTransition(before.state, after);
                // timeout後のHELLO再送と通常health周期を新stateへ同期する。
                UpdateBridgeHandshakeTimer();
                UpdateBridgeEchoTimer();
                UpdateBridgeHealthTimer();
                UpdateBridgeUi();
                QueueBridgeSendMessage();
                return 0;
            }

            // 取消済みtimerの残存messageではremote desktopを変更しない。
            if (wparam == kDynamicResolutionTimerId &&
                dynamic_resolution_timer_active_) {
                CancelDynamicResolutionTimer();
                const HRESULT result =
                    ApplyDynamicResolutionUpdate();
                // LoginComplete直後の一時失敗だけを有限回待ち、恒久失敗は縮退する。
                if (FAILED(result) &&
                    !ScheduleDynamicResolutionRetry(result)) {
                    HandleDynamicResolutionFailure(result);
                } else if (SUCCEEDED(result)) {
                    dynamic_resolution_retries_scheduled_ = 0U;
                }
                return 0;
            }
            break;
        case WM_CLOSE:
            OnClose();
            return 0;
        case WM_DESTROY:
            CancelPreLoginConnectionRetry();
            CancelDynamicResolutionUpdate();
            StopBridgeSession();
            client_proxy_worker_.Stop();
            rdp_host_.Destroy();
            return 0;
        case kMessageCloseReady:
            DestroyWindow(window_);
            return 0;
        case kMessageSetFullScreen: {
            const bool full_screen = wparam != FALSE;
            const HRESULT result = rdp_host_.SetFullScreen(full_screen);
            // ActiveXの状態変更に失敗した場合はUIだけを先行更新しない。
            if (FAILED(result)) {
                ShowError(IDS_CONNECT_FAILED, result);
            } else {
                // button以外の全画面要求も、現在状態に対応する同じ寸法更新へ収束させる。
                ScheduleDynamicResolutionUpdate();
            }
            return 0;
        }
        case kMessageShowDisconnectError: {
            std::wstring dialog_message =
                std::move(pending_disconnect_error_);
            pending_disconnect_error_.clear();
            // 正常終了中または表示内容なしでは、不要なモーダルダイアログを出さない。
            if (!dialog_message.empty() && !closing_) {
                const std::wstring title =
                    LoadStringResource(IDS_RDP_ERROR_TITLE);
                MessageBoxW(
                    window_,
                    dialog_message.c_str(),
                    title.c_str(),
                    MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case kMessageAutoConnect:
            // managerで検証済みの一件だけを独立sessionとして自動開始する。
            if (!closing_ &&
                !IsConnectionActive(
                    rdp_host_.state())) {
                OnConnect();
            }
            return 0;
        case kMessageBridgeSend:
            DrainBridgeSendQueue();
            return 0;
        case kMessageProxyWorker:
            OnProxyWorkerNotification();
            return 0;
        default:
            break;
    }

    return DefWindowProcW(window_, message, wparam, lparam);
}

bool MainWindow::RegisterWindowClass() const noexcept {
    const auto large_icon = static_cast<HICON>(
        LoadImageW(
            instance_,
            MAKEINTRESOURCEW(IDI_ZRDP_APP),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR | LR_SHARED));
    const auto small_icon = static_cast<HICON>(
        LoadImageW(
            instance_,
            MAKEINTRESOURCEW(IDI_ZRDP_APP),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR | LR_SHARED));
    // Branding resource欠落時にWindows既定iconへ黙って縮退しない。
    if (large_icon == nullptr ||
        small_icon == nullptr) {
        return false;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    window_class.lpfnWndProc = &MainWindow::WindowProcedure;
    window_class.hInstance = instance_;
    window_class.hIcon = large_icon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground =
        reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
    window_class.lpszClassName = kMainWindowClassName;
    window_class.hIconSm = small_icon;

    // 新規登録成功、または同一プロセスで既登録の場合だけ生成可能とする。
    if (RegisterClassExW(&window_class) != 0U) {
        return true;
    }

    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// ---------------------------------------------------------------------------
// 子コントロールの生成とローカル表示領域の配置
// ---------------------------------------------------------------------------

bool MainWindow::CreateControls() {
    // 接続入力はmanagerだけに表示し、session側では表示modeと全画面切替だけを公開する。
    const auto create_control = [this](
                                    const wchar_t* class_name,
                                    const std::wstring& text,
                                    const DWORD style,
                                    const int identifier) {
        return CreateWindowExW(
            0U,
            class_name,
            text.c_str(),
            WS_CHILD | style,
            0,
            0,
            0,
            0,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
            instance_,
            nullptr);
    };

    server_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_SERVER_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    server_combo_ = create_control(
        L"COMBOBOX",
        L"",
        WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
        IDC_SERVER);
    domain_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_DOMAIN_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    domain_combo_ = create_control(
        L"COMBOBOX",
        L"",
        WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
        IDC_DOMAIN);
    user_name_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_USER_NAME_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    user_name_combo_ = create_control(
        L"COMBOBOX",
        L"",
        WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
        IDC_USER_NAME);
    connect_button_ = create_control(
        L"BUTTON",
        LoadStringResource(IDS_CONNECT),
        WS_TABSTOP | BS_DEFPUSHBUTTON,
        IDC_CONNECT);
    disconnect_button_ = create_control(
        L"BUTTON",
        LoadStringResource(IDS_DISCONNECT),
        WS_TABSTOP | BS_PUSHBUTTON,
        IDC_DISCONNECT);
    display_mode_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_DISPLAY_MODE_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    native_mode_radio_ = create_control(
        L"BUTTON",
        LoadStringResource(IDS_DISPLAY_MODE_NATIVE),
        WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON,
        IDC_DISPLAY_MODE_NATIVE);
    smart_sizing_mode_radio_ = create_control(
        L"BUTTON",
        LoadStringResource(IDS_DISPLAY_MODE_SMART_SIZING),
        WS_TABSTOP | BS_AUTORADIOBUTTON,
        IDC_DISPLAY_MODE_SMART_SIZING);
    dynamic_resolution_mode_radio_ = create_control(
        L"BUTTON",
        LoadStringResource(IDS_DISPLAY_MODE_DYNAMIC_RESOLUTION),
        WS_TABSTOP | BS_AUTORADIOBUTTON,
        IDC_DISPLAY_MODE_DYNAMIC_RESOLUTION);
    full_screen_button_ = create_control(
        L"BUTTON",
        LoadStringResource(IDS_FULL_SCREEN),
        WS_GROUP | WS_TABSTOP | BS_PUSHBUTTON,
        IDC_FULL_SCREEN);
    bridge_label_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_BRIDGE_LABEL),
        SS_LEFT | SS_CENTERIMAGE,
        -1);
    bridge_state_text_ = create_control(
        L"STATIC",
        LoadStringResource(IDS_BRIDGE_STOPPED),
        SS_LEFT | SS_CENTERIMAGE,
        IDC_BRIDGE_STATE);
    bridge_echo_button_ = create_control(
        L"BUTTON",
        LoadStringResource(IDS_BRIDGE_ECHO_TEST),
        WS_GROUP | WS_TABSTOP | BS_PUSHBUTTON,
        IDC_BRIDGE_ECHO_TEST);
    status_text_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"STATIC",
        L"",
        WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
        0,
        0,
        0,
        0,
        window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)),
        instance_,
        nullptr);

    const std::array<HWND, 17> controls{
        server_label_,
        server_combo_,
        domain_label_,
        domain_combo_,
        user_name_label_,
        user_name_combo_,
        connect_button_,
        disconnect_button_,
        display_mode_label_,
        native_mode_radio_,
        smart_sizing_mode_radio_,
        dynamic_resolution_mode_radio_,
        full_screen_button_,
        bridge_label_,
        bridge_state_text_,
        bridge_echo_button_,
        status_text_,
    };

    // 接続意図に必要な欄が欠けたまま操作させないため、全コントロールを必須とする。
    for (const HWND control : controls) {
        // 一つでも生成失敗していればトップレベルウィンドウの生成を中止する。
        if (control == nullptr) {
            return false;
        }
    }

    SendMessageW(server_combo_, CB_LIMITTEXT, 255U, 0L);
    SendMessageW(
        domain_combo_,
        CB_LIMITTEXT,
        kMaximumRdpDomainCharacters,
        0L);
    SendMessageW(
        user_name_combo_,
        CB_LIMITTEXT,
        kMaximumRdpUserNameCharacters,
        0L);
    SetWindowTextW(
        server_combo_,
        launch_settings_.connection.server.c_str());
    SetWindowTextW(
        domain_combo_,
        launch_settings_.connection.domain.c_str());
    SetWindowTextW(
        user_name_combo_,
        launch_settings_.connection.user_name.c_str());
    // managerで選択した排他的modeをsession生成前から一貫して表示する。
    SetDisplayModeSelection(display_mode_);

    ApplyControlFont();
    const std::array<HWND, 5> visible_controls{
        display_mode_label_,
        native_mode_radio_,
        smart_sizing_mode_radio_,
        dynamic_resolution_mode_radio_,
        full_screen_button_,
    };
    // session中に必要な表示modeと全画面切替だけを一行の操作領域へ公開する。
    for (const HWND control : visible_controls) {
        ShowWindow(control, SW_SHOW);
    }

    UpdateUiForState(rdp::RdpConnectionState::Disconnected);
    UpdateBridgeUi();
    SetStatus(IDS_STATUS_DISCONNECTED);
    return true;
}

void MainWindow::LayoutControls(
    const int client_width,
    const int client_height) {
    const int margin = Scale(kDisplayModeToolbarMargin);
    const int gap = Scale(kDisplayModeToolbarGap);
    const int control_height =
        Scale(kDisplayModeControlHeight);
    const int display_mode_label_width = Scale(82);
    const int native_mode_width = Scale(180);
    const int smart_sizing_mode_width = Scale(220);
    const int dynamic_resolution_mode_width = Scale(240);
    const int full_screen_button_width = Scale(100);
    int x = margin;
    MoveWindow(
        display_mode_label_,
        x,
        margin,
        display_mode_label_width,
        control_height,
        TRUE);
    x += display_mode_label_width + gap;
    MoveWindow(
        native_mode_radio_,
        x,
        margin,
        native_mode_width,
        control_height,
        TRUE);
    x += native_mode_width + gap;
    MoveWindow(
        smart_sizing_mode_radio_,
        x,
        margin,
        smart_sizing_mode_width,
        control_height,
        TRUE);
    x += smart_sizing_mode_width + gap;
    MoveWindow(
        dynamic_resolution_mode_radio_,
        x,
        margin,
        dynamic_resolution_mode_width,
        control_height,
        TRUE);
    x += dynamic_resolution_mode_width + gap;
    MoveWindow(
        full_screen_button_,
        x,
        margin,
        full_screen_button_width,
        control_height,
        TRUE);

    const RECT rdp_bounds = RdpBounds(client_width, client_height);
    // 表示操作一行以外をRDPへ割り当て、接続情報と状態はtitle barへ集約する。
    if (IsWindow(rdp_host_.window()) &&
        rdp_bounds.right > rdp_bounds.left &&
        rdp_bounds.bottom > rdp_bounds.top) {
        const HRESULT result = rdp_host_.Resize(rdp_bounds);
        // host resize失敗時は異なる表示領域をremote sessionへ適用しない。
        if (SUCCEEDED(result)) {
            ScheduleDynamicResolutionUpdate();
        }
    }
}

bool MainWindow::ConstrainManualSizing(
    const UINT sizing_edge,
    RECT& bounds) const noexcept {
    // 接続比率の確定前、ActiveX全画面中、最大化中はWindowsの配置を維持する。
    if (window_ == nullptr ||
        sizing_desktop_width_ <= 0L ||
        sizing_desktop_height_ <= 0L ||
        rdp_host_.full_screen() ||
        IsZoomed(window_)) {
        return false;
    }

    RECT window_bounds{};
    RECT client_bounds{};
    // 同じ時点のouter／client矩形を取得できない場合は固定領域を推測しない。
    if (!GetWindowRect(window_, &window_bounds) ||
        !GetClientRect(window_, &client_bounds)) {
        return false;
    }

    const std::int64_t window_width =
        static_cast<std::int64_t>(window_bounds.right) -
        static_cast<std::int64_t>(window_bounds.left);
    const std::int64_t window_height =
        static_cast<std::int64_t>(window_bounds.bottom) -
        static_cast<std::int64_t>(window_bounds.top);
    const std::int64_t client_width =
        static_cast<std::int64_t>(client_bounds.right) -
        static_cast<std::int64_t>(client_bounds.left);
    const std::int64_t client_height =
        static_cast<std::int64_t>(client_bounds.bottom) -
        static_cast<std::int64_t>(client_bounds.top);
    const std::int64_t non_rdp_width =
        window_width - client_width;
    const std::int64_t non_rdp_height =
        window_height - client_height +
        Scale(kDisplayModeToolbarHeight);
    // 非client寸法の異常値をintへ切り詰めてWindow候補へ適用しない。
    if (non_rdp_width < 0 ||
        non_rdp_height < 0 ||
        window_width <= 0 ||
        window_height <= 0 ||
        non_rdp_width >
            std::numeric_limits<int>::max() ||
        non_rdp_height >
            std::numeric_limits<int>::max() ||
        window_width >
            std::numeric_limits<int>::max() ||
        window_height >
            std::numeric_limits<int>::max()) {
        return false;
    }

    const RdpWindowSizingConstraints constraints{
        .desktop_width = sizing_desktop_width_,
        .desktop_height = sizing_desktop_height_,
        .non_rdp_width =
            static_cast<int>(non_rdp_width),
        .non_rdp_height =
            static_cast<int>(non_rdp_height),
        .minimum_window_width =
            Scale(kMinimumWindowWidth),
        .minimum_window_height =
            Scale(kMinimumWindowHeight),
        .reference_window_width =
            manual_sizing_reference_width_ > 0
                ? manual_sizing_reference_width_
                : static_cast<int>(window_width),
        .reference_window_height =
            manual_sizing_reference_height_ > 0
                ? manual_sizing_reference_height_
                : static_cast<int>(window_height),
    };
    return ConstrainRdpWindowSizingRect(
        sizing_edge,
        constraints,
        bounds);
}

void MainWindow::ApplyControlFont() const noexcept {
    const HFONT font = static_cast<HFONT>(
        GetStockObject(DEFAULT_GUI_FONT));
    const std::array<HWND, 17> controls{
        server_label_,
        server_combo_,
        domain_label_,
        domain_combo_,
        user_name_label_,
        user_name_combo_,
        connect_button_,
        disconnect_button_,
        display_mode_label_,
        native_mode_radio_,
        smart_sizing_mode_radio_,
        dynamic_resolution_mode_radio_,
        full_screen_button_,
        bridge_label_,
        bridge_state_text_,
        bridge_echo_button_,
        status_text_,
    };

    // システム所有の標準GUIフォントを、所有権を移さず全子ウィンドウで共有する。
    for (const HWND control : controls) {
        SendMessageW(
            control,
            WM_SETFONT,
            reinterpret_cast<WPARAM>(font),
            TRUE);
    }
}

// ---------------------------------------------------------------------------
// 利用者コマンド
// ---------------------------------------------------------------------------

void MainWindow::OnConnect() {
    // 新しい利用者接続意図では、以前の有限retry回数を引き継がない。
    CancelPreLoginConnectionRetry();
    common::WriteDiagnosticLog(
        common::DiagnosticLevel::Information,
        common::DiagnosticEvent::ConnectButtonClicked);
    BeginConnectionAttempt();
}

void MainWindow::BeginConnectionAttempt() {
    pending_disconnect_error_.clear();
    login_completed_ = false;
    authentication_dialog_active_ = false;
    authentication_dialog_was_displayed_ = false;
    authentication_dialog_display_count_ = 0U;
    sizing_desktop_width_ = 0L;
    sizing_desktop_height_ = 0L;

    std::wstring server = TrimCopy(ReadWindowText(server_combo_));
    // 空の接続先ではWindows標準認証UIを開始せず、利用者へ入力を要求する。
    if (server.empty()) {
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Warning,
            common::DiagnosticEvent::ConnectInputRejected,
            {
                common::DiagnosticValue(
                    common::DiagnosticFieldName::Reason,
                    1U),
            });
        const std::wstring message =
            LoadStringResource(IDS_SERVER_REQUIRED);
        MessageBoxW(
            window_,
            message.c_str(),
            L"ZRdp SVN Bridge",
            MB_OK | MB_ICONWARNING);
        SetFocus(server_combo_);
        return;
    }

    RdpUserIdentity identity;
    HRESULT result = NormalizeRdpUserIdentity(
        TrimCopy(ReadWindowText(domain_combo_)),
        TrimCopy(ReadWindowText(user_name_combo_)),
        identity);
    // 曖昧なdomain指定をMicrosoft credential UIへ渡して別accountへ接続しない。
    if (FAILED(result)) {
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Warning,
            common::DiagnosticEvent::ConnectInputRejected,
            {
                common::DiagnosticValue(
                    common::DiagnosticFieldName::Reason,
                    3U),
                common::DiagnosticHResult(result),
            });
        const std::wstring message =
            LoadStringResource(IDS_RDP_IDENTITY_INVALID);
        MessageBoxW(
            window_,
            message.c_str(),
            L"ZRdp SVN Bridge",
            MB_OK | MB_ICONWARNING);
        SetFocus(user_name_combo_);
        return;
    }

    // 旧DOMAIN\user入力を分離した場合は、認証前に利用者へ正規形を見せる。
    SetWindowTextW(domain_combo_, identity.domain.c_str());
    SetWindowTextW(user_name_combo_, identity.user_name.c_str());
    // managerで確定した接続条件と現在状態を認証開始前のtitleへ反映する。
    UpdateWindowTitle();

    rdp::RdpConnectionSettings settings{};
    settings.server = std::move(server);
    settings.domain = std::move(identity.domain);
    settings.user_name = std::move(identity.user_name);
    settings.display_mode = display_mode_;
    settings.audio = launch_settings_.connection.audio;
    result = display_mode_ ==
            rdp::RdpDisplayMode::DynamicResolution
        ? ResolveRdpDisplayAreaSize(
              rdp_host_.window(),
              settings.desktop_width,
              settings.desktop_height)
        : ResolveInitialDesktopSize(
              window_,
              settings.desktop_width,
              settings.desktop_height);
    // modeに対応する初期寸法が不明なままActiveX既定値へfallbackしない。
    if (FAILED(result)) {
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Error,
            common::DiagnosticEvent::ConnectInputRejected,
            {
                common::DiagnosticValue(
                    common::DiagnosticFieldName::Reason,
                    2U),
                common::DiagnosticHResult(result),
            });
        ShowError(IDS_CONNECT_FAILED, result);
        return;
    }

    common::WriteDiagnosticLog(
        common::DiagnosticLevel::Information,
        common::DiagnosticEvent::ConnectionSettingsApplyStarted,
        {
            common::DiagnosticValue(
                common::DiagnosticFieldName::DisplayMode,
                static_cast<std::uint64_t>(display_mode_)),
        });
    result = rdp_host_.ApplyConnectionSettings(settings);
    common::WriteDiagnosticLog(
        FAILED(result)
            ? common::DiagnosticLevel::Error
            : common::DiagnosticLevel::Information,
        common::DiagnosticEvent::ConnectionSettingsApplyReturned,
        {common::DiagnosticHResult(result)});
    // 接続前設定が一部でも失敗した状態では、ActiveXの接続を開始しない。
    if (FAILED(result)) {
        ShowError(IDS_CONNECT_FAILED, result);
        return;
    }

    SetStatus(IDS_STATUS_CONNECT_REQUESTED);
    UpdateUiForState(rdp::RdpConnectionState::Connecting);
    UpdateWindow(status_text_);
    UpdateWindow(connect_button_);

    common::WriteDiagnosticLog(
        common::DiagnosticLevel::Information,
        common::DiagnosticEvent::ConnectCallStarted);
    result = rdp_host_.Connect();
    common::WriteDiagnosticLog(
        FAILED(result)
            ? common::DiagnosticLevel::Error
            : common::DiagnosticLevel::Information,
        common::DiagnosticEvent::ConnectCallReturned,
        {common::DiagnosticHResult(result)});
    // 同期的なConnect呼出し失敗時は、UIを未接続状態へ戻す。
    if (FAILED(result)) {
        SetStatus(IDS_STATUS_DISCONNECTED);
        UpdateUiForState(rdp::RdpConnectionState::Disconnected);
        ShowError(IDS_CONNECT_FAILED, result);
        return;
    }

    // Connectを受理したsessionだけが、接続時desktop比率を手動変更へ適用する。
    sizing_desktop_width_ = settings.desktop_width;
    sizing_desktop_height_ = settings.desktop_height;
    SetStatus(IDS_STATUS_CONNECTING);
    UpdateUiForState(rdp::RdpConnectionState::Connecting);
    rdp_host_.Focus();
}

bool MainWindow::SchedulePreLoginConnectionRetry(
    const rdp::RdpEvent& event,
    const std::uint32_t delay_milliseconds) {
    CancelPreLoginConnectionRetryTimer();
    // 有効Windowと有限delayがない場合はcallback後の再試行を予約しない。
    if (window_ == nullptr ||
        delay_milliseconds == 0U) {
        return false;
    }

    const UINT_PTR timer = SetTimer(
        window_,
        kPreLoginConnectionRetryTimerId,
        static_cast<UINT>(delay_milliseconds),
        nullptr);
    // USER32 timerを確保できない場合は無期限待機へ移行せず既存errorを表示する。
    if (timer == 0U) {
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Error,
            common::DiagnosticEvent::
                PreLoginConnectionRetryExhausted,
            {
                common::DiagnosticValue(
                    common::DiagnosticFieldName::AttemptCount,
                    pre_login_connection_retries_scheduled_),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::
                        MaximumAttemptCount,
                    kMaximumPreLoginConnectionRetries),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::Reason,
                    static_cast<std::uint32_t>(event.code)),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::ExtendedReason,
                    static_cast<std::uint32_t>(
                        event.extended_disconnect_reason)),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::Port,
                    launch_settings_.proxy_port),
            });
        return false;
    }

    pre_login_connection_retry_timer_active_ = true;
    ++pre_login_connection_retries_scheduled_;
    pending_disconnect_error_.clear();
    common::WriteDiagnosticLog(
        common::DiagnosticLevel::Warning,
        common::DiagnosticEvent::
            PreLoginConnectionRetryScheduled,
        {
            common::DiagnosticValue(
                common::DiagnosticFieldName::AttemptCount,
                pre_login_connection_retries_scheduled_),
            common::DiagnosticValue(
                common::DiagnosticFieldName::
                    MaximumAttemptCount,
                kMaximumPreLoginConnectionRetries),
            common::DiagnosticValue(
                common::DiagnosticFieldName::Reason,
                static_cast<std::uint32_t>(event.code)),
            common::DiagnosticValue(
                common::DiagnosticFieldName::ExtendedReason,
                static_cast<std::uint32_t>(
                    event.extended_disconnect_reason)),
            common::DiagnosticValue(
                common::DiagnosticFieldName::Port,
                launch_settings_.proxy_port),
        });
    SetStatus(IDS_STATUS_PRE_LOGIN_RETRY);
    UpdateUiForState(rdp::RdpConnectionState::Connecting);
    UpdateFullScreenButton(false);
    return true;
}

void MainWindow::CancelPreLoginConnectionRetryTimer() noexcept {
    // 予約済みtimerだけを解除し、同じIDの将来retryへ影響させない。
    if (pre_login_connection_retry_timer_active_ &&
        window_ != nullptr) {
        static_cast<void>(
            KillTimer(
                window_,
                kPreLoginConnectionRetryTimerId));
    }

    pre_login_connection_retry_timer_active_ = false;
}

void MainWindow::CancelPreLoginConnectionRetry() noexcept {
    CancelPreLoginConnectionRetryTimer();
    pre_login_connection_retries_scheduled_ = 0U;
}

void MainWindow::OnDisconnect() {
    common::WriteDiagnosticLog(
        common::DiagnosticLevel::Information,
        common::DiagnosticEvent::DisconnectButtonClicked);
    const bool retry_waiting =
        pre_login_connection_retry_timer_active_;
    CancelPreLoginConnectionRetry();
    // retry待機中はActiveXが既切断なので、timer取消だけで未接続へ戻す。
    if (retry_waiting &&
        rdp_host_.state() ==
            rdp::RdpConnectionState::Disconnected) {
        SetStatus(IDS_STATUS_DISCONNECTED);
        UpdateUiForState(rdp::RdpConnectionState::Disconnected);
        return;
    }

    const HRESULT result = rdp_host_.Disconnect();
    common::WriteDiagnosticLog(
        FAILED(result)
            ? common::DiagnosticLevel::Error
            : common::DiagnosticLevel::Information,
        common::DiagnosticEvent::DisconnectCallReturned,
        {common::DiagnosticHResult(result)});
    // 切断要求を受理できなかった場合は接続状態を維持して原因を表示する。
    if (FAILED(result)) {
        ShowError(IDS_DISCONNECT_FAILED, result);
        return;
    }

    CancelDynamicResolutionUpdate();
    SetStatus(IDS_STATUS_DISCONNECTING);
    UpdateUiForState(rdp::RdpConnectionState::Disconnecting);
}

void MainWindow::OnFullScreen() {
    const bool requested_full_screen = !rdp_host_.full_screen();
    const HRESULT result =
        rdp_host_.SetFullScreen(requested_full_screen);
    common::WriteDiagnosticLog(
        FAILED(result)
            ? common::DiagnosticLevel::Error
            : common::DiagnosticLevel::Information,
        common::DiagnosticEvent::FullScreenCallReturned,
        {common::DiagnosticHResult(result)});
    // ActiveXが拒否した場合はボタン表示を切り替えない。
    if (FAILED(result)) {
        ShowError(IDS_CONNECT_FAILED, result);
        return;
    }

    // event通知が遅れるControlでも、受理済みの現在状態に対応する更新を予約する。
    ScheduleDynamicResolutionUpdate();

    UpdateFullScreenButton(requested_full_screen);
}

void MainWindow::OnBridgeEchoTest() {
    const bridge::ClientBridgeState before =
        client_bridge_.snapshot().state;
    // handshake未完了またはtest中の重複操作はstateを変更しない。
    if (!client_bridge_.BeginEchoTest()) {
        UpdateBridgeUi();
        return;
    }

    // Echo用HELLOがAgent streamをresetする前にlocal Proxy connectionを停止する。
    client_proxy_worker_.EnqueueBridgeStop();
    common::WriteDiagnosticLog(
        common::DiagnosticLevel::Information,
        common::DiagnosticEvent::BridgeEchoStarted);
    UpdateBridgeHandshakeTimer();
    UpdateBridgeEchoTimer();
    UpdateBridgeHealthTimer();
    const bridge::ClientBridgeSnapshot after =
        client_bridge_.snapshot();
    LogBridgeTransition(before, after);
    UpdateBridgeUi();
    QueueBridgeSendMessage();
}

void MainWindow::OnDisplayModeChanged(
    const rdp::RdpDisplayMode requested_mode) {
    const rdp::RdpDisplayMode previous_mode = display_mode_;
    // 自動radio通知の重複ではActiveX propertyを再設定しない。
    if (requested_mode == previous_mode) {
        SetDisplayModeSelection(previous_mode);
        return;
    }

    const rdp::RdpConnectionState state = rdp_host_.state();
    // 接続遷移中の通知は無効化済みUIから届いても直前のmodeへ戻す。
    if (IsConnectionActive(state) && !CanChangeDisplayMode(state)) {
        SetDisplayModeSelection(previous_mode);
        return;
    }

    CancelDynamicResolutionUpdate();
    // 未接続時は次回Connectへ適用するmodelだけを変更する。
    if (!CanChangeDisplayMode(state)) {
        display_mode_ = requested_mode;
        SetDisplayModeSelection(display_mode_);
        UpdateUiForState(state);
        return;
    }

    const bool smart_sizing_enabled =
        requested_mode == rdp::RdpDisplayMode::SmartSizing;
    HRESULT result =
        rdp_host_.SetSmartSizing(smart_sizing_enabled);
    common::WriteDiagnosticLog(
        FAILED(result)
            ? common::DiagnosticLevel::Warning
            : common::DiagnosticLevel::Information,
        common::DiagnosticEvent::SmartSizingCallReturned,
        {
            common::DiagnosticHResult(result),
            common::DiagnosticValue(
                common::DiagnosticFieldName::Enabled,
                smart_sizing_enabled ? 1U : 0U),
        });
    // Smart Sizing境界を適用できなければ、radio選択だけを先行させない。
    if (FAILED(result)) {
        SetDisplayModeSelection(previous_mode);
        // 取消した動的modeの追従はrollback後に再開する。
        if (previous_mode == rdp::RdpDisplayMode::DynamicResolution) {
            ScheduleDynamicResolutionUpdate();
        }

        ShowError(IDS_DISPLAY_SETTING_FAILED, result);
        return;
    }

    display_mode_ = requested_mode;
    SetDisplayModeSelection(display_mode_);
    // 動的解像度へ入る時は、現在の表示領域を直ちに一度だけ適用する。
    if (requested_mode == rdp::RdpDisplayMode::DynamicResolution) {
        result = ApplyDynamicResolutionUpdate();
        // 利用者操作直後の失敗は、暗黙の別modeではなく直前のmodeへ戻す。
        if (FAILED(result)) {
            display_mode_ = previous_mode;
            SetDisplayModeSelection(display_mode_);

            const bool restore_smart_sizing =
                previous_mode == rdp::RdpDisplayMode::SmartSizing;
            const HRESULT restore_result =
                rdp_host_.SetSmartSizing(restore_smart_sizing);
            common::WriteDiagnosticLog(
                FAILED(restore_result)
                    ? common::DiagnosticLevel::Warning
                    : common::DiagnosticLevel::Information,
                common::DiagnosticEvent::SmartSizingCallReturned,
                {
                    common::DiagnosticHResult(restore_result),
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::Enabled,
                        restore_smart_sizing ? 1U : 0U),
                });
            // Smart Sizingへのrollbackも失敗した場合は、実状態に近い等倍表示へ縮退する。
            if (FAILED(restore_result)) {
                display_mode_ = rdp::RdpDisplayMode::NativeScroll;
                SetDisplayModeSelection(display_mode_);
            }

            ShowError(IDS_DISPLAY_SETTING_FAILED, result);
        }
    }

    UpdateUiForState(rdp_host_.state());
}

void MainWindow::OnClose() {
    common::WriteDiagnosticLog(
        common::DiagnosticLevel::Information,
        common::DiagnosticEvent::CloseRequested,
        {
            common::DiagnosticValue(
                common::DiagnosticFieldName::Port,
                launch_settings_.proxy_port),
        });
    // 既に切断完了待ちなら、重複した終了要求を処理しない。
    if (closing_) {
        return;
    }

    const rdp::RdpConnectionState connection_state =
        rdp_host_.state();
    const SessionCloseStartAction close_action =
        SelectSessionCloseStartAction(
            IsConnectionActive(connection_state),
            connection_state ==
                rdp::RdpConnectionState::Disconnecting);
    // 未接続時はActiveXの非同期切断を待つ必要がない。
    if (close_action ==
        SessionCloseStartAction::DestroyImmediately) {
        DestroyWindow(window_);
        return;
    }

    const std::wstring confirmation =
        LoadStringResource(IDS_CLOSE_CONFIRM);
    const std::wstring confirmation_title =
        LoadStringResource(IDS_CLOSE_CONFIRM_TITLE);
    const int answer = MessageBoxW(
        window_,
        confirmation.c_str(),
        confirmation_title.c_str(),
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    // 明示的な同意がなければ成立中のRDPセッションを維持する。
    if (answer != IDYES) {
        return;
    }

    closing_ = true;
    CancelPreLoginConnectionRetry();
    CancelDynamicResolutionUpdate();

    // 既に切断中なら二重COM呼出しを避け、既存OnDisconnectedだけを待つ。
    if (close_action ==
        SessionCloseStartAction::WaitForDisconnect) {
        return;
    }

    const HRESULT result = rdp_host_.Disconnect();
    common::WriteDiagnosticLog(
        FAILED(result)
            ? common::DiagnosticLevel::Error
            : common::DiagnosticLevel::Information,
        common::DiagnosticEvent::DisconnectCallReturned,
        {common::DiagnosticHResult(result)});
    // 切断要求失敗時は終了待ち状態を解除し、再操作可能にする。
    if (FAILED(result)) {
        closing_ = false;
        ScheduleDynamicResolutionUpdate();
        ShowError(IDS_DISCONNECT_FAILED, result);
        return;
    }

    // 既切断を表すS_FALSEでは新しいOnDisconnectedが来ないため直ちに閉じる。
    if (result == S_FALSE) {
        PostMessageW(window_, kMessageCloseReady, 0U, 0L);
        return;
    }

    SetStatus(IDS_STATUS_DISCONNECTING);
    UpdateUiForState(rdp::RdpConnectionState::Disconnecting);
}

// ---------------------------------------------------------------------------
// 共通接続履歴ownerへの成功通知
// ---------------------------------------------------------------------------

void MainWindow::RememberSuccessfulConnection() {
    // 接続先が取得できない異常イベントでは空履歴を保存しない。
    if (launch_settings_.connection.server.empty()) {
        return;
    }

    // session中もtitleで使用する設定は保持し、一つのapplication-owned workerへcopyする。
    observer_.RememberSuccessfulConnection(
        launch_settings_.connection.connection_name,
        launch_settings_.connection.server,
        launch_settings_.connection.domain,
        launch_settings_.connection.user_name,
        launch_settings_.connection.audio);
}

// ---------------------------------------------------------------------------
// Static Virtual Channel Bridge連携
// ---------------------------------------------------------------------------

void MainWindow::StartBridgeSession() {
    StopBridgeSession();
    const bridge::ClientBridgeState before =
        client_bridge_.snapshot().state;
    static_cast<void>(client_bridge_.StartSession());
    const bridge::ClientBridgeSnapshot after =
        client_bridge_.snapshot();
    LogBridgeTransition(before, after);
    UpdateBridgeHandshakeTimer();
    UpdateBridgeEchoTimer();
    UpdateBridgeHealthTimer();
    UpdateBridgeUi();
    QueueBridgeSendMessage();
}

void MainWindow::StopBridgeSession() noexcept {
    // stale timerが切断後のsessionへHELLOやtimeoutを適用しないよう先に停止する。
    if (window_ != nullptr) {
        static_cast<void>(
            KillTimer(window_, kBridgeHandshakeTimerId));
        static_cast<void>(
            KillTimer(window_, kBridgeEchoTimerId));
        static_cast<void>(
            KillTimer(window_, kBridgeHealthTimerId));
    }
    bridge_echo_deadline_tick_ = 0U;
    bridge_health_sent_tick_ = 0U;

    client_proxy_worker_.EnqueueBridgeStop();
    client_bridge_.StopSession();
    // noexcept終了経路ではresourceをstack bufferへ直接読み込み、allocationを避ける。
    if (bridge_state_text_ != nullptr) {
        std::array<wchar_t, 128> text{};
        LoadStringW(
            instance_,
            IDS_BRIDGE_STOPPED,
            text.data(),
            static_cast<int>(text.size()));
        SetWindowTextW(bridge_state_text_, text.data());
    }

    if (bridge_echo_button_ != nullptr) {
        EnableWindow(bridge_echo_button_, FALSE);
    }
}

void MainWindow::QueueBridgeSendMessage() noexcept {
    // Window破棄後にCOM送信messageを予約しない。
    if (window_ == nullptr ||
        !IsWindow(window_)) {
        return;
    }

    // Post失敗時はqueueを残して無期限retryせずBridgeだけを停止する。
    if (!PostMessageW(
            window_,
            kMessageBridgeSend,
            0U,
            0L)) {
        const bridge::ClientBridgeState before =
            client_bridge_.snapshot().state;
        client_bridge_.OnTransportSendFailed();
        UpdateBridgeHandshakeTimer();
        UpdateBridgeEchoTimer();
        UpdateBridgeHealthTimer();
        const bridge::ClientBridgeSnapshot after =
            client_bridge_.snapshot();
        LogBridgeTransition(before, after);
    }
}

void MainWindow::DrainBridgeSendQueue() {
    bool more_data = false;
    // 一回のmessageで有限chunkだけ送り、RDP描画とUI inputへ制御を戻す。
    for (std::size_t count = 0U;
         count < kMaximumBridgeChunksPerDispatch;
         ++count) {
        std::vector<std::byte> chunk;
        if (!client_bridge_.TakeOutboundChunk(
                chunk,
                more_data)) {
            return;
        }

        const HRESULT result =
            rdp_host_.SendStaticVirtualChannelData(chunk);
        // 一chunkでも失敗したframeは再送せず、Bridge sessionをfail-closedにする。
        if (FAILED(result)) {
            const bridge::ClientBridgeState before =
                client_bridge_.snapshot().state;
            client_bridge_.OnTransportSendFailed();
            const bridge::ClientBridgeSnapshot after =
                client_bridge_.snapshot();
            LogBridgeTransition(before, after);
            UpdateBridgeHandshakeTimer();
            UpdateBridgeEchoTimer();
            UpdateBridgeHealthTimer();
            UpdateBridgeUi();
            return;
        }

        // queue末尾まで送信した場合は追加messageを予約しない。
        if (!more_data) {
            return;
        }
    }

    // 残queueは次messageで処理し、一度の長いCOM連続呼出しを避ける。
    if (more_data) {
        QueueBridgeSendMessage();
    }
}

void MainWindow::OnProxyWorkerNotification() {
    // 処理中の新着Frame／状態変化が次messageを予約できるよう先に解除する。
    client_proxy_worker_.AcknowledgeOwnerNotification();

    bool more_frames = false;
    bool queued_svc_data = false;
    // 一回のUI messageで有限FrameだけをSVC queueへ移す。
    for (std::size_t count = 0U;
         count < kMaximumProxyFramesPerDispatch;
         ++count) {
        protocol::Frame frame{};
        if (!client_proxy_worker_.TakeOutboundFrame(
                frame,
                more_frames)) {
            break;
        }

        const bridge::ClientBridgeState bridge_state =
            client_bridge_.snapshot().state;
        if (!client_bridge_.QueueStreamFrame(frame)) {
            // session停止と交差した旧worker Frameは新HELLO待機をFailedへ戻さない。
            if (bridge_state ==
                    bridge::ClientBridgeState::Ready ||
                bridge_state ==
                    bridge::ClientBridgeState::EchoPassed) {
                client_bridge_.OnStreamRelayFailed();
            }

            client_proxy_worker_.EnqueueBridgeStop();
            more_frames = false;
            break;
        }

        queued_svc_data = true;
    }

    if (queued_svc_data) {
        QueueBridgeSendMessage();
    }

    // 一dispatch上限後の残Frameは別messageへ分け、UI描画をstarvationさせない。
    if (more_frames &&
        window_ != nullptr &&
        IsWindow(window_)) {
        static_cast<void>(PostMessageW(
            window_,
            kMessageProxyWorker,
            0U,
            0L));
    }

    const proxy::ClientProxyWorkerSnapshot proxy_snapshot =
        client_proxy_worker_.snapshot();
    // worker内部relay failureはRDPを維持したままBridge data planeだけ停止する。
    const bridge::ClientBridgeState bridge_state =
        client_bridge_.snapshot().state;
    if ((proxy_snapshot.error ==
             proxy::ClientProxyWorkerError::
                 CommandQueueLimitExceeded ||
         proxy_snapshot.error ==
             proxy::ClientProxyWorkerError::
                 OutboundQueueLimitExceeded ||
         proxy_snapshot.error ==
             proxy::ClientProxyWorkerError::
                 SessionFailed) &&
        (bridge_state ==
             bridge::ClientBridgeState::Ready ||
         bridge_state ==
             bridge::ClientBridgeState::EchoPassed)) {
        const bridge::ClientBridgeState before =
            bridge_state;
        client_bridge_.OnStreamRelayFailed();
        client_proxy_worker_.EnqueueBridgeStop();
        const bridge::ClientBridgeSnapshot after =
            client_bridge_.snapshot();
        LogBridgeTransition(before, after);
        UpdateBridgeHandshakeTimer();
        UpdateBridgeEchoTimer();
        UpdateBridgeHealthTimer();
    }

    // listener／Bridge／stream counterの変化を専用状態欄へ反映する。
    if (last_proxy_snapshot_.state !=
            proxy_snapshot.state ||
        last_proxy_snapshot_.error !=
            proxy_snapshot.error ||
        last_proxy_snapshot_.configuration_error !=
            proxy_snapshot.configuration_error ||
        last_proxy_snapshot_.bridge_ready !=
            proxy_snapshot.bridge_ready) {
        const bool failed =
            proxy_snapshot.state ==
                proxy::ClientProxyWorkerState::Failed ||
            proxy_snapshot.error !=
                proxy::ClientProxyWorkerError::None;
        common::WriteDiagnosticLog(
            failed
                ? common::DiagnosticLevel::Error
                : common::DiagnosticLevel::Information,
            failed
                ? common::DiagnosticEvent::
                    ClientProxyStartFailed
                : common::DiagnosticEvent::
                    ClientProxyStateChanged,
            {
                common::DiagnosticValue(
                    common::DiagnosticFieldName::
                        ProxyState,
                    static_cast<std::uint64_t>(
                        proxy_snapshot.state)),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::
                        ProxyError,
                    static_cast<std::uint64_t>(
                        proxy_snapshot.error)),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::
                        Reason,
                    proxy_snapshot.winsock_error),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::
                        Code,
                    static_cast<std::uint64_t>(
                        proxy_snapshot.
                            configuration_error)),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::
                        Port,
                    proxy_snapshot.listening_port),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::
                        Enabled,
                    proxy_snapshot.bridge_ready ? 1U : 0U),
            });
    }

    last_proxy_snapshot_ = proxy_snapshot;
    UpdateBridgeUi();
}

bool MainWindow::DrainInboundStreamFramesToProxy() noexcept {
    protocol::Frame frame{};
    // 一callbackでdecode済みの全stream Frameをworker queueへ所有moveする。
    while (client_bridge_.TakeInboundStreamFrame(frame)) {
        if (!client_proxy_worker_.EnqueueBridgeFrame(
                std::move(frame))) {
            return false;
        }
    }

    return true;
}

bool MainWindow::SynchronizeProxyBridgeSession(
    const bridge::ClientBridgeState before,
    const bridge::ClientBridgeState after) noexcept {
    const bool handshake_completed =
        before == bridge::ClientBridgeState::WaitingForAgent &&
        after == bridge::ClientBridgeState::Ready;
    const bool echo_completed =
        before == bridge::ClientBridgeState::EchoRunning &&
        after == bridge::ClientBridgeState::EchoPassed;
    if (handshake_completed || echo_completed) {
        const protocol::HelloCapabilities capabilities =
            client_bridge_.negotiated_capabilities();
        // Echo-only Agentではlistenerを維持し、local接続は503へ安全に縮退する。
        if ((capabilities.feature_bits &
             protocol::kFeatureMultiplexedStreams) == 0U) {
            client_proxy_worker_.EnqueueBridgeStop();
            return true;
        }

        return client_proxy_worker_.EnqueueBridgeStart(
            capabilities);
    }

    const bool session_ended =
        after == bridge::ClientBridgeState::Stopped ||
        after ==
            bridge::ClientBridgeState::WaitingForAgent ||
        after == bridge::ClientBridgeState::Failed ||
        after == bridge::ClientBridgeState::Incompatible;
    // CLOSE／fatal／RDP切断への遷移で旧Socketとstreamを新sessionへ残さない。
    if (session_ended && before != after) {
        client_proxy_worker_.EnqueueBridgeStop();
    }

    return true;
}

void MainWindow::UpdateBridgeHandshakeTimer() noexcept {
    const bridge::ClientBridgeState state =
        client_bridge_.snapshot().state;
    // HELLOはAgent streamをresetするため、ACK待機中だけretry timerを維持する。
    if (state != bridge::ClientBridgeState::WaitingForAgent) {
        if (window_ != nullptr) {
            static_cast<void>(
                KillTimer(window_, kBridgeHandshakeTimerId));
        }
        return;
    }

    // timer生成失敗時も既にqueue済みのHELLOまたは成立中RDPを維持する。
    if (window_ != nullptr) {
        static_cast<void>(
            SetTimer(
                window_,
                kBridgeHandshakeTimerId,
                kBridgeHandshakeRetryMilliseconds,
                nullptr));
    }
}

void MainWindow::UpdateBridgeEchoTimer() noexcept {
    // Echo中以外では以前のPINGに対応するtimerを残さない。
    if (client_bridge_.snapshot().state !=
        bridge::ClientBridgeState::EchoRunning) {
        bridge_echo_deadline_tick_ = 0U;
        if (window_ != nullptr) {
            static_cast<void>(
                KillTimer(window_, kBridgeEchoTimerId));
        }
        return;
    }

    // SetTimerの同一ID更新でPONGごとに期限を延長し、無応答だけを停止対象にする。
    if (window_ != nullptr) {
        const std::uint64_t now =
            GetTickCount64();
        bridge_echo_deadline_tick_ =
            now + kBridgeEchoTimeoutMilliseconds;
        // timerを確保できない場合はEchoを無期限待機へ残さない。
        if (SetTimer(
                window_,
                kBridgeEchoTimerId,
                kBridgeEchoTimeoutMilliseconds,
                nullptr) == 0U) {
            bridge_echo_deadline_tick_ = 0U;
            client_bridge_.OnEchoTimeout();
        }
    }
}

void MainWindow::UpdateBridgeHealthTimer() noexcept {
    const bridge::ClientBridgeSnapshot snapshot =
        client_bridge_.snapshot();
    const bool health_state =
        snapshot.state == bridge::ClientBridgeState::Ready ||
        snapshot.state == bridge::ClientBridgeState::EchoPassed;
    // handshake未成立、手動Echo、停止、失敗では旧sessionの周期通知を残さない。
    if (!health_state) {
        bridge_health_sent_tick_ = 0U;
        if (window_ != nullptr) {
            static_cast<void>(
                KillTimer(window_, kBridgeHealthTimerId));
        }

        return;
    }

    // PONG受理後は次intervalを応答時点から測り直す。
    if (!snapshot.health_check_outstanding) {
        bridge_health_sent_tick_ = 0U;
    }

    if (window_ == nullptr) {
        return;
    }

    // 同一IDを更新し、死活確認中もUI threadへ5秒ごとにだけ制御を返す。
    static_cast<void>(
        KillTimer(window_, kBridgeHealthTimerId));
    if (SetTimer(
            window_,
            kBridgeHealthTimerId,
            kBridgeHealthIntervalMilliseconds,
            nullptr) != 0U) {
        return;
    }

    // timerなしでAgent接続済み表示を無期限維持せず、Bridgeだけをfail-closedにする。
    const bridge::ClientBridgeState before = snapshot.state;
    client_bridge_.OnStreamRelayFailed();
    client_proxy_worker_.EnqueueBridgeStop();
    const bridge::ClientBridgeSnapshot after =
        client_bridge_.snapshot();
    LogBridgeTransition(before, after);
}

void MainWindow::UpdateBridgeUi() {
    const bridge::ClientBridgeSnapshot snapshot =
        client_bridge_.snapshot();
    std::array<wchar_t, 512> text{};
    int written = 0;
    const std::wstring fallback =
        LoadStringResource(IDS_UNEXPECTED_ERROR);

    // state固有値だけを固定resourceへ埋め、接続情報やpayloadを表示しない。
    switch (snapshot.state) {
        case bridge::ClientBridgeState::Stopped: {
            const std::wstring value =
                LoadStringResource(IDS_BRIDGE_STOPPED);
            SetWindowTextW(
                bridge_state_text_,
                value.c_str());
            break;
        }
        case bridge::ClientBridgeState::WaitingForAgent: {
            const std::wstring value =
                LoadStringResource(IDS_BRIDGE_WAITING_AGENT);
            SetWindowTextW(
                bridge_state_text_,
                value.c_str());
            break;
        }
        case bridge::ClientBridgeState::Ready: {
            const std::wstring value =
                LoadStringResource(IDS_BRIDGE_READY);
            SetWindowTextW(
                bridge_state_text_,
                value.c_str());
            break;
        }
        case bridge::ClientBridgeState::EchoRunning: {
            const std::wstring format =
                LoadStringResource(
                    IDS_BRIDGE_ECHO_RUNNING_FORMAT);
            written = swprintf_s(
                text.data(),
                text.size(),
                format.c_str(),
                static_cast<unsigned long long>(
                    snapshot.echo_completed_bytes),
                static_cast<unsigned long long>(
                    snapshot.echo_total_bytes));
            SetWindowTextW(
                bridge_state_text_,
                written < 0
                    ? fallback.c_str()
                    : text.data());
            break;
        }
        case bridge::ClientBridgeState::EchoPassed: {
            const std::wstring format =
                LoadStringResource(
                    IDS_BRIDGE_ECHO_PASSED_FORMAT);
            written = swprintf_s(
                text.data(),
                text.size(),
                format.c_str(),
                static_cast<unsigned long long>(
                    snapshot.echo_completed_bytes));
            SetWindowTextW(
                bridge_state_text_,
                written < 0
                    ? fallback.c_str()
                    : text.data());
            break;
        }
        case bridge::ClientBridgeState::Failed: {
            const std::wstring format =
                LoadStringResource(
                    IDS_BRIDGE_FAILED_FORMAT);
            written = swprintf_s(
                text.data(),
                text.size(),
                format.c_str(),
                static_cast<unsigned int>(
                    snapshot.error));
            SetWindowTextW(
                bridge_state_text_,
                written < 0
                    ? fallback.c_str()
                    : text.data());
            break;
        }
        case bridge::ClientBridgeState::Incompatible: {
            const std::wstring value =
                LoadStringResource(
                    IDS_BRIDGE_INCOMPATIBLE);
            SetWindowTextW(
                bridge_state_text_,
                value.c_str());
            break;
        }
    }

    const proxy::ClientProxyWorkerSnapshot proxy_snapshot =
        client_proxy_worker_.snapshot();
    std::array<wchar_t, 256> proxy_text{};
    int proxy_written = 0;
    // listener addressは固定loopbackだけをresource formatへ埋める。
    if (launch_settings_.proxy_configuration_error !=
        proxy::ClientProxyConfigurationError::None) {
        const std::wstring format =
            LoadStringResource(
                IDS_PROXY_FAILED_FORMAT);
        proxy_written = swprintf_s(
            proxy_text.data(),
            proxy_text.size(),
            format.c_str(),
            static_cast<unsigned int>(
                proxy::ClientProxyWorkerError::
                    ConfigurationFailed),
            0U,
            static_cast<unsigned int>(
                launch_settings_.
                    proxy_configuration_error));
    } else if (proxy_snapshot.error !=
            proxy::ClientProxyWorkerError::None ||
        proxy_snapshot.state ==
            proxy::ClientProxyWorkerState::Failed) {
        const std::wstring format =
            LoadStringResource(
                IDS_PROXY_FAILED_FORMAT);
        proxy_written = swprintf_s(
            proxy_text.data(),
            proxy_text.size(),
            format.c_str(),
            static_cast<unsigned int>(
                proxy_snapshot.error),
            static_cast<unsigned int>(
                proxy_snapshot.winsock_error),
            static_cast<unsigned int>(
                proxy_snapshot.
                    configuration_error));
    } else {
        switch (proxy_snapshot.state) {
            case proxy::ClientProxyWorkerState::Starting: {
                const std::wstring value =
                    LoadStringResource(
                        IDS_PROXY_STARTING);
                wcsncpy_s(
                    proxy_text.data(),
                    proxy_text.size(),
                    value.c_str(),
                    _TRUNCATE);
                proxy_written =
                    static_cast<int>(wcslen(
                        proxy_text.data()));
                break;
            }
            case proxy::ClientProxyWorkerState::Listening: {
                const UINT resource =
                    proxy_snapshot.bridge_ready
                        ? IDS_PROXY_READY_FORMAT
                        : IDS_PROXY_LISTENING_FORMAT;
                const std::wstring format =
                    LoadStringResource(resource);
                if (proxy_snapshot.bridge_ready) {
                    proxy_written = swprintf_s(
                        proxy_text.data(),
                        proxy_text.size(),
                        format.c_str(),
                        static_cast<unsigned int>(
                            proxy_snapshot.listening_port),
                        static_cast<unsigned int>(
                            proxy_snapshot.active_streams),
                        static_cast<unsigned long long>(
                            proxy_snapshot.
                                bytes_from_local),
                        static_cast<unsigned long long>(
                            proxy_snapshot.
                                bytes_to_local));
                } else {
                    proxy_written = swprintf_s(
                        proxy_text.data(),
                        proxy_text.size(),
                        format.c_str(),
                        static_cast<unsigned int>(
                            proxy_snapshot.listening_port));
                }
                break;
            }
            case proxy::ClientProxyWorkerState::Stopped: {
                const std::wstring value =
                    LoadStringResource(
                        IDS_PROXY_STOPPED);
                wcsncpy_s(
                    proxy_text.data(),
                    proxy_text.size(),
                    value.c_str(),
                    _TRUNCATE);
                proxy_written =
                    static_cast<int>(wcslen(
                        proxy_text.data()));
                break;
            }
            case proxy::ClientProxyWorkerState::Failed:
                break;
        }
    }

    // BridgeとProxyを一つの専用状態欄で同時確認できるよう固定区切りで連結する。
    if (proxy_written > 0) {
        std::wstring combined =
            ReadWindowText(bridge_state_text_);
        combined.append(L" | ");
        combined.append(proxy_text.data());
        SetWindowTextW(
            bridge_state_text_,
            combined.c_str());
    }

    const bool can_start_echo =
        (snapshot.state == bridge::ClientBridgeState::Ready ||
         snapshot.state ==
             bridge::ClientBridgeState::EchoPassed) &&
        proxy_snapshot.active_streams == 0U;
    EnableWindow(
        bridge_echo_button_,
        can_start_echo ? TRUE : FALSE);
    UpdateWindowTitle();
}

void MainWindow::LogBridgeTransition(
    const bridge::ClientBridgeState before,
    const bridge::ClientBridgeSnapshot& after) const noexcept {
    // PONGごとのprogressではlogを増やさず、state遷移だけを記録する。
    if (before == after.state) {
        return;
    }

    switch (after.state) {
        case bridge::ClientBridgeState::WaitingForAgent:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::BridgeSessionStarted,
                {
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::AttemptCount,
                        after.hello_attempt_count),
                });
            break;
        case bridge::ClientBridgeState::Ready:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::BridgeReady);
            break;
        case bridge::ClientBridgeState::EchoPassed:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Information,
                common::DiagnosticEvent::BridgeEchoPassed,
                {
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::ByteCount,
                        after.echo_completed_bytes),
                });
            break;
        case bridge::ClientBridgeState::Failed:
        case bridge::ClientBridgeState::Incompatible:
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Error,
                common::DiagnosticEvent::BridgeFailed,
                {
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::BridgeState,
                        static_cast<std::uint64_t>(
                            after.state)),
                    common::DiagnosticValue(
                        common::DiagnosticFieldName::BridgeError,
                        static_cast<std::uint64_t>(
                            after.error)),
                });
            break;
        case bridge::ClientBridgeState::Stopped:
        case bridge::ClientBridgeState::EchoRunning:
            break;
    }
}

// ---------------------------------------------------------------------------
// 接続状態、状態表示、診断ダイアログ
// ---------------------------------------------------------------------------

void MainWindow::UpdateUiForState(
    const rdp::RdpConnectionState state) noexcept {
    const bool active = IsConnectionActive(state);
    const bool can_full_screen = CanUseFullScreen(state);
    const bool can_disconnect =
        active && state != rdp::RdpConnectionState::Disconnecting;

    EnableWindow(server_combo_, !active);
    EnableWindow(domain_combo_, !active);
    EnableWindow(user_name_combo_, !active);
    EnableWindow(
        native_mode_radio_,
        !active || CanChangeDisplayMode(state));
    EnableWindow(
        smart_sizing_mode_radio_,
        !active || CanChangeDisplayMode(state));
    EnableWindow(
        dynamic_resolution_mode_radio_,
        !active || CanChangeDisplayMode(state));
    EnableWindow(connect_button_, !active);
    EnableWindow(disconnect_button_, can_disconnect);
    EnableWindow(full_screen_button_, can_full_screen);
    title_rdp_state_ = state;
    UpdateWindowTitle();
}

void MainWindow::UpdateFullScreenButton(
    const bool full_screen) {
    const std::wstring text = LoadStringResource(
        full_screen ? IDS_WINDOW_MODE : IDS_FULL_SCREEN);
    SetWindowTextW(full_screen_button_, text.c_str());
}

void MainWindow::SetStatus(const UINT string_id) const {
    const std::wstring status = LoadStringResource(string_id);
    SetWindowTextW(status_text_, status.c_str());
}

void MainWindow::SetStatusWithCode(
    const UINT format_id,
    const long code) const {
    const std::wstring format = LoadStringResource(format_id);
    std::array<wchar_t, 512> status{};
    const int written = swprintf_s(
        status.data(),
        status.size(),
        format.c_str(),
        code);
    // resource書式が不正でも未終端の文字列を表示しない。
    if (written < 0) {
        SetStatus(IDS_UNEXPECTED_ERROR);
        return;
    }

    SetWindowTextW(status_text_, status.data());
}

void MainWindow::SetDisconnectedStatus(
    const long reason,
    const long extended_reason) const {
    const std::wstring format =
        LoadStringResource(IDS_STATUS_DISCONNECTED_FORMAT);
    std::array<wchar_t, 512> status{};
    const int written = swprintf_s(
        status.data(),
        status.size(),
        format.c_str(),
        reason,
        extended_reason);
    // resource書式が不正な場合は、安全な固定状態表示へ縮退する。
    if (written < 0) {
        SetStatus(IDS_STATUS_DISCONNECTED);
        return;
    }

    SetWindowTextW(status_text_, status.data());
}

void MainWindow::SetInitialStatus() const {
    const std::wstring format =
        LoadStringResource(IDS_STATUS_INITIAL_FORMAT);
    const std::wstring control_name(
        rdp_host_.selected_control_name());
    std::array<wchar_t, 512> status{};
    const int written = swprintf_s(
        status.data(),
        status.size(),
        format.c_str(),
        control_name.c_str());
    // 選択Control名を組み込めない場合も、未接続状態は明示する。
    if (written < 0) {
        SetStatus(IDS_STATUS_DISCONNECTED);
        return;
    }

    SetWindowTextW(status_text_, status.data());
}

void MainWindow::QueueDisconnectError(
    const rdp::RdpEvent& event) {
    // 成立済み外部切断は終了済みとし、残るlocal／取消／終了処理でもdialogを出さない。
    if (event.locally_requested_disconnect ||
        event.credential_prompt_cancelled ||
        closing_ ||
        window_ == nullptr) {
        return;
    }

    const std::wstring format =
        LoadStringResource(IDS_DISCONNECT_ERROR_FORMAT);
    const std::wstring fallback_reason =
        LoadStringResource(IDS_DISCONNECT_REASON_UNAVAILABLE);
    const std::wstring& description = event.error_description.empty()
        ? fallback_reason
        : event.error_description;
    std::array<wchar_t, 4096> detail{};
    const int written = swprintf_s(
        detail.data(),
        detail.size(),
        format.c_str(),
        description.c_str(),
        event.code,
        static_cast<unsigned long>(
            static_cast<std::uint32_t>(event.code)),
        event.extended_disconnect_reason,
        static_cast<unsigned long>(
            static_cast<std::uint32_t>(
                event.extended_disconnect_reason)));
    pending_disconnect_error_ = written < 0
        ? fallback_reason
        : std::wstring(detail.data(), static_cast<std::size_t>(written));

    // モーダル表示はActiveXのCOM callbackから復帰した後のメッセージ処理へ遅延する。
    if (!PostMessageW(window_, kMessageShowDisconnectError, 0U, 0L)) {
        pending_disconnect_error_.clear();
    }
}

void MainWindow::ShowError(
    const UINT message_id,
    const HRESULT error) const {
    const std::wstring message = LoadStringResource(message_id);
    std::array<wchar_t, 768> detail{};
    const int written = swprintf_s(
        detail.data(),
        detail.size(),
        L"%s\n\nHRESULT: 0x%08lX",
        message.c_str(),
        static_cast<unsigned long>(error));
    const wchar_t* text = written < 0 ? message.c_str() : detail.data();
    MessageBoxW(
        window_,
        text,
        L"ZRdp SVN Bridge",
        MB_OK | MB_ICONERROR);
}

void MainWindow::SetDisplayModeSelection(
    const rdp::RdpDisplayMode mode) const noexcept {
    const std::array<std::pair<HWND, rdp::RdpDisplayMode>, 3> radios{{
        {native_mode_radio_, rdp::RdpDisplayMode::NativeScroll},
        {smart_sizing_mode_radio_, rdp::RdpDisplayMode::SmartSizing},
        {
            dynamic_resolution_mode_radio_,
            rdp::RdpDisplayMode::DynamicResolution,
        },
    }};

    // radio groupを常に一つだけ選択し、modelとUIの不一致を防ぐ。
    for (const auto& [radio, radio_mode] : radios) {
        // control生成前または破棄後の無効なHWNDへmessageを送らない。
        if (radio != nullptr) {
            SendMessageW(
                radio,
                BM_SETCHECK,
                radio_mode == mode ? BST_CHECKED : BST_UNCHECKED,
                0L);
        }
    }
}

void MainWindow::ScheduleDynamicResolutionUpdate() {
    dynamic_resolution_retries_scheduled_ = 0U;
    // 明示された動的modeの成立済みsessionだけをWindow／全画面追従対象とする。
    if (display_mode_ != rdp::RdpDisplayMode::DynamicResolution ||
        window_ == nullptr ||
        !CanChangeDisplayMode(rdp_host_.state())) {
        return;
    }

    // 通常のresize debounceを予約できた場合はUI message loopへ直ちに戻る。
    if (ArmDynamicResolutionTimer(
            kDynamicResolutionDelayMilliseconds)) {
        return;
    }

    const HRESULT result = ApplyDynamicResolutionUpdate();
    // timer確保不能時も一時失敗なら別delayを一度ずつ試し、要求を失わない。
    if (FAILED(result) &&
        !ScheduleDynamicResolutionRetry(result)) {
        HandleDynamicResolutionFailure(result);
    } else if (SUCCEEDED(result)) {
        dynamic_resolution_retries_scheduled_ = 0U;
    }
}

bool MainWindow::ArmDynamicResolutionTimer(
    const UINT delay_milliseconds) noexcept {
    // 0 msのtimerによるbusy loopを作らず、有限delayだけをUSER32へ渡す。
    if (window_ == nullptr ||
        delay_milliseconds == 0U) {
        return false;
    }

    const UINT_PTR timer = SetTimer(
        window_,
        kDynamicResolutionTimerId,
        delay_milliseconds,
        nullptr);
    // SetTimer失敗時はactive flagを残さず、呼出し元へ同期fallbackを委ねる。
    if (timer == 0U) {
        dynamic_resolution_timer_active_ = false;
        return false;
    }

    dynamic_resolution_timer_active_ = true;
    return true;
}

bool MainWindow::ScheduleDynamicResolutionRetry(
    const HRESULT error) noexcept {
    const DynamicResolutionRetryDecision decision =
        SelectDynamicResolutionRetry(
            error,
            dynamic_resolution_retries_scheduled_);
    // 恒久errorまたは再試行上限では、既存の表示mode縮退へ制御を返す。
    if (!decision.should_retry) {
        return false;
    }

    // timer確保失敗を成功扱いせず、利用者へ最終errorを通知できるようにする。
    if (!ArmDynamicResolutionTimer(
            decision.delay_milliseconds)) {
        return false;
    }

    ++dynamic_resolution_retries_scheduled_;
    return true;
}

void MainWindow::CancelDynamicResolutionTimer() noexcept {
    // 予約済みtimerだけを解除し、同じIDを使う将来処理へ影響させない。
    if (dynamic_resolution_timer_active_ && window_ != nullptr) {
        static_cast<void>(
            KillTimer(window_, kDynamicResolutionTimerId));
    }

    dynamic_resolution_timer_active_ = false;
}

void MainWindow::CancelDynamicResolutionUpdate() noexcept {
    CancelDynamicResolutionTimer();
    dynamic_resolution_retries_scheduled_ = 0U;
}

HRESULT MainWindow::ApplyDynamicResolutionUpdate() noexcept {
    // mode変更または切断によって不要になった遅延要求は正常なno-opとする。
    if (display_mode_ != rdp::RdpDisplayMode::DynamicResolution ||
        !CanChangeDisplayMode(rdp_host_.state())) {
        return S_FALSE;
    }

    long width = 0L;
    long height = 0L;
    const DynamicResolutionTarget target =
        SelectDynamicResolutionTarget(
            rdp_host_.full_screen());
    // 全画面ではmonitor全体、Window表示ではRDP child領域をremote寸法にする。
    HRESULT result =
        target ==
                DynamicResolutionTarget::FullScreenMonitor
            ? ResolveInitialDesktopSize(
                  window_,
                  width,
                  height)
            : ResolveRdpDisplayAreaSize(
                  rdp_host_.window(),
                  width,
                  height);
    // 有効な表示領域を取得できた場合だけremote sessionへ寸法を送る。
    if (SUCCEEDED(result)) {
        result = rdp_host_.UpdateSessionDisplaySettings(
            width,
            height);
    }

    common::WriteDiagnosticLog(
        FAILED(result)
            ? common::DiagnosticLevel::Warning
            : common::DiagnosticLevel::Information,
        common::DiagnosticEvent::DynamicResolutionCallReturned,
        {
            common::DiagnosticHResult(result),
            common::DiagnosticValue(
                common::DiagnosticFieldName::AttemptCount,
                dynamic_resolution_retries_scheduled_ + 1U),
            common::DiagnosticValue(
                common::DiagnosticFieldName::Port,
                launch_settings_.proxy_port),
        });
    return result;
}

void MainWindow::HandleDynamicResolutionFailure(
    const HRESULT error) {
    CancelDynamicResolutionUpdate();
    // 既に別modeへ移行済みなら古いtimer結果で現在選択を上書きしない。
    if (display_mode_ != rdp::RdpDisplayMode::DynamicResolution) {
        return;
    }

    const rdp::RdpConnectionState state = rdp_host_.state();
    // 成立済みsessionではSmart Sizingを明示的にOFFにして等倍表示へ縮退する。
    if (CanChangeDisplayMode(state)) {
        const HRESULT reset_result =
            rdp_host_.SetSmartSizing(false);
        common::WriteDiagnosticLog(
            FAILED(reset_result)
                ? common::DiagnosticLevel::Warning
                : common::DiagnosticLevel::Information,
            common::DiagnosticEvent::SmartSizingCallReturned,
            {
                common::DiagnosticHResult(reset_result),
                common::DiagnosticValue(
                    common::DiagnosticFieldName::Enabled,
                    0U),
            });
    }

    display_mode_ = rdp::RdpDisplayMode::NativeScroll;
    SetDisplayModeSelection(display_mode_);
    UpdateUiForState(state);
    ShowError(IDS_DISPLAY_SETTING_FAILED, error);
}

RECT MainWindow::RdpBounds(
    const int client_width,
    const int client_height) const noexcept {
    const int toolbar_height =
        Scale(kDisplayModeToolbarHeight);
    return RECT{
        0,
        toolbar_height,
        std::max(0, client_width),
        std::max(toolbar_height, client_height),
    };
}

int MainWindow::Scale(const int value) const noexcept {
    const UINT dpi = window_ == nullptr ? GetDpiForSystem() : GetDpiForWindow(window_);
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

// ---------------------------------------------------------------------------
// resource文字列と有限長UI入力の取得
// ---------------------------------------------------------------------------

std::wstring MainWindow::LoadStringResource(
    const UINT string_id) const {
    std::array<wchar_t, 1024> buffer{};
    const int length = LoadStringW(
        instance_,
        string_id,
        buffer.data(),
        static_cast<int>(buffer.size()));
    // resource欠落時は、バッファの未使用領域を文字列として返さない。
    if (length <= 0) {
        return {};
    }

    return std::wstring(
        buffer.data(),
        static_cast<std::size_t>(length));
}

std::wstring MainWindow::ReadWindowText(
    const HWND control) const {
    const int length = GetWindowTextLengthW(control);
    // 空入力または取得失敗では不要なバッファを確保しない。
    if (length <= 0) {
        return {};
    }

    std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(
        control,
        value.data(),
        static_cast<int>(value.size()));
    // 取得失敗時は部分入力を接続先、domain、ユーザー名として使用しない。
    if (copied <= 0) {
        return {};
    }

    value.resize(static_cast<std::size_t>(copied));
    return value;
}

void MainWindow::UpdateWindowTitle() noexcept {
    if (window_ == nullptr) {
        return;
    }

    try {
        const std::wstring rdp_state =
            LoadStringResource(
                RdpTitleStateResource(
                    title_rdp_state_));
        const bridge::ClientBridgeSnapshot bridge_snapshot =
            client_bridge_.snapshot();
        const std::wstring agent_state =
            LoadStringResource(
                AgentTitleStateResource(
                    bridge_snapshot.state));
        const proxy::ClientProxyWorkerSnapshot proxy_snapshot =
            client_proxy_worker_.snapshot();
        const std::wstring proxy_state =
            LoadStringResource(
                ProxyTitleStateResource(
                    proxy_snapshot));
        std::wstring connection_label =
            launch_settings_.connection.server;
        // 任意名だけで接続先を隠さず、誤認防止のため実接続先も併記する。
        if (!launch_settings_.
                 connection.connection_name.empty()) {
            connection_label =
                launch_settings_.connection.connection_name +
                L" (" +
                launch_settings_.connection.server +
                L")";
        }

        std::array<wchar_t, 1024> title{};
        int written = -1;
        // 検証済みportだけをtitleへ出し、設定異常時は分類codeへ切り替える。
        if (launch_settings_.proxy_configuration_error ==
            proxy::ClientProxyConfigurationError::None) {
            const std::wstring format =
                LoadStringResource(
                    IDS_SESSION_TITLE_FORMAT);
            written = swprintf_s(
                title.data(),
                title.size(),
                format.c_str(),
                connection_label.c_str(),
                rdp_state.c_str(),
                agent_state.c_str(),
                proxy_state.c_str(),
                static_cast<unsigned int>(
                    launch_settings_.proxy_port),
                static_cast<unsigned int>(
                    proxy_snapshot.active_streams),
                static_cast<unsigned long long>(
                    proxy_snapshot.bytes_from_local),
                static_cast<unsigned long long>(
                    proxy_snapshot.bytes_to_local));
        } else {
            const std::wstring format =
                LoadStringResource(
                    IDS_SESSION_TITLE_CONFIG_ERROR_FORMAT);
            written = swprintf_s(
                title.data(),
                title.size(),
                format.c_str(),
                connection_label.c_str(),
                rdp_state.c_str(),
                agent_state.c_str(),
                static_cast<unsigned int>(
                    launch_settings_.
                        proxy_configuration_error));
        }

        const std::wstring fallback =
            LoadStringResource(IDS_APP_TITLE);
        SetWindowTextW(
            window_,
            written < 0
                ? fallback.c_str()
                : title.data());
    } catch (...) {
        // title更新失敗でRDP sessionやProxy転送を停止しない。
        SetWindowTextW(
            window_,
            L"ZRdp SVN Bridge");
    }
}

}  // namespace zrdp::client::app
