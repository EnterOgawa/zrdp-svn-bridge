//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_deployment_window.cpp
 * @brief remote folder確認、標準clipboard公開、配置の利用者確認を実装する。
 * @version 01.00.09
 * @date 2026/10/08
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  配置準備と確認済み完了を区別するUIを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  初期配置の停止段階とHRESULT診断を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/09  自動入力のfocusとWindowsキー転送を確認。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.07   Shunji Ogawa  2026/10/10  トレイ終了後の同じRDP内Agent再起動を修正。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.09   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/app/agent_deployment_window.h"
#include "client/app/resource.h"
#include "common/diagnostic_log.h"
#include <array>
#include <utility>
#include <algorithm>
#include <windowsx.h>

namespace zrdp::client::app {
namespace {
/**
 * @brief 他アプリのclipboardを競合で消さないよう、所有・世代・排他を確認する。
 * @param[in] object このSTAがOleSetClipboardで公開した一参照。
 * @return 他所有はS_FALSE、消去成功はS_OK、競合・判定不能は失敗。
 * @note COMの所有判定はOpenClipboardの前に行い、排他中はWin32だけを呼ぶ。
 */
[[nodiscard]] HRESULT ClearOwnedClipboard( IDataObject *object) noexcept
{
	const auto sequence = GetClipboardSequenceNumber() ;
	// clipboard権限を確認できない場合は所有を推測しない
	if( sequence == 0U) {
		return E_ACCESSDENIED ;
	}

	const auto current = OleIsCurrentClipboard( object) ;
	// 他所有と判定不能は区別し、どちらも内容を消さない
	if( current != S_OK) {
		return current ;
	}

	// clipboard全体の排他取得に失敗したら利用者の再試行に委ねる
	if( !OpenClipboard( nullptr)) {
		const auto error = GetLastError() ;
		return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32( error) ;
	}

	// 所有判定から排他取得までに変化した内容は、自分のものと扱わない
	if( GetClipboardSequenceNumber() != sequence) {
		CloseClipboard() ;
		return HRESULT_FROM_WIN32( ERROR_RETRY) ;
	}

	const auto emptied	   = EmptyClipboard() ;
	const auto empty_error = emptied ? ERROR_SUCCESS : GetLastError() ;
	const auto closed	   = CloseClipboard() ;
	// 排他の終了にも失敗した場合は公開終了と表示しない
	if( !closed) {
		const auto error = GetLastError() ;
		return error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32( error) ;
	}

	return emptied ? S_OK : empty_error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32( empty_error) ;
}

/** @brief modal起動確認の非所有data。callbackの間だけstack上で生存する。 */
struct LaunchDialogContext {
	/// resource所有module。
	HINSTANCE									 instance ;
	/// runtime表示名。設定内容は表示しない。
	const std::wstring							&target ;
	/// callerが完全検証した保存済み一覧。
	const agent_settings::AgentSettingsProfiles &profiles ;
	/// 明示起動または設定表示だけをcallerへ返す。
	AgentLaunchChoice							 choice ;
	/// fixture初期化境界。製品では空。
	std::function<void( HWND)>					 initialized ;
} ;

/** @brief 今回の選択に一致する全文警告をreadonly欄へ表示する。
 * @param[in] window 自作modal dialog。
 * @param[in] context 同じdialogの非所有data。
 * @return resourceと有限formatが正常な場合だけtrue。
 */
[[nodiscard]] bool UpdateLaunchWarning( HWND window, const LaunchDialogContext &context)
{
	std::array<wchar_t, 2048U> format{} ;
	std::array<wchar_t, 3072U> message{} ;
	const int length = LoadStringW( context.instance, IDS_AUTO_DEPLOY_CONFIRM, format.data(),
								   static_cast<int>( format.size())) ;
	// 警告の欠落または切詰めでは明示起動を受付けない
	if( length <= 0 || length >= static_cast<int>( format.size()) - 1 ||
	   swprintf_s( message.data(), message.size(), format.data(), context.target.c_str()) < 0) {
		return false ;
	}

	std::wstring warning ;
	warning.reserve( message.size() * 2U) ;
	// Win32 multiline EDITではLFをCRLFへ正規化し、段落の警告を潰さない
	for( const auto character : std::wstring_view( message.data())) {
		// 固定resourceとruntime表示内の改行だけを表示規則へ合わせる
		if( character == L'\n') {
			warning.push_back( L'\r') ;
		}

		warning.push_back( character) ;
	}

	SetDlgItemTextW( window, IDC_LAUNCH_WARNING, warning.c_str()) ;
	const auto selected = ComboBox_GetCurSel( GetDlgItem( window, IDC_LAUNCH_PROFILE)) ;
	EnableWindow( GetDlgItem( window, IDOK),
				 selected >= 0 && static_cast<std::size_t>( selected) < context.profiles.size()) ;
	return true ;
}

/** @brief 設定編集を含まない固定buttonだけを処理する。
 * @param[in] window この確認画面。
 * @param[in] message Win32 message。
 * @param[in] wparam command値。
 * @param[in] lparam 初期化時だけstack context。
 * @return 処理済みならTRUE。
 */
INT_PTR CALLBACK LaunchDialogProcedure( HWND window, UINT message, WPARAM wparam,
									   LPARAM lparam) noexcept
{
	auto *context = reinterpret_cast<LaunchDialogContext *>( GetWindowLongPtrW( window, DWLP_USER)) ;
	try {
		// dialog生存中だけ非所有contextを参照し、実設定には触れない
		if( message == WM_INITDIALOG) {
			context = reinterpret_cast<LaunchDialogContext *>( lparam) ;
			SetWindowLongPtrW( window, DWLP_USER, lparam) ;
			SetDlgItemTextW( window, IDC_LAUNCH_TARGET, context->target.c_str()) ;
			const auto combo = GetDlgItem( window, IDC_LAUNCH_PROFILE) ;
			// 最大16件の保存名だけを選択肢にし、宛先を表示しない
			for( const auto &profile : context->profiles) {
				// 不完全なcomboでは別profile indexを採用しない
				if( ComboBox_AddString( combo, profile.name.c_str()) < 0) {
					EndDialog( window, IDCANCEL) ;
					return TRUE ;
				}
			}

			ComboBox_SetCurSel( combo, context->profiles.size() == 1U ? 0 : -1) ;
			// 警告全文を表示できなければ既定取消で終了する
			if( !UpdateLaunchWarning( window, *context)) {
				EndDialog( window, IDCANCEL) ;
				return TRUE ;
			}

			SetFocus( GetDlgItem( window, IDCANCEL)) ;
			// 自作UIの試験だけで初期化callbackを実行する
			if( context->initialized) {
				context->initialized( window) ;
			}

			return FALSE ;
		}

		// 初期化前のmessageでstack contextを参照しない
		if( context == nullptr) {
			return FALSE ;
		}

		// 閉じるは起動承認として扱わない
		if( message == WM_CLOSE || (message == WM_COMMAND && LOWORD( wparam) == IDCANCEL)) {
			EndDialog( window, IDCANCEL) ;
			return TRUE ;
		}

		// 操作対象は固定controlの選択とclickだけ
		if( message != WM_COMMAND) {
			return FALSE ;
		}

		const auto id = LOWORD( wparam) ;
		// 今回の警告またはprofile選択に一致する起動可否を反映する
		if( id == IDC_LAUNCH_PROFILE && HIWORD( wparam) == CBN_SELCHANGE) {
			// 更新不能の警告では起動を継続しない
			if( !UpdateLaunchWarning( window, *context)) {
				EndDialog( window, IDCANCEL) ;
			}

			return TRUE ;
		}

		// 設定は保存編集専用画面への明示要求とし、起動しない
		if( id == IDC_LAUNCH_SETTINGS && HIWORD( wparam) == BN_CLICKED) {
			context->choice.open_settings = true ;
			EndDialog( window, IDC_LAUNCH_SETTINGS) ;
			return TRUE ;
		}

		// 有効な保存済み選択とbutton clickがあるときだけ承認する
		if( id == IDOK && HIWORD( wparam) == BN_CLICKED) {
			const auto selected = ComboBox_GetCurSel( GetDlgItem( window, IDC_LAUNCH_PROFILE)) ;
			// 初期未選択や範囲外のindexは起動対象へ変換しない
			if( selected < 0 || static_cast<std::size_t>( selected) >= context->profiles.size() ||
			   !IsAgentLaunchProfileUsable( context->profiles[static_cast<std::size_t>( selected)])) {
				return TRUE ;
			}

			context->choice.profile_index = static_cast<std::size_t>( selected) ;
			EndDialog( window, IDOK) ;
			return TRUE ;
		}
	} catch( ...) {
		// 例外で部分的な選択を起動承認へ持ち越さない
		if( context != nullptr) {
			context->choice = {} ;
		}

		EndDialog( window, IDCANCEL) ;
		common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
								   common::DiagnosticEvent::UnexpectedException) ;
	}

