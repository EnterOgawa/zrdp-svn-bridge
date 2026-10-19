//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file diagnostic_log.cpp
 * @brief 秘密情報を含まない固定形式の診断ログを非同期で永続化する。
 * @version 01.00.09
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * 呼び出し側のスレッドでは有限長キューへの投入だけを行い、ファイル操作は
 * 専用ワーカーへ集約する。ログ項目名は列挙値から決定し、任意文字列や通信
 * payload を受け取らないことで、認証情報や接続先情報の混入を防ぐ。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/09  初期配置の停止段階とHRESULT診断を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  自動入力のfocusとWindowsキー転送を確認。
 * Ver.01.00.03   Shunji Ogawa  2026/10/09  配置から独立した開発用Win+R testを追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Win単独とR単独の固定結果eventを追加。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  RのWindows入力比較eventとWin32数値fieldを追加。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  固定Win+RのSendInput比較と停止境界を追加。
 * Ver.01.00.07   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  配置なしの固定Ctrl+V／Enter診断と失効確認を追加。
 * Ver.01.00.09   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "common/diagnostic_log.h"

#include <Windows.h>
#include <Objbase.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace zrdp::common {
namespace {

// ログのメモリ使用量、パス探索、保存世代数を有限値へ制限するための上限値
constexpr std::size_t kMaximumQueuedRecords		   = 512U ;
// EXE配置照会を始めるUTF-16 bufferのcode unit数
constexpr std::size_t kInitialPathBufferCharacters = 1024U ;
// path照会のbuffer増加を止めるUTF-16 code unit上限
constexpr std::size_t kMaximumPathBufferCharacters = 32768U ;
// EXE隣接で固定診断logを置く専用directory名
constexpr wchar_t	  kLogDirectoryName[]		   = L"logs" ;
// EXE隣接へ保存できない場合のLOCALAPPDATA内専用directory名
constexpr wchar_t	  kFallbackDirectoryName[]	   = L"ZRdpSvnBridge" ;
// 開発PC側Clientの固定診断log名。接続先をfile名へ含めない
constexpr wchar_t	  kClientLogFileName[]		   = L"ZRdpSvnBridge.log" ;
// RDP user session側Agentの固定診断log名。endpointをfile名へ含めない
constexpr wchar_t	  kAgentLogFileName[]		   = L"ZRdpSvnAgent.log" ;

#ifndef ZRDP_VERSION
#error "ZRDP_VERSION must be provided by the build system."
#endif

// ---------------------------------------------------------------------------
// 固定ログ語彙
// ---------------------------------------------------------------------------

/**
 * @brief プロセス種別に対応する固定ログファイル名を返す。
 * @param[in] role 保存先を選択するプロセス種別。
 * @return Client／Agentそれぞれの固定名。不正な種別では空文字列。
 *
 * 同じディレクトリへ両EXEを配置しても、ログと世代ファイルを相互に
 * 上書きしないよう、プロセス種別ごとに異なる名前を使用する。
 */
[[nodiscard]] std::wstring_view LogFileName( const DiagnosticRole role) noexcept
{
	// processの役割に応じて固定の診断file名を選び、任意pathを受け付けない
	switch( role) {
	// Client診断は開発PC側の専用fileへ分離する
	case DiagnosticRole::Client:
		return kClientLogFileName ;
	// Agent診断は支給PC側の専用fileへ分離する
	case DiagnosticRole::Agent:
		return kAgentLogFileName ;
	}

	return {} ;
}

/**
 * @brief プロセス種別をログ用の固定文字列へ変換する。
 * @param[in] role 変換するプロセス種別。
 * @return 秘密情報を含まない静的文字列。
 */
[[nodiscard]] std::string_view RoleName( const DiagnosticRole role) noexcept
{
	// processの役割をpayloadなしの固定分類文字列へ変換する
	switch( role) {
	// Clientの実行場所を識別する固定role名を選ぶ
	case DiagnosticRole::Client:
		return "client" ;
	// Agentの実行場所を識別する固定role名を選ぶ
	case DiagnosticRole::Agent:
		return "agent" ;
	}

	return "unknown" ;
}

/**
 * @brief 重要度をログ用の固定文字列へ変換する。
 * @param[in] level 変換する重要度。
 * @return 秘密情報を含まない静的文字列。
 */
[[nodiscard]] std::string_view LevelName( const DiagnosticLevel level) noexcept
{
	// severityを固定の診断level名へ変換する
	switch( level) {
	// 正常な状態変化と完了は情報levelとして記録する
	case DiagnosticLevel::Information:
		return "INFO" ;
	// 接続継続可能な異常は警告levelとして記録する
	case DiagnosticLevel::Warning:
		return "WARN" ;
	// 操作失敗または継続不能はerror levelとして記録する
	case DiagnosticLevel::Error:
		return "ERROR" ;
	}

	return "UNKNOWN" ;
}

/**
 * @brief 診断イベントをログ用の固定イベント名へ変換する。
 * @param[in] event 変換するイベント。
 * @return 任意文字列を含まない静的イベント名。
 */
[[nodiscard]] std::string_view EventName( const DiagnosticEvent event) noexcept
{
	// enumで限定されたeventだけを固定文字列へ変換して自由形式messageを禁止する
	switch( event) {
	// 固定event名を選ぶ: processが起動条件の確認を開始したことを記録する
	case DiagnosticEvent::ProcessStarted:
		return "process.started" ;
	// 固定event名を選ぶ: processの資源解放と停止を記録する
	case DiagnosticEvent::ProcessStopping:
		return "process.stopping" ;
	// 固定event名を選ぶ: COM STAの初期化失敗を記録する
	case DiagnosticEvent::ComInitializationFailed:
		return "app.com_initialization_failed" ;
	// 固定event名を選ぶ: top-level Window生成の開始を記録する
	case DiagnosticEvent::MainWindowCreateStarted:
		return "ui.main_window_create_started" ;
	// 固定event名を選ぶ: top-level Window生成の成功を記録する
	case DiagnosticEvent::MainWindowCreated:
		return "ui.main_window_created" ;
	// 固定event名を選ぶ: top-level Window生成の失敗を記録する
	case DiagnosticEvent::MainWindowCreateFailed:
		return "ui.main_window_create_failed" ;
	// 固定event名を選ぶ: Microsoft Control生成の開始を記録する
	case DiagnosticEvent::ActiveXCreateStarted:
		return "rdp.activex_create_started" ;
	// 固定event名を選ぶ: Microsoft Control生成の成功を記録する
	case DiagnosticEvent::ActiveXCreated:
		return "rdp.activex_created" ;
	// 固定event名を選ぶ: Microsoft Control生成の失敗を記録する
	case DiagnosticEvent::ActiveXCreateFailed:
		return "rdp.activex_create_failed" ;
	// 固定event名を選ぶ: 利用可能なMicrosoft Controlの選択結果を記録する
	case DiagnosticEvent::ActiveXControlSelected:
		return "rdp.activex_control_selected" ;
	// 固定event名を選ぶ: 認証UIの親をControlの既定Windowへ委譲したことを記録する
	case DiagnosticEvent::ActiveXDefaultDialogParentSelected:
		return "rdp.activex_default_dialog_parent_selected" ;
	// 固定event名を選ぶ: 認証dialogの互換設定の結果を記録する
	case DiagnosticEvent::SecurityDialogConfigurationReturned:
		return "rdp.security_dialog_configuration_returned" ;
	// 固定event名を選ぶ: server認証level設定の結果を記録する
	case DiagnosticEvent::AuthenticationLevelConfigurationReturned:
		return "rdp.authentication_level_configuration_returned" ;
	// 固定event名を選ぶ: CredSSP設定の結果を記録する
	case DiagnosticEvent::CredSspConfigurationReturned:
		return "rdp.credssp_configuration_returned" ;
	// 固定event名を選ぶ: SVCの接続前登録の結果を記録する
	case DiagnosticEvent::VirtualChannelRegistrationReturned:
		return "rdp.virtual_channel_registration_returned" ;
	// 固定event名を選ぶ: DPAPI履歴の読込みの結果を記録する
	case DiagnosticEvent::ConnectionHistoryLoadReturned:
		return "ui.connection_history_load_returned" ;
	// 固定event名を選ぶ: 接続履歴の暗号化保存の結果を記録する
	case DiagnosticEvent::ConnectionHistorySaveReturned:
		return "ui.connection_history_save_returned" ;
	// 固定event名を選ぶ: 利用者から新しい接続要求を受けたことを記録する
	case DiagnosticEvent::ConnectButtonClicked:
		return "ui.connect_button_clicked" ;
	// 固定event名を選ぶ: 接続入力が事前条件を満たさず拒否したことを記録する
	case DiagnosticEvent::ConnectInputRejected:
		return "ui.connect_input_rejected" ;
	// 固定event名を選ぶ: RDP接続前設定の適用の開始を記録する
	case DiagnosticEvent::ConnectionSettingsApplyStarted:
		return "rdp.connection_settings_apply_started" ;
	// 固定event名を選ぶ: RDP接続前設定の適用の結果を記録する
	case DiagnosticEvent::ConnectionSettingsApplyReturned:
		return "rdp.connection_settings_apply_returned" ;
	// 固定event名を選ぶ: ActiveXの接続要求の開始を記録する
	case DiagnosticEvent::ConnectCallStarted:
		return "rdp.connect_call_started" ;
	// 固定event名を選ぶ: ActiveXの接続要求の結果を記録する
	case DiagnosticEvent::ConnectCallReturned:
		return "rdp.connect_call_returned" ;
	// 固定event名を選ぶ: 条件に一致するlogin前失敗の有限再試行予約を記録する
	case DiagnosticEvent::PreLoginConnectionRetryScheduled:
		return "rdp.pre_login_retry_scheduled" ;
	// 固定event名を選ぶ: 予約済みのlogin前再試行の開始を記録する
	case DiagnosticEvent::PreLoginConnectionRetryStarted:
		return "rdp.pre_login_retry_started" ;
	// 固定event名を選ぶ: 有限再試行後のlogin成立を記録する
	case DiagnosticEvent::PreLoginConnectionRetryCompleted:
		return "rdp.pre_login_retry_completed" ;
	// 固定event名を選ぶ: login前再試行上限への到達を記録する
	case DiagnosticEvent::PreLoginConnectionRetryExhausted:
		return "rdp.pre_login_retry_exhausted" ;
	// 固定event名を選ぶ: local操作による接続終了要求を記録する
	case DiagnosticEvent::DisconnectButtonClicked:
		return "ui.disconnect_button_clicked" ;
	// 固定event名を選ぶ: ActiveXの切断要求の結果を記録する
	case DiagnosticEvent::DisconnectCallReturned:
		return "rdp.disconnect_call_returned" ;
	// 固定event名を選ぶ: containerとControlの全画面切替の結果を記録する
	case DiagnosticEvent::FullScreenCallReturned:
		return "rdp.full_screen_call_returned" ;
	// 固定event名を選ぶ: Client側表示の拡縮設定の結果を記録する
	case DiagnosticEvent::SmartSizingCallReturned:
		return "rdp.smart_sizing_call_returned" ;
	// 固定event名を選ぶ: remote desktop寸法の更新の結果を記録する
	case DiagnosticEvent::DynamicResolutionCallReturned:
		return "rdp.dynamic_resolution_call_returned" ;
	// 固定event名を選ぶ: Windowの終了要求を受けたことを記録する
	case DiagnosticEvent::CloseRequested:
		return "ui.close_requested" ;
	// 固定event名を選ぶ: 旧streamを破棄し新しいBridge交渉を開始したことを記録する
	case DiagnosticEvent::BridgeSessionStarted:
		return "bridge.session_started" ;
	// 固定event名を選ぶ: HELLO交渉が成立したことを記録する
	case DiagnosticEvent::BridgeReady:
		return "bridge.ready" ;
	// 固定event名を選ぶ: stream受付を停止してbinary Echoを開始したことを記録する
	case DiagnosticEvent::BridgeEchoStarted:
		return "bridge.echo_started" ;
	// 固定event名を選ぶ: Echo内容と順序の検証成功を記録する
	case DiagnosticEvent::BridgeEchoPassed:
		return "bridge.echo_passed" ;
	// 固定event名を選ぶ: peer応答期限を超えたことを記録する
	case DiagnosticEvent::BridgePeerTimeout:
		return "bridge.peer_timeout" ;
	// 固定event名を選ぶ: Bridgeの継続不能な異常を記録する
	case DiagnosticEvent::BridgeFailed:
		return "bridge.failed" ;
	// 固定event名を選ぶ: Proxy workerの状態遷移を記録する
	case DiagnosticEvent::ClientProxyStateChanged:
		return "proxy.client_state_changed" ;
	// 固定event名を選ぶ: Proxy listenerの開始失敗を記録する
	case DiagnosticEvent::ClientProxyStartFailed:
		return "proxy.client_start_failed" ;
	// 固定event名を選ぶ: current RDP sessionの検証の結果を記録する
	case DiagnosticEvent::AgentSessionValidationReturned:
		return "agent.session_validation_returned" ;
	// 固定event名を選ぶ: 同じRDP sessionのSVC openの結果を記録する
	case DiagnosticEvent::AgentChannelOpenReturned:
		return "agent.channel_open_returned" ;
	// 固定event名を選ぶ: allowlist全体の検証の結果を記録する
	case DiagnosticEvent::AgentAllowlistLoadReturned:
		return "agent.allowlist_load_returned" ;
	// 固定event名を選ぶ: AgentのWinsock初期化の結果を記録する
	case DiagnosticEvent::AgentWinsockInitializationReturned:
		return "agent.winsock_initialization_returned" ;
	// 固定event名を選ぶ: allowlist限定中継を有効にしたことを記録する
	case DiagnosticEvent::AgentStreamRelayEnabled:
		return "agent.stream_relay_enabled" ;
	// 固定event名を選ぶ: 旧SVC handleの読込み失敗を記録する
	case DiagnosticEvent::AgentChannelReadFailed:
		return "agent.channel_read_failed" ;
	// 固定event名を選ぶ: SVCの完全書込みに失敗したことを記録する
	case DiagnosticEvent::AgentChannelWriteFailed:
		return "agent.channel_write_failed" ;
	// 固定event名を選ぶ: RDP接続と認証処理が進行中である
	case DiagnosticEvent::EventConnecting:
		return "rdp.event.connecting" ;
	// 固定event名を選ぶ: RDP transportが成立したが、ログイン完了をまだ保証しない
	case DiagnosticEvent::EventConnected:
		return "rdp.event.connected" ;
	// 固定event名を選ぶ: 認証完了後のsession利用開始を通知する
	case DiagnosticEvent::EventLoginComplete:
		return "rdp.event.login_complete" ;
	// 固定event名を選ぶ: RDP接続を保持せず、新しい接続操作を受け付ける
	case DiagnosticEvent::EventDisconnected:
		return "rdp.event.disconnected" ;
	// 固定event名を選ぶ: Controlが全画面状態へ移ったことを通知する
	case DiagnosticEvent::EventEnterFullScreen:
		return "rdp.event.enter_full_screen" ;
	// 固定event名を選ぶ: Controlが通常Window状態へ戻ったことを通知する
	case DiagnosticEvent::EventLeaveFullScreen:
		return "rdp.event.leave_full_screen" ;
	// 固定event名を選ぶ: 受信BSTRのbinary dataを所有copyとしてworkerへ渡す
	case DiagnosticEvent::EventChannelDataReceived:
		return "rdp.event.channel_data_received" ;
	// 固定event名を選ぶ: Controlからcontainer全体の全画面化を要求している
	case DiagnosticEvent::EventRequestGoFullScreen:
		return "rdp.event.request_go_full_screen" ;
	// 固定event名を選ぶ: Controlからcontainerの通常配置への復帰を要求している
	case DiagnosticEvent::EventRequestLeaveFullScreen:
		return "rdp.event.request_leave_full_screen" ;
	// 固定event名を選ぶ: 接続バーからSession Window全体の標準最小化を要求している
	case DiagnosticEvent::EventRequestContainerMinimize:
		return "rdp.event.request_container_minimize" ;
	// 固定event名を選ぶ: Controlの致命的errorを分類済み数値で通知する
	case DiagnosticEvent::EventFatalError:
		return "rdp.event.fatal_error" ;
	// 固定event名を選ぶ: 操作継続可能な警告を分類済み数値で通知する
	case DiagnosticEvent::EventWarning:
		return "rdp.event.warning" ;
	// 固定event名を選ぶ: Control破棄に先立つ接続終了の確認を通知する
	case DiagnosticEvent::EventConfirmClose:
		return "rdp.event.confirm_close" ;
	// 固定event名を選ぶ: Microsoft Controlによる同じsessionへの自動再接続を通知する
	case DiagnosticEvent::EventAutoReconnecting:
		return "rdp.event.auto_reconnecting" ;
	// 固定event名を選ぶ: RDPが自動回復し、新しいBridge交渉を開始できる
	case DiagnosticEvent::EventAutoReconnected:
		return "rdp.event.auto_reconnected" ;
	// 固定event名を選ぶ: 認証警告UIの表示中であり、containerはfocusを奪わない
	case DiagnosticEvent::EventAuthenticationWarningDisplayed:
		return "rdp.event.authentication_warning_displayed" ;
	// 固定event名を選ぶ: 認証警告UIの終了を通知し、利用者の判断を維持する
	case DiagnosticEvent::EventAuthenticationWarningDismissed:
		return "rdp.event.authentication_warning_dismissed" ;
	// 固定event名を選ぶ: ActiveX eventの検証またはUIへの配送失敗を記録する
	case DiagnosticEvent::EventDispatchFailed:
		return "rdp.event.dispatch_failed" ;
	// 固定event名を選ぶ: 外部callbackへ漏らさず捕捉した予期しない例外を記録する
	case DiagnosticEvent::UnexpectedException:
		return "app.unexpected_exception" ;
	// 固定event名を選ぶ: bounded log queueから記録を破棄した件数を記録する
	case DiagnosticEvent::LogRecordsDropped:
		return "log.records_dropped" ;
	// 初回配置の入力段階と固定分類だけを記録する
	case DiagnosticEvent::AutomaticDeploymentInputReturned:
		return "agent_deployment.input_returned" ;
	// 画面側の最初の停止段階を記録する
	case DiagnosticEvent::AutomaticDeploymentStopped:
		return "agent_deployment.stopped" ;
	// キー転送設定はhook modeと有効値だけを記録する
	case DiagnosticEvent::DeploymentKeyboardConfigurationReturned:
		return "rdp.deployment_keyboard_configuration_returned" ;
	// 前景Windowの識別情報は出さず固定focus分類だけを記録する
	case DiagnosticEvent::DeploymentInputFocusReturned:
		return "rdp.deployment_input_focus_returned" ;
	// 開発用testは接続情報や任意入力を記録しない
	case DiagnosticEvent::RunInputTestReturned:
		return "rdp.run_input_test_returned" ;
	// 実入力内容を取得せず、選択した固定診断だけを識別する
	case DiagnosticEvent::WinKeyInputTestReturned:
		return "rdp.win_key_input_test_returned" ;
	// メモ帳内容、IME状態、実接続先を記録しない
	case DiagnosticEvent::RKeyInputTestReturned:
		return "rdp.r_key_input_test_returned" ;
	// SendKeysとの比較結果だけを固定数値で識別する
	case DiagnosticEvent::RKeyEmulationTestReturned:
		return "rdp.r_key_emulation_test_returned" ;
	// 入力内容・key状態・Window識別子は記録しない
	case DiagnosticEvent::RKeyEmulationInputReturned:
		return "rdp.r_key_emulation_input_returned" ;
	// Win+R比較を従来SendKeysと配置結果から分離する
	case DiagnosticEvent::WinRKeyEmulationTestReturned:
		return "rdp.win_r_key_emulation_test_returned" ;
	// 受付数だけを保持し、Run画面の内容や接続先は記録しない
	case DiagnosticEvent::WinRKeyEmulationInputReturned:
		return "rdp.win_r_key_emulation_input_returned" ;
	// 起動文字列やRun画面の内容は記録しない
	case DiagnosticEvent::DeploymentRunInputReturned:
		return "rdp.deployment_run_input_returned" ;
	// clipboard内容や貼付け先は記録しない
	case DiagnosticEvent::DeploymentPasteInputReturned:
		return "rdp.deployment_paste_input_returned" ;
	// 固定起動の受付と実Agent応答を混同しない
	case DiagnosticEvent::AgentStartInputReturned:
		return "rdp.agent_start_input_returned" ;
	// 貼付け診断の進行結果は配置の結果と分ける
	case DiagnosticEvent::PasteKeyEmulationTestReturned:
		return "rdp.paste_key_emulation_test_returned" ;
	// clipboard内容や世代番号を記録しない
	case DiagnosticEvent::PasteKeyEmulationInputReturned:
		return "rdp.paste_key_emulation_input_returned" ;
	// Enter診断の進行結果を実行成功とは扱わない
	case DiagnosticEvent::EnterKeyEmulationTestReturned:
		return "rdp.enter_key_emulation_test_returned" ;
	// 入力先や文字・commandを記録しない
	case DiagnosticEvent::EnterKeyEmulationInputReturned:
		return "rdp.enter_key_emulation_input_returned" ;
	}

	return "unknown" ;
}

// ---------------------------------------------------------------------------
// 保存先とレコード書式
// ---------------------------------------------------------------------------

/**
 * @brief 実行中モジュールの絶対パスを取得する。
 * @return 取得したパス。Windows API の失敗または上限超過時は空文字列。
 */
[[nodiscard]] std::wstring ModulePath()
{
	std::vector<wchar_t> buffer( kInitialPathBufferCharacters) ;

	// 切り詰められていない実行ファイルパスが得られるまで、有限上限内で拡張する
	while( buffer.size() <= kMaximumPathBufferCharacters) {
		SetLastError( ERROR_SUCCESS) ;
		const DWORD length =
			GetModuleFileNameW( nullptr, buffer.data(), static_cast<DWORD>( buffer.size())) ;
		// API 失敗時は不完全なパスを保存先として使用しない
		if( length == 0U) {
			return {} ;
		}

		// Windows の版差を考慮し、戻り値と LastError の両方から完結を判定する
		if( length < buffer.size() - 1U ||
		   (length < buffer.size() && GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
			return std::wstring( buffer.data(), length) ;
		}

		buffer.resize( buffer.size() * 2U) ;
	}

	return {} ;
}

/**
 * @brief 現在ユーザーの LocalAppData パスを環境変数から取得する。
 * @return 検証済みパス。欠落、不正長、取得失敗時は空文字列。
 */
[[nodiscard]] std::wstring LocalAppDataPath()
{
	DWORD required = GetEnvironmentVariableW( L"LOCALAPPDATA", nullptr, 0U) ;
	// 異常な長さでのメモリ確保と切り詰めた保存先の使用を防ぐ
	if( required == 0U || required > kMaximumPathBufferCharacters) {
		return {} ;
	}

	std::wstring value( required, L'\0') ;
	const DWORD	 written = GetEnvironmentVariableW( L"LOCALAPPDATA", value.data(), required) ;
	// 取得途中で値が変化した場合も、不完全なパスを採用しない
	if( written == 0U || written >= required) {
		return {} ;
	}

	value.resize( written) ;
	return value ;
}

/**
 * @brief 一つのプロセス実行を識別する匿名セッションIDを生成する。
 * @return GUID、または GUID 生成失敗時のプロセス内一意性を補う代替ID。
 */
[[nodiscard]] std::string CreateSessionIdentifier() noexcept
{
	GUID				 session_id{} ;
	const HRESULT		 result = CoCreateGuid( &session_id) ;
	std::array<char, 64> text{} ;
	// GUID を利用できる場合は実ホスト情報を含めずにセッションを識別する
	if( SUCCEEDED( result)) {
		const int written = sprintf_s(
			text.data(), text.size(),
			"%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX", session_id.Data1,
			session_id.Data2, session_id.Data3, session_id.Data4[0], session_id.Data4[1],
			session_id.Data4[2], session_id.Data4[3], session_id.Data4[4], session_id.Data4[5],
			session_id.Data4[6], session_id.Data4[7]) ;
		// 書式化失敗時は下段の安全な代替IDへフォールバックする
		if( written > 0) {
			return std::string( text.data(), static_cast<std::size_t>( written)) ;
		}
	}

	FILETIME timestamp{} ;
	GetSystemTimeAsFileTime( &timestamp) ;
	const ULARGE_INTEGER timestamp_value{
		timestamp.dwLowDateTime,
		timestamp.dwHighDateTime,
	} ;
	const int written = sprintf_s( text.data(), text.size(), "fallback-%08lX-%016llX",
								  GetCurrentProcessId(), timestamp_value.QuadPart) ;
	return written > 0 ? std::string( text.data(), static_cast<std::size_t>( written))
					   : std::string( "fallback") ;
}

/**
 * @brief 現在UTC時刻を一つのログ行へ追加する。
 * @param[in,out] line 追記先のログ行。
 */
void AppendTimestamp( std::string &line)
{
	FILETIME file_time{} ;
	GetSystemTimePreciseAsFileTime( &file_time) ;

	SYSTEMTIME timestamp{} ;
	// 時刻変換に失敗しても、レコード書式を維持して診断処理を継続する
	if( !FileTimeToSystemTime( &file_time, &timestamp)) {
		line.append( "timestamp=unavailable") ;
		return ;
	}

	std::array<char, 64> text{} ;
	const int			 written =
		sprintf_s( text.data(), text.size(), "timestamp=%04hu-%02hu-%02huT%02hu:%02hu:%02hu.%03huZ",
				  timestamp.wYear, timestamp.wMonth, timestamp.wDay, timestamp.wHour,
				  timestamp.wMinute, timestamp.wSecond, timestamp.wMilliseconds) ;
	// 書式化失敗時は、欠落ではなく取得不能であることを固定語で記録する
	if( written > 0) {
		line.append( text.data(), static_cast<std::size_t>( written)) ;
	} else {
		line.append( "timestamp=unavailable") ;
	}
}

/**
 * @brief 許可済みの数値診断項目を一つのログ行へ追加する。
 * @param[in,out] line 追記先のログ行。
 * @param[in] field 項目名列挙値と数値。
 *
 * 項目名を列挙値へ限定し、URL・ユーザー名・payload 等の任意文字列を
 * ログAPIへ持ち込めない境界を構成する。
 */
void AppendField( std::string &line, const DiagnosticField field)
{
	std::array<char, 96> text{} ;
	int					 written = -1 ;

	// field分類に対応する数値形式だけを使い、任意textを記録しない
	switch( field.name) {
	// 固定数値fieldを書式化する: COM境界のHRESULTをbit patternで保持する
	case DiagnosticFieldName::HResult:
		written = sprintf_s( text.data(), text.size(), " hresult=0x%08lX",
							static_cast<unsigned long>( static_cast<std::uint32_t>( field.value))) ;
		break ;

	// HRESULTへ変換する前のWin32 errorをbit patternのまま残す
	case DiagnosticFieldName::Win32Error:
		written = sprintf_s( text.data(), text.size(), " win32_error=0x%08lX",
							static_cast<unsigned long>( static_cast<std::uint32_t>( field.value))) ;
		break ;

	// 固定数値fieldを書式化する: 呼出し境界が定義した固定errorまたは状態code
	case DiagnosticFieldName::Code:
		written = sprintf_s( text.data(), text.size(), " code=%lld",
							static_cast<long long>( static_cast<std::int64_t>( field.value))) ;
		break ;

	// 固定数値fieldを書式化する: 現在の回復期間で完了した試行回数
	case DiagnosticFieldName::AttemptCount:
		written = sprintf_s( text.data(), text.size(), " attempt=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: 実行可能な再試行回数の上限
	case DiagnosticFieldName::MaximumAttemptCount:
		written = sprintf_s( text.data(), text.size(), " max_attempts=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: network利用可能性をbool相当の数値で示す
	case DiagnosticFieldName::NetworkAvailable:
		written = sprintf_s( text.data(), text.size(), " network_available=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: 転送または処理したbyte数
	case DiagnosticFieldName::ByteCount:
		written = sprintf_s( text.data(), text.size(), " bytes=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: 読み込んだentryまたは破棄したlog recordの件数
	case DiagnosticFieldName::RecordCount:
		written = sprintf_s( text.data(), text.size(), " records=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: 選択したMicrosoft Controlのversion
	case DiagnosticFieldName::ControlVersion:
		written = sprintf_s( text.data(), text.size(), " control_version=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: Microsoft認証dialogの互換version
	case DiagnosticFieldName::DialogVersion:
		written = sprintf_s( text.data(), text.size(), " dialog_version=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: 設定したserver認証のlevel
	case DiagnosticFieldName::AuthenticationLevel:
		written = sprintf_s( text.data(), text.size(), " authentication_level=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: processまたは境界処理の終了code
	case DiagnosticFieldName::ExitCode:
		written = sprintf_s( text.data(), text.size(), " exit_code=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: Microsoft Control等が返した基本終了理由
	case DiagnosticFieldName::Reason:
		written = sprintf_s( text.data(), text.size(), " reason=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: Microsoft Controlが返した拡張切断理由
	case DiagnosticFieldName::ExtendedReason:
		written = sprintf_s( text.data(), text.size(), " extended_reason=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: 対象機能の有効／無効をbool相当で示す
	case DiagnosticFieldName::Enabled:
		written = sprintf_s( text.data(), text.size(), " enabled=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: 排他的RDP表示modeの固定値
	case DiagnosticFieldName::DisplayMode:
		written = sprintf_s( text.data(), text.size(), " display_mode=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: Bridgeのlifecycle状態
	case DiagnosticFieldName::BridgeState:
		written = sprintf_s( text.data(), text.size(), " bridge_state=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: Bridge停止の分類済みerror
	case DiagnosticFieldName::BridgeError:
		written = sprintf_s( text.data(), text.size(), " bridge_error=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: 利用者がlocal操作で切断を要求したかを示す
	case DiagnosticFieldName::LocallyRequested:
		written = sprintf_s( text.data(), text.size(), " locally_requested=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: Windows資格情報UIの取消を識別する
	case DiagnosticFieldName::CredentialPromptCancelled:
		written = sprintf_s( text.data(), text.size(), " credential_prompt_cancelled=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: server認証警告の明示拒否を識別する
	case DiagnosticFieldName::SecurityWarningCancelled:
		written = sprintf_s( text.data(), text.size(), " security_warning_cancelled=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: local Proxy workerのlifecycle状態
	case DiagnosticFieldName::ProxyState:
		written = sprintf_s( text.data(), text.size(), " proxy_state=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: Proxy停止の分類済みerror
	case DiagnosticFieldName::ProxyError:
		written = sprintf_s( text.data(), text.size(), " proxy_error=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;

	// 固定数値fieldを書式化する: sessionへ割り当てたloopback TCP port
	case DiagnosticFieldName::Port:
		written = sprintf_s( text.data(), text.size(), " port=%llu",
							static_cast<unsigned long long>( field.value)) ;
		break ;
	}

	// 書式化に成功した固定名の数値項目だけをログへ含める
	if( written > 0) {
		line.append( text.data(), static_cast<std::size_t>( written)) ;
	}
}

// ---------------------------------------------------------------------------
// 有限キューを持つ非同期ログ状態
// ---------------------------------------------------------------------------

/**
 * @brief 診断ログの保存先、有限キュー、書込ワーカーを一括所有する。
 *
 * mutex_ が状態とキューを保護し、実ファイルへの書き込みは WorkerLoop()
 * だけが行う。デストラクタは停止要求とワーカーの join を完了させる。
 */
class DiagnosticLogState final {
public:
	/**
	 * @brief 未初期化状態のログ管理オブジェクトを構築する。
	 */
	DiagnosticLogState() = default ;

	/**
	 * @brief 未停止であっても安全にワーカーとファイルを終了する。
	 */
	~DiagnosticLogState()
	{
		Shutdown() ;
	}

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	DiagnosticLogState( const DiagnosticLogState &)			  = delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	DiagnosticLogState &operator=( const DiagnosticLogState &) = delete ;

	/**
	 * @brief 保存先を開き、診断ログワーカーを開始する。
	 * @param[in] role このプロセスが担う役割。
	 * @return 利用可能になった場合は true。既初期化時は現在の可用性。
	 */
	[[nodiscard]] bool	Initialize( const DiagnosticRole role)
	{
		std::scoped_lock lock( mutex_) ;
		// 二重初期化で複数ワーカーや複数ファイル所有者を作らない
		if( initialized_) {
			return available_.load( std::memory_order_acquire) ;
		}

		role_					   = role ;
		session_id_				   = CreateSessionIdentifier() ;
		stopping_				   = false ;
		process_stopping_recorded_ = false ;
		dropped_records_		   = 0U ;

		// 保存先が確立できない場合は、ワーカーを開始せず fail-closed とする
		if( !OpenPreferredLog()) {
			path_.clear() ;
			return false ;
		}

		try {
			worker_ = std::thread( &DiagnosticLogState::WorkerLoop, this) ;
		} catch( ...) {
			// スレッド生成失敗時は部分初期化したファイルを確実に破棄する
			file_.close() ;
			path_.clear() ;
			return false ;
		}

		initialized_ = true ;
		available_.store( true, std::memory_order_release) ;
		EnqueueUnlocked(
			BuildLine( DiagnosticLevel::Information, DiagnosticEvent::ProcessStarted, {})) ;
		return true ;
	}

	/**
	 * @brief 固定イベントと数値項目を有限キューへ投入する。
	 * @param[in] level 記録する重要度。
	 * @param[in] event 記録する固定イベント。
	 * @param[in] fields 記録を許可された数値項目。
	 */
	void Write( const DiagnosticLevel level, const DiagnosticEvent event,
			   const std::initializer_list<DiagnosticField> fields)
	{
		std::scoped_lock lock( mutex_) ;
		// 未初期化、停止中、I/O障害後には新規レコードを受け付けない
		if( !initialized_ || stopping_ || !available_.load( std::memory_order_acquire)) {
			return ;
		}

		EnqueueUnlocked( BuildLine( level, event, fields)) ;
		// 呼び出し側が停止イベントを明示した場合、終了処理で重複記録しない
		if( event == DiagnosticEvent::ProcessStopping) {
			process_stopping_recorded_ = true ;
		}
	}

	/**
	 * @brief 未記録の停止イベントを補い、キュー排出後にワーカーを終了する。
	 *
	 * 例外を外へ漏らさず、複数回呼ばれても安全な終了境界とする。
	 */
	void Shutdown() noexcept
	{
		try {
			{
				std::scoped_lock lock( mutex_) ;
				// 未初期化または終了済みなら追加の join を行わない
				if( !initialized_) {
					return ;
				}

				// 停止イベントがまだ無い場合に限り、一度だけ末尾へ補う
				if( !stopping_ && !process_stopping_recorded_) {
					EnqueueUnlocked( BuildLine( DiagnosticLevel::Information,
											  DiagnosticEvent::ProcessStopping, {})) ;
				}

				stopping_ = true ;
			}

			queue_ready_.notify_one() ;
			// ワーカーの所有権がある場合だけ、キュー排出完了を待機する
			if( worker_.joinable()) {
				worker_.join() ;
			}

			std::scoped_lock lock( mutex_) ;
			initialized_ = false ;
			available_.store( false, std::memory_order_release) ;
			queue_.clear() ;
		} catch( ...) {
			available_.store( false, std::memory_order_release) ;
		}
	}

	/**
	 * @brief 現在のログファイルパスをスナップショットとして返す。
	 * @return ログ未初期化時は空のパス。
	 */
	[[nodiscard]] std::wstring Path() const
	{
		std::scoped_lock lock( mutex_) ;
		return path_.wstring() ;
	}

private:
	/**
	 * @brief 実行ファイル隣接を優先し、失敗時は LocalAppData を試す。
	 * @return いずれかの保存先を開けた場合は true。
	 */
	[[nodiscard]] bool OpenPreferredLog()
	{
		const std::wstring module_path = ModulePath() ;
		// ポータブル配布では、書き込み可能なら exe 隣接の logs を優先する
		if( !module_path.empty()) {
			const std::filesystem::path executable_directory =
				std::filesystem::path( module_path).parent_path() ;
			// EXE隣接の専用log directoryを開けた場合はユーザー別directoryへ移らない
			if( TryOpen( executable_directory / kLogDirectoryName)) {
				return true ;
			}
		}

		const std::wstring local_app_data = LocalAppDataPath() ;
		// 両方の安全な保存先を確定できない場合はログ機能を無効化する
		if( local_app_data.empty()) {
			return false ;
		}

		return TryOpen( std::filesystem::path( local_app_data) / kFallbackDirectoryName /
					   kLogDirectoryName) ;
	}

	/**
	 * @brief 指定ディレクトリを作成し、サイズ上限を適用してログを開く。
	 * @param[in] directory 保存先ディレクトリ。
	 * @return 追記可能なファイルを確立できた場合は true。
	 */
	[[nodiscard]] bool TryOpen( const std::filesystem::path &directory)
	{
		std::error_code error ;
		std::filesystem::create_directories( directory, error) ;
		// 保存先作成失敗時は別の候補へフォールバックできるよう失敗を返す
		if( error) {
			return false ;
		}

		const std::wstring_view log_file_name = LogFileName( role_) ;
		// 不明なroleで共有名へfallbackするとログの所有者を誤認するため拒否する
		if( log_file_name.empty()) {
			path_.clear() ;
			return false ;
		}

		path_ = directory / std::wstring( log_file_name) ;
		const std::uint64_t existing_size =
			std::filesystem::exists( path_, error) ? std::filesystem::file_size( path_, error) : 0U ;
		// ファイル状態を取得できない保存先は、上限管理不能なので使用しない
		if( error) {
			path_.clear() ;
			return false ;
		}

		// 起動時点で上限に達している場合は、追記前に世代ローテーションする
		if( existing_size >= kDiagnosticLogMaximumBytes) {
			// 世代管理に失敗した状態で上限を超えて書き続けない
			if( !RotateFiles()) {
				path_.clear() ;
				return false ;
			}

			return true ;
		}

		file_.clear() ;
		file_.open( path_, std::ios::binary | std::ios::out | std::ios::app) ;
		// ファイルを開けなければ可用状態へ遷移させない
		if( !file_.is_open()) {
			path_.clear() ;
			return false ;
		}

		current_bytes_ =
			std::filesystem::exists( path_, error) ? std::filesystem::file_size( path_, error) : 0U ;
		// 実サイズが不明なまま上限計算を行わない
		if( error) {
			file_.close() ;
			path_.clear() ;
			return false ;
		}

		return true ;
	}

	/**
	 * @brief 現行ログを有限世代へ移動し、新しい空ログを開く。
	 * @return 全移動と新規ファイル作成に成功した場合は true。
	 */
	[[nodiscard]] bool RotateFiles()
	{
		file_.close() ;
		std::error_code				error ;

		const std::filesystem::path oldest =
			path_.wstring() + L"." + std::to_wstring( kDiagnosticLogBackupGenerations) ;
		std::filesystem::remove( oldest, error) ;
		// 最古世代を除去できなければ、既存ログを上書きせず停止する
		if( error) {
			return false ;
		}

		// 現行ファイルを移す前に、古い有限世代を後ろから一つずつ移動する
		for( unsigned int generation = kDiagnosticLogBackupGenerations; generation > 1U;
			--generation) {
			const std::filesystem::path source =
				path_.wstring() + L"." + std::to_wstring( generation - 1U) ;
			const std::filesystem::path destination =
				path_.wstring() + L"." + std::to_wstring( generation) ;

			error.clear() ;
			// 存在しない中間世代は正常な空きとして読み飛ばす
			if( !std::filesystem::exists( source, error)) {
				// 存在確認自体の失敗はローテーション不能として扱う
				if( error) {
					return false ;
				}

				continue ;
			}

			std::filesystem::rename( source, destination, error) ;
			// 部分的な世代更新後に書き続けず、呼び出し側へ障害を返す
			if( error) {
				return false ;
			}
		}

		error.clear() ;
		// 現行ログが存在する場合だけ、第1世代へ移動する
		if( std::filesystem::exists( path_, error)) {
			// 存在確認に失敗したパスを移動対象として扱わない
			if( error) {
				return false ;
			}

			std::filesystem::rename( path_, path_.wstring() + L".1", error) ;
			// 現行ログの保存に失敗した場合は新規ログを作成しない
			if( error) {
				return false ;
			}
		}
		// log file状態の照会errorではrotationを続けて既存fileを損なわない
		else if( error) {
			return false ;
		}

		file_.clear() ;
		file_.open( path_, std::ios::binary | std::ios::out | std::ios::trunc) ;
		current_bytes_ = 0U ;
		return file_.is_open() ;
	}

	/**
	 * @brief 一つの固定形式ログ行をメモリ上で構築する。
	 * @param[in] level 記録する重要度。
	 * @param[in] event 記録する固定イベント。
	 * @param[in] fields 記録する数値項目。
	 * @return CRLF で終端した UTF-8/ASCII 互換ログ行。
	 */
	[[nodiscard]] std::string BuildLine( const DiagnosticLevel level, const DiagnosticEvent event,
										const std::initializer_list<DiagnosticField> fields) const
	{
		std::string line ;
		line.reserve( 384U) ;
		AppendTimestamp( line) ;
		line.append( " level=") ;
		line.append( LevelName( level)) ;
		line.append( " role=") ;
		line.append( RoleName( role_)) ;
		line.append( " version=") ;
		line.append( ZRDP_VERSION) ;
		line.append( " session=") ;
		line.append( session_id_) ;
		line.append( " event=") ;
		line.append( EventName( event)) ;

		// 任意文字列を排除するため、列挙値で名前が固定された数値項目だけを追加する
		for( const DiagnosticField field : fields) {
			AppendField( line, field) ;
		}

		line.push_back( '\r') ;
		line.push_back( '\n') ;
		return line ;
	}

	/**
	 * @brief mutex_ 保持中にレコードを有限キューへ追加する。
	 * @param[in] line 所有権をキューへ移す完成済みログ行。
	 */
	void EnqueueUnlocked( std::string line)
	{
		// 診断イベントの集中でメモリを無制限に消費しないよう、キューを有限化する
		if( queue_.size() >= kMaximumQueuedRecords) {
			++dropped_records_ ;
			return ;
		}

		queue_.push_back( std::move( line)) ;
		queue_ready_.notify_one() ;
	}

	/**
	 * @brief キューを順次ファイルへ書き出し、停止時に残件を排出する。
	 *
	 * この関数だけが file_ と current_bytes_ を更新する。
	 */
	void WorkerLoop() noexcept
	{
		try {
			// 停止要求後も、キューと欠落通知が空になるまで処理を継続する
			while( true) {
				std::deque<std::string> records ;
				std::uint64_t			dropped = 0U ;
				{
					std::unique_lock lock( mutex_) ;
					/**
					 * @cond LOCAL_CALLBACK_CONTRACT
					 * @brief 停止またはログ書込み・drop集計が必要になるまでworkerを待つ。
					 * @return 停止中、未処理record、未反映dropのいずれかがあればtrue。
					 * @note mutex_保護下の有限queueとcounterだけを参照する。
					 * @endcond
					 */
					queue_ready_.wait( lock, [this]() {
						return stopping_ || !queue_.empty() || dropped_records_ > 0U ;
					}) ;
					records.swap( queue_) ;
					dropped = std::exchange( dropped_records_, 0U) ;

					// 通常レコードと欠落件数通知をすべて排出した後だけ終了する
					if( stopping_ && records.empty() && dropped == 0U) {
						break ;
					}
				}

				// 欠落が発生した場合は内容ではなく件数だけを一つの警告として残す
				if( dropped > 0U) {
					const std::string dropped_line =
						BuildLine( DiagnosticLevel::Warning, DiagnosticEvent::LogRecordsDropped,
								  {
									  DiagnosticValue( DiagnosticFieldName::RecordCount, dropped),
								  }) ;
					// ファイル障害後に同じ失敗を繰り返さず、ワーカーを停止する
					if( !WriteLine( dropped_line)) {
						break ;
					}
				}

				// 物理ファイルへの書き込みと flush は、このワーカーで直列化する
				for( const std::string &record : records) {
					// 一件でも書込不能になれば可用性が失われるため、残件処理を止める
					if( !WriteLine( record)) {
						break ;
					}
				}

				// I/O障害で不可用になった後は、新しい待機サイクルへ入らない
				if( !available_.load( std::memory_order_acquire)) {
					break ;
				}
			}
		} catch( ...) {
			available_.store( false, std::memory_order_release) ;
		}

		file_.flush() ;
		file_.close() ;
	}

	/**
	 * @brief サイズ上限と世代管理を適用して一行を書き込む。
	 * @param[in] line CRLF 終端済みのログ行。
	 * @return 書き込みと flush に成功した場合は true。
	 */
	[[nodiscard]] bool WriteLine( const std::string &line)
	{
		const std::uint64_t line_size = static_cast<std::uint64_t>( line.size()) ;

		// 内部レコードが破損していても、構成済みのファイル上限を迂回させない
		if( line_size > kDiagnosticLogMaximumBytes) {
			available_.store( false, std::memory_order_release) ;
			return false ;
		}

		// 追記によって上限を超える前にローテーションし、減算の underflow も避ける
		if( current_bytes_ > kDiagnosticLogMaximumBytes - line_size && !RotateFiles()) {
			available_.store( false, std::memory_order_release) ;
			return false ;
		}

		file_.write( line.data(), static_cast<std::streamsize>( line.size())) ;
		file_.flush() ;
		// 書き込みまたは flush 失敗後はログを不可用にして、部分記録を増やさない
		if( !file_) {
			available_.store( false, std::memory_order_release) ;
			return false ;
		}

		current_bytes_ += line_size ;
		return true ;
	}

	/// 共有状態と queue_ を保護する。
	mutable std::mutex		mutex_ ;
	/// キュー追加または停止要求をワーカーへ通知する。
	std::condition_variable queue_ready_ ;
	/// 上限付きの未書込レコードキュー。
	std::deque<std::string> queue_ ;
	/// file_ を単独で操作する書込ワーカー。
	std::thread				worker_ ;
	/// WorkerLoop() だけが書き込む現行ログファイル。
	std::ofstream			file_ ;
	/// 現行ログファイルの絶対パス。
	std::filesystem::path	path_ ;
	/// 実接続情報を含まないプロセス単位の匿名ID。
	std::string				session_id_ ;
	/// 固定ログ項目として記録するプロセス種別。
	DiagnosticRole			role_{DiagnosticRole::Client} ;
	/// 現行ログへ書き込み済みの byte 数。
	std::uint64_t			current_bytes_{0U} ;
	/// キュー上限により省略したレコード数。
	std::uint64_t			dropped_records_{0U} ;
	/// 呼び出し側が新規記録を試みてよいかを示す。
	std::atomic_bool		available_{false} ;
	/// 初期化とワーカー所有が成立しているかを示す。
	bool					initialized_{false} ;
	/// 新規受付を止め、残件排出中であることを示す。
	bool					stopping_{false} ;
	/// 停止イベントの重複記録を防ぐ。
	bool					process_stopping_recorded_{false} ;
} ;

/**
 * @brief プロセス内で唯一の診断ログ状態を返す。
 * @return 静的記憶域期間を持つログ状態への参照。
 */
[[nodiscard]] DiagnosticLogState &LogState()
{
	static DiagnosticLogState state ;
	return state ;
}

} // namespace

// ---------------------------------------------------------------------------
// 例外を越境させない公開API
// ---------------------------------------------------------------------------

bool InitializeDiagnosticLog( const DiagnosticRole role) noexcept
{
	try {
		return LogState().Initialize( role) ;
	} catch( ...) {
		// 診断機能の失敗でアプリ本体を停止させず、デバッガへ固定文だけを通知する
		OutputDebugStringW( L"ZRdp SVN Bridge: diagnostic log initialization failed.\n") ;
		return false ;
	}
}

void WriteDiagnosticLog( const DiagnosticLevel level, const DiagnosticEvent event,
						const std::initializer_list<DiagnosticField> fields) noexcept
{
	try {
		LogState().Write( level, event, fields) ;
	} catch( ...) {
		// ログ記録失敗を COM/UI 呼び出し境界へ伝播させない
		OutputDebugStringW( L"ZRdp SVN Bridge: diagnostic record was not written.\n") ;
	}
}

void ShutdownDiagnosticLog() noexcept
{
	try {
		LogState().Shutdown() ;
	} catch( ...) {
		// 終了処理では例外を再送出せず、プロセス終了を妨げない
		OutputDebugStringW( L"ZRdp SVN Bridge: diagnostic log shutdown failed.\n") ;
	}
}

std::wstring DiagnosticLogPath() noexcept
{
	try {
		return LogState().Path() ;
	} catch( ...) {
		// 診断パスの参照失敗は空文字列として安全に縮退する
		return {} ;
	}
}

} // namespace zrdp::common
