//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file main.cpp
 * @brief ZRdpSvnAgentのsession検証、SVC／TCP I/O loop、診断log lifecycleを実装する。
 * @version 01.00.06
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details 通常ユーザーのcurrent RDP sessionだけで動作し、service、inbound port、
 *          allowlist一致時だけoutbound Socketを作成する。WTSとSocket I/Oは
 *          同一owner threadで直列化し、一時RDP切断では全streamを破棄して
 *          SVCを再openする。tray終了またはRDP session終了時に全resourceを解放する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  接続別設定と自動Agent管理・診断削除を反映。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  自動setupと通常Agentの失敗をdialogなしの診断へ統一。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "agent/app/channel_recovery_policy.h"
#include "agent/app/agent_tray_window.h"
#include "agent/app/agent_configuration_policy.h"
#include "common/agent_setup.h"
#include "client/update/update_windows_handles.h"
#include "agent/app/session_validation.h"
#include "agent/bridge/agent_bridge.h"
#include "agent/bridge/agent_stream_relay.h"
#include "agent/channel/wts_virtual_channel.h"
#include "agent/network/allowlist.h"
#include "agent/network/upstream_connection.h"
#include "common/diagnostic_log.h"
#include "common/resource_ids.h"
#include "protocol/frame.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

// trayの終了要求／console control handlerとI/O loopが共有する停止flag
std::atomic_bool	  g_stop_requested{false} ;

// WTS readから協調停止flagの確認へ戻る最大待機時間
constexpr ULONG		  kChannelReadTimeoutMilliseconds = 100U ;

// portable AgentがEXEと同じdirectoryから読む固定設定名
constexpr wchar_t	  kAgentConfigurationFileName[] = L"agent.ini" ;

// Windows long path上限を越えたmodule pathを設定探索へ使用しない
constexpr std::size_t kMaximumModulePathCharacters = 32768U ;

/**
 * @brief SVC切断を検出したI/O方向の固定分類。
 */
enum class ChannelFailureKind : std::uint64_t {
	/// Virtual Channelの読込み経路で失敗した。
	Read  = 1U,
	/// Virtual Channelの書込み経路で失敗した。
	Write = 2U,
} ;

/**
 * @brief 一回のRDP自動再接続期間を通して保持するbounded retry状態。
 */
struct ChannelRecoveryContext {
	/// channel断を検出してから新HELLOが成立するまでtrue。
	bool									active{false} ;

	/// 現在の回復期間で実行したSVC open回数。
	std::uint64_t							attempt_count{0U} ;

	/// 次のSVC openを許可する単調clock値。
	ULONGLONG								next_attempt_tick{0U} ;

	/// 診断間引きのため直前WTS観測を保持している場合はtrue。
	bool									has_session_observation{false} ;

	/// 直前に観測したsession検証分類。
	zrdp::agent::app::SessionValidationCode last_validation_code{
		zrdp::agent::app::SessionValidationCode::ProcessSessionQueryFailed} ;

	/// 直前に観測したWTS connection state。
	WTS_CONNECTSTATE_CLASS last_connect_state{WTSInit} ;

	/// 直前のsession照会に付随したWin32 error。
	DWORD				   last_validation_error{ERROR_SUCCESS} ;
} ;

/**
 * @brief console control eventをallocationなしの停止要求へ変換する。
 * @param[in] control_type Windowsが通知したcontrol event。
 * @return 処理対象eventならTRUE。
 */
BOOL WINAPI ConsoleControlHandler( const DWORD control_type) noexcept
{
	// process終了系eventだけを協調停止へ変換する
	if( control_type == CTRL_C_EVENT || control_type == CTRL_BREAK_EVENT ||
	   control_type == CTRL_CLOSE_EVENT || control_type == CTRL_LOGOFF_EVENT ||
	   control_type == CTRL_SHUTDOWN_EVENT) {
		g_stop_requested.store( true, std::memory_order_release) ;
		return TRUE ;
	}

	return FALSE ;
}

/**
 * @brief 固定の非機密status文字列をWindows consoleへ表示する。
 * @param[in] message compile-time固定の日本語message。
 */
