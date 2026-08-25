/**
 * @file main_window.h
 * @brief ZRdp ClientのWin32 UIとRDP ActiveX event連携を定義する。
 *
 * @details MainWindowはsession専用COM STAであるUI threadに所属し、
 *          ActiveX操作、UI状態遷移、Bridge通知をsession内で直列化する。
 */
#pragma once

#include "client/bridge/client_bridge.h"
#include "client/app/session_launch_settings.h"
#include "client/proxy/client_proxy_worker.h"
#include "client/rdp/rdp_control_host.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace zrdp::client::app {

/**
 * @brief 一RDP接続・表示とSVN Bridge／localhost Proxyを所有するsession Window。
 *
 * @note 全public methodとWindowProcedureは、Createを呼び出したCOM STAの
 *       UI threadで実行する。
 */
class MainWindow final : public rdp::IRdpEventObserver {
public:
    /**
     * @brief 未生成のsession Window controllerを構築する。
     * @param[in] instance resourceとwindow classを所有するmodule instance。
     * @param[in] observer 認証成功を共通履歴ownerへ渡すapplication境界。
     * @param[in] settings managerで確定したRDP設定と専用Proxy port。
     */
    MainWindow(
        HINSTANCE instance,
        ISessionWindowObserver& observer,
        SessionLaunchSettings settings);

    /**
     * @brief COM STA終了前に履歴workerとhosted ActiveXを解放する。
     */
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    /**
     * @brief main application windowと子controlを生成して表示する。
     * @param[in] show_command WinMainから渡された初期表示command。
     * @return main windowを利用可能にできた場合はtrue。
     */
    [[nodiscard]] bool Create(int show_command);

    /**
     * @brief Tab移動等のdialog互換messageをこのsession階層で処理する。
     * @param[in] message session専用message loopが取得した一件。
     * @return このWindow階層で処理した場合はtrue。
     */
    [[nodiscard]] bool ProcessDialogMessage(
        MSG& message) const noexcept;

    /**
     * @brief 現在のsessionトップレベルHWNDを返す。
     * @return 生成前またはWM_NCDESTROY後はnullptr。
     */
    [[nodiscard]] HWND window() const noexcept;

    /**
     * @brief 所有copy化された一件のActiveX callbackをUI状態へ反映する。
     * @param[in] event COM callback境界から移動されたevent。
     */
    void OnRdpEvent(rdp::RdpEvent event) override;

private:
    // -------------------------------------------------------------------------
    // Win32 message境界
    // -------------------------------------------------------------------------

