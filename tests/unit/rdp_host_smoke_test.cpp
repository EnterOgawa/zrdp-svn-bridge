//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file rdp_host_smoke_test.cpp
 * @brief 非表示Win32環境でRDP ActiveX生成と接続前設定の契約を検証する。
 * @version 01.00.13
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * 実際のConnectは行わず、文書用IPv4アドレスだけを設定する。実環境へのRDP
 * 通信やcredential promptを発生させないsmoke testとする。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  配置用キーの未接続・非所有thread拒否を検証。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  session画面のAgent配置・確認付き起動を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/09  自動入力のfocusとWindowsキー転送を確認。
 * Ver.01.00.05   Shunji Ogawa  2026/10/09  遅延入力readbackの未認証・別STA拒否を確認。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  三種類の入力testと未知種別を接続前に拒否。
 * Ver.01.00.07   Shunji Ogawa  2026/10/10  R比較の未認証・他STA拒否を実入力なしで検証。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  固定Win+RのSendInput比較と停止境界を追加。
 * Ver.01.00.09   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.10   Shunji Ogawa  2026/10/10  配置なしの固定Ctrl+V／Enter診断と失効確認を追加。
 * Ver.01.00.11   Shunji Ogawa  2026/10/10  認証前に通常focusを移さない契約を検証。
 * Ver.01.00.12   Shunji Ogawa  2026/10/10  接続別設定・自動Agent管理の回帰検証を追加。
 * Ver.01.00.13   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/rdp/rdp_control_host.h"
#include "client/rdp/rdp_input_focus.h"
#include "common/agent_setup.h"
#include "client/rdp/rdp_import.h"
#include <functional>
#include <iostream>

#include <Windows.h>

#include <atlbase.h>
#include <atlhost.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <thread>
#include <utility>

namespace {

/**
 * @brief ATL ActiveX host機能のprocess単位初期化を所有するtest module。
 */
class ZRdpTestAtlModule	 final : public ATL::CAtlExeModuleT<ZRdpTestAtlModule> {} ;

// test process全体でATL module lifetimeを保持する
ZRdpTestAtlModule		 g_atl_module ;

/**
 * @brief 非表示smoke-test hostの存続中にRDP callbackを安全に破棄する。
 */
class DiscardingObserver final : public zrdp::client::rdp::IRdpEventObserver {
public:
	/**
	 * @brief 所有copy化済みcallbackを受け取り、内容を参照せず破棄する。
	 * @param[in] event RdpControlHostから移動されたtest event。
	 */
	void OnRdpEvent( zrdp::client::rdp::RdpEvent event) override
	{
		static_cast<void>( std::move( event)) ;
	}
} ;

/**
 * @brief 未接続の実ActiveXへFocusを要求しても別controlのfocusを維持するか確認する。
 * @param[in,out] host 接続しないMicrosoft ActiveX host。
 * @param[in] parent fixtureが所有する非表示の親Window。
 * @return 実hostがfocusを移動せず、fixture資源を回収できた場合true。
 * @note keyboard入力、認証UI、通信、他processのWindow操作は行わない。
 */
bool TestUnconnectedFocus( zrdp::client::rdp::RdpControlHost &host, HWND parent)
{
	const HWND marker =
		CreateWindowExW( 0U, L"BUTTON", L"Focus marker", WS_CHILD | WS_TABSTOP, 0, 0, 100, 24,
						parent, nullptr, GetModuleHandleW( nullptr), nullptr) ;
	// fixture自身のcontrolを生成できなければfocus契約を確認済みとしない
	if( marker == nullptr) {
		return false ;
	}

	SetFocus( marker) ;
	const bool prepared = GetFocus() == marker ;
	host.Focus() ;
	const bool unchanged = GetFocus() == marker ;
	DestroyWindow( marker) ;
	return prepared && unchanged ;
}

// test専用STAへActiveX破棄を要求するthread message
constexpr UINT kMessageDestroyIsolatedHost = WM_APP + 1U ;

// 別STAが応答可能か確認するtest thread message
constexpr UINT kMessagePingIsolatedHost = WM_APP + 2U ;

/**
 * @brief 一つの非接続RDP ActiveXを専用STAで生成し、test commandを処理する。
 */
struct IsolatedHostThreadState {
	/// isolated COM STA threadへ確認messageを配送するためのID。
	std::atomic<DWORD>	  thread_id{0U} ;
	/// isolated threadでControlの生成を完了したことを通知するatomic flag。
	std::promise<HRESULT> ready ;
	/// 対象Controlの破棄を完了したことを通知するatomic flag。
	std::promise<void>	  destroyed ;
	/// 別sessionの確認messageへ応答できたことを通知するatomic flag。
	std::promise<void>	  pinged ;
} ;

/**
 * @brief ActiveXを一つの専用STAに閉じ込め、破棄と応答確認をmessage化する。
 * @param[in,out] state main test threadとの有限同期状態。
 */
void RunIsolatedHostThread( IsolatedHostThreadState &state) noexcept
{
	state.thread_id.store( GetCurrentThreadId(), std::memory_order_release) ;

	// main threadがPostThreadMessageを行う前に、このSTAのmessage queueを生成する
	MSG queue_probe{} ;
	static_cast<void>( PeekMessageW( &queue_probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE)) ;

	const HRESULT com_result = CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED) ;
	// COM初期化失敗時はActiveX生成へ進まず、待機中testを解放する
	if( FAILED( com_result)) {
		state.ready.set_value( com_result) ;
		return ;
	}