	return FALSE ;
}
} // namespace

bool IsAgentLaunchProfileUsable( const agent_settings::AgentSettingsProfile &profile) noexcept
{
	return !profile.endpoints.empty() &&
		   agent_settings::ValidateAgentSettingsProfile( profile).succeeded() &&
		   /**
		    * @cond LOCAL_CALLBACK_CONTRACT
		    * @brief 公開INIの全entryに用途と業務利用承認があることを確認する。
		    * @param[in] entry 公開候補のallowlist entry。認証情報は含まない。
		    * @return entryが承認済みで用途も明示されている場合だけtrue。
		    * @endcond
		    */
		   std::all_of( profile.endpoints.begin(), profile.endpoints.end(), []( const auto &entry) {
			   return entry.approved &&
					  entry.purpose != agent_settings::AgentEndpointPurpose::Unspecified ;
		   }) ;
}

AgentLaunchChoice
AgentDeploymentWindow::ConfirmLaunch( std::wstring								  target,
									 const agent_settings::AgentSettingsProfiles &profiles,
									 std::function<void( HWND)>					  initialized)
{
	// 不完全な境界、競合操作、不正／未承認一覧を警告なしで採用しない
	if( active() || !ole_initialized_ || !clipboard_.publish || !clipboard_.current ||
	   !clipboard_.clear || profiles.empty() ||
	   !agent_settings::ValidateAgentSettingsProfiles( profiles).succeeded() ||
	   !std::all_of( profiles.begin(), profiles.end(), IsAgentLaunchProfileUsable)) {
		return {} ;
	}

	LaunchDialogContext context{instance_, target, profiles, {}, std::move( initialized)} ;
	preparing_		  = true ;
	const auto result = DialogBoxParamW( instance_, MAKEINTRESOURCEW( IDD_AGENT_LAUNCH), nullptr,
										LaunchDialogProcedure, reinterpret_cast<LPARAM>( &context)) ;
	// 完了した起動承認だけをworker準備へ渡す。取消と設定表示は公開しない
	if( result != IDOK || !context.choice.profile_index) {
		preparing_ = false ;
		return result == IDC_LAUNCH_SETTINGS ? context.choice : AgentLaunchChoice{} ;
	}

	target_ = std::move( target) + L"\nAgent設定: " + profiles[*context.choice.profile_index].name +
			  L" (" + std::to_wstring( profiles[*context.choice.profile_index].endpoints.size()) +
			  L"件)" ;
	automatic_ = true ;
	return context.choice ;
}