    /**
     * @brief Win32 messageを対応するMainWindow instanceへdispatchする。
     * @param[in] window messageを受信したwindow。
     * @param[in] message Win32 message ID。
     * @param[in] wparam message固有のWPARAM。
     * @param[in] lparam message固有のLPARAM。
     * @return 処理結果を表すLRESULT。
     *
     * @note C++例外をこのcallback境界で捕捉し、USER32へ漏らさない。
     */
    static LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam) noexcept;

    /**
     * @brief instanceへrouting済みのWin32 messageを処理する。
     * @param[in] message Win32 message ID。
     * @param[in] wparam message固有のWPARAM。
     * @param[in] lparam message固有のLPARAM。
     * @return 処理結果、未処理時はDefWindowProcWの結果。
     */
    [[nodiscard]] LRESULT HandleMessage(
        UINT message,
        WPARAM wparam,
        LPARAM lparam);

    /**
     * @brief main window classをprocessへ一度だけ登録する。
     * @return 登録済みまたは登録成功の場合はtrue。
     */
    [[nodiscard]] bool RegisterWindowClass() const noexcept;

    /**
     * @brief 接続操作、表示切替、状態表示用の全子controlを生成する。
     * @return 必須controlをすべて生成できた場合はtrue。
     */
    [[nodiscard]] bool CreateControls();

    /**
     * @brief 現在のclient領域とDPIに合わせて子controlを配置する。
     * @param[in] client_width main window client領域の幅。
     * @param[in] client_height main window client領域の高さ。
     *
     * ActiveX hostは即時resizeし、動的解像度modeだけはdebounce更新を予約する。
     */
    void LayoutControls(int client_width, int client_height);

    /**
     * @brief 手動変更候補を接続時remote desktopのアスペクト比へ補正する。
     * @param[in] sizing_edge `WM_SIZING`の`WMSZ_*`値。
     * @param[in,out] bounds top-level Windowの画面座標矩形。
     * @return 通常Windowの有効な接続比率を適用できた場合はtrue。
     *
     * 最大化、Windowsスナップ、全画面は`WM_SIZING`を経由しないため、
     * Windows標準の配置を維持する。
     */
    [[nodiscard]] bool ConstrainManualSizing(
        UINT sizing_edge,
        RECT& bounds) const noexcept;

    /**
     * @brief system所有のGUI fontを全子controlへ設定する。
     */
    void ApplyControlFont() const noexcept;

    // -------------------------------------------------------------------------
    // UI command処理
    // -------------------------------------------------------------------------

    /**
     * @brief 入力を検証し、非機密RDP設定を適用して接続を開始する。
     */
    void OnConnect();

    /**
     * @brief 現在のsession設定を再検証して一回のActiveX接続試行を開始する。
     *
     * 初回接続と有限再試行で共有し、passwordは受け取らずMicrosoft認証UIへ
     * 引き続き委譲する。
     */
    void BeginConnectionAttempt();

    /**
     * @brief 観測済みログイン前切断に対する次の遅延再試行を予約する。
     * @param[in] event 完全一致を確認済みの切断数値情報。
     * @param[in] delay_milliseconds COM callback後に待機する有限時間。
     * @return timerを予約できた場合はtrue。
     */
    [[nodiscard]] bool SchedulePreLoginConnectionRetry(
        const rdp::RdpEvent& event,
        std::uint32_t delay_milliseconds);

    /**
     * @brief 再試行回数を維持したまま予約timerだけを取り消す。
     */
    void CancelPreLoginConnectionRetryTimer() noexcept;

    /**
     * @brief 予約timerと現在の接続意図に対する再試行回数を破棄する。
     */
    void CancelPreLoginConnectionRetry() noexcept;

    /**
     * @brief 現在のRDP sessionへ非同期切断を要求する。
     */
    void OnDisconnect();

    /**
     * @brief Window modeとMicrosoft ActiveX全画面modeを切り替える。
     */
    void OnFullScreen();

    /**
     * @brief ActiveXと所有top-level Windowを一つの全画面状態へ遷移させる。
     * @param[in] full_screen 全画面へ移行する場合はtrue。
     * @return 両方の状態を適用できた場合はS_OK、失敗時は原因HRESULT。
     *
     * @note COM STA UI threadでだけ呼び出し、片側の失敗時は直前状態へrollbackする。
     */
    [[nodiscard]] HRESULT ApplyFullScreenMode(bool full_screen) noexcept;

    /**
     * @brief 所有top-level Windowだけをborderless全画面または保存済み配置へ切り替える。
     * @param[in] full_screen 全画面Windowへ移行する場合はtrue。
     * @return 適用成功時はS_OK、既に同じ状態ならS_FALSE、失敗時は原因HRESULT。
     */
    [[nodiscard]] HRESULT SetContainerFullScreen(bool full_screen) noexcept;

    /**
     * @brief UI／ActiveX eventから非同期化された全画面要求を実行して診断する。
     * @param[in] full_screen 全画面へ移行する場合はtrue。
     */
    void ExecuteFullScreenRequest(bool full_screen);

    /**
     * @brief handshake済みAgentとの段階的binary Echo検証を開始する。
     */
    void OnBridgeEchoTest();

    /**
     * @brief 排他的な表示modeを接続前設定または成立済みsessionへ反映する。
     * @param[in] requested_mode 利用者が選択した表示mode。
     *
     * Smart SizingはClient側描画だけ、動的解像度はremote desktop寸法を変更する。
     * 実行時反映に失敗した場合は直前のmodeへrollbackする。
     */
    void OnDisplayModeChanged(rdp::RdpDisplayMode requested_mode);

    /**
     * @brief 接続中の確認と安全なActiveX shutdownを含む終了処理を行う。
     */
    void OnClose();

    /**
     * @brief 認証完了した現在の接続組を共通履歴ownerへ通知する。
     */
    void RememberSuccessfulConnection();

    // -------------------------------------------------------------------------
    // Static Virtual Channel Bridge連携
    // -------------------------------------------------------------------------

    /**
     * @brief RDP login完了後に新しいHELLO sessionとretry timerを開始する。
     */
    void StartBridgeSession();

    /**
     * @brief RDP切断または再接続開始時に旧Bridge stateを破棄する。
     */
    void StopBridgeSession() noexcept;

    /**
     * @brief bounded queueの送信要求をCOM callback外のUI messageへpostする。
     */
    void QueueBridgeSendMessage() noexcept;

    /**
     * @brief 一回のUI messageで有限数のSVC chunkをActiveXへ送信する。
     */
    void DrainBridgeSendQueue();

    /**
     * @brief worker通知を受け、outbound FrameとProxy状態をUI threadへ反映する。
     */
    void OnProxyWorkerNotification();

    /**
     * @brief ClientBridgeがdecodeしたstream Frameをbounded worker queueへ移す。
     * @return 全Frameをqueueできた場合はtrue。
     */
    [[nodiscard]] bool DrainInboundStreamFramesToProxy() noexcept;

    /**
     * @brief HELLO／Echo／CLOSE遷移をProxy workerのmultiplexer sessionへ対応させる。
     * @param[in] before Channel処理前のBridge state。
     * @param[in] after Channel処理後のBridge state。
     * @return worker commandを予約できた場合はtrue。
     */
    [[nodiscard]] bool SynchronizeProxyBridgeSession(
        bridge::ClientBridgeState before,
        bridge::ClientBridgeState after) noexcept;

    /**
     * @brief HELLO_ACK待ちの間だけ1秒周期の再送timerを維持する。
     */
    void UpdateBridgeHandshakeTimer() noexcept;

    /**
     * @brief 未応答PINGがある間だけ応答期限timerを維持する。
     */
    void UpdateBridgeEchoTimer() noexcept;

    /**
     * @brief handshake成立中だけAgent死活確認の周期timerを維持する。
     */
    void UpdateBridgeHealthTimer() noexcept;

    /**
     * @brief 現在Bridge snapshotを専用状態欄とEcho buttonへ反映する。
     */
    void UpdateBridgeUi();

    /**
     * @brief Bridge state遷移だけを固定eventと数値fieldで診断記録する。
     * @param[in] before 処理前state。
     * @param[in] after 処理後snapshot。
     */
    void LogBridgeTransition(
        bridge::ClientBridgeState before,
        const bridge::ClientBridgeSnapshot& after) const noexcept;

    // -------------------------------------------------------------------------
    // UI状態・表示helper
    // -------------------------------------------------------------------------

    /**
     * @brief RDP lifecycleに応じて各controlの操作可否を更新する。
     * @param[in] state UIへ反映するRDP接続状態。
     */
    void UpdateUiForState(rdp::RdpConnectionState state) noexcept;

    /**
     * @brief 現在modeに応じて全画面buttonの表示文言を更新する。
     * @param[in] full_screen ActiveXが全画面中の場合はtrue。
     */
    void UpdateFullScreenButton(bool full_screen);

    /**
     * @brief Window mode専用の表示mode一行と全画面buttonを一括表示または非表示にする。
     * @param[in] visible 表示する場合はtrue。
     */
    void SetDisplayControlsVisible(bool visible) const noexcept;

    /**
     * @brief resource文字列を状態欄へ設定する。
     * @param[in] string_id 表示するstring resource ID。
     */
    void SetStatus(UINT string_id) const;

    /**
     * @brief 一つの数値codeをresource formatへ埋めて状態欄へ表示する。
     * @param[in] format_id 数値placeholderを持つresource ID。
     * @param[in] code 表示するActiveX code。
     */
    void SetStatusWithCode(UINT format_id, long code) const;

    /**
     * @brief 切断reasonを秘密情報なしで状態欄へ表示する。
     * @param[in] reason ActiveX disconnect reason。
     * @param[in] extended_reason ActiveX extended disconnect reason。
     */
    void SetDisconnectedStatus(long reason, long extended_reason) const;

    /**
     * @brief 選択済みActiveX versionを含む初期状態を表示する。
     */
    void SetInitialStatus() const;

    /**
     * @brief COM callback復帰後に必要な切断errorだけを所有bufferへ準備する。
     * @param[in] event Windows由来の切断診断を持つevent。
     *
     * 成立済みsessionの外部切断は呼出し前にWindow終了へ分岐する。
     * ZRdpからの切断、credential UIの利用者取消、終了処理でも表示を抑止する。
     */
    void QueueDisconnectError(const rdp::RdpEvent& event);

    /**
     * @brief resource messageとHRESULTをmodal error dialogへ表示する。
     * @param[in] message_id 利用者向けresource ID。
     * @param[in] error 分類済みHRESULT。
     */
    void ShowError(UINT message_id, HRESULT error) const;

    /**
     * @brief 3つのラジオボタンを指定表示modeと一致させる。
     * @param[in] mode 選択状態として表示するmode。
     */
    void SetDisplayModeSelection(rdp::RdpDisplayMode mode) const noexcept;

    /**
     * @brief 最後のwindow resizeから250 ms後の動的解像度更新を予約する。
     *
     * 等倍表示、Smart Sizing、未接続では予約しない。ActiveX全画面では
     * monitor全体、Window表示ではRDP child領域への更新を予約する。
     */
    void ScheduleDynamicResolutionUpdate();

    /**
     * @brief 指定delayで一回の動的解像度timerを予約する。
     * @param[in] delay_milliseconds 次回試行までの有限時間。
     * @return timerを予約できた場合はtrue。
     */
    [[nodiscard]] bool ArmDynamicResolutionTimer(
        UINT delay_milliseconds) noexcept;

    /**
     * @brief 一時的なActiveX失敗に対する次回試行を有限回だけ予約する。
     * @param[in] error 直前の動的解像度APIが返したHRESULT。
     * @return 再試行を予約した場合はtrue。
     */
    [[nodiscard]] bool ScheduleDynamicResolutionRetry(
        HRESULT error) noexcept;

    /**
     * @brief retry回数を維持したまま予約中timerだけを取消す。
     */
    void CancelDynamicResolutionTimer() noexcept;

    /**
     * @brief 予約中timerと現在要求のretry回数を取消す。
     */
    void CancelDynamicResolutionUpdate() noexcept;

    /**
     * @brief Window／全画面に対応する寸法を成立済みremote sessionへ適用する。
     * @return 更新不要時はS_FALSE、API成功時はS_OK、失敗時は原因HRESULT。
     */
    [[nodiscard]] HRESULT ApplyDynamicResolutionUpdate() noexcept;

    /**
     * @brief 動的更新失敗後に等倍表示へ縮退して利用者へ通知する。
     * @param[in] error 動的解像度APIが返したHRESULT。
     */
    void HandleDynamicResolutionFailure(HRESULT error);

    /**
     * @brief 表示mode一行を除いたActiveX表示領域を計算する。
     * @param[in] client_width main window client領域の幅。
     * @param[in] client_height main window client領域の高さ。
     * @return ActiveX hostへ適用する子window矩形。
     */
    [[nodiscard]] RECT RdpBounds(int client_width, int client_height) const noexcept;

    /**
     * @brief 96 DPI基準値を現在windowのDPIへ変換する。
     * @param[in] value 96 DPI基準のpixel値。
     * @return 現在DPIへscaleしたpixel値。
     */
    [[nodiscard]] int Scale(int value) const noexcept;

    /**
     * @brief UTF-16 string resourceを所有文字列として読込む。
     * @param[in] string_id 読み込むresource ID。
     * @return 読込み結果。存在しない場合は空文字列。
     */
    [[nodiscard]] std::wstring LoadStringResource(UINT string_id) const;

    /**
     * @brief Win32 controlの現在textをbounded所有文字列へcopyする。
     * @param[in] control 読込み対象の子window。
     * @return control text。読込み不能時は空文字列。
     */
    [[nodiscard]] std::wstring ReadWindowText(HWND control) const;

    /**
     * @brief 接続先、RDP／Agent状態、session専用Proxy統計をtitleへ反映する。
     *
     * title表示の失敗で成立中のRDP sessionとProxy転送を停止しない。
     */
    void UpdateWindowTitle() noexcept;

    /// resourceとwindow classのmodule instance。MainWindowは所有しない。
    HINSTANCE instance_;

    /// application lifetime中に生存する非owning通知先。
    ISessionWindowObserver& observer_;

    /// managerで確定した接続設定とsession専用Proxy境界。
    SessionLaunchSettings launch_settings_;

    /// top-level window。WM_NCDESTROY後はnullptrへ戻す。
    HWND window_{nullptr};

    // 子window HANDLEはtop-level windowが所有し、MainWindowは参照だけを保持する。
    HWND server_label_{nullptr};  ///< 接続先label。
    HWND server_combo_{nullptr};  ///< 接続先履歴を表示する読取専用combo。
    HWND domain_label_{nullptr};  ///< RDP domain label。
    HWND domain_combo_{nullptr};  ///< session profileのdomain表示。
    HWND user_name_label_{nullptr};  ///< RDP user name label。
    HWND user_name_combo_{nullptr};  ///< session profileのuser name表示。
    HWND connect_button_{nullptr};  ///< 自動接続前だけ存在する接続button。
    HWND disconnect_button_{nullptr};  ///< 現sessionだけを切断するbutton。
    HWND display_mode_label_{nullptr};  ///< 排他的表示mode群のlabel。
    HWND native_mode_radio_{nullptr};  ///< 等倍表示選択。
    HWND smart_sizing_mode_radio_{nullptr};  ///< Client側縮小表示選択。
    HWND dynamic_resolution_mode_radio_{nullptr};  ///< remote寸法追従選択。
    HWND full_screen_button_{nullptr};  ///< 全画面切替command control。
    HWND bridge_label_{nullptr};  ///< Bridge状態群のlabel。
    HWND bridge_state_text_{nullptr};  ///< Agent／Proxy状態表示。
    HWND bridge_echo_button_{nullptr};  ///< 手動Echo test command。
    HWND status_text_{nullptr};  ///< RDP状態と分類済みerror表示。

    /// COM STA上でActiveX lifecycleを所有するRDP host。
    rdp::RdpControlHost rdp_host_;

    /// handshake、Echo、stream routing、bounded SVC queueの所有者。
    bridge::ClientBridge client_bridge_;

    /// loopback listener、HTTP parser、Client multiplexerを所有するworker。
    proxy::ClientProxyWorker client_proxy_worker_;

    /// Proxy状態遷移logの重複を防ぐ直前snapshot。
    proxy::ClientProxyWorkerSnapshot last_proxy_snapshot_;

    /// COM callback復帰後のdialog表示まで保持するMicrosoft由来error。
    std::wstring pending_disconnect_error_;

    /// Create失敗時にWinMainへ返す原因。
    HRESULT create_error_{S_OK};

    /// UIと接続設定modelの正本となる排他的な表示mode。
    rdp::RdpDisplayMode display_mode_{
        rdp::RdpDisplayMode::NativeScroll};

    /// 手動Window変更で維持する、接続開始時remote desktop幅。
    long sizing_desktop_width_{0L};

    /// 手動Window変更で維持する、接続開始時remote desktop高さ。
    long sizing_desktop_height_{0L};

    /// 一回の手動move／size loop開始時に固定するtop-level Window幅。
    int manual_sizing_reference_width_{0};

    /// 一回の手動move／size loop開始時に固定するtop-level Window高さ。
    int manual_sizing_reference_height_{0};

    /// 全画面移行前のframeと通常／最大化配置を保持するtop-level Window style。
    LONG_PTR windowed_style_{0};

    /// 全画面からWindow modeへ戻すためのUSER32配置snapshot。
    WINDOWPLACEMENT windowed_placement_{};

    /// top-level Windowのframeと表示mode欄を全画面用へ切り替え済みであることを示す。
    bool container_full_screen_{false};

    /// session titleへ表示する直近のRDP lifecycle。
    rdp::RdpConnectionState title_rdp_state_{
        rdp::RdpConnectionState::NotInitialized};

    /// SetTimer成功後、対応するWM_TIMERだけを一度処理するためのflag。
    bool dynamic_resolution_timer_active_{false};

    /// 現在の表示更新要求に対して予約済みの一時失敗retry数。
    std::size_t dynamic_resolution_retries_scheduled_{0U};

    /// ログイン前SSL失敗後の有限再試行timerが予約済みであることを示す。
    bool pre_login_connection_retry_timer_active_{false};

    /// 現在の接続意図に対して予約したログイン前接続再試行数。
    std::size_t pre_login_connection_retries_scheduled_{0U};

    /// Microsoft認証dialogが表示処理中で、ZRdpがfocusを変更してはならない状態。
    bool authentication_dialog_active_{false};

    /// 現在の一回のConnect試行でMicrosoft認証dialogへ到達したことを示す。
    bool authentication_dialog_was_displayed_{false};

    /// credential UIと後続security UIを区別する、二回で飽和する表示回数。
    std::size_t authentication_dialog_display_count_{0U};

    /// 現在の未応答PINGへ設定した単調clock上の期限。未設定時は0。
    std::uint64_t bridge_echo_deadline_tick_{0U};

    /// Agent health PINGをqueueした単調clock時刻。未応答でない場合は0。
    std::uint64_t bridge_health_sent_tick_{0U};

    /// 現在の接続試行が一度ログイン完了し、成立済みsessionになったかを示す。
    bool login_completed_{false};

    /// 終了確認後の重複close処理を防ぐflag。
    bool closing_{false};
};

}  // namespace zrdp::client::app