void WriteConsoleStatus( const wchar_t *message) noexcept
{
	const HANDLE output = GetStdHandle( STD_OUTPUT_HANDLE) ;
	// consoleなしで起動された場合は診断logだけを使用する
	if( output == nullptr || output == INVALID_HANDLE_VALUE || message == nullptr) {
		return ;
	}

	DWORD length = 0U ;
	// 固定文字列だけを有限走査し、実環境値をconsoleへ含めない
	while( message[length] != L'\0' && length < 1024U) {
		++length ;
	}

	DWORD written = 0U ;
	static_cast<void>( WriteConsoleW( output, message, length, &written, nullptr)) ;
}

/**
 * @brief channel failureから旧sessionを破棄して再open待機へ移る。
 * @param[in] failure_kind read／writeの固定失敗分類。
 * @param[in] win32_error WTS APIが返した秘密情報を含まないerror。
 * @param[in,out] agent_bridge 旧decoder、queue、Socketを破棄するBridge。
 * @param[in,out] channel 旧SVC handleを閉じるtransport owner。
 * @param[in,out] recovery 回復期間をまたいで試行回数を保持する状態。
 *
 * @note 同じ回復期間中の連続失敗は最初の原因だけを記録し、logを増幅しない。
 */
void BeginChannelRecovery( const ChannelFailureKind failure_kind, const DWORD win32_error,
						  zrdp::agent::bridge::AgentBridge		  &agent_bridge,
						  zrdp::agent::channel::WtsVirtualChannel &channel,
						  ChannelRecoveryContext				  &recovery) noexcept
{
	// 最初のtransport異常だけを記録し、継続失敗で同じ診断を増幅しない
	if( !recovery.active) {
		const zrdp::common::DiagnosticEvent event =
			failure_kind == ChannelFailureKind::Read
				? zrdp::common::DiagnosticEvent::AgentChannelReadFailed
				: zrdp::common::DiagnosticEvent::AgentChannelWriteFailed ;
		zrdp::common::WriteDiagnosticLog(
			zrdp::common::DiagnosticLevel::Warning, event,
			{
				zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::Reason,
											  win32_error),
				zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::ExtendedReason,
											  static_cast<std::uint64_t>( failure_kind)),
			}) ;
		WriteConsoleStatus(
			L"ZRdp SVN Agent: RDP通信が一時切断されました。session復帰後に自動再接続します。\r\n") ;
		recovery.active					 = true ;
		recovery.attempt_count			 = 0U ;
		recovery.has_session_observation = false ;
	}

	// 旧channelのfragment、stream ID、Socketを新HELLOへ持ち越さない
	agent_bridge.Reset() ;
	channel.Close() ;
	recovery.next_attempt_tick =
		GetTickCount64() +
		zrdp::agent::app::ChannelRecoveryDelayMilliseconds( recovery.attempt_count) ;
}

/**
 * @brief 回復待機中のWTS state変化だけを固定数値fieldで記録する。
 * @param[in,out] recovery 直前観測値を保持する回復状態。
 * @param[in] result current processだけを照会したsession検証結果。
 */
void LogChannelRecoverySessionObservation(
	ChannelRecoveryContext							&recovery,
	const zrdp::agent::app::SessionValidationResult &result) noexcept
{
	// 同じsession観測結果の繰返しは記録せず、状態変化だけを診断に残す
	if( recovery.has_session_observation && recovery.last_validation_code == result.code &&
	   recovery.last_connect_state == result.connect_state &&
	   recovery.last_validation_error == result.win32_error) {
		return ;
	}

	zrdp::common::WriteDiagnosticLog(
		result.code == zrdp::agent::app::SessionValidationCode::Valid
			? zrdp::common::DiagnosticLevel::Information
			: zrdp::common::DiagnosticLevel::Warning,
		zrdp::common::DiagnosticEvent::AgentSessionValidationReturned,
		{
			zrdp::common::DiagnosticCode( static_cast<long>( result.code)),
			zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::Reason,
										  result.win32_error),
			zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::ExtendedReason,
										  static_cast<std::uint64_t>( result.connect_state)),
		}) ;
	recovery.has_session_observation = true ;
	recovery.last_validation_code	 = result.code ;
	recovery.last_connect_state		 = result.connect_state ;
	recovery.last_validation_error	 = result.win32_error ;
}