AgentDeploymentWindow::AgentDeploymentWindow( HINSTANCE instance, DeploymentClipboardOps clipboard,
											 DeploymentConfirmation		confirm,
											 std::function<ULONGLONG()> clock)
	: instance_( instance), clipboard_( std::move( clipboard)), confirm_( std::move( confirm)),
	  clock_( std::move( clock))
{
	// 空の境界だけを標準OLEにし、不完全なtest境界を実clipboardへfallbackしない
	if( !clipboard_.publish && !clipboard_.current && !clipboard_.clear) {
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT
		 * @brief 現在のOLE STAで検証済み固定配布物だけをclipboardへ公開する。
		 * @param[in] object caller所有のIDataObject。成功後の保持参照はOLEが管理する。
		 * @return OleSetClipboardの元HRESULT。
		 * @note public clipboardを変更するため、callerの世代・取消確認後だけ呼ぶ。
		 * @endcond
		 */
		clipboard_.publish = []( IDataObject *object) { return OleSetClipboard( object) ; } ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT
		 * @brief 公開した同一IDataObjectが現在もclipboardの正本か確認する。
		 * @param[in] object 今回の配布要求が所有するIDataObject。新たな保持参照は作らない。
		 * @return OleIsCurrentClipboardの元HRESULT。
		 * @endcond
		 */
		clipboard_.current = []( IDataObject *object) { return OleIsCurrentClipboard( object) ; } ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT
		 * @brief 自分が公開したclipboardだけを所有確認後に回収する。
		 * @param[in] object 回収対象のIDataObject。別appのclipboardを消さない。
		 * @return 所有確認と回収の元HRESULT。
		 * @endcond
		 */
		clipboard_.clear   = []( IDataObject *object) { return ClearOwnedClipboard( object) ; } ;
	}

	// callback構築例外時にOLE参照を取り残さず、構築完了直前に初期化する
	ole_initialized_ = SUCCEEDED( OleInitialize( nullptr)) ;
}

AgentDeploymentWindow::~AgentDeploymentWindow()
{
	Finish() ;
	// process終了時だけはclear失敗後も自分のCOM参照とWindowを取り残さない
	if( object_ != nullptr) {
		object_->Release() ;
		object_ = nullptr ;
	}

	// OLE解除前に生存dialogを回収する
	if( window_ != nullptr) {
		DestroyWindow( window_) ;
	}

	// managerのCOM初期化とは独立したOLE参照だけを解放する
	if( ole_initialized_) {
		OleUninitialize() ;
	}
}