	DiscardingObserver				  observer ;
	zrdp::client::rdp::RdpControlHost host( observer) ;
	const HWND						  parent =
		CreateWindowExW( 0U, L"STATIC", L"ZRdp isolated STA test parent", WS_OVERLAPPED, 0, 0, 800,
						600, nullptr, nullptr, GetModuleHandleW( nullptr), nullptr) ;
	HRESULT create_result = parent == nullptr ? HRESULT_FROM_WIN32( GetLastError())
											  : host.Create( parent, RECT{0, 0, 800, 600}) ;
	state.ready.set_value( create_result) ;

	// 生成成功時だけ専用message loopを維持し、他STAの処理と直列化しない
	while( SUCCEEDED( create_result)) {
		MSG		   message{} ;
		const BOOL result = GetMessageW( &message, nullptr, 0U, 0U) ;
		// 隔離STAのmessage loop終了・取得失敗ではCOM破棄へ進む
		if( result <= 0) {
			break ;
		}

		// 隔離host破棄要求は所有STAで実行し、完了をpromiseへ通知する
		if( message.message == kMessageDestroyIsolatedHost) {
			host.Destroy() ;
			state.destroyed.set_value() ;
			continue ;
		}

		// ping messageへの応答で残存STAがdispatch可能なことを通知する
		if( message.message == kMessagePingIsolatedHost) {
			state.pinged.set_value() ;
			continue ;
		}

		TranslateMessage( &message) ;
		DispatchMessageW( &message) ;
	}

	host.Destroy() ;
	// ActiveX child解放後だけtest親Windowを同じ所有STAで破棄する
	if( parent != nullptr) {
		DestroyWindow( parent) ;
	}