/**
 * @brief Agent EXEと同じdirectoryの固定allowlist pathを取得する。
 * @return 完全なmodule pathを取得できた場合の`agent.ini` path。
 *
 * @warning 戻り値pathは診断logへ出力しない。
 */
[[nodiscard]] std::filesystem::path AgentConfigurationPath() noexcept
{
	try {
		std::vector<wchar_t> module_path( kMaximumModulePathCharacters) ;
		const DWORD			 length =
			GetModuleFileNameW( nullptr, module_path.data(), static_cast<DWORD>( module_path.size())) ;
		// API失敗または切り詰めpathから別directoryを推測しない
		if( length == 0U || length >= module_path.size()) {
			return {} ;
		}

		return std::filesystem::path( std::wstring( module_path.data(), length)).parent_path() /
			   kAgentConfigurationFileName ;
	} catch( ...) {
		// path allocation失敗は設定利用不能へ縮退し、例外をentry pointへ漏らさない
		return {} ;
	}
}

/**
 * @brief Agent Bridge失敗を固定数値fieldだけで診断記録する。
 * @param[in] snapshot payloadを含まないAgent state。
 */
void LogBridgeFailure( const zrdp::agent::bridge::AgentBridgeSnapshot &snapshot) noexcept
{
	zrdp::common::WriteDiagnosticLog(
		zrdp::common::DiagnosticLevel::Error, zrdp::common::DiagnosticEvent::BridgeFailed,
		{
			zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::BridgeState,
										  static_cast<std::uint64_t>( snapshot.state)),
			zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::BridgeError,
										  static_cast<std::uint64_t>( snapshot.error)),
		}) ;
}

/** @brief 自動実行をmodal表示で止めず、固定分類と終了codeだけを記録する。
 * @param[in] identifier 設定やpayloadを含まない固定resource ID。
 * @param[in] exit_code 失敗時に返す既存の非0終了code。
 * @note 診断logの開始後・停止前に呼び、pathや接続情報は記録しない。
 */
void LogAgentWarning( UINT identifier, int exit_code) noexcept
{
	zrdp::common::WriteDiagnosticLog(
		zrdp::common::DiagnosticLevel::Warning, zrdp::common::DiagnosticEvent::ProcessStopping,
		{
			zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::Reason, identifier),
			zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::ExitCode,
										  static_cast<std::uint64_t>( exit_code)),
		}) ;
}
} // namespace

/**
 * @brief AgentのUnicode console entry point。
 * @param[in] argc 実行fileを含む引数件数。
 * @param[in] argv 固定setup引数または引数なしだけを受け付ける。
 * @return 正常停止は0、session／channel／protocol失敗は固定非0 code。
 */