std::wstring AgentDeploymentWindow::Text( UINT id) const
{
	std::array<wchar_t, 2048U> text{} ;
	const auto length = LoadStringW( instance_, id, text.data(), static_cast<int>( text.size())) ;
	return length > 0 ? std::wstring( text.data(), static_cast<std::size_t>( length))
					  : std::wstring{} ;
}

bool AgentDeploymentWindow::Confirm( UINT id) const
{
	// testではdialogの自動承認を実clipboard変更へ結びつけない
	if( confirm_) {
		return confirm_( window_, id, target_) ;
	}

	const auto text = Text( id) ;
	// resource欠落で空の承認dialogを出さない
	if( text.empty()) {
		return false ;
	}

	std::array<wchar_t, 3072U> buffer{} ;
	// 接続先を埋めるのは事前確認resourceだけ。他の%LOCALAPPDATA%はliteral
	if( id == IDS_DEPLOY_CONFIRM || id == IDS_AUTO_DEPLOY_CONFIRM) {
		// 表示が切詰められた場合に承認済みと扱わない
		if( swprintf_s( buffer.data(), buffer.size(), text.c_str(), target_.c_str()) < 0) {
			return false ;
		}
	} else {
		wcscpy_s( buffer.data(), buffer.size(), text.c_str()) ;
	}

	return MessageBoxW( window_, buffer.data(), L"ZRdp SVN Bridge",
					   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES ;
}

bool AgentDeploymentWindow::ConfirmPreparation( std::wstring target, bool automatic)
{
	// OLE不在・競合操作・不完全な境界では公開を開始しない
	if( active() || !ole_initialized_ || !clipboard_.publish || !clipboard_.current ||
	   !clipboard_.clear) {
		return false ;
	}

	target_	   = std::move( target) ;
	automatic_ = automatic ;
	preparing_ = true ;
	// 明示承認なしではclipboardを一切変更しない
	if( !Confirm( automatic_ ? IDS_AUTO_DEPLOY_CONFIRM : IDS_DEPLOY_CONFIRM)) {
		preparing_ = false ;
		target_.clear() ;
		return false ;
	}

	return true ;
}

bool AgentDeploymentWindow::ShowAutomatic(
	std::shared_ptr<common::agent_setup::DeploymentDrive> drive, DeploymentPaste launch,
	std::function<agent_deployment::AutomaticDeploymentSnapshot()> status)
{
	// 確認済みの封じた二fileと所有STAへの有限callbackだけを採用する
	if( !preparing_ || !automatic_ || window_ != nullptr || drive == nullptr || !drive->sealed() ||
	   !launch || !status) {
		return false ;
	}

	drive_				= std::move( drive) ;
	paste_				= std::move( launch) ;
	automatic_status_	= std::move( status) ;
	pending_			= true ;
	sent_				= false ;
	response_confirmed_ = false ;
	invalidated_		= false ;
	preparing_			= false ;
	automatic_failure_	= {} ;
	automatic_started_	= AutomaticTick() ;
	window_ =
		CreateDialogParamW( instance_, MAKEINTRESOURCEW( IDD_AGENT_DEPLOYMENT), nullptr,
						   &AgentDeploymentWindow::DialogProcedure, reinterpret_cast<LPARAM>( this)) ;
	// 画面を生成できなければ固定入力を送らない
	if( window_ == nullptr) {
		drive_->Cancel() ;
		drive_.reset() ;
		CancelPreparation() ;
		return false ;
	}

	ShowWindow( window_, SW_SHOWNORMAL) ;
	// queue失敗では自動retryせず、画面へ失敗を表示する
	if( !invalidated_ && !paste_( 0U)) {
		ShowAutomaticFailure( {-1, agent_deployment::AutomaticDeploymentStage::Request,
							  agent_deployment::AutomaticDeploymentReason::QueueFailure,
							  HRESULT_FROM_WIN32( ERROR_BUSY)}) ;
	}

	return true ;
}

void AgentDeploymentWindow::CancelPreparation() noexcept
{
	preparing_ = false ;
	target_.clear() ;
}

bool AgentDeploymentWindow::Show( std::shared_ptr<const agent_deployment::Package> package,
								 DeploymentPaste								  paste)
{
	// 未承認、二重画面、不完全callbackでは公開しない
	if( !preparing_ || window_ != nullptr || !paste) {
		return false ;
	}

	const auto created = agent_deployment::MakeDataObject( std::move( package), &object_) ;
	// 完全二ファイル以外をOLEへ渡さない
	if( FAILED( created)) {
		CancelPreparation() ;
		return false ;
	}

	paste_		 = std::move( paste) ;
	preparing_	 = false ;
	sent_		 = false ;
	invalidated_ = false ;
	pending_	 = false ;
	window_ =
		CreateDialogParamW( instance_, MAKEINTRESOURCEW( IDD_AGENT_DEPLOYMENT), nullptr,
						   &AgentDeploymentWindow::DialogProcedure, reinterpret_cast<LPARAM>( this)) ;
	// 画面を先に確保し、生成失敗時にはclipboardを変更しない
	if( window_ == nullptr) {
		object_->Release() ;
		object_ = nullptr ;
		CancelPreparation() ;
		return false ;
	}

	// 既存clipboardをclearしてからpublishする二段階操作を行わない
	if( FAILED( clipboard_.publish( object_))) {
		object_->Release() ;
		object_ = nullptr ;
		DestroyWindow( window_) ;
		CancelPreparation() ;
		return false ;
	}

	ShowWindow( window_, SW_SHOWNORMAL) ;
	return true ;
}

bool AgentDeploymentWindow::ProcessDialogMessage( MSG &message) const noexcept
{
	return window_ != nullptr && IsDialogMessageW( window_, &message) != FALSE ;
}

bool AgentDeploymentWindow::active() const noexcept
{
	return preparing_ || window_ != nullptr || object_ != nullptr ;
}

HWND AgentDeploymentWindow::window() const noexcept
{
	return window_ ;
}

void AgentDeploymentWindow::OnPasteResult( HRESULT result) noexcept
{
	// 古い画面の結果を新しい貼付けの完了と誤認しない
	if( window_ == nullptr || !pending_) {
		return ;
	}

	pending_ = false ;
	// 失敗を配置完了と扱わず、以降の要求を停止する
	if( result != S_OK) {
		// ownerの詳細を通知前に公開した結果から読み、元のHRESULTを保持する
		if( automatic_) {
			auto failure = automatic_status_ ? automatic_status_()
											 : agent_deployment::AutomaticDeploymentSnapshot{} ;
			// 詳細不在でも入力失敗を45秒timeoutへ置き換えない
			if( failure.status >= 0) {
				failure = {-1, agent_deployment::AutomaticDeploymentStage::Validation,
						   agent_deployment::AutomaticDeploymentReason::StatusUnavailable, result} ;
			}

			ShowAutomaticFailure( failure) ;
			return ;
		}

		Invalidate() ;
		return ;
	}

	// 自動方式のキー受付はAgent応答と区別し、まだ完了buttonを有効にしない
	if( automatic_) {
		sent_ = true ;
		SetDlgItemTextW( window_, IDC_DEPLOY_STATUS, Text( IDS_AUTO_DEPLOY_WAITING).c_str()) ;
		return ;
	}

	sent_ = true ;
	try {
		SetDlgItemTextW( window_, IDC_DEPLOY_STATUS, Text( IDS_DEPLOY_PASTE_SENT).c_str()) ;
		EnableWindow( GetDlgItem( window_, IDC_DEPLOY_COMPLETE), TRUE) ;
		EnableWindow( GetDlgItem( window_, IDC_DEPLOY_PASTE), !invalidated_) ;
	} catch( ...) {
		Invalidate() ;
	}
}

ULONGLONG AgentDeploymentWindow::AutomaticTick() const
{
	return clock_ ? clock_() : GetTickCount64() ;
}

void AgentDeploymentWindow::ShowAutomaticFailure(
	agent_deployment::AutomaticDeploymentSnapshot failure) noexcept
{
	// 最初の停止理由を残し、失効通知で別のerrorへ上書きしない
	if( automatic_failure_.status < 0) {
		return ;
	}

	failure.status	   = -1 ;
	automatic_failure_ = failure ;
	invalidated_	   = true ;
	pending_		   = false ;
	// ownerの未送信入力だけを停止し、配置済みfileやAgentへ触れない
	if( drive_ != nullptr) {
		drive_->Cancel() ;
	}

	common::WriteDiagnosticLog( common::DiagnosticLevel::Warning,
							   common::DiagnosticEvent::AutomaticDeploymentStopped,
							   {common::DiagnosticValue( common::DiagnosticFieldName::Code,
														static_cast<std::uint64_t>( failure.stage)),
								common::DiagnosticValue( common::DiagnosticFieldName::Reason,
														static_cast<std::uint64_t>( failure.reason)),
								common::DiagnosticHResult( failure.result)}) ;
	// 画面生成失敗では無効HWNDへ反映しない
	if( window_ == nullptr) {
		return ;
	}

	KillTimer( window_, 1U) ;
	EnableWindow( GetDlgItem( window_, IDC_DEPLOY_COMPLETE), TRUE) ;
	EnableWindow( GetDlgItem( window_, IDC_DEPLOY_PASTE), FALSE) ;
	EnableWindow( GetDlgItem( window_, IDC_DEPLOY_FOLDER_READY), FALSE) ;
	try {
		const std::array<UINT, 10U> names{
			IDS_AUTO_STAGE_NONE,	   IDS_AUTO_STAGE_REQUEST,
			IDS_AUTO_STAGE_VALIDATION, IDS_AUTO_STAGE_CLIPBOARD,
			IDS_AUTO_STAGE_RUN,		   IDS_AUTO_STAGE_TIMER,
			IDS_AUTO_STAGE_PASTE,	   IDS_AUTO_STAGE_ENTER,
			IDS_AUTO_STAGE_RESPONSE,   IDS_AUTO_STAGE_INPUT_PREPARATION} ;
		const auto index = static_cast<std::size_t>( failure.stage) ;
		const auto stage = Text( index < names.size() ? names[index] : IDS_AUTO_STAGE_NONE) ;
		std::array<wchar_t, 1024U> message{} ;
		// 起動文字列やclipboard内容を説明文へ含めない
		const UINT				   detail =
			failure.reason == agent_deployment::AutomaticDeploymentReason::PartialInput
								? IDS_AUTO_DEPLOY_PARTIAL_DETAILS
								: IDS_AUTO_DEPLOY_DETAILS ;
		// 部分受付ではキー解放案内を含む全文を固定bufferへ構築する
		if( swprintf_s( message.data(), message.size(), Text( detail).c_str(), stage.c_str(),
					  static_cast<unsigned int>( failure.stage),
					  static_cast<unsigned int>( failure.reason),
					  static_cast<unsigned int>( failure.result)) >= 0) {
			SetDlgItemTextW( window_, IDC_DEPLOY_STATUS, message.data()) ;
		}
	} catch( ...) {
		SetDlgItemTextW( window_, IDC_DEPLOY_STATUS,
						L"初期配置を停止しました。診断ログを確認してください。") ;
	}
}

void AgentDeploymentWindow::Invalidate() noexcept
{
	// 自動方式の世代失効を入力APIやtimeoutの失敗と区別する
	if( automatic_ && window_ != nullptr) {
		const auto failure = automatic_status_ ? automatic_status_()
											   : agent_deployment::AutomaticDeploymentSnapshot{} ;
		ShowAutomaticFailure( failure.status < 0
								 ? failure
								 : agent_deployment::AutomaticDeploymentSnapshot{
									   -1, agent_deployment::AutomaticDeploymentStage::Validation,
									   agent_deployment::AutomaticDeploymentReason::SessionChanged,
									   HRESULT_FROM_WIN32( ERROR_INVALID_STATE)}) ;
		return ;
	}

	invalidated_ = true ;
	pending_	 = false ;
	// 取消と世代失効でまだ送っていない自動入力を止める
	if( drive_ != nullptr) {
		drive_->Cancel() ;
	}

	// 画面不在では無効HWNDへ触れない
	if( window_ == nullptr) {
		return ;
	}

	EnableWindow( GetDlgItem( window_, IDC_DEPLOY_PASTE), FALSE) ;
	EnableWindow( GetDlgItem( window_, IDC_DEPLOY_FOLDER_READY), FALSE) ;
	try {
		SetDlgItemTextW( window_, IDC_DEPLOY_STATUS,
						Text( automatic_ ? IDS_AUTO_DEPLOY_FAILED : IDS_DEPLOY_INVALIDATED).c_str()) ;
	} catch( ...) {
	}
}

bool AgentDeploymentWindow::CloseCompleted() noexcept
{
	// 応答未確認・入力途中・失敗の画面を再起動要求で隠さない
	if( window_ == nullptr || !automatic_ || !response_confirmed_ || pending_ || invalidated_ ||
	   automatic_failure_.status < 0) {
		return false ;
	}

	Finish() ;
	return !active() ;
}

void AgentDeploymentWindow::Finish() noexcept
{
	// 既に送信した起動や配置済みfileは自動削除・強制終了しない
	if( drive_ != nullptr) {
		drive_->Cancel() ;
		drive_.reset() ;
	}

	try {
		// 別アプリが置換したclipboardは消さず、旧内容の読取・復元もしない
		if( object_ != nullptr) {
			const auto owns = clipboard_.current ? clipboard_.current( object_) : E_UNEXPECTED ;
			// 判定不能を他アプリ所有と誤認せず、公開状態と追加接続制約を維持する
			if( (owns != S_OK && owns != S_FALSE) ||
			   (owns == S_OK && FAILED( clipboard_.clear( object_)))) {
				Invalidate() ;
				// clear失敗と接続切断を別の固定説明へ分類する
				if( window_ != nullptr) {
					SetDlgItemTextW( window_, IDC_DEPLOY_STATUS,
									Text( IDS_DEPLOY_CLEAR_FAILED).c_str()) ;
				}

				return ;
			}
		}
	} catch( ...) {
		Invalidate() ;
		return ;
	}

	// 自分の参照だけを解放する。返却済みstreamはsnapshotを保持し続ける
	if( object_ != nullptr) {
		object_->Release() ;
		object_ = nullptr ;
	}

	preparing_			= false ;
	pending_			= false ;
	response_confirmed_ = false ;
	paste_				= {} ;
	target_.clear() ;
	// WM_NCDESTROYでwindow_を無効化する
	if( window_ != nullptr) {
		DestroyWindow( window_) ;
	}
}

INT_PTR CALLBACK AgentDeploymentWindow::DialogProcedure( HWND window, UINT message, WPARAM wparam,
														LPARAM lparam) noexcept
{
	auto *self = reinterpret_cast<AgentDeploymentWindow *>( GetWindowLongPtrW( window, DWLP_USER)) ;
	// controllerは所有manager STAからのみ設定する
	if( message == WM_INITDIALOG) {
		self = reinterpret_cast<AgentDeploymentWindow *>( lparam) ;
		SetWindowLongPtrW( window, DWLP_USER, lparam) ;
		self->window_ = window ;
	}

	// 初期化前は標準dialog処理へ戻す
	if( self == nullptr) {
		return FALSE ;
	}

	try {
		// 初期表示はfolder未確認・貼付け不可・完了不可に固定する
		if( message == WM_INITDIALOG) {
			SetDlgItemTextW( window, IDC_DEPLOY_TARGET, self->target_.c_str()) ;
			SetDlgItemTextW( window, IDC_DEPLOY_STATUS, self->Text( IDS_DEPLOY_READY).c_str()) ;
			EnableWindow( GetDlgItem( window, IDC_DEPLOY_PASTE), FALSE) ;
			EnableWindow( GetDlgItem( window, IDC_DEPLOY_COMPLETE), FALSE) ;
			// 自動方式ではfolder確認と貼付け操作を利用者へ要求しない
			if( self->automatic_) {
				SetWindowTextW( window, self->Text( IDS_AUTO_DEPLOY_TITLE).c_str()) ;
				SetDlgItemTextW( window, IDC_DEPLOY_INSTRUCTIONS,
								self->Text( IDS_AUTO_DEPLOY_INSTRUCTIONS).c_str()) ;
				ShowWindow( GetDlgItem( window, IDC_DEPLOY_FOLDER_READY), SW_HIDE) ;
				ShowWindow( GetDlgItem( window, IDC_DEPLOY_PASTE), SW_HIDE) ;
				SetDlgItemTextW( window, IDC_DEPLOY_COMPLETE, L"閉じる") ;
				SetDlgItemTextW(
					window, IDC_DEPLOY_FOOTER,
					L"Windowsの警告は接続先で確認してください。更新前ファイルは接続先にバックアップします。") ;
				SetDlgItemTextW( window, IDC_DEPLOY_STATUS,
								self->Text( IDS_AUTO_DEPLOY_WAITING).c_str()) ;
				// 状態確認timerが作れない場合は入力を開始しない
				if( SetTimer( window, 1U, 250U, nullptr) == 0U) {
					const DWORD error = GetLastError() ;
					self->ShowAutomaticFailure(
						{-1, agent_deployment::AutomaticDeploymentStage::Timer,
						 agent_deployment::AutomaticDeploymentReason::TimerFailure,
						 error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32( error)}) ;
				}
			}

			return TRUE ;
		}

		// 自動配置は有効なowner応答と有限timeoutだけを確認する
		if( message == WM_TIMER && self->automatic_) {
			const auto status =
				self->automatic_status_
					? self->automatic_status_()
					: agent_deployment::AutomaticDeploymentSnapshot{
						  -1, agent_deployment::AutomaticDeploymentStage::Validation,
						  agent_deployment::AutomaticDeploymentReason::StatusUnavailable,
						  E_UNEXPECTED} ;
			// 即時停止では元の段階とHRESULTを表示する
			if( status.status < 0) {
				self->ShowAutomaticFailure( status) ;
			}
			// 入力受付とは別に、同じRDP sessionの応答を確認した場合だけ閉じる
			else if( status.status == 1 && self->sent_ && !self->pending_ && !self->invalidated_) {
				self->response_confirmed_ = true ;
				// キー受付とは別に、同じRDP sessionのAgent応答を確認する
				KillTimer( window, 1U) ;
				SetDlgItemTextW( window, IDC_DEPLOY_STATUS, self->Text( IDS_START_CONNECTED).c_str()) ;
				EnableWindow( GetDlgItem( window, IDC_DEPLOY_COMPLETE), TRUE) ;
			}
			// 応答待ち期限は入力失敗とは別の分類として残す
			else if( agent_deployment::AutomaticDeploymentTimedOut( self->automatic_started_,
																  self->AutomaticTick())) {
				const bool awaiting =
					status.stage == agent_deployment::AutomaticDeploymentStage::AgentResponse ;
				self->ShowAutomaticFailure(
					{-1,
					 status.stage == agent_deployment::AutomaticDeploymentStage::None
						 ? agent_deployment::AutomaticDeploymentStage::Request
						 : status.stage,
					 awaiting ? agent_deployment::AutomaticDeploymentReason::AgentTimeout
							  : agent_deployment::AutomaticDeploymentReason::InputTimeout,
					 HRESULT_FROM_WIN32( WAIT_TIMEOUT)}) ;
			}

			return TRUE ;
		}

		// 破棄後のHWNDをcallbackへ残さない
		if( message == WM_NCDESTROY) {
			self->window_ = nullptr ;
			return FALSE ;
		}

		// closeは転送が終了または中断した明示確認を必要とする
		if( message == WM_CLOSE || (message == WM_COMMAND && LOWORD( wparam) == IDCANCEL)) {
			// 予約済みowner結果を待ってから中止を受け付ける
			if( self->Confirm( IDS_DEPLOY_CANCEL_CONFIRM)) {
				self->Finish() ;
			}

			return TRUE ;
		}

		// 固定buttonのclickだけを処理し、他control通知を誤認しない
		if( message != WM_COMMAND || HIWORD( wparam) != BN_CLICKED) {
			return FALSE ;
		}

		const auto id	  = LOWORD( wparam) ;
		const bool folder = IsDlgButtonChecked( window, IDC_DEPLOY_FOLDER_READY) == BST_CHECKED ;
		// 明示確認の取り消しを即時に貼付け可否へ反映する
		if( id == IDC_DEPLOY_FOLDER_READY) {
			EnableWindow( GetDlgItem( window, IDC_DEPLOY_PASTE),
						 folder && !self->pending_ && !self->invalidated_) ;
			return TRUE ;
		}

		// 二ファイル確認はキー要求成功と別の利用者確認にする
		if( id == IDC_DEPLOY_COMPLETE) {
			// 自動方式の閉じる操作は人手での配置確認と混同しない
			if( self->automatic_ && IsWindowEnabled( GetDlgItem( window, IDC_DEPLOY_COMPLETE))) {
				self->Finish() ;
				return TRUE ;
			}

			// 送信前／送信中に「完了」の確認へ進まない
			if( self->sent_ && !self->pending_ && self->Confirm( IDS_DEPLOY_COMPLETE_CONFIRM)) {
				self->Finish() ;
			}

			return TRUE ;
		}

		// 任意入力は送らず、folder確認済みの一回のCtrl+Vだけを予約する
		if( id == IDC_DEPLOY_PASTE && folder && !self->pending_ && !self->invalidated_) {
			// clipboardを別内容へ置き換えた場合はキーを送らない
			if( self->clipboard_.current( self->object_) != S_OK) {
				self->Invalidate() ;
				return TRUE ;
			}

			self->pending_ = true ;
			EnableWindow( GetDlgItem( window, IDC_DEPLOY_PASTE), FALSE) ;
			EnableWindow( GetDlgItem( window, IDC_DEPLOY_COMPLETE), FALSE) ;
			SetDlgItemTextW( window, IDC_DEPLOY_STATUS,
							self->Text( IDS_DEPLOY_PASTE_PENDING).c_str()) ;
			// owner queue受付失敗でも「キー送信成功」と表示しない
			if( !self->paste_( GetClipboardSequenceNumber())) {
				self->OnPasteResult( E_FAIL) ;
			}

			return TRUE ;
		}
	} catch( ...) {
		self->Invalidate() ;
		return TRUE ;
	}

	return FALSE ;
}
} // namespace zrdp::client::app
