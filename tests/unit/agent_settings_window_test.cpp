//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_settings_window_test.cpp
 * @brief 自作設定画面の登録・削除・自動保存と接続分離を確認する。
 * @version 01.00.07
 * @date 2026/10/08
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  実設定を読まないWin32画面testを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  接続別設定・自動Agent管理の回帰検証を追加。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  簡素な登録削除と保存失敗・旧設定保全を検証。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  登録済み接続別復元と切替時の保存保全を検証。
 * Ver.01.00.07   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 *
 * @note RDP ActiveX、実history、KnownFolder、file、networkを使用しない。
 */
//------------------------------------------------------------------------------
#include "client/app/agent_settings_window.h"
#include "client/app/resource.h"

#include <commctrl.h>
#include <windowsx.h>

#include <array>
#include <atomic>
#include <iostream>
#include <vector>

namespace {
// 設定境界を短く参照するtest用alias
namespace settings			  = zrdp::client::agent_settings ;
// testだけのworker通知message
constexpr UINT kNoticeMessage = WM_APP + 0x200U ;

/** @brief fake backendと画面testの共有snapshot。 */
struct FakeState {
	/// 完了結果を回収した後だけtestが参照する保存snapshot。
	settings::AgentSettingsProfiles profiles ;
	/// 保存中のUI抑止を確認するfake待機gate。
	std::atomic_bool				hold_save{false} ;
	/// 次の保存で旧snapshotを保持したまま失敗させる状態。
	bool							fail_save{false} ;
	/// 復号失敗後の空一覧保存を防ぐためのfake読込み失敗。
	bool							fail_load{false} ;
	/// 完了後だけ参照する保存受付回数。
	unsigned int					saves{0U} ;
	/// 廃止commandがbackendへ到達していないことを測る回数。
	unsigned int					transfers{0U} ;
} ;

/** @brief UIが使用するbackendをfileなしで実装する。 */
class UiBackend final : public settings::IAgentSettingsBackend {
public:
	/**
	 * @brief testより短く保持する共有参照を受ける。
	 * @param[in,out] state testが所有するsnapshot。
	 */
	explicit UiBackend( FakeState &state) noexcept : state_( state)
	{
	}

	/**
	 * @brief fake保存一覧だけを返す。
	 * @param[out] profiles snapshotのcopy。
	 * @param[in] stop このfakeでは待機しないため参照しない。
	 * @return copy成功または資源失敗。
	 */
	[[nodiscard]] settings::AgentSettingsResult Load( settings::AgentSettingsProfiles &profiles,
													 std::stop_token stop) noexcept override
	{
		static_cast<void>( stop) ;
		try {
			// 復号失敗を空一覧の成功へ変換しない
			if( state_.fail_load) {
				return {settings::AgentSettingsError::ProtectionFailed} ;
			}

			profiles = state_.profiles ;
			return {} ;
		} catch( ...) {
			return {settings::AgentSettingsError::ResourceExhausted} ;
		}
	}

	/**
	 * @brief fake保存一覧を完全置換する。
	 * @param[in] profiles serviceが検証した一覧。
	 * @param[in] stop 保存待機gateから安全に退出するための取消通知。
	 * @return copy成功または資源失敗。
	 */
	[[nodiscard]] settings::AgentSettingsResult
	Save( const settings::AgentSettingsProfiles &profiles, std::stop_token stop) noexcept override
	{
		// 保存結果をdrainする前の二重操作・終了抑止を再現する
		while( state_.hold_save.load() && !stop.stop_requested()) {
			Sleep( 1U) ;
		}

		// fixture終了で待機したworkerを残さない
		if( stop.stop_requested()) {
			return {settings::AgentSettingsError::Cancelled} ;
		}

		++state_.saves ;
		// 失敗した保存候補を永続snapshotへ公開しない
		if( state_.fail_save) {
			return {settings::AgentSettingsError::FileFailure} ;
		}

		try {
			state_.profiles = profiles ;
			return {} ;
		} catch( ...) {
			return {settings::AgentSettingsError::ResourceExhausted} ;
		}
	}