int wmain( int argc, wchar_t **argv)
{
	using zrdp::agent::app::SessionValidationCode ;
	using zrdp::agent::bridge::AgentBridge ;
	using zrdp::agent::bridge::AgentBridgeState ;
	using zrdp::agent::channel::ChannelReadStatus ;
	// 初期配置は固定argument一つだけを受け付け、通常Agent処理と分離する
	if( argc == 2 && std::wstring_view( argv[1]) == L"--rdp-setup") {
		const int result = zrdp::common::agent_setup::RunRdpSetup() ;
		// setup拒否もdialogを表示せず、従来の終了codeを非機密logへ残す
		if( result != 0) {
			static_cast<void>(
				zrdp::common::InitializeDiagnosticLog( zrdp::common::DiagnosticRole::Agent)) ;
			zrdp::common::WriteDiagnosticLog(
				zrdp::common::DiagnosticLevel::Warning,
				zrdp::common::DiagnosticEvent::ProcessStopping,
				{
					zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::ExitCode,
												  static_cast<std::uint64_t>( result)),
				}) ;
			zrdp::common::ShutdownDiagnosticLog() ;
		}

		return result ;
	}

	// 任意argumentや任意commandによる遠隔実行を受け付けない
	if( argc != 1) {
		return 31 ;
	}

	static_cast<void>( zrdp::common::InitializeDiagnosticLog( zrdp::common::DiagnosticRole::Agent)) ;
	WriteConsoleStatus( L"ZRdp SVN Agent: RDPセッションを確認しています。\r\n") ;

	const zrdp::agent::app::SessionValidationResult session_result =
		zrdp::agent::app::ValidateCurrentSession() ;
	// current active RDP session以外ではSVC openへ進まない
	if( session_result.code != SessionValidationCode::Valid) {
		zrdp::common::WriteDiagnosticLog(
			zrdp::common::DiagnosticLevel::Error,
			zrdp::common::DiagnosticEvent::AgentSessionValidationReturned,
			{
				zrdp::common::DiagnosticCode( static_cast<long>( session_result.code)),
				zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::Reason,
											  session_result.win32_error),
			}) ;
		WriteConsoleStatus(
			L"ZRdp SVN Agent: activeなRDPユーザーセッション内で起動してください。\r\n") ;
		LogAgentWarning( IDS_AGENT_SESSION_REQUIRED, 2) ;
		zrdp::common::ShutdownDiagnosticLog() ;
		return 2 ;
	}

	zrdp::common::WriteDiagnosticLog( zrdp::common::DiagnosticLevel::Information,
									 zrdp::common::DiagnosticEvent::AgentSessionValidationReturned) ;
	DWORD session_id = 0U ;
	// 同じRDP session内の重複Agentでchannelを競合させない
	if( !ProcessIdToSessionId( GetCurrentProcessId(), &session_id)) {
		zrdp::common::ShutdownDiagnosticLog() ;
		return 32 ;
	}

	const std::wstring mutex_name = L"Local\\ZRdpSvnAgent.Session." + std::to_wstring( session_id) ;
	zrdp::client::update::detail::KernelHandle singleton(
		CreateMutexW( nullptr, FALSE, mutex_name.c_str())) ;
	const DWORD singleton_error = GetLastError() ;
	// 起動済みAgentを強制終了せず、新しい起動だけを拒否する
	if( !singleton.valid() || singleton_error == ERROR_ALREADY_EXISTS) {
		LogAgentWarning( IDS_AGENT_ALREADY_RUNNING, 32) ;
		zrdp::common::ShutdownDiagnosticLog() ;
		return 32 ;
	}

	zrdp::client::update::detail::KernelHandle stop_event(
		zrdp::common::agent_setup::CreateAgentStopEvent( session_id)) ;
	// 安全な協調停止導線を持てないAgentは起動しない
	if( !stop_event.valid()) {
		zrdp::common::ShutdownDiagnosticLog() ;
		return 32 ;
	}

	zrdp::agent::app::AgentTrayWindow tray( GetModuleHandleW( nullptr), g_stop_requested) ;
	// 終了導線を持たない不可視Agentはchannelを開く前に停止する
	if( !tray.Start()) {
		LogAgentWarning( IDS_AGENT_TRAY_FAILED, 33) ;
		zrdp::common::ShutdownDiagnosticLog() ;
		return 33 ;
	}

	// 自分だけをconsoleからdetachし、共有Windows Terminalのwindowを隠さない
	static_cast<void>( FreeConsole()) ;
	zrdp::agent::channel::WtsVirtualChannel channel ;
	const DWORD								open_error = channel.Open() ;
	// ClientがESVNBRGを登録していないsessionでは待受や別経路へfallbackしない
	if( open_error != ERROR_SUCCESS) {
		zrdp::common::WriteDiagnosticLog(
			zrdp::common::DiagnosticLevel::Error,
			zrdp::common::DiagnosticEvent::AgentChannelOpenReturned,
			{
				zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::Reason,
											  open_error),
			}) ;
		WriteConsoleStatus(
			L"ZRdp SVN Agent: ESVNBRGを開けません。ZRdp Clientから接続した同じRDPセッションで起動してください。\r\n") ;
		tray.Stop() ;
		LogAgentWarning( IDS_AGENT_CHANNEL_UNAVAILABLE, 3) ;
		zrdp::common::ShutdownDiagnosticLog() ;
		return 3 ;
	}

	zrdp::common::WriteDiagnosticLog( zrdp::common::DiagnosticLevel::Information,
									 zrdp::common::DiagnosticEvent::AgentChannelOpenReturned) ;
	WriteConsoleStatus( L"ZRdp SVN Agent: allowlist設定を確認しています。\r\n") ;

	zrdp::agent::network::Allowlist		  allowlist ;
	zrdp::agent::network::AllowlistResult allowlist_result{
		zrdp::agent::network::AllowlistError::FileUnavailable,
		0U,
	} ;
	const std::filesystem::path configuration_path = AgentConfigurationPath() ;
	// 不完全なmodule pathではcurrent directory等へ設定探索をfallbackしない
	if( !configuration_path.empty()) {
		allowlist_result = zrdp::agent::network::LoadAllowlistFile( configuration_path, allowlist) ;
	}

	zrdp::agent::network::WinsockRuntime		   winsock_runtime ;
	zrdp::agent::network::WinsockUpstreamConnector upstream_connector ;
	zrdp::agent::bridge::AgentStreamRelay		   stream_relay( allowlist, upstream_connector) ;
	bool										   stream_relay_enabled = false ;
	// allowlistの全件検証が成功した場合だけ設定の受理を記録する
	if( allowlist_result.succeeded()) {
		zrdp::common::WriteDiagnosticLog(
			zrdp::common::DiagnosticLevel::Information,
			zrdp::common::DiagnosticEvent::AgentAllowlistLoadReturned,
			{
				zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::RecordCount,
											  allowlist.size()),
			}) ;
		const std::uint32_t winsock_error = winsock_runtime.Start() ;
		zrdp::common::WriteDiagnosticLog(
			winsock_error == 0U ? zrdp::common::DiagnosticLevel::Information
								: zrdp::common::DiagnosticLevel::Warning,
			zrdp::common::DiagnosticEvent::AgentWinsockInitializationReturned,
			{
				zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::Reason,
											  winsock_error),
			}) ;
		// valid allowlistとWinsockの両方が揃った場合だけstream capabilityを広告する
		if( winsock_error == 0U) {
			stream_relay_enabled = true ;
			zrdp::common::WriteDiagnosticLog(
				zrdp::common::DiagnosticLevel::Information,
				zrdp::common::DiagnosticEvent::AgentStreamRelayEnabled,
				{
					zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::Enabled, 1U),
				}) ;
			WriteConsoleStatus(
				L"ZRdp SVN Agent: allowlist限定SVN streamを有効にしました。ClientのHELLOを待機しています。\r\n") ;
		} else {
			WriteConsoleStatus(
				L"ZRdp SVN Agent: Winsockを初期化できないため、RDP Echoのみで継続します。\r\n") ;
		}
	} else {
		zrdp::common::WriteDiagnosticLog(
			zrdp::common::DiagnosticLevel::Warning,
			zrdp::common::DiagnosticEvent::AgentAllowlistLoadReturned,
			{
				zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::Reason,
											  static_cast<std::uint64_t>( allowlist_result.error)),
				zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::ExtendedReason,
											  allowlist_result.line),
			}) ;
		WriteConsoleStatus(
			L"ZRdp SVN Agent: agent.iniが未設定または不正なため、RDP Echoのみで継続します。\r\n") ;
	}

	tray.SetStatus( stream_relay_enabled ? zrdp::agent::app::AgentTrayStatus::Waiting
										: zrdp::agent::app::AgentTrayStatus::Unavailable) ;
	static_cast<void>( SetConsoleCtrlHandler( &ConsoleControlHandler, TRUE)) ;

	AgentBridge agent_bridge ;
	agent_bridge.EnableConfigurationControl( true) ;
	// 中継開始条件が揃った場合だけBridgeへSocket relayを関連付ける
	if( stream_relay_enabled) {
		agent_bridge.ConfigureStreamRelay( &stream_relay) ;
	} else {
		agent_bridge.Reset() ;
	}

	std::array<std::byte, zrdp::protocol::kStaticVirtualChannelChunkBytes> read_buffer{} ;
	AgentBridgeState	   last_state = agent_bridge.snapshot().state ;
	ChannelRecoveryContext channel_recovery{} ;
	int					   exit_code		 = 0 ;
	bool				   restart_requested = false ;

	// 一つのI/O ownerだけがWTS read、再open、writer queueを順番に処理する
	while( !g_stop_requested.load( std::memory_order_acquire)) {
		// setupによる停止もtrayと同じ通常終了経路へ渡す
		if( WaitForSingleObject( stop_event.get(), 0U) == WAIT_OBJECT_0) {
			break ;
		}

		// 通信状態は数値だけで通知し、Shell操作をI/O ownerへ持ち込まない
		tray.SetStatus( channel_recovery.active
						   ? zrdp::agent::app::AgentTrayStatus::Recovering
						   : (!stream_relay_enabled
								  ? zrdp::agent::app::AgentTrayStatus::Unavailable
								  : (agent_bridge.snapshot().state == AgentBridgeState::Ready &&
											 agent_bridge.ConfigurationReady()
										 ? zrdp::agent::app::AgentTrayStatus::Ready
										 : zrdp::agent::app::AgentTrayStatus::Waiting))) ;
		// channel断ではcurrent RDP sessionだけを確認し、active復帰後に同名SVCを開く
		if( !channel.is_open()) {
			const auto current_session = zrdp::agent::app::ValidateCurrentSession() ;
			LogChannelRecoverySessionObservation( channel_recovery, current_session) ;
			const auto recovery_action =
				zrdp::agent::app::SelectChannelRecoveryAction( current_session) ;
			// session終了を確認した場合は再openを止めてAgentの終了処理へ進む
			if( recovery_action == zrdp::agent::app::ChannelRecoveryAction::Stop) {
				WriteConsoleStatus( L"ZRdp SVN Agent: RDPセッションが終了したため停止します。\r\n") ;
				break ;
			}

			// 切断中またはWTS照会失敗中は別sessionや別経路を探索しない
			if( recovery_action == zrdp::agent::app::ChannelRecoveryAction::Wait) {
				Sleep( zrdp::agent::app::kChannelRecoveryPollMilliseconds) ;
				continue ;
			}

			const ULONGLONG now = GetTickCount64() ;
			// 旧transport解放と再接続完了を待つ有限backoff中も協調停止を確認する
			if( now < channel_recovery.next_attempt_tick) {
				Sleep( zrdp::agent::app::kChannelRecoveryPollMilliseconds) ;
				continue ;
			}

			// 長期切断でも診断counterをwrapさせず最大値で飽和させる
			if( channel_recovery.attempt_count < std::numeric_limits<std::uint64_t>::max()) {
				++channel_recovery.attempt_count ;
			}

			const DWORD reopen_error = channel.Open() ;
			// SVC再open失敗ではchannelを使用せず、有限backoff後の次の試行へ戻す
			if( reopen_error != ERROR_SUCCESS) {
				// 長時間断では1、2、4、8回目だけを記録してlog増幅を防ぐ
				if( zrdp::agent::app::ShouldLogChannelRecoveryAttempt(
					   channel_recovery.attempt_count)) {
					zrdp::common::WriteDiagnosticLog(
						zrdp::common::DiagnosticLevel::Warning,
						zrdp::common::DiagnosticEvent::AgentChannelOpenReturned,
						{
							zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::Reason,
														  reopen_error),
							zrdp::common::DiagnosticValue(
								zrdp::common::DiagnosticFieldName::AttemptCount,
								channel_recovery.attempt_count),
						}) ;
				}

				channel_recovery.next_attempt_tick =
					now + zrdp::agent::app::ChannelRecoveryDelayMilliseconds(
							  channel_recovery.attempt_count) ;
				continue ;
			}

			zrdp::common::WriteDiagnosticLog(
				zrdp::common::DiagnosticLevel::Information,
				zrdp::common::DiagnosticEvent::AgentChannelOpenReturned,
				{
					zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::Reason,
												  ERROR_SUCCESS),
					zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::AttemptCount,
												  channel_recovery.attempt_count),
				}) ;
			// open成功だけでは復旧扱いにせず、新しいClient HELLOを待つ
			agent_bridge.Reset() ;
			last_state = agent_bridge.snapshot().state ;
			WriteConsoleStatus(
				L"ZRdp SVN Agent: ESVNBRGを再度開きました。ClientのHELLOを待機しています。\r\n") ;
			continue ;
		}

		const zrdp::agent::channel::ChannelReadResult read_result =
			channel.Read( read_buffer, kChannelReadTimeoutMilliseconds) ;
		// RDP自動再接続で無効化されたhandleを終了理由にせず回復待機へ移す
		if( read_result.status == ChannelReadStatus::Error) {
			BeginChannelRecovery( ChannelFailureKind::Read, read_result.win32_error, agent_bridge,
								 channel, channel_recovery) ;
			continue ;
		}

		// timeout以外で受信したfragmentだけをincremental decoderへ渡す
		if( read_result.status != ChannelReadStatus::Timeout) {
			const std::span<const std::byte> received( read_buffer.data(), read_result.bytes_read) ;
			// malformed protocolではPONGや別frameを追加送信しない
			if( !agent_bridge.OnChannelData( received)) {
				LogBridgeFailure( agent_bridge.snapshot()) ;
				WriteConsoleStatus( L"ZRdp SVN Agent: protocol errorでBridgeを停止しました。\r\n") ;
				exit_code = 5 ;
				break ;
			}
		}

		zrdp::protocol::Frame configuration ;
		// negotiated controlだけから固定INIを更新し、内容は診断へ出さない
		if( agent_bridge.TakeConfiguration( configuration)) {
			const std::string_view ini( reinterpret_cast<const char *>( configuration.payload.data()),
									   configuration.payload.size()) ;
			const auto			   installed = zrdp::common::agent_setup::InstallConfiguration(
				configuration_path.parent_path(), ini) ;
			zrdp::agent::network::Allowlist requested_allowlist ;
			bool							active_matches =
				zrdp::agent::network::ParseAllowlist( ini, requested_allowlist).succeeded() &&
				requested_allowlist.size() == allowlist.size() ;
			// 起動後の外部INI変更でも古いmemoryのallowlistへACKReadyを返さない
			for( std::size_t index = 0U; active_matches && index < allowlist.size(); ++index) {
				const auto &requested = requested_allowlist.Entries()[index] ;
				const auto &active	  = allowlist.Entries()[index] ;
				active_matches		  = requested.endpoint_id == active.endpoint_id &&
								 requested.target.ipv4 == active.target.ipv4 &&
								 requested.target.port == active.target.port ;
			}

			const auto status = zrdp::agent::app::SelectAgentConfigurationStatus(
				installed == zrdp::common::agent_setup::InstallResult::Updated,
				installed == zrdp::common::agent_setup::InstallResult::AlreadyInstalled,
				active_matches, stream_relay_enabled) ;
			const bool acknowledged = agent_bridge.CompleteConfiguration( status) ;
			SecureZeroMemory( configuration.payload.data(), configuration.payload.size()) ;
			// 応答をqueueできない場合は旧設定で通信を再開しない
			if( !acknowledged) {
				exit_code = 5 ;
				break ;
			}

			restart_requested = status == 2U ;
			// 再起動と拒否ではACKを通常終了時に排出して全Socketを閉じる
			if( status != 1U) {
				break ;
			}
		}

		// Clientの設定削除は任意process操作ではなく自身の協調停止だけを行う
		if( agent_bridge.StopRequested()) {
			break ;
		}

		// WTS timeout中もnon-blocking connect／Socket I/Oを有限回進める
		if( !agent_bridge.PumpStreams( GetTickCount64())) {
			LogBridgeFailure( agent_bridge.snapshot()) ;
			WriteConsoleStatus( L"ZRdp SVN Agent: SVN stream処理を安全に停止しました。\r\n") ;
			exit_code = 5 ;
			break ;
		}

		const auto current_snapshot	   = agent_bridge.snapshot() ;
		bool	   bridge_became_ready = false ;
		// HELLO成立時だけ一度、payloadを含まないready状態を表示する
		if( last_state != current_snapshot.state &&
		   current_snapshot.state == AgentBridgeState::Ready) {
			zrdp::common::WriteDiagnosticLog( zrdp::common::DiagnosticLevel::Information,
											 zrdp::common::DiagnosticEvent::BridgeReady) ;
			// channel回復中はHELLO_ACKの完全送信まで利用可能表示を保留する
			if( channel_recovery.active) {
				// HELLO_ACKを完全送信してから回復完了を表示する
				bridge_became_ready = true ;
			} else {
				WriteConsoleStatus(
					stream_relay_enabled
						? L"ZRdp SVN Agent: Clientと接続しました。Echo／SVN streamを待機しています。\r\n"
						: L"ZRdp SVN Agent: Clientと接続しました。Echo要求を待機しています。\r\n") ;
			}
		}

		last_state = current_snapshot.state ;

		bool more_data			  = false ;
		bool channel_write_failed = false ;
		// protocol handlerが生成した応答を一つのserialized writer経路で順に送る
		do {
			std::vector<std::byte> chunk ;
			// 送信queueが空ならWTS writeを呼ばず、この回の排出を終える
			if( !agent_bridge.TakeOutboundChunk( chunk, more_data)) {
				break ;
			}

			const zrdp::agent::channel::ChannelWriteResult write_result = channel.Write( chunk) ;
			// partial writeも旧channelの破損として全streamを破棄し回復待機へ移す
			if( !write_result.succeeded) {
				BeginChannelRecovery( ChannelFailureKind::Write, write_result.win32_error,
									 agent_bridge, channel, channel_recovery) ;
				channel_write_failed = true ;
				break ;
			}
		} while( more_data) ;

		// write failure後は閉じたchannelを次loopのsession検証へ渡す
		if( channel_write_failed) {
			continue ;
		}

		// 新HELLOのACKを完全送信した時点でだけ一回の回復期間を完了する
		if( bridge_became_ready) {
			WriteConsoleStatus(
				stream_relay_enabled
					? L"ZRdp SVN Agent: RDP復帰後にClientと再接続しました。SVN streamを利用できます。\r\n"
					: L"ZRdp SVN Agent: RDP復帰後にClientと再接続しました。Echo要求を待機しています。\r\n") ;
			channel_recovery = ChannelRecoveryContext{} ;
		}
	}

	// 正常終了かつchannel生存時だけClientをHELLO待機へ戻すCLOSEを送る
	if( exit_code == 0 && channel.is_open() && agent_bridge.QueueSessionClose()) {
		bool more_data = false ;
		// CLOSEを含む残queueをchannel close前にwire順で排出する
		do {
			std::vector<std::byte> chunk ;
			// 終了通知までの送信queueを排出したら追加のwriteを行わない
			if( !agent_bridge.TakeOutboundChunk( chunk, more_data)) {
				break ;
			}

			const zrdp::agent::channel::ChannelWriteResult write_result = channel.Write( chunk) ;
			// 終了通知のwrite失敗もpayloadなしの固定errorとして記録する
			if( !write_result.succeeded) {
				zrdp::common::WriteDiagnosticLog(
					zrdp::common::DiagnosticLevel::Error,
					zrdp::common::DiagnosticEvent::AgentChannelWriteFailed,
					{
						zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::Reason,
													  write_result.win32_error),
					}) ;
				exit_code = 6 ;
				break ;
			}
		} while( more_data) ;
	}

	// RDP切断やwrite failureを含む全経路でupstream Socketをchannel close前に閉じる
	agent_bridge.Reset() ;
	static_cast<void>( SetConsoleCtrlHandler( &ConsoleControlHandler, FALSE)) ;
	channel.Close() ;
	WriteConsoleStatus( L"ZRdp SVN Agent: 停止しました。\r\n") ;
	zrdp::common::WriteDiagnosticLog(
		zrdp::common::DiagnosticLevel::Information, zrdp::common::DiagnosticEvent::ProcessStopping,
		{
			zrdp::common::DiagnosticValue( zrdp::common::DiagnosticFieldName::ExitCode,
										  static_cast<std::uint64_t>( exit_code)),
		}) ;

	tray.Stop() ;
	// icon消失も固定分類を記録し、接続済み常駐と誤認させない
	if( tray.failed()) {
		LogAgentWarning( IDS_AGENT_TRAY_FAILED, 33) ;
		exit_code = 33 ;
	}

	// protocol停止はmodal表示せず、固定分類を診断へ残す
	if( exit_code == 5) {
		LogAgentWarning( IDS_AGENT_BRIDGE_STOPPED, 5) ;
	}

	zrdp::common::ShutdownDiagnosticLog() ;
	stop_event.reset() ;
	singleton.reset() ;
	// 全channel、Socket、tray、singleton解放後だけ固定自身を再起動する
	if( restart_requested && exit_code == 0 && !tray.failed()) {
		return zrdp::common::agent_setup::RestartInstalledAgent() ? 0 : 29 ;
	}

	return exit_code ;
}
