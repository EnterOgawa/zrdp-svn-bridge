//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file main_window.h
 * @brief ZRdp ClientのWin32 UIとRDP ActiveX event連携を定義する。
 * @version 01.00.14
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details MainWindowはsession専用COM STAであるUI threadに所属し、
 *          ActiveX操作、UI状態遷移、Bridge通知をsession内で直列化する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  session画面のAgent配置・確認付き起動を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/09  初期配置の停止段階とHRESULT診断を追加。
 * Ver.01.00.05   Shunji Ogawa  2026/10/09  自動入力のfocusとWindowsキー転送を確認。
 * Ver.01.00.06   Shunji Ogawa  2026/10/09  配置から独立した開発用Win+R testを追加。
 * Ver.01.00.07   Shunji Ogawa  2026/10/09  Win+R testの準備・送信・結果をtimer分離。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  単独キー診断を既定取消付きmenuへ追加。
 * Ver.01.00.09   Shunji Ogawa  2026/10/10  R単独のWindows入力比較結果を試行別に保持。
 * Ver.01.00.10   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.11   Shunji Ogawa  2026/10/10  配置なしの固定Ctrl+V／Enter診断と失効確認を追加。
 * Ver.01.00.12   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.13   Shunji Ogawa  2026/10/10  接続別設定と自動Agent管理・診断削除を反映。
 * Ver.01.00.14   Shunji Ogawa  2026/10/11  session破棄時のworker所有関係を訂正。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "client/bridge/client_bridge.h"