	/**
	 * @brief 廃止したINI操作への到達を失敗と回数で記録する。
	 * @param[in] path fake選択path。実fileを開かない。
	 * @param[in] name 使用しない旧保存名。
	 * @param[out] profile 変更しない旧draft。
	 * @param[in] stop このfakeでは待機しないため参照しない。
	 * @return 廃止操作の固定失敗。
	 */
	[[nodiscard]] settings::AgentSettingsResult Import( const std::filesystem::path	  &path,
													   std::wstring_view			   name,
													   settings::AgentSettingsProfile &profile,
													   std::stop_token stop) noexcept override
	{
		static_cast<void>( path) ;
		static_cast<void>( name) ;
		static_cast<void>( profile) ;
		static_cast<void>( stop) ;
		++state_.transfers ;
		return {settings::AgentSettingsError::InvalidData} ;
	}

	/**
	 * @brief 廃止したINI出力への到達を失敗と回数で記録する。
	 * @param[in] path fakeでは参照しないpath。
	 * @param[in] profile UIが明示出力した完全値。
	 * @param[in] overwrite_confirmed UIが標準選択確認を経た状態。
	 * @param[in] stop このfakeでは待機しないため参照しない。
	 * @return 廃止操作の固定失敗。
	 */
	[[nodiscard]] settings::AgentSettingsResult
	Export( const std::filesystem::path &path, const settings::AgentSettingsProfile &profile,
		   bool overwrite_confirmed, std::stop_token stop) noexcept override
	{
		static_cast<void>( path) ;
		static_cast<void>( profile) ;
		static_cast<void>( overwrite_confirmed) ;
		static_cast<void>( stop) ;
		++state_.transfers ;
		return {settings::AgentSettingsError::InvalidData} ;
	}

private:
	/// serviceとcontrollerより長く生存するtest状態。
	FakeState &state_ ;
} ;

/**
 * @brief 有限時間messageをdispatchし、設定workerの一件を画面へ渡す。
 * @param[in,out] window testが作ったcontroller。
 * @param[in,out] service 一件queue worker。
 * @return 二秒以内に正常完了した場合はtrue。
 */
[[nodiscard]] bool Drain( zrdp::client::app::AgentSettingsWindow &window,
						 settings::AgentSettingsService			&service)
{
	const auto deadline = GetTickCount64() + 2000U ;
	// Win32の自作画面だけをdispatchし、I/Oは別workerへ保持する
	while( GetTickCount64() < deadline) {
		settings::AgentSettingsNotice notice ;
		// 所有結果を受け取った場合だけ画面へ反映して完了する
		if( service.TakeNotice( notice)) {
			const bool success = notice.result.succeeded() ;
			window.OnNotice( std::move( notice)) ;
			return success ;
		}

		MSG message{} ;
		// 自作UI messageを溜めず、Tab処理もproductionと同じ経路へ渡す
		while( PeekMessageW( &message, nullptr, 0U, 0U, PM_REMOVE)) {
			// worker起床messageをUSER32へdispatchしない
			if( message.message != kNoticeMessage && !window.ProcessDialogMessage( message)) {
				TranslateMessage( &message) ;
				DispatchMessageW( &message) ;
			}
		}

		static_cast<void>( MsgWaitForMultipleObjects( 0U, nullptr, FALSE, 5U, QS_ALLINPUT)) ;
	}

	return false ;
}

/**
 * @brief 自作dialogのbutton操作だけを直接通知する。
 * @param[in] window testが所有するdialog。
 * @param[in] identifier resourceの固定button ID。
 */
void Click( HWND window, int identifier)
{
	SendMessageW( window, WM_COMMAND, MAKEWPARAM( identifier, BN_CLICKED),
				 reinterpret_cast<LPARAM>( GetDlgItem( window, identifier))) ;
}
} // namespace