	CoUninitialize() ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief RDP ActiveXの生成、破棄、複数STA session分離をsmoke検証する。
 * @param[in] argc workspace fixtureを指定する引数件数。
 * @param[in] argv 実接続を伴わない専用drive fixtureの基点。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain( int argc, wchar_t **argv)
{
	// 配布drive試験はCTest指定のworkspace fixtureだけで実行する
	if( argc != 2) {
		return 44 ;
	}

	auto deployment_drive = zrdp::common::agent_setup::DeploymentDrive::Create(
		std::filesystem::path( argv[1]) / L"rdp-drive-test") ;
	// 専用driveが作れない場合は標準driveを代用しない
	if( deployment_drive == nullptr) {
		return 45 ;
	}

	using zrdp::client::rdp::CanUpdateSessionDisplaySettings ;
	using zrdp::client::rdp::IsCredentialPromptCancellation ;
	using zrdp::client::rdp::RdpConnectionState ;
	using zrdp::client::rdp::CanFocusRemoteDesktop ;
	using zrdp::client::rdp::CanAcquireDeploymentInputFocus ;
	using zrdp::client::rdp::HasDeploymentInputFocus ;
	// event前・dialog間・再接続を含む全stateと警告flagで通常focusを検証する
	for( const auto state : {RdpConnectionState::NotInitialized, RdpConnectionState::Disconnected,
							RdpConnectionState::Connecting, RdpConnectionState::Connected,
							RdpConnectionState::LoggedIn, RdpConnectionState::Reconnecting,
							RdpConnectionState::Disconnecting, RdpConnectionState::Failed,
							static_cast<RdpConnectionState>( 999U)}) {
		// 警告解除だけでは接続前へfocusを渡さず、未知stateも拒否する
		if( CanFocusRemoteDesktop( state, true) ||
		   CanFocusRemoteDesktop( state, false) != (state == RdpConnectionState::LoggedIn)) {
			return 56 ;
		}
	}

	// 別アプリ前景、対象非準備、最小化の各条件を単独でも拒否する
	if( !CanAcquireDeploymentInputFocus( true, true, false) ||
	   CanAcquireDeploymentInputFocus( false, true, false) ||
	   CanAcquireDeploymentInputFocus( true, false, false) ||
	   CanAcquireDeploymentInputFocus( true, true, true)) {
		return 46 ;
	}

	// 前景、active、Control focusの全readbackがそろわない場合は成功としない
	if( !HasDeploymentInputFocus( true, true, true) || HasDeploymentInputFocus( false, true, true) ||
	   HasDeploymentInputFocus( true, false, true) || HasDeploymentInputFocus( true, true, false)) {
		return 47 ;
	}

	constexpr long kCredentialPromptCancellationReason = 0x1F07L ;

	// transport接続だけではremote desktopが未初期化のため動的更新を許可しない
	if( CanUpdateSessionDisplaySettings( RdpConnectionState::Connected)) {
		return 36 ;
	}

	// OnLoginComplete後だけを動的解像度APIの呼出し可能状態とする
	if( !CanUpdateSessionDisplaySettings( RdpConnectionState::LoggedIn)) {
		return 37 ;
	}

	// 再接続中は旧session寸法を変更せず、完了eventによる状態復元を待つ
	if( CanUpdateSessionDisplaySettings( RdpConnectionState::Reconnecting)) {
		return 38 ;
	}

	// 接続処理中の完全一致reasonをcredential UIの利用者取消として扱う
	if( !IsCredentialPromptCancellation( RdpConnectionState::Connecting,
									   kCredentialPromptCancellationReason, 0L, false)) {
		return 21 ;
	}

	// transport接続後でもLoginComplete前なら同じcredential取消経路を許可する
	if( !IsCredentialPromptCancellation( RdpConnectionState::Connected,
									   kCredentialPromptCancellationReason, 0L, false)) {
		return 22 ;
	}

	// LoginComplete後の同じ数値を通常の切断errorから除外しない
	if( IsCredentialPromptCancellation( RdpConnectionState::LoggedIn,
									  kCredentialPromptCancellationReason, 0L, false)) {
		return 23 ;
	}

	// 未知または別原因のdisconnect reasonを利用者取消へ近似しない
	if( IsCredentialPromptCancellation( RdpConnectionState::Connecting,
									  kCredentialPromptCancellationReason - 1L, 0L, false)) {
		return 24 ;
	}

	// 拡張切断理由がある接続失敗は通常の診断対象として維持する
	if( IsCredentialPromptCancellation( RdpConnectionState::Connecting,
									  kCredentialPromptCancellationReason, 1L, false)) {
		return 25 ;
	}

	// ZRdp自身が要求した切断をcredential prompt取消と重複分類しない
	if( IsCredentialPromptCancellation( RdpConnectionState::Connecting,
									  kCredentialPromptCancellationReason, 0L, true)) {
		return 26 ;
	}

	// ActiveXを所有するtest threadをCOM STAとして初期化する
	const HRESULT com_result = CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED) ;
	// COM初期化失敗時はATL/ActiveX操作へ進まない
	if( FAILED( com_result)) {
		return 1 ;
	}