#include "client/app/session_launch_settings.h"
#include "client/proxy/client_proxy_worker.h"
#include "client/rdp/rdp_control_host.h"
#include "client/rdp/rdp_fixed_input_sequence.h"
#include "client/agent_deployment/automatic_deployment_status.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <memory>

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
	MainWindow( HINSTANCE instance, ISessionWindowObserver &observer,
			   SessionLaunchSettings settings) ;

	/**
	 * @brief sessionのProxy workerを停止し、COM STA終了前にhosted ActiveXを解放する。
	 */
	~MainWindow() override ;

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	MainWindow( const MainWindow &)							  = delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	MainWindow				   &operator=( const MainWindow &) = delete ;

	/**
	 * @brief main application windowと子controlを生成して表示する。
	 * @param[in] show_command WinMainから渡された初期表示command。
	 * @return main windowを利用可能にできた場合はtrue。
	 */
	[[nodiscard]] bool			Create( int show_command) ;

	/**
	 * @brief Tab移動等のdialog互換messageをこのsession階層で処理する。
	 * @param[in] message session専用message loopが取得した一件。
	 * @return このWindow階層で処理した場合はtrue。
	 */
	[[nodiscard]] bool			ProcessDialogMessage( MSG &message) const noexcept ;

	/**
	 * @brief 現在のsessionトップレベルHWNDを返す。
	 * @return 生成前またはWM_NCDESTROY後はnullptr。
	 */
	[[nodiscard]] HWND			window() const noexcept ;
	/** @brief 認証済みの接続世代を返す。 @return 未認証・終了中は0。 */
	[[nodiscard]] std::uint64_t DeploymentEpoch() const noexcept ;
	/** @brief clipboardと接続世代を再確認してCtrl+Vだけを送る。
	 * @param[in] epoch 準備開始時の認証済み世代。
	 * @param[in] clipboard_sequence 確認直後のWindows clipboard世代。
	 * @return キー要求のHRESULT。配置成功は意味しない。
	 */
	[[nodiscard]] HRESULT		PasteDeploymentFiles( std::uint64_t epoch,
													 DWORD		   clipboard_sequence) noexcept ;
	/** @brief 固定配布driveのAgentだけの起動を予約する。
	 * @param[in] epoch 確認済み認証世代。
	 * @return 最初の入力を受付けた場合だけS_OK。起動成功ではない。
	 */
	[[nodiscard]] HRESULT		StartAutomaticDeployment( std::uint64_t epoch) noexcept ;
	/** @brief 入力進行と最初の停止理由を返す。 @return 同じ段階の固定数値snapshot。 */
	[[nodiscard]] agent_deployment::AutomaticDeploymentSnapshot
						   AutomaticDeploymentStatus() const noexcept ;

	/**
	 * @brief 所有copy化された一件のActiveX callbackをUI状態へ反映する。
	 * @param[in] event COM callback境界から移動されたevent。
	 */
	void				   OnRdpEvent( rdp::RdpEvent event) override ;

	/** @brief managerで承認した設定を所有STAへ適用する。
	 * @param[in] epoch 現在の認証世代。
	 * @param[in] ini 承認済みINI。空は停止・失効。
	 * @param[in] bootstrap 固定Agent更新を設定確認より先に実行する場合true。
	 * @return 同じ生存世代に採用できた場合true。
	 */
	[[nodiscard]] bool	   ApplyAgentConfiguration( std::uint64_t epoch, std::string ini,
												   bool bootstrap = false) ;
	/** @brief 対象RDPが前面で固定自動入力を開始可能か返す。 @return 安全条件が揃った場合true。 */
	[[nodiscard]] bool	   CanBeginAutomaticAgentInput() const noexcept ;
	/** @brief 完了した起動試行だけを有限復旧のため初期化する。
	 * @param[in] epoch managerが確認した認証世代。
	 * @return 入力中でない同じ世代ならtrue。
	 */
	[[nodiscard]] bool	   ResetAutomaticAgentLaunch( std::uint64_t epoch) noexcept ;
	/** @brief 自動復旧の有限停止を固定分類で表示し、旧設定のProxyを止める。
	 * @param[in] epoch 失敗した同じ認証世代。
	 */
	void				   MarkAgentLifecycleFailure( std::uint64_t epoch) noexcept ;
	/** @brief 設定確認と接続の数値状態を返す。 @return 0待機、1利用可、2再起動、3拒否、4異常。 */
	[[nodiscard]] unsigned AgentLifecycleStatus() const noexcept ;

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
	static LRESULT CALLBACK	   WindowProcedure( HWND window, UINT message, WPARAM wparam,
											   LPARAM lparam) noexcept ;

	/**
	 * @brief instanceへrouting済みのWin32 messageを処理する。
	 * @param[in] message Win32 message ID。
	 * @param[in] wparam message固有のWPARAM。
	 * @param[in] lparam message固有のLPARAM。
	 * @return 処理結果、未処理時はDefWindowProcWの結果。
	 */
	[[nodiscard]] LRESULT	   HandleMessage( UINT message, WPARAM wparam, LPARAM lparam) ;

	/**
	 * @brief main window classをprocessへ一度だけ登録する。
	 * @return 登録済みまたは登録成功の場合はtrue。
	 */
	[[nodiscard]] bool		   RegisterWindowClass() const noexcept ;

	/**
	 * @brief 接続操作、表示切替、状態表示用の全子controlを生成する。
	 * @return 必須controlをすべて生成できた場合はtrue。
	 */
	[[nodiscard]] bool		   CreateControls() ;

	/**
	 * @brief 現在のclient領域とDPIに合わせて子controlを配置する。
	 * @param[in] client_width main window client領域の幅。
	 * @param[in] client_height main window client領域の高さ。
	 *
	 * ActiveX hostは即時resizeし、動的解像度modeだけはdebounce更新を予約する。
	 */
	void					   LayoutControls( int client_width, int client_height) ;

	/**
	 * @brief 手動変更候補を接続時remote desktopのアスペクト比へ補正する。
	 * @param[in] sizing_edge `WM_SIZING`の`WMSZ_*`値。
	 * @param[in,out] bounds top-level Windowの画面座標矩形。
	 * @return 通常Windowの有効な接続比率を適用できた場合はtrue。
	 *
	 * 最大化、Windowsスナップ、全画面は`WM_SIZING`を経由しないため、
	 * Windows標準の配置を維持する。
	 */
	[[nodiscard]] bool		   ConstrainManualSizing( UINT sizing_edge, RECT &bounds) const noexcept ;

	/**
	 * @brief system所有のGUI fontを全子controlへ設定する。
	 */
	void					   ApplyControlFont() const noexcept ;

	// -------------------------------------------------------------------------
	// UI command処理
	// -------------------------------------------------------------------------

	/**
	 * @brief 入力を検証し、非機密RDP設定を適用して接続を開始する。
	 */
	void					   OnConnect() ;
	/** @brief 認証状態とAgent応答を各操作入口へ反映する。 */
	void					   UpdateAgentControls() noexcept ;
	/** @brief timerで固定入力を一段ずつ進め、世代変化で停止する。 */
	void					   AdvanceAutomaticDeployment() noexcept ;
	/** @brief 自分の起動command clipboardだけを終了する。 */
	void					   ClearAutomaticClipboard() noexcept ;

	/**
	 * @brief 現在のsession設定を再検証して一回のActiveX接続試行を開始する。
	 *
	 * 初回接続と有限再試行で共有し、passwordは受け取らずMicrosoft認証UIへ
	 * 引き続き委譲する。
	 */
	void					   BeginConnectionAttempt() ;

	/**
	 * @brief 観測済みログイン前切断に対する次の遅延再試行を予約する。
	 * @param[in] event 完全一致を確認済みの切断数値情報。
	 * @param[in] delay_milliseconds COM callback後に待機する有限時間。
	 * @return timerを予約できた場合はtrue。
	 */
	[[nodiscard]] bool		   SchedulePreLoginConnectionRetry( const rdp::RdpEvent &event,
															   std::uint32_t		delay_milliseconds) ;

	/**
	 * @brief 再試行回数を維持したまま予約timerだけを取り消す。
	 */
	void					   CancelPreLoginConnectionRetryTimer() noexcept ;

	/**
	 * @brief 予約timerと現在の接続意図に対する再試行回数を破棄する。
	 */
	void					   CancelPreLoginConnectionRetry() noexcept ;

	/**
	 * @brief 現在のRDP sessionへ非同期切断を要求する。
	 */
	void					   OnDisconnect() ;

	/**
	 * @brief Window modeとMicrosoft ActiveX全画面modeを切り替える。
	 */
	void					   OnFullScreen() ;

	/**
	 * @brief ActiveXと所有top-level Windowを一つの全画面状態へ遷移させる。
	 * @param[in] full_screen 全画面へ移行する場合はtrue。
	 * @return 両方の状態を適用できた場合はS_OK、失敗時は原因HRESULT。
	 *
	 * @note COM STA UI threadでだけ呼び出し、片側の失敗時は直前状態へrollbackする。
	 */
	[[nodiscard]] HRESULT	   ApplyFullScreenMode( bool full_screen) noexcept ;

	/**
	 * @brief 所有top-level Windowだけをborderless全画面または保存済み配置へ切り替える。
	 * @param[in] full_screen 全画面Windowへ移行する場合はtrue。
	 * @return 適用成功時はS_OK、既に同じ状態ならS_FALSE、失敗時は原因HRESULT。
	 */
	[[nodiscard]] HRESULT	   SetContainerFullScreen( bool full_screen) noexcept ;

	/**
	 * @brief UI／ActiveX eventから非同期化された全画面要求を実行して診断する。
	 * @param[in] full_screen 全画面へ移行する場合はtrue。
	 */
	void					   ExecuteFullScreenRequest( bool full_screen) ;

	/**
	 * @brief handshake済みAgentとの段階的binary Echo検証を開始する。
	 */
	void					   OnBridgeEchoTest() ;

	/**
	 * @brief 排他的な表示modeを接続前設定または成立済みsessionへ反映する。
	 * @param[in] requested_mode 利用者が選択した表示mode。
	 *
	 * Smart SizingはClient側描画だけ、動的解像度はremote desktop寸法を変更する。
	 * 実行時反映に失敗した場合は直前のmodeへrollbackする。
	 */
	void					   OnDisplayModeChanged( rdp::RdpDisplayMode requested_mode) ;

	/**
	 * @brief 接続中の確認と安全なActiveX shutdownを含む終了処理を行う。
	 */
	void					   OnClose() ;

	/**
	 * @brief 認証完了した現在の接続組を共通履歴ownerへ通知する。
	 */
	void					   RememberSuccessfulConnection() ;

	// -------------------------------------------------------------------------
	// Static Virtual Channel Bridge連携
	// -------------------------------------------------------------------------

	/**
	 * @brief RDP login完了後に新しいHELLO sessionとretry timerを開始する。
	 */
	void					   StartBridgeSession() ;

	/**
	 * @brief RDP切断または再接続開始時に旧Bridge stateを破棄する。
	 */
	void					   StopBridgeSession() noexcept ;

	/**
	 * @brief bounded queueの送信要求をCOM callback外のUI messageへpostする。
	 */
	void					   QueueBridgeSendMessage() noexcept ;

	/**
	 * @brief 一回のUI messageで有限数のSVC chunkをActiveXへ送信する。
	 */
	void					   DrainBridgeSendQueue() ;

	/**
	 * @brief worker通知を受け、outbound FrameとProxy状態をUI threadへ反映する。
	 */
	void					   OnProxyWorkerNotification() ;

	/**
	 * @brief ClientBridgeがdecodeしたstream Frameをbounded worker queueへ移す。
	 * @return 全Frameをqueueできた場合はtrue。
	 */
	[[nodiscard]] bool		   DrainInboundStreamFramesToProxy() noexcept ;

	/**
	 * @brief HELLO／Echo／CLOSE遷移をProxy workerのmultiplexer sessionへ対応させる。
	 * @param[in] before Channel処理前のBridge state。
	 * @param[in] after Channel処理後のBridge state。
	 * @return worker commandを予約できた場合はtrue。
	 */
	[[nodiscard]] bool		   SynchronizeProxyBridgeSession( bridge::ClientBridgeState before,
															 bridge::ClientBridgeState after) noexcept ;

	/**
	 * @brief HELLO_ACK待ちの間だけ1秒周期の再送timerを維持する。
	 */
	void					   UpdateBridgeHandshakeTimer() noexcept ;

	/**
	 * @brief 未応答PINGがある間だけ応答期限timerを維持する。
	 */
	void					   UpdateBridgeEchoTimer() noexcept ;

	/**
	 * @brief handshake成立中だけAgent死活確認の周期timerを維持する。
	 */
	void					   UpdateBridgeHealthTimer() noexcept ;

	/**
	 * @brief 現在Bridge snapshotを専用状態欄とEcho buttonへ反映する。
	 */
	void					   UpdateBridgeUi() ;

	/**
	 * @brief Bridge state遷移だけを固定eventと数値fieldで診断記録する。
	 * @param[in] before 処理前state。
	 * @param[in] after 処理後snapshot。
	 */
	void					   LogBridgeTransition( bridge::ClientBridgeState		   before,
												   const bridge::ClientBridgeSnapshot &after) const noexcept ;

	// -------------------------------------------------------------------------
	// UI状態・表示helper
	// -------------------------------------------------------------------------

	/**
	 * @brief RDP lifecycleに応じて各controlの操作可否を更新する。
	 * @param[in] state UIへ反映するRDP接続状態。
	 */
	void					   UpdateUiForState( rdp::RdpConnectionState state) noexcept ;

	/**
	 * @brief 現在modeに応じて全画面buttonの表示文言を更新する。
	 * @param[in] full_screen ActiveXが全画面中の場合はtrue。
	 */
	void					   UpdateFullScreenButton( bool full_screen) ;

	/**
	 * @brief Window mode専用の表示mode一行と全画面buttonを一括表示または非表示にする。
	 * @param[in] visible 表示する場合はtrue。
	 */
	void					   SetDisplayControlsVisible( bool visible) const noexcept ;

	/**
	 * @brief resource文字列を状態欄へ設定する。
	 * @param[in] string_id 表示するstring resource ID。
	 */
	void					   SetStatus( UINT string_id) const ;

	/**
	 * @brief 一つの数値codeをresource formatへ埋めて状態欄へ表示する。
	 * @param[in] format_id 数値placeholderを持つresource ID。
	 * @param[in] code 表示するActiveX code。
	 */
	void					   SetStatusWithCode( UINT format_id, long code) const ;

	/**
	 * @brief 切断reasonを秘密情報なしで状態欄へ表示する。
	 * @param[in] reason ActiveX disconnect reason。
	 * @param[in] extended_reason ActiveX extended disconnect reason。
	 */
	void					   SetDisconnectedStatus( long reason, long extended_reason) const ;

	/**
	 * @brief 選択済みActiveX versionを含む初期状態を表示する。
	 */
	void					   SetInitialStatus() const ;

	/**
	 * @brief COM callback復帰後に必要な切断errorだけを所有bufferへ準備する。
	 * @param[in] event Windows由来の切断診断を持つevent。
	 *
	 * 成立済みsessionの外部切断は呼出し前にWindow終了へ分岐する。
	 * ZRdpからの切断、credential UIの利用者取消、終了処理でも表示を抑止する。
	 */
	void					   QueueDisconnectError( const rdp::RdpEvent &event) ;

	/**
	 * @brief resource messageとHRESULTをmodal error dialogへ表示する。
	 * @param[in] message_id 利用者向けresource ID。
	 * @param[in] error 分類済みHRESULT。
	 */
	void					   ShowError( UINT message_id, HRESULT error) const ;

	/**
	 * @brief 3つのラジオボタンを指定表示modeと一致させる。
	 * @param[in] mode 選択状態として表示するmode。
	 */
	void					   SetDisplayModeSelection( rdp::RdpDisplayMode mode) const noexcept ;

	/**
	 * @brief 最後のwindow resizeから250 ms後の動的解像度更新を予約する。
	 *
	 * 等倍表示、Smart Sizing、未接続では予約しない。ActiveX全画面では
	 * monitor全体、Window表示ではRDP child領域への更新を予約する。
	 */
	void					   ScheduleDynamicResolutionUpdate() ;

	/**
	 * @brief 指定delayで一回の動的解像度timerを予約する。
	 * @param[in] delay_milliseconds 次回試行までの有限時間。
	 * @return timerを予約できた場合はtrue。
	 */
	[[nodiscard]] bool		   ArmDynamicResolutionTimer( UINT delay_milliseconds) noexcept ;

	/**
	 * @brief 一時的なActiveX失敗に対する次回試行を有限回だけ予約する。
	 * @param[in] error 直前の動的解像度APIが返したHRESULT。
	 * @return 再試行を予約した場合はtrue。
	 */
	[[nodiscard]] bool		   ScheduleDynamicResolutionRetry( HRESULT error) noexcept ;

	/**
	 * @brief retry回数を維持したまま予約中timerだけを取消す。
	 */
	void					   CancelDynamicResolutionTimer() noexcept ;

	/**
	 * @brief 予約中timerと現在要求のretry回数を取消す。
	 */
	void					   CancelDynamicResolutionUpdate() noexcept ;

	/**
	 * @brief Window／全画面に対応する寸法を成立済みremote sessionへ適用する。
	 * @return 更新不要時はS_FALSE、API成功時はS_OK、失敗時は原因HRESULT。
	 */
	[[nodiscard]] HRESULT	   ApplyDynamicResolutionUpdate() noexcept ;

	/**
	 * @brief 動的更新失敗後に等倍表示へ縮退して利用者へ通知する。
	 * @param[in] error 動的解像度APIが返したHRESULT。
	 */
	void					   HandleDynamicResolutionFailure( HRESULT error) ;

	/**
	 * @brief 表示mode一行を除いたActiveX表示領域を計算する。
	 * @param[in] client_width main window client領域の幅。
	 * @param[in] client_height main window client領域の高さ。
	 * @return ActiveX hostへ適用する子window矩形。
	 */
	[[nodiscard]] RECT		   RdpBounds( int client_width, int client_height) const noexcept ;

	/**
	 * @brief 96 DPI基準値を現在windowのDPIへ変換する。
	 * @param[in] value 96 DPI基準のpixel値。
	 * @return 現在DPIへscaleしたpixel値。
	 */
	[[nodiscard]] int		   Scale( int value) const noexcept ;

	/**
	 * @brief UTF-16 string resourceを所有文字列として読込む。
	 * @param[in] string_id 読み込むresource ID。
	 * @return 読込み結果。存在しない場合は空文字列。
	 */
	[[nodiscard]] std::wstring LoadStringResource( UINT string_id) const ;

	/**
	 * @brief Win32 controlの現在textをbounded所有文字列へcopyする。
	 * @param[in] control 読込み対象の子window。
	 * @return control text。読込み不能時は空文字列。
	 */
	[[nodiscard]] std::wstring ReadWindowText( HWND control) const ;

	/**
	 * @brief 接続先、RDP／Agent状態、session専用Proxy統計をtitleへ反映する。
	 *
	 * title表示の失敗で成立中のRDP sessionとProxy転送を停止しない。
	 */
	void					   UpdateWindowTitle() noexcept ;

	/// resourceとwindow classのmodule instance。MainWindowは所有しない。
	HINSTANCE				   instance_ ;

	/// application lifetime中に生存する非owning通知先。
	ISessionWindowObserver	  &observer_ ;

	/// managerで確定した接続設定とsession専用Proxy境界。
	SessionLaunchSettings	   launch_settings_ ;

	/// top-level window。WM_NCDESTROY後はnullptrへ戻す。
	HWND					   window_{nullptr} ;

	// 子window HANDLEはtop-level windowが所有し、MainWindowは参照だけを保持する
	/// 接続先label。
	HWND					   server_label_{nullptr} ;
	/// 接続先履歴を表示する読取専用combo。
	HWND					   server_combo_{nullptr} ;
	/// RDP domain label。
	HWND					   domain_label_{nullptr} ;
	/// session profileのdomain表示。
	HWND					   domain_combo_{nullptr} ;
	/// RDP user name label。
	HWND					   user_name_label_{nullptr} ;
	/// session profileのuser name表示。
	HWND					   user_name_combo_{nullptr} ;
	/// 自動接続前だけ存在する接続button。
	HWND					   connect_button_{nullptr} ;
	/// 現sessionだけを切断するbutton。
	HWND					   disconnect_button_{nullptr} ;
	/// 排他的表示mode群のlabel。
	HWND					   display_mode_label_{nullptr} ;
	/// 等倍表示選択。
	HWND					   native_mode_radio_{nullptr} ;
	/// Client側縮小表示選択。
	HWND					   smart_sizing_mode_radio_{nullptr} ;
	/// remote寸法追従選択。
	HWND					   dynamic_resolution_mode_radio_{nullptr} ;
	/// 全画面切替command control。
	HWND					   full_screen_button_{nullptr} ;
	/** @brief 入力段階の結果を固定数値で記録し、失敗時は残りの入力を停止する。
	 * @param[in] stage 実行または拒否した段階。
	 * @param[in] reason 固定分類。Noneは進行。
	 * @param[in] result 元のAPI HRESULTまたは分類値。
	 */
	void SetAutomaticDeploymentResult( agent_deployment::AutomaticDeploymentStage  stage,
									  agent_deployment::AutomaticDeploymentReason reason,
									  HRESULT									  result) noexcept ;
	/** @brief focus準備の前後で同じ入力段階の安全条件を再確認する。
	 * @param[in] input_stage 送信予定の固定入力段階。
	 * @param[in] expected_phase 準備開始時の入力phase。
	 * @param[in] epoch 準備開始時の認証世代。
	 * @param[in] acquire_focus 初回準備だけtrue。後続は変更せずreadbackする。
	 * @return 全条件と入力準備を確認できた場合だけS_OK。
	 */
	[[nodiscard]] HRESULT
	PrepareAutomaticDeploymentInput( agent_deployment::AutomaticDeploymentStage input_stage,
									int expected_phase, std::uint64_t epoch,
									bool acquire_focus) noexcept ;
	/** @brief COM照会後にも同じ固定入力のowner条件が成立するかを確認する。
	 * @param[in] phase 予約した固定入力段階。
	 * @param[in] epoch 予約時の認証世代。
	 * @return 世代・phase・drive・Bridge・clipboard・UI条件が全て一致した場合true。
	 */
	[[nodiscard]] bool AutomaticInputAllowed( int phase, std::uint64_t epoch) const noexcept ;
	/** @brief 他Window切替・最小化で未送信Agent入力だけを失効する。 */
	void			   CancelPendingAgentInput() noexcept ;
	/// 最初の停止理由と最後の実行段階。所有STAだけが変更する。
	agent_deployment::AutomaticDeploymentSnapshot automatic_result_ ;
	/// 固定入力の進行。0未開始、1Run、2Paste、3Enter、4応答待ち、-1失敗。
	int											  automatic_phase_{0} ;
	/// 最小待機・期限・再入拒否を持つ、所有STA専用の一回固定sequence。
	rdp::FixedInputSequence						  automatic_sequence_{} ;
	/// 自動入力開始時に固定した認証世代。
	std::uint64_t								  automatic_epoch_{0U} ;
	/// CF_UNICODETEXTへ公開した固定commandの世代。
	DWORD										  automatic_clipboard_{0U} ;
	/// Bridge状態群のlabel。
	HWND										  bridge_label_{nullptr} ;
	/// Agent／Proxy状態表示。
	HWND										  bridge_state_text_{nullptr} ;
	/// 手動Echo test command。
	HWND										  bridge_echo_button_{nullptr} ;
	/// RDP状態と分類済みerror表示。
	HWND										  status_text_{nullptr} ;

	/// COM STA上でActiveX lifecycleを所有するRDP host。
	rdp::RdpControlHost							  rdp_host_ ;

	/// handshake、Echo、stream routing、bounded SVC queueの所有者。
	bridge::ClientBridge						  client_bridge_ ;

	/// loopback listener、HTTP parser、Client multiplexerを所有するworker。
	proxy::ClientProxyWorker					  client_proxy_worker_ ;

	/// Proxy状態遷移logの重複を防ぐ直前snapshot。
	proxy::ClientProxyWorkerSnapshot			  last_proxy_snapshot_ ;

	/// COM callback復帰後のdialog表示まで保持するMicrosoft由来error。
	std::wstring								  pending_disconnect_error_ ;

	/// Create失敗時にWinMainへ返す原因。
	HRESULT										  create_error_{S_OK} ;

	/// UIと接続設定modelの正本となる排他的な表示mode。
	rdp::RdpDisplayMode							  display_mode_{rdp::RdpDisplayMode::NativeScroll} ;

	/// 手動Window変更で維持する、接続開始時remote desktop幅。
	long										  sizing_desktop_width_{0L} ;

	/// 手動Window変更で維持する、接続開始時remote desktop高さ。
	long										  sizing_desktop_height_{0L} ;

	/// 一回の手動move／size loop開始時に固定するtop-level Window幅。
	int											  manual_sizing_reference_width_{0} ;

	/// 一回の手動move／size loop開始時に固定するtop-level Window高さ。
	int											  manual_sizing_reference_height_{0} ;

	/// 全画面移行前のframeと通常／最大化配置を保持するtop-level Window style。
	LONG_PTR									  windowed_style_{0} ;

	/// 全画面からWindow modeへ戻すためのUSER32配置snapshot。
	WINDOWPLACEMENT								  windowed_placement_{} ;

	/// top-level Windowのframeと表示mode欄を全画面用へ切り替え済みであることを示す。
	bool										  container_full_screen_{false} ;

	/// session titleへ表示する直近のRDP lifecycle。
	rdp::RdpConnectionState title_rdp_state_{rdp::RdpConnectionState::NotInitialized} ;
	/// 切断・再接続で古い配置要求を拒否する認証完了世代。
	std::uint64_t			deployment_epoch_{0U} ;

	/// SetTimer成功後、対応するWM_TIMERだけを一度処理するためのflag。
	bool					dynamic_resolution_timer_active_{false} ;

	/// 現在の表示更新要求に対して予約済みの一時失敗retry数。
	std::size_t				dynamic_resolution_retries_scheduled_{0U} ;

	/// ログイン前SSL失敗後の有限再試行timerが予約済みであることを示す。
	bool					pre_login_connection_retry_timer_active_{false} ;

	/// 現在の接続意図に対して予約したログイン前接続再試行数。
	std::size_t				pre_login_connection_retries_scheduled_{0U} ;

	/// Microsoft認証dialogが表示処理中で、ZRdpがfocusを変更してはならない状態。
	bool					authentication_dialog_active_{false} ;

	/// 現在の一回のConnect試行でMicrosoft認証dialogへ到達したことを示す。
	bool					authentication_dialog_was_displayed_{false} ;

	/// credential UIと後続security UIを区別する、二回で飽和する表示回数。
	std::size_t				authentication_dialog_display_count_{0U} ;

	/// 現在の未応答PINGへ設定した単調clock上の期限。未設定時は0。
	std::uint64_t			bridge_echo_deadline_tick_{0U} ;

	/// Agent health PINGをqueueした単調clock時刻。未応答でない場合は0。
	std::uint64_t			bridge_health_sent_tick_{0U} ;

	/// 現在の接続試行が一度ログイン完了し、成立済みsessionになったかを示す。
	bool					login_completed_{false} ;

	/// 終了確認後の重複close処理を防ぐflag。
	bool					closing_{false} ;
	/// 接続managerが承認した完全INI。実endpointはlogへ出力しない。
	std::string				agent_configuration_ ;
	/// 現HELLOへ設定を一回だけ要求した状態。
	bool					agent_configuration_sent_{false} ;
	/// 接続時と復旧時の固定EXE更新を設定確認より先に行う。
	bool					agent_bootstrap_required_{true} ;
	/// ACKReadyを確認してProxyへ開始要求済みの状態。
	bool					agent_proxy_started_{false} ;
} ;

} // namespace zrdp::client::app