// -----------------------------------------------------------------------------
// 設定画面の正常操作と表示
// -----------------------------------------------------------------------------

/**
 * @brief production resourceの自作設定画面をfake backendだけで操作する。
 * @param[in] argument_count preview指定を含む引数個数。
 * @param[in] arguments --previewだけは自作画面を短時間表示する。通常は引数なし。
 * @return 全契約が成立する場合は0。失敗段階は固定code。
 */
int wmain( int argument_count, wchar_t *arguments[])
{
	SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ;
	FakeState  state ;
	const auto binding = settings::BuildAgentConnectionKey( L"192.0.2.1", L"", L"fixture-user") ;
	const auto other_binding = settings::BuildAgentConnectionKey( L"192.0.2.1", L"", L"other-user") ;
	state.profiles			 = {
		  {L"UI Fixture",
				   {{1001U, {{192U, 0U, 2U, 10U}, 80U}, settings::AgentEndpointPurpose::Svn, true}},
				   binding},
		  {L"Other Fixture",
				   {{2001U, {{203U, 0U, 113U, 10U}, 80U}, settings::AgentEndpointPurpose::Svn, true}},
				   other_binding}} ;
	const auto other_profile = state.profiles[1] ;
	const auto thread		 = GetCurrentThreadId() ;
	MSG		   initialize{} ;
	static_cast<void>( PeekMessageW( &initialize, nullptr, 0U, 0U, PM_NOREMOVE)) ;
	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief fake backendの完了をtest所有UI threadへ非blockingで知らせる。
	 * @note captured threadはtest message queueだけで、実履歴・設定へは接続しない。
	 * @endcond
	 */
	settings::AgentSettingsService		   service( std::make_unique<UiBackend>( state), [thread] {
		static_cast<void>( PostThreadMessageW( thread, kNoticeMessage, 0U, 0L)) ;
	}) ;
	zrdp::client::app::AgentSettingsWindow window( GetModuleHandleW( nullptr), service) ;
	// 実設定やKnownFolderを使わず接続固定の一件を表示する
	if( !window.Show( nullptr, L"UI Fixture", binding) || !Drain( window, service)) {
		return 1 ;
	}

	const auto dialog = window.window() ;
	const auto list	  = GetDlgItem( dialog, IDC_AGENT_ENDPOINTS) ;
	// 三列と静的接続表示だけを持ち、別profileを選択できない
	if( ListView_GetItemCount( list) != 1 || Header_GetItemCount( ListView_GetHeader( list)) != 3 ||
	   (GetWindowLongPtrW( GetDlgItem( dialog, IDC_AGENT_PROFILE), GWL_STYLE) & WS_TABSTOP) != 0 ||
	   window.connection_key() != binding) {
		return 2 ;
	}

	// 廃止した保存・INI・承認・更新・起動controlを再公開しない
	for( const auto identifier :
		{IDC_AGENT_SAVE, IDC_AGENT_DELETE_PROFILE, IDC_AGENT_IMPORT, IDC_AGENT_EXPORT,
		 IDC_AGENT_APPROVED, IDC_AGENT_UPDATE, IDC_AGENT_DEPLOY, 1115}) {
		// 旧controlは見えないだけでなく存在しないことを確認する
		if( GetDlgItem( dialog, identifier) != nullptr) {
			return 3 ;
		}
	}

	// 接続AからBへ開いた画面を再読込みし、保存済みの一件だけを表示する
	window.ChangeConnection( L"Other Fixture", other_binding) ;
	// 読込みで対象だけを変更し、登録や別接続の保存を行わない
	if( !Drain( window, service) || window.connection_key() != other_binding ||
	   ListView_GetItemCount( list) != 1 || state.saves != 0U) {
		return 26 ;
	}

	const auto unknown_binding = settings::BuildAgentConnectionKey( L"192.0.2.2", L"", L"user") ;
	window.ChangeConnection( L"Unknown Fixture", unknown_binding) ;
	// 未登録接続は他接続の候補を表示せず、空一覧から新規登録できる
	if( !Drain( window, service) || ListView_GetItemCount( list) != 0 ||
	   !IsWindowEnabled( GetDlgItem( dialog, IDC_AGENT_ADD))) {
		return 27 ;
	}

	window.ChangeConnection( L"", L"") ;
	// 接続未選択へ切り替えた画面では直接commandも保存に進めない
	if( !Drain( window, service) || IsWindowEnabled( GetDlgItem( dialog, IDC_AGENT_ADD))) {
		return 28 ;
	}

	Click( dialog, IDC_AGENT_ADD) ;
	window.ChangeConnection( L"UI Fixture", binding) ;
	// 元接続の登録済み一覧を復元し、切替だけでは設定を保存しない
	if( !Drain( window, service) || ListView_GetItemCount( list) != 1 || state.saves != 0U) {
		return 29 ;
	}

	SetDlgItemTextW( dialog, IDC_AGENT_IPV4, L"198.51.100.20") ;
	SetDlgItemTextW( dialog, IDC_AGENT_PORT, L"8080") ;
	ComboBox_SetCurSel( GetDlgItem( dialog, IDC_AGENT_PURPOSE), 1) ;
	state.hold_save.store( true) ;
	Click( dialog, IDC_AGENT_ADD) ;
	// 保存成功前は一覧を変えず、二重登録・削除と終了を抑止する
	if( ListView_GetItemCount( list) != 1 || IsWindowEnabled( GetDlgItem( dialog, IDC_AGENT_ADD)) ||
	   IsWindowEnabled( GetDlgItem( dialog, IDCANCEL))) {
		return 4 ;
	}

	Click( dialog, IDC_AGENT_ADD) ;
	Click( dialog, IDC_AGENT_REMOVE) ;
	SendMessageW( dialog, WM_CLOSE, 0U, 0L) ;
	// title barの終了要求も保存中は無作用にする
	if( window.window() != dialog) {
		return 5 ;
	}

	state.hold_save.store( false) ;
	// 登録一回だけで明示Trac承認と永続保存まで完了する
	if( !Drain( window, service) || state.saves != 1U || state.profiles.size() != 2U ||
	   state.profiles[0].endpoints.size() != 2U || !state.profiles[0].endpoints[1].approved ||
	   state.profiles[0].endpoints[1].purpose != settings::AgentEndpointPurpose::Trac ||
	   state.profiles[1] != other_profile || ListView_GetItemCount( list) != 2) {
		return 6 ;
	}

	// 廃止commandを直接送っても保存・出力・起動を予約しない
	for( const auto identifier :
		{IDC_AGENT_SAVE, IDC_AGENT_DELETE_PROFILE, IDC_AGENT_IMPORT, IDC_AGENT_EXPORT,
		 IDC_AGENT_APPROVED, IDC_AGENT_UPDATE, IDC_AGENT_DEPLOY}) {
		Click( dialog, identifier) ;
	}

	// 除去した操作の内部要求も残さない
	if( service.Busy() || state.saves != 1U || state.transfers != 0U) {
		return 7 ;
	}

	// previewは自作dataの画面だけを短時間表示する
	if( argument_count == 2 && std::wstring_view( arguments[1]) == L"--preview") {
		ShowWindow( dialog, SW_SHOWNOACTIVATE) ;
		const auto deadline = GetTickCount64() + 20000U ;
		// screenshot用fixtureを有限時間維持し、閉じる要求で正常終了する
		while( GetTickCount64() < deadline && window.window() != nullptr) {
			MSG message{} ;
			// 自作dialogに未処理のmessageだけを標準dispatchへ渡す
			while( PeekMessageW( &message, nullptr, 0U, 0U, PM_REMOVE)) {
				// RDPや他appに入力を送る経路を使用しない
				if( !window.ProcessDialogMessage( message)) {
					TranslateMessage( &message) ;
					DispatchMessageW( &message) ;
				}
			}

			static_cast<void>( MsgWaitForMultipleObjects( 0U, nullptr, FALSE, 50U, QS_ALLINPUT)) ;
		}

		SendMessageW( window.window(), WM_CLOSE, 0U, 0L) ;
		service.Stop() ;
		return 0 ;
	}

	const auto identifier = state.profiles[0].endpoints[1].endpoint_id ;
	ListView_SetItemState( list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED) ;
	ComboBox_SetCurSel( GetDlgItem( dialog, IDC_AGENT_PURPOSE), 0) ;
	Click( dialog, IDC_AGENT_ADD) ;
	// 同一IP・portの登録は重複を作らずIDを保って用途を更新する
	if( !Drain( window, service) || state.profiles[0].endpoints.size() != 2U ||
	   state.profiles[0].endpoints[1].endpoint_id != identifier ||
	   state.profiles[0].endpoints[1].purpose != settings::AgentEndpointPurpose::Svn) {
		return 8 ;
	}

	ListView_SetItemState( list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED) ;
	SetDlgItemTextW( dialog, IDC_AGENT_PORT, L"8081") ;
	Click( dialog, IDC_AGENT_ADD) ;
	// 宛先変更は新規登録とし、選択していた旧宛先を黙って消さない
	if( !Drain( window, service) || state.profiles[0].endpoints.size() != 3U ||
	   state.profiles[0].endpoints[1].target.port != 8080U ||
	   state.profiles[0].endpoints[2].target.port != 8081U) {
		return 9 ;
	}

	const auto saves_before_invalid = state.saves ;
	SetDlgItemTextW( dialog, IDC_AGENT_IPV4, L"999.0.0.1") ;
	Click( dialog, IDC_AGENT_ADD) ;
	// 不正入力はpopupや保存要求を作らず旧一覧を保持する
	if( service.Busy() || state.saves != saves_before_invalid || ListView_GetItemCount( list) != 3) {
		return 10 ;
	}

	SetDlgItemTextW( dialog, IDC_AGENT_IPV4, L"203.0.113.25") ;
	SetDlgItemTextW( dialog, IDC_AGENT_PORT, L"80") ;
	ComboBox_SetCurSel( GetDlgItem( dialog, IDC_AGENT_PURPOSE), -1) ;
	Click( dialog, IDC_AGENT_ADD) ;
	// 未選択用途を承認済みSVNとして推定しない
	if( service.Busy() || state.saves != saves_before_invalid) {
		return 11 ;
	}

	ComboBox_SetCurSel( GetDlgItem( dialog, IDC_AGENT_PURPOSE), 1) ;
	state.fail_save = true ;
	Click( dialog, IDC_AGENT_ADD) ;
	std::array<wchar_t, 32U> input{} ;
	// 保存失敗では旧一覧・入力を保持し、登録成功と表示しない
	if( Drain( window, service) || ListView_GetItemCount( list) != 3 ||
	   state.profiles[0].endpoints.size() != 3U ||
	   !IsWindowEnabled( GetDlgItem( dialog, IDC_AGENT_ADD)) ||
	   GetDlgItemTextW( dialog, IDC_AGENT_IPV4, input.data(), static_cast<int>( input.size())) <= 0 ||
	   std::wstring_view( input.data()) != L"203.0.113.25") {
		return 12 ;
	}

	state.fail_save = false ;
	Click( dialog, IDC_AGENT_ADD) ;
	// 同じ入力の再登録だけで保存をやり直せる
	if( !Drain( window, service) || ListView_GetItemCount( list) != 4 ||
	   state.profiles[0].endpoints.size() != 4U) {
		return 13 ;
	}

	state.hold_save.store( true) ;
	settings::AgentSettingsRequest competing ;
	competing.operation = settings::AgentSettingsOperation::Save ;
	competing.profiles	= state.profiles ;
	// application側worker処理中のqueue競合を自作requestだけで再現する
	if( service.Request( std::move( competing)) == 0U) {
		return 24 ;
	}

	SetDlgItemTextW( dialog, IDC_AGENT_IPV4, L"198.51.100.40") ;
	const auto saves_before_busy = state.saves ;
	Click( dialog, IDC_AGENT_ADD) ;
	state.hold_save.store( false) ;
	// queue競合時は候補を公開せず、旧一覧と再登録用入力を保つ
	if( !Drain( window, service) || ListView_GetItemCount( list) != 4 ||
	   state.profiles[0].endpoints.size() != 4U || state.saves != saves_before_busy + 1U ||
	   GetDlgItemTextW( dialog, IDC_AGENT_IPV4, input.data(), static_cast<int>( input.size())) <= 0 ||
	   std::wstring_view( input.data()) != L"198.51.100.40") {
		return 25 ;
	}

	ListView_SetItemState( list, 2, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED) ;
	state.fail_save = true ;
	Click( dialog, IDC_AGENT_REMOVE) ;
	// 削除保存の失敗でも表示と永続一覧を一致させる
	if( Drain( window, service) || ListView_GetItemCount( list) != 4 ||
	   state.profiles[0].endpoints.size() != 4U) {
		return 14 ;
	}

	state.fail_save = false ;
	Click( dialog, IDC_AGENT_REMOVE) ;
	// 明示選択した一件だけを自動保存で削除する
	if( !Drain( window, service) || ListView_GetItemCount( list) != 3 ||
	   state.profiles[0].endpoints[1].target.port != 8080U || state.profiles[1] != other_profile) {
		return 15 ;
	}

	// 有限三件を順に削除し、最後も空設定として永続化する
	for( int remaining = 3; remaining > 0; --remaining) {
		ListView_SetItemState( list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED) ;
		Click( dialog, IDC_AGENT_REMOVE) ;
		// 保存結果ごとに一件だけ減り、別接続は変更されない
		if( !Drain( window, service) || ListView_GetItemCount( list) != remaining - 1 ||
		   state.profiles[1] != other_profile) {
			return 16 ;
		}
	}

	std::string ini ;
	// 最後の削除は既存managerが認識する中継不可の空INIへ変換される
	if( !state.profiles[0].endpoints.empty() ||
	   settings::GenerateAgentSettingsIni( state.profiles[0], ini).succeeded()) {
		return 17 ;
	}

	SetDlgItemTextW( dialog, IDC_AGENT_IPV4, L"198.51.100.30") ;
	SendMessageW( dialog, WM_CLOSE, 0U, 0L) ;
	// 未登録の欄入力だけでは終了確認や保存を追加しない
	if( window.window() != nullptr) {
		return 18 ;
	}

	state.profiles = {
		{L"Legacy",
		 {{9001U, {{192U, 0U, 2U, 20U}, 80U}, settings::AgentEndpointPurpose::Svn, true},
		  {9002U, {{192U, 0U, 2U, 21U}, 80U}, settings::AgentEndpointPurpose::Svn, true}}}} ;
	const auto legacy = state.profiles[0] ;
	// 未関連付け旧設定を登録済み行として新接続へ表示しない
	if( !window.Show( nullptr, L"Bound fixture", binding) || !Drain( window, service) ||
	   ListView_GetItemCount( GetDlgItem( window.window(), IDC_AGENT_ENDPOINTS)) != 0 ||
	   state.profiles[0] != legacy) {
		return 19 ;
	}

	const auto legacy_dialog = window.window() ;
	const auto legacy_list	 = GetDlgItem( legacy_dialog, IDC_AGENT_ENDPOINTS) ;
	window.ChangeConnection( L"Other fixture", other_binding) ;
	// 別接続へ変更しても旧設定を候補として使わない
	if( !Drain( window, service) || ListView_GetItemCount( legacy_list) != 0) {
		return 20 ;
	}

	window.ChangeConnection( L"Bound fixture", binding) ;
	// 新接続に戻した読込み結果だけを確定してから登録する
	if( !Drain( window, service)) {
		return 21 ;
	}

	SetDlgItemTextW( legacy_dialog, IDC_AGENT_IPV4, L"192.0.2.20") ;
	Click( legacy_dialog, IDC_AGENT_ADD) ;
	// 明示登録した一件だけを接続key付きで保存し、旧profileは変更しない
	if( !Drain( window, service) || state.profiles.size() != 2U || state.profiles[0] != legacy ||
	   state.profiles[1].connection_key != binding || state.profiles[1].endpoints.size() != 1U ||
	   !state.profiles[1].endpoints[0].approved ||
	   !settings::GenerateAgentSettingsIni( state.profiles[1], ini).succeeded() ||
	   ListView_GetItemCount( legacy_list) != 1) {
		return 30 ;
	}

	SendMessageW( legacy_dialog, WM_CLOSE, 0U, 0L) ;
	// 閉じて開き直しても保存済みの登録状態を復元する
	if( !window.Show( nullptr, L"Bound fixture", binding) || !Drain( window, service) ||
	   ListView_GetItemCount( GetDlgItem( window.window(), IDC_AGENT_ENDPOINTS)) != 1) {
		return 31 ;
	}

	SendMessageW( window.window(), WM_CLOSE, 0U, 0L) ;
	settings::AgentSettingsProfile mixed{
		L"Mixed fixture",
		{{3001U, {{192U, 0U, 2U, 30U}, 80U}, settings::AgentEndpointPurpose::Svn, false},
		 {3002U, {{192U, 0U, 2U, 31U}, 80U}, settings::AgentEndpointPurpose::Svn, true},
		 {3003U, {{192U, 0U, 2U, 32U}, 80U}, settings::AgentEndpointPurpose::Svn, false},
		 {3004U, {{192U, 0U, 2U, 33U}, 80U}, settings::AgentEndpointPurpose::Trac, true}},
		binding} ;
	state.profiles = {mixed, other_profile} ;
	// 接続固定のmixed保存を完全に復元してから表示row対応を確認する
	if( !window.Show( nullptr, L"Mixed fixture", binding) || !Drain( window, service)) {
		return 32 ;
	}

	const auto mixed_dialog = window.window() ;
	const auto mixed_list	= GetDlgItem( mixed_dialog, IDC_AGENT_ENDPOINTS) ;
	// 保存内の未承認候補は保全するが、一覧には登録済み二件だけを表示する
	if( ListView_GetItemCount( mixed_list) != 2 || state.profiles[0] != mixed) {
		return 33 ;
	}

	ListView_SetItemState( mixed_list, 1, LVIS_SELECTED | LVIS_FOCUSED,
						  LVIS_SELECTED | LVIS_FOCUSED) ;
	Click( mixed_dialog, IDC_AGENT_REMOVE) ;
	// 表示row1は元index3だけを削除し、hidden候補と別接続を保全する
	if( !Drain( window, service) || state.profiles[0].endpoints.size() != 3U ||
	   state.profiles[0].endpoints[0] != mixed.endpoints[0] ||
	   state.profiles[0].endpoints[2] != mixed.endpoints[2] || state.profiles[1] != other_profile ||
	   ListView_GetItemCount( mixed_list) != 1) {
		return 34 ;
	}

	ListView_SetItemState( mixed_list, 0, LVIS_SELECTED | LVIS_FOCUSED,
						  LVIS_SELECTED | LVIS_FOCUSED) ;
	std::array<wchar_t, 128U> selected_input{} ;
	GetDlgItemTextW( mixed_dialog, IDC_AGENT_IPV4, selected_input.data(),
					static_cast<int>( selected_input.size())) ;
	// 表示row0の選択はhidden index0ではなく登録済みindex1を編集欄へ戻す
	if( std::wstring_view( selected_input.data()) != L"192.0.2.31") {
		return 35 ;
	}

	state.hold_save.store( true) ;
	SetDlgItemTextW( mixed_dialog, IDC_AGENT_IPV4, L"198.51.100.50") ;
	Click( mixed_dialog, IDC_AGENT_ADD) ;
	window.ChangeConnection( L"Other Fixture", other_binding) ;
	// 保存中の変更は元keyを維持し、別接続へ保存内容を転送しない
	if( window.connection_key() != binding ||
	   IsWindowEnabled( GetDlgItem( mixed_dialog, IDC_AGENT_ADD))) {
		return 36 ;
	}

	state.hold_save.store( false) ;
	// 元保存と切替先の再読込みを別結果として回収する
	if( !Drain( window, service) || !Drain( window, service) ||
	   window.connection_key() != other_binding || ListView_GetItemCount( mixed_list) != 1 ||
	   state.profiles[0].endpoints.size() != 4U || state.profiles[1] != other_profile) {
		return 37 ;
	}

	// Load中の変更は最後の接続一件だけを保持し、古い結果で一覧を上書きしない
	window.ChangeConnection( L"Mixed fixture", binding) ;
	window.ChangeConnection( L"Unknown", unknown_binding) ;
	window.ChangeConnection( L"Other Fixture", other_binding) ;
	// 古いLoadと最新接続Loadの二結果だけで最後の対象へ到達する
	if( !Drain( window, service) || !Drain( window, service) ||
	   window.connection_key() != other_binding || ListView_GetItemCount( mixed_list) != 1) {
		return 38 ;
	}

	window.ChangeConnection( L"Mixed fixture", binding) ;
	// 元接続には保存した登録済み二件だけが復元される
	if( !Drain( window, service) || ListView_GetItemCount( mixed_list) != 2) {
		return 39 ;
	}

	state.fail_save = true ;
	state.hold_save.store( true) ;
	SetDlgItemTextW( mixed_dialog, IDC_AGENT_IPV4, L"198.51.100.51") ;
	Click( mixed_dialog, IDC_AGENT_ADD) ;
	window.ChangeConnection( L"Other Fixture", other_binding) ;
	state.hold_save.store( false) ;
	// 保存失敗では元接続と再登録用入力を保持し、別接続へ切り替えない
	if( Drain( window, service) || window.connection_key() != binding ||
	   state.profiles[0].endpoints.size() != 4U ||
	   GetDlgItemTextW( mixed_dialog, IDC_AGENT_IPV4, selected_input.data(),
					   static_cast<int>( selected_input.size())) <= 0 ||
	   std::wstring_view( selected_input.data()) != L"198.51.100.51") {
		return 40 ;
	}

	state.fail_save = false ;
	Click( mixed_dialog, IDC_AGENT_ADD) ;
	// 元接続への再保存成功後にだけ保留した別接続へ表示を切り替える
	if( !Drain( window, service) || !Drain( window, service) ||
	   window.connection_key() != other_binding || state.profiles[0].endpoints.size() != 5U ||
	   state.profiles[1] != other_profile) {
		return 41 ;
	}

	SendMessageW( window.window(), WM_CLOSE, 0U, 0L) ;
	state.fail_load			  = true ;
	const auto before_failure = state.profiles ;
	// 復号失敗を空設定の登録画面へ縮退させない
	if( !window.Show( nullptr, L"Bound fixture", binding) || Drain( window, service) ||
	   IsWindowEnabled( GetDlgItem( window.window(), IDC_AGENT_ADD))) {
		return 22 ;
	}

	Click( window.window(), IDC_AGENT_ADD) ;
	// disabled controlへの直接通知でも旧設定を上書きしない
	if( service.Busy() || state.profiles != before_failure) {
		return 23 ;
	}

	SendMessageW( window.window(), WM_CLOSE, 0U, 0L) ;
	service.Stop() ;
	std::cout << "Simple Agent settings UI passed\n" ;
	return 0 ;
}