	int result = 0 ;
	// ATL ActiveX containmentをprocessへ登録してからhostを生成する
	if( !ATL::AtlAxWinInit()) {
		result = 2 ;
	} else {
		const HWND parent =
			CreateWindowExW( 0U, L"STATIC", L"ZRdp test parent", WS_OVERLAPPED, 0, 0, 800, 600,
							nullptr, nullptr, GetModuleHandleW( nullptr), nullptr) ;
		// 親window生成失敗時はActiveX hostを作らない
		if( parent == nullptr) {
			result = 3 ;
		} else {
			const HWND second_parent =
				CreateWindowExW( 0U, L"STATIC", L"ZRdp second test parent", WS_OVERLAPPED, 0, 0, 800,
								600, nullptr, nullptr, GetModuleHandleW( nullptr), nullptr) ;
			bool second_created = false ;
			{
				DiscardingObserver				  observer ;
				DiscardingObserver				  second_observer ;
				zrdp::client::rdp::RdpControlHost host( observer) ;
				zrdp::client::rdp::RdpControlHost second_host( second_observer) ;
				const RECT						  bounds{0, 0, 800, 600} ;
				const HRESULT					  create_result = host.Create( parent, bounds) ;
				// 対応Microsoft Controlを生成できなければ設定検証を中止する
				if( FAILED( create_result)) {
					result = 4 ;
				}
				// 二つ目の親Windowを作れなければ複数host試験を失敗にする
				else if( second_parent == nullptr) {
					result = 28 ;
				} else {
					// 同じCOM STA上の別Windowへ二つ目のActiveXを同時生成する
					const HRESULT second_create_result = second_host.Create( second_parent, bounds) ;
					second_created					   = SUCCEEDED( second_create_result) &&
									 host.window() != second_host.window() &&
									 IsWindow( second_host.window()) ;
					// 同一STA内の二つのsessionがActiveX childを共有する状態を拒否する
					if( !second_created) {
						result = 29 ;
					}
				}

				// 一つ目のActiveX生成成功後だけ既存の接続前契約を確認する
				if( SUCCEEDED( create_result)) {
					// 認証前にはhost自体のFocusもmodal UIへ介入できない
					if( !TestUnconnectedFocus( host, parent)) {
						result = 57 ;
					}

					ATL::CComPtr<IUnknown> embedded_control ;
					const HRESULT		   embedded_result =
						ATL::AtlAxGetControl( host.window(), &embedded_control) ;
					ATL::CComQIPtr<IOleWindow> embedded_ole_window( embedded_control) ;
					HWND					   embedded_window = nullptr ;
					// 比較で使う標準OLE Window取得と所有host配下の関係を、接続せず確認する
					if( FAILED( embedded_result) || embedded_ole_window == nullptr ||
					   embedded_ole_window->GetWindow( &embedded_window) != S_OK ||
					   !IsWindow( embedded_window) || !IsChild( host.window(), embedded_window)) {
						result = 55 ;
					}

					// 接続前Controlへは配置用キーを一切送らない
					if( host.SendDeploymentPaste().result != E_UNEXPECTED ||
					   host.SendAgentStart().result != E_UNEXPECTED ||
					   host.SendDeploymentRun().result != E_UNEXPECTED ||
					   host.PrepareDeploymentInput() != E_UNEXPECTED ||
					   host.VerifyDeploymentInput() != HRESULT_FROM_WIN32( ERROR_INVALID_STATE)) {
						result = 42 ;
					}

					HRESULT		foreign_paste = S_OK ;
					std::thread foreign_thread(
						/**
						 * @cond LOCAL_CALLBACK_CONTRACT
						 * @brief 別threadの固定Paste要求をCOM操作前に拒否する契約を検証する。
						 * @note foreign_pasteはjoin前まで生存し、未接続fixtureで実入力を発行しない。
						 * @endcond
						 */
						[&] { foreign_paste = host.SendDeploymentPaste().result ; }) ;
					foreign_thread.join() ;
					HRESULT		foreign_start = S_OK ;
					std::thread foreign_start_thread(
						/**
						 * @cond LOCAL_CALLBACK_CONTRACT
						 * @brief 別threadの固定Enter要求が所有STA以外では失敗することを検証する。
						 * @note foreign_startはjoinまで生存し、実AgentやRDP接続を使わない。
						 * @endcond
						 */
						[&] { foreign_start = host.SendAgentStart().result ; }) ;
					foreign_start_thread.join() ;
					HRESULT		foreign_prepare = S_OK ;
					std::thread foreign_prepare_thread(
						/**
						 * @cond LOCAL_CALLBACK_CONTRACT
						 * @brief 別threadのfocus準備を所有STA guardで拒否することを検証する。
						 * @note foreign_prepareはjoinまで生存し、焦点や実接続を操作しない。
						 * @endcond
						 */
						[&] { foreign_prepare = host.PrepareDeploymentInput() ; }) ;
					foreign_prepare_thread.join() ;
					HRESULT		foreign_verify = S_OK ;
					/**
					 * @cond LOCAL_CALLBACK_CONTRACT @brief 非所有STAからのreadback拒否だけを確認する。
					 * @endcond
					 */
					std::thread foreign_verify_thread(
						[&] { foreign_verify = host.VerifyDeploymentInput() ; }) ;
					foreign_verify_thread.join() ;
					// workerから呼んだ場合はCOM interface取得の前に拒否する
					if( foreign_paste != RPC_E_WRONG_THREAD || foreign_start != RPC_E_WRONG_THREAD ||
					   foreign_prepare != RPC_E_WRONG_THREAD ||
					   foreign_verify != RPC_E_WRONG_THREAD) {
						result = 43 ;
					}

					// 二つ目を維持したまま一つ目の既存接続前契約を回帰確認する
					// 空接続先は接続前設定のsecurity boundaryで拒否される
					zrdp::client::rdp::RdpConnectionSettings invalid_settings{} ;
					// 空の接続先がE_INVALIDARGで拒否されることを確認する
					if( host.ApplyConnectionSettings( invalid_settings) != E_INVALIDARG) {
						result = 5 ;
					}

					zrdp::client::rdp::RdpConnectionSettings valid_settings{} ;
					// remote desktopを維持する等倍scrollを既定modeとする
					if( valid_settings.display_mode !=
					   zrdp::client::rdp::RdpDisplayMode::NativeScroll) {
						result = 6 ;
					}

					// privacy既定はマイクOFF、音声はClient側再生とする
					if( valid_settings.audio.redirect_microphone ||
					   valid_settings.audio.playback_mode !=
						   zrdp::client::rdp::RdpAudioPlaybackMode::PlayOnClient) {
						result = 32 ;
					}

					// 実通信せず、文書用接続先への有効な接続前設定だけを適用する
					valid_settings.server			= L"192.0.2.1" ;
					valid_settings.deployment_drive = deployment_drive->letter() ;
					// 正常な例示接続先と初期表示条件を適用できることを確認する
					if( FAILED( host.ApplyConnectionSettings( valid_settings))) {
						ATL::CComPtr<IUnknown> control ;
						static_cast<void>( ATL::AtlAxGetControl( host.window(), &control)) ;
						ATL::CComQIPtr<IMsRdpClientNonScriptable3> settings( control) ;
						ATL::CComPtr<IMsRdpDriveCollection>		   drives ;
						// 失敗時は実endpointを含まないdrive名と状態だけを確認する
						if( settings != nullptr &&
						   SUCCEEDED( settings->get_DriveCollection( &drives)) && drives != nullptr) {
							ULONG count = 0U ;
							static_cast<void>( drives->get_DriveCount( &count)) ;
							// 有限collectionだけを診断する
							for( ULONG index = 0U; index < count && index < 26U; ++index) {
								ATL::CComPtr<IMsRdpDrive> drive ;
								ATL::CComBSTR			  name ;
								VARIANT_BOOL			  enabled = VARIANT_FALSE ;
								// 未取得driveを診断結果へ混ぜない
								if( SUCCEEDED( drives->get_DriveByIndex( index, &drive)) &&
								   drive != nullptr && SUCCEEDED( drive->get_Name( &name))) {
									static_cast<void>( drive->get_RedirectionState( &enabled)) ;
									std::wcerr << L"drive-name-length=" << SysStringLen( name)
											   << L" enabled=" << enabled << L"\n" ;
								}
							}
						}

						result = 7 ;
					}

					ATL::CComPtr<IUnknown> configured_control ;
					static_cast<void>( ATL::AtlAxGetControl( host.window(), &configured_control)) ;
					ATL::CComQIPtr<IMsRdpClient> configured_client( configured_control) ;
					ATL::CComPtr<IMsRdpClientAdvancedSettings> configured_advanced ;
					ATL::CComPtr<IMsRdpClientSecuredSettings>  configured_secured ;
					long									   windows_key = 0L, hook_mode = -1L ;
					// 実際の未接続Microsoft Controlで標準キー転送設定のreadbackを検証する
					if( configured_client == nullptr ||
					   FAILED( configured_client->get_AdvancedSettings2( &configured_advanced)) ||
					   configured_advanced == nullptr ||
					   FAILED( configured_client->get_SecuredSettings2( &configured_secured)) ||
					   configured_secured == nullptr ||
					   FAILED( configured_advanced->get_EnableWindowsKey( &windows_key)) ||
					   FAILED( configured_secured->get_KeyboardHookMode( &hook_mode)) ||
					   windows_key == 0L || hook_mode != 1L) {
						result = 48 ;
					}

					// 設定成功だけでは認証済みとみなさず、前景を変更しない
					if( host.PrepareDeploymentInput() != E_UNEXPECTED ||
					   host.VerifyDeploymentInput() != HRESULT_FROM_WIN32( ERROR_INVALID_STATE)) {
						result = 49 ;
					}

					// DomainとUserNameを別propertyへ設定する接続前契約を確認する
					valid_settings.domain	 = L"EXAMPLE" ;
					valid_settings.user_name = L"test-user" ;
					// DomainとUserNameの分離property設定を適用できることを確認する
					if( FAILED( host.ApplyConnectionSettings( valid_settings))) {
						result = 15 ;
					}

					// domainなしへ戻す設定でも前回値を明示的にclearできる
					valid_settings.domain.clear() ;
					// Domainを空へ戻して前回値を持ち越さない設定が受理されることを確認する
					if( FAILED( host.ApplyConnectionSettings( valid_settings))) {
						result = 20 ;
					}

					// 専用設定modelでは旧DOMAIN\user結合値を受け付けない
					valid_settings.user_name = L"EXAMPLE\\test-user" ;
					// 専用modelの結合済みUserNameが不正入力として拒否されることを確認する
					if( host.ApplyConnectionSettings( valid_settings) != E_INVALIDARG) {
						result = 16 ;
					}

					// Domainだけを指定した不完全なlogon識別子を拒否する
					valid_settings.domain = L"EXAMPLE" ;
					valid_settings.user_name.clear() ;
					// UserNameなしのDomainだけ指定が拒否されることを確認する
					if( host.ApplyConnectionSettings( valid_settings) != E_INVALIDARG) {
						result = 17 ;
					}

					// COMへ渡すdomain上限を超える入力を接続前に拒否する
					valid_settings.domain.assign( 256U, L'd') ;
					valid_settings.user_name = L"test-user" ;
					// Domain長の上限超過が接続前に拒否されることを確認する
					if( host.ApplyConnectionSettings( valid_settings) != E_INVALIDARG) {
						result = 18 ;
					}

					valid_settings.domain = L"EXAMPLE" ;
					valid_settings.domain.push_back( L'\0') ;
					valid_settings.domain.append( L"hidden") ;
					// ActiveX表示と異なるNUL後のdomainを隠せないよう拒否する
					if( host.ApplyConnectionSettings( valid_settings) != E_INVALIDARG) {
						result = 19 ;
					}

					valid_settings.domain	 = L"EXAMPLE" ;
					valid_settings.user_name = L"test-user" ;
					// 接続先上限を超える入力は表示用に切り詰めて受け入れない
					valid_settings.server.assign( 256U, L'a') ;
					// 接続先上限超過を表示切詰めで受理せず拒否することを確認する
					if( host.ApplyConnectionSettings( valid_settings) != E_INVALIDARG) {
						result = 8 ;
					}

					// NUL後の非表示部分をconnection barへ隠せる接続先は拒否する
					valid_settings.server = L"test-target" ;
					valid_settings.server.push_back( L'\0') ;
					valid_settings.server.append( L"hidden") ;
					// 接続先の埋込みNULと隠れたsuffixが拒否されることを確認する
					if( host.ApplyConnectionSettings( valid_settings) != E_INVALIDARG) {
						result = 9 ;
					}

					// 接続前設定ではClient側だけを縮小するSmart Sizingも受け付ける
					valid_settings.server		= L"192.0.2.1" ;
					valid_settings.display_mode = zrdp::client::rdp::RdpDisplayMode::SmartSizing ;
					// Smart Sizingが接続前の排他的表示modeとして受理されることを確認する
					if( FAILED( host.ApplyConnectionSettings( valid_settings))) {
						result = 10 ;
					}

					// 動的解像度はIMsRdpClient9を利用できるControlで接続前設定できる
					valid_settings.display_mode =
						zrdp::client::rdp::RdpDisplayMode::DynamicResolution ;
					// 動的解像度が対応Controlで接続前設定できることを確認する
					if( FAILED( host.ApplyConnectionSettings( valid_settings))) {
						result = 11 ;
					}

					valid_settings.audio.redirect_microphone = true ;
					valid_settings.audio.playback_mode =
						zrdp::client::rdp::RdpAudioPlaybackMode::PlayOnRemoteComputer ;
					// マイク転送と接続先PC側の音声再生を接続前に設定できる
					if( FAILED( host.ApplyConnectionSettings( valid_settings))) {
						result = 33 ;
					}

					valid_settings.audio.playback_mode =
						zrdp::client::rdp::RdpAudioPlaybackMode::DoNotPlay ;
					// 音声を再生しない選択も明示的な第三modeとして受け付ける
					if( FAILED( host.ApplyConnectionSettings( valid_settings))) {
						result = 34 ;
					}

					valid_settings.audio.playback_mode =
						static_cast<zrdp::client::rdp::RdpAudioPlaybackMode>( 99U) ;
					// 未知の音声modeはActiveX固有値へ暗黙変換せず拒否する
					if( host.ApplyConnectionSettings( valid_settings) != E_INVALIDARG) {
						result = 35 ;
					}

					valid_settings.audio = {} ;

					// 未知のenum値は暗黙に等倍表示へ変換せず拒否する
					valid_settings.display_mode =
						static_cast<zrdp::client::rdp::RdpDisplayMode>( 99U) ;
					// 未知の表示enum値が暗黙fallbackせず拒否されることを確認する
					if( host.ApplyConnectionSettings( valid_settings) != E_INVALIDARG) {
						result = 12 ;
					}

					// 実行時切替は確立済みRDP sessionだけに限定する
					if( host.SetSmartSizing( true) != HRESULT_FROM_WIN32( ERROR_INVALID_STATE)) {
						result = 13 ;
					}

					// remote desktop寸法の動的更新も未接続状態では拒否する
					if( host.UpdateSessionDisplaySettings( 800L, 600L) !=
					   HRESULT_FROM_WIN32( ERROR_INVALID_STATE)) {
						result = 14 ;
					}

					constexpr std::array<std::byte, 1> test_chunk{
						std::byte{0U},
					} ;
					// 登録済みでも未接続時のSVC送信はfail-closedにする
					if( host.SendStaticVirtualChannelData( test_chunk) !=
					   HRESULT_FROM_WIN32( ERROR_INVALID_STATE)) {
						result = 27 ;
					}
				}

				// 一つ目のsession破棄が二つ目のActiveX lifetimeへ波及しないことを確認する
				host.Destroy() ;
				// 一つ目のhost破棄で二つ目のWindowまで消えていないことを確認する
				if( second_created && !IsWindow( second_host.window()) && result == 0) {
					result = 30 ;
				}

				// 二つ目のhostが生存している場合だけ残存session設定を回帰確認する
				if( second_created) {
					zrdp::client::rdp::RdpConnectionSettings remaining_settings{} ;
					remaining_settings.server = L"198.51.100.1" ;
					// 残存sessionが破棄後も接続前設定を受け付けることを確認する
					if( FAILED( second_host.ApplyConnectionSettings( remaining_settings)) &&
					   result == 0) {
						result = 31 ;
					}
				}

				second_host.Destroy() ;
			}

			// ActiveX childを先に破棄してから二つのtest親Windowを解放する
			if( second_parent != nullptr) {
				DestroyWindow( second_parent) ;
			}

			DestroyWindow( parent) ;
		}

		IsolatedHostThreadState first_isolated ;
		IsolatedHostThreadState second_isolated ;
		std::future<HRESULT>	first_ready		= first_isolated.ready.get_future() ;
		std::future<HRESULT>	second_ready	= second_isolated.ready.get_future() ;
		std::future<void>		first_destroyed = first_isolated.destroyed.get_future() ;
		std::future<void>		second_pinged	= second_isolated.pinged.get_future() ;
		std::thread				first_thread( RunIsolatedHostThread, std::ref( first_isolated)) ;
		std::thread				second_thread( RunIsolatedHostThread, std::ref( second_isolated)) ;
		constexpr auto			kThreadTestTimeout = std::chrono::seconds( 5) ;
		const bool				first_started =
			first_ready.wait_for( kThreadTestTimeout) == std::future_status::ready &&
			SUCCEEDED( first_ready.get()) ;
		const bool second_started =
			second_ready.wait_for( kThreadTestTimeout) == std::future_status::ready &&
			SUCCEEDED( second_ready.get()) ;
		// 専用STAを二つとも生成できない環境ではisolation Gateを通過させない
		if( (!first_started || !second_started) && result == 0) {
			result = 39 ;
		}

		// 両方の隔離STAが開始した場合だけ片側破棄と他側pingを試験する
		if( first_started && second_started) {
			static_cast<void>(
				PostThreadMessageW( first_isolated.thread_id.load( std::memory_order_acquire),
								   kMessageDestroyIsolatedHost, 0U, 0L)) ;
			// 一方のActiveX破棄完了後も他方のSTAへmessageを送れることを確認する
			if( first_destroyed.wait_for( kThreadTestTimeout) != std::future_status::ready) {
				// 先行errorがなければ片側hostの破棄timeoutを固定codeへ保持する
				if( result == 0) {
					result = 40 ;
				}
			} else {
				static_cast<void>(
					PostThreadMessageW( second_isolated.thread_id.load( std::memory_order_acquire),
									   kMessagePingIsolatedHost, 0U, 0L)) ;
				// 他側STAのping応答timeoutを先行errorを失わず検証失敗にする
				if( second_pinged.wait_for( kThreadTestTimeout) != std::future_status::ready &&
				   result == 0) {
					result = 41 ;
				}
			}
		}

		// test結果にかかわらず両STAを起床してからjoinし、残留threadを作らない
		static_cast<void>( PostThreadMessageW(
			first_isolated.thread_id.load( std::memory_order_acquire), WM_QUIT, 0U, 0L)) ;
		static_cast<void>( PostThreadMessageW(
			second_isolated.thread_id.load( std::memory_order_acquire), WM_QUIT, 0U, 0L)) ;
		first_thread.join() ;
		second_thread.join() ;

		ATL::AtlAxWinTerm() ;
	}

	CoUninitialize() ;
	return result ;
}
