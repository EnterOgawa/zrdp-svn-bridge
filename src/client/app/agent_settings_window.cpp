//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_settings_window.cpp
 * @brief 接続専用Agent設定の登録・削除と非同期自動保存を実装する。
 * @version 01.00.07
 * @date 2026/10/08
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  Agent設定のmodeless UIを実装。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  接続別設定と自動Agent管理・診断削除を反映。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  登録・削除だけの自動保存画面へ簡素化。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  接続変更と登録済み宛先だけの表示・起動を統一。
 * Ver.01.00.07   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/app/agent_settings_window.h"
#include "client/app/resource.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <utility>

namespace zrdp::client::app {
namespace {

// 設定profileだけを短く参照するnamespace alias
namespace settings = agent_settings ;

} // namespace

AgentSettingsWindow::AgentSettingsWindow( const HINSTANCE					   instance,
										 agent_settings::AgentSettingsService &service)
	: instance_( instance), service_( service)
{
}

AgentSettingsWindow::~AgentSettingsWindow()
{
	// applicationより短い画面のWin32資源だけをUI threadで解放する
	if( window_ != nullptr) {
		DestroyWindow( window_) ;
	}
}

HWND AgentSettingsWindow::window() const noexcept
{
	return window_ ;
}

bool AgentSettingsWindow::Show( const HWND owner, std::wstring preferred_name,
							   std::wstring connection_key)
{
	// 生存画面は現在選択へ追従させ、重複Windowを生成しない
	if( window_ != nullptr) {
		ChangeConnection( std::move( preferred_name), std::move( connection_key)) ;
		ShowWindow( window_, SW_RESTORE) ;
		SetForegroundWindow( window_) ;
		return true ;
	}

	preferred_name_ =
		preferred_name.empty() ? Text( IDS_AGENT_SETTINGS_DEFAULT_NAME) : std::move( preferred_name) ;
	connection_key_ = std::move( connection_key) ;
	pending_connection_.reset() ;
	pending_		= 0U ;
	waiting_load_	= false ;
	storage_loaded_ = false ;
	saving_			= false ;
	profiles_.clear() ;
	draft_ = {preferred_name_, {}} ;
	INITCOMMONCONTROLSEX controls{sizeof( controls), ICC_LISTVIEW_CLASSES} ;
	// 標準list viewを登録できない場合は部分画面を公開しない
	if( !InitCommonControlsEx( &controls)) {
		return false ;
	}

	window_ =
		CreateDialogParamW( instance_, MAKEINTRESOURCEW( IDD_AGENT_SETTINGS), owner,
						   &AgentSettingsWindow::DialogProcedure, reinterpret_cast<LPARAM>( this)) ;
	// resourceまたはcontrol生成失敗を空の画面として公開しない
	if( window_ == nullptr) {
		return false ;
	}

	ShowWindow( window_, SW_SHOW) ;
	RequestLoad() ;
	return true ;
}

void AgentSettingsWindow::ChangeConnection( std::wstring preferred_name, std::wstring connection_key)
{
	// 閉じた設定画面を接続変更だけで新規表示しない
	if( window_ == nullptr) {
		return ;
	}

	preferred_name =
		preferred_name.empty() ? Text( IDS_AGENT_SETTINGS_DEFAULT_NAME) : std::move( preferred_name) ;
	// 同じ接続へ戻した場合は保留した別接続への変更も取消す
	if( preferred_name == preferred_name_ &&
	   (connection_key == connection_key_ ||
		settings::SameAgentConnectionKey( connection_key, connection_key_))) {
		pending_connection_.reset() ;
		return ;
	}

	// 保存対象を途中で変えず、最後の入力変更だけを一件保持する
	if( pending_ != 0U || waiting_load_ || saving_) {
		pending_connection_ =
			PendingConnection{std::move( preferred_name), std::move( connection_key)} ;
		return ;
	}

	pending_connection_.reset() ;
	preferred_name_ = std::move( preferred_name) ;
	connection_key_ = std::move( connection_key) ;
	storage_loaded_ = false ;
	profiles_.clear() ;
	draft_ = {preferred_name_, {}, connection_key_} ;
	RenderEndpoints() ;
	ClearEndpointFields() ;
	SetDlgItemTextW( window_, IDC_AGENT_PROFILE, preferred_name_.c_str()) ;
	RequestLoad() ;
}

bool AgentSettingsWindow::ProcessDialogMessage( MSG &message) const noexcept
{
	return window_ != nullptr && IsDialogMessageW( window_, &message) != FALSE ;
}

INT_PTR CALLBACK AgentSettingsWindow::DialogProcedure( const HWND window, const UINT message,
													  const WPARAM wparam,
													  const LPARAM lparam) noexcept
{
	auto *self = reinterpret_cast<AgentSettingsWindow *>( GetWindowLongPtrW( window, DWLP_USER)) ;
	// 最初のmessageだけでcontrollerの非所有参照をWindowへ関連付ける
	if( message == WM_INITDIALOG) {
		self = reinterpret_cast<AgentSettingsWindow *>( lparam) ;
		SetWindowLongPtrW( window, DWLP_USER, reinterpret_cast<LONG_PTR>( self)) ;
		self->window_ = window ;
	}

	// 関連付け前のmessageをinstanceへ渡さない
	if( self == nullptr) {
		return FALSE ;
	}

	try {
		return self->HandleMessage( message, wparam, lparam) ;
	} catch( ...) {
		self->ShowError( {settings::AgentSettingsError::ResourceExhausted}) ;
		return TRUE ;
	}
}

INT_PTR AgentSettingsWindow::HandleMessage( const UINT message, const WPARAM wparam,
										   const LPARAM lparam)
{
	// 登録・削除・画面終了と一覧選択だけを固定messageへ対応付ける
	switch( message) {
	// 有限列と入力上限を所有UI threadで初期化する
	case WM_INITDIALOG:
		InitializeControls() ;
		return TRUE ;
	// 古い画面世代の結果を新しいHWNDへ反映しない
	case WM_NCDESTROY:
		window_		  = nullptr ;
		pending_	  = 0U ;
		waiting_load_ = false ;
		saving_		  = false ;
		return FALSE ;
	// 保存結果を確認するまで画面を閉じて失敗を見失わない
	case WM_CLOSE:
		// workerの保存中だけ終了を待ち、未登録入力の破棄確認は増やさない
		if( saving_) {
			return TRUE ;
		}

		DestroyWindow( window_) ;
		return TRUE ;
	// 選択した一entryを登録欄へ戻す
	case WM_NOTIFY: {
		const auto *header = reinterpret_cast<const NMHDR *>( lparam) ;
		// 復元中とworker処理中の選択通知を新しい操作にしない
		if( !restoring_ && pending_ == 0U && header != nullptr &&
		   header->idFrom == IDC_AGENT_ENDPOINTS && header->code == LVN_ITEMCHANGED) {
			const auto *change = reinterpret_cast<const NMLISTVIEW *>( lparam) ;
			// 所有snapshot内の選択状態だけを復元する
			if( (change->uChanged & LVIF_STATE) != 0U && (change->uNewState & LVIS_SELECTED) != 0U &&
			   change->iItem >= 0) {
				const auto index = RegisteredEndpointIndex( change->iItem) ;
				// hidden候補や改変されたitem dataを編集対象にしない
				if( index) {
					SelectEndpoint( *index) ;
				}
			}
		}

		return FALSE ;
	}

	// 保存・INI・profileの廃止commandから要求を作らない
	case WM_COMMAND: {
		const WORD identifier = LOWORD( wparam) ;
		// 登録前の入力通知は永続設定に反映しない
		if( restoring_ || HIWORD( wparam) != BN_CLICKED) {
			return FALSE ;
		}

		// 閉じるだけは初回Load中も受け付ける
		if( identifier == IDCANCEL) {
			SendMessageW( window_, WM_CLOSE, 0U, 0L) ;
			return TRUE ;
		}

		// 二重操作と読込み失敗後の空一覧上書きを拒否する
		if( pending_ != 0U || !storage_loaded_ || connection_key_.empty()) {
			return TRUE ;
		}

		// 現行の二操作だけを候補検証へ渡す
		switch( identifier) {
		// 入力した宛先の明示承認と保存を一操作にする
		case IDC_AGENT_ADD:
			RegisterEndpoint() ;
			return TRUE ;
		// 選択宛先だけを削除して保存する
		case IDC_AGENT_REMOVE:
			RemoveEndpoint() ;
			return TRUE ;
		// 廃止commandは標準処理へ渡し、workerを動かさない
		default:
			return FALSE ;
		}
	}

	// dialogの背景をWindows標準色へ合わせる
	case WM_CTLCOLORDLG:
		return reinterpret_cast<INT_PTR>( GetSysColorBrush( COLOR_3DFACE)) ;
	// 接続表示とlabelの背景を親へ揃える
	case WM_CTLCOLORSTATIC:
		SetBkColor( reinterpret_cast<HDC>( wparam), GetSysColor( COLOR_3DFACE)) ;
		return reinterpret_cast<INT_PTR>( GetSysColorBrush( COLOR_3DFACE)) ;
	// 標準の移動・描画・focus処理を維持する
	default:
		return FALSE ;
	}
}

void AgentSettingsWindow::InitializeControls()
{
	SetDlgItemTextW( window_, IDC_AGENT_PROFILE, preferred_name_.c_str()) ;
	SendDlgItemMessageW( window_, IDC_AGENT_IPV4, EM_SETLIMITTEXT, 15U, 0L) ;
	SendDlgItemMessageW( window_, IDC_AGENT_PORT, EM_SETLIMITTEXT, 5U, 0L) ;
	ComboBox_AddString( GetDlgItem( window_, IDC_AGENT_PURPOSE),
					   Text( IDS_AGENT_SETTINGS_SVN).c_str()) ;
	ComboBox_AddString( GetDlgItem( window_, IDC_AGENT_PURPOSE),
					   Text( IDS_AGENT_SETTINGS_TRAC).c_str()) ;
	const HWND list = GetDlgItem( window_, IDC_AGENT_ENDPOINTS) ;
	ListView_SetExtendedListViewStyle( list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER) ;
	RECT rectangle{} ;
	GetClientRect( list, &rectangle) ;
	const std::array<UINT, 3U> headers{IDS_AGENT_SETTINGS_COL_IP, IDS_AGENT_SETTINGS_COL_PORT,
									   IDS_AGENT_SETTINGS_COL_PURPOSE} ;
	const std::array<int, 3U>  ratios{42, 20, 38} ;
	// IP・port・用途だけを現在DPIのlist幅へ配置する
	for( std::size_t index = 0U; index < headers.size(); ++index) {
		auto	  name = Text( headers[index]) ;
		LVCOLUMNW column{} ;
		column.mask	   = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM ;
		column.pszText = name.data() ;
		column.cx	   = (rectangle.right - rectangle.left - GetSystemMetrics( SM_CXVSCROLL) - 4) *
					ratios[index] / 100 ;
		column.iSubItem = static_cast<int>( index) ;
		static_cast<void>( ListView_InsertColumn( list, static_cast<int>( index), &column)) ;
	}

	ClearEndpointFields() ;
}

void AgentSettingsWindow::RequestLoad()
{
	waiting_load_ = true ;
	pending_	  = service_.Request( {}) ;
	// 古い画面世代の結果をdrain後に初回Loadを再予約する
	if( pending_ != 0U) {
		waiting_load_ = false ;
	}

	SetBusy( true) ;
}

void AgentSettingsWindow::SaveCandidate( settings::AgentSettingsProfile candidate)
{
	// 復号失敗や別要求の結果を待つ間に保存snapshotを作らない
	if( !storage_loaded_ || pending_ != 0U || waiting_load_ || connection_key_.empty()) {
		ShowText( IDS_AGENT_SETTINGS_QUEUE_BUSY) ;
		return ;
	}

	candidate.name			 = preferred_name_ ;
	candidate.connection_key = connection_key_ ;
	settings::AgentSettingsRequest request ;
	request.operation  = settings::AgentSettingsOperation::Save ;
	request.profiles   = profiles_ ;
	const auto checked = settings::UpsertAgentSettingsProfile( request.profiles, candidate) ;
	// 不正・重複・profile上限では旧一覧を変更しない
	if( !checked.succeeded()) {
		ShowError( checked) ;
		return ;
	}

	const auto ticket = service_.Request( std::move( request)) ;
	// 他の設定worker要求を上書きせず、入力を残して再登録できるようにする
	if( ticket == 0U) {
		ShowText( IDS_AGENT_SETTINGS_QUEUE_BUSY) ;
		return ;
	}

	pending_ = ticket ;
	saving_	 = true ;
	SetBusy( true) ;
}

void AgentSettingsWindow::SetBusy( const bool busy)
{
	const std::array<int, 6U> identifiers{IDC_AGENT_ENDPOINTS, IDC_AGENT_IPV4, IDC_AGENT_PORT,
										  IDC_AGENT_PURPOSE,   IDC_AGENT_ADD,  IDC_AGENT_REMOVE} ;
	// file I/O中と読込み失敗では登録・削除を止め、RDPは維持する
	for( const int identifier : identifiers) {
		EnableWindow( GetDlgItem( window_, identifier),
					 !busy && storage_loaded_ && !connection_key_.empty()) ;
	}

	EnableWindow( GetDlgItem( window_, IDCANCEL), !saving_) ;
	// worker待機だけを内容を含まない固定表示にする
	if( busy) {
		SetDlgItemTextW( window_, IDC_AGENT_STATUS, Text( IDS_AGENT_SETTINGS_BUSY).c_str()) ;
	}
}

void AgentSettingsWindow::OnNotice( settings::AgentSettingsNotice notice) noexcept
{
	try {
		// 閉じた画面へ古い結果を公開しない
		if( window_ == nullptr) {
			return ;
		}

		// 古い画面の結果と新しいLoadを受付IDで分離する
		if( notice.request_id != pending_) {
			// 他のworker結果を回収した後に初回Loadだけを再予約する
			if( waiting_load_) {
				RequestLoad() ;
			}

			return ;
		}

		pending_ = 0U ;
		saving_	 = false ;
		// 保存失敗では旧一覧と再登録用入力を保持する
		if( !notice.result.succeeded()) {
			SetBusy( false) ;
			ShowError( notice.result) ;
			return ;
		}

		// Loadと保存だけを公開し、廃止INI操作を成功表示しない
		if( notice.operation != settings::AgentSettingsOperation::Load &&
		   notice.operation != settings::AgentSettingsOperation::Save) {
			SetBusy( false) ;
			ShowError( {settings::AgentSettingsError::InvalidData}) ;
			return ;
		}

		profiles_		= std::move( notice.profiles) ;
		storage_loaded_ = true ;
		// 保存成功後だけ対象を切り替え、古い結果を新接続の一覧として公開しない
		if( pending_connection_) {
			auto connection = std::move( *pending_connection_) ;
			pending_connection_.reset() ;
			ChangeConnection( std::move( connection.name), std::move( connection.key)) ;
			return ;
		}

		SelectProfile( preferred_name_) ;
		SetBusy( false) ;
		SetDlgItemTextW(
			window_, IDC_AGENT_STATUS,
			Text( notice.operation == settings::AgentSettingsOperation::Load
					 ? (connection_key_.empty() ? IDS_AGENT_SETTINGS_SELECT_CONNECTION
						: ListView_GetItemCount( GetDlgItem( window_, IDC_AGENT_ENDPOINTS)) == 0
							? IDS_AGENT_SETTINGS_EMPTY
							: IDS_AGENT_SETTINGS_LOADED)
					 : IDS_AGENT_SETTINGS_SAVED)
				.c_str()) ;
	} catch( ...) {
		saving_ = false ;
		SetBusy( false) ;
		ShowError( {settings::AgentSettingsError::ResourceExhausted}) ;
	}
}

void AgentSettingsWindow::SelectProfile( std::wstring name)
{
	const auto found =
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT
		 * @brief 設定画面の接続キーに一致する保存profileだけを選ぶ。
		 * @param[in] profile 読込み済みの候補。別接続の設定を表示しない。
		 * @return 同じ接続キーの場合はtrue。
		 * @endcond
		 */
		std::find_if( profiles_.begin(), profiles_.end(), [this]( const auto &profile) {
			return settings::SameAgentConnectionKey( profile.connection_key, connection_key_) ;
		}) ;
	draft_ =
		found == profiles_.end() ? settings::AgentSettingsProfile{std::move( name), {}} : *found ;
	draft_.name			  = preferred_name_ ;
	draft_.connection_key = connection_key_ ;
	SetDlgItemTextW( window_, IDC_AGENT_PROFILE, preferred_name_.c_str()) ;
	RenderEndpoints() ;
	ClearEndpointFields() ;
}

void AgentSettingsWindow::RenderEndpoints()
{
	restoring_		= true ;
	const HWND list = GetDlgItem( window_, IDC_AGENT_ENDPOINTS) ;
	ListView_DeleteAllItems( list) ;
	// 有限snapshotの三列だけをcontrolへcopyする
	for( std::size_t index = 0U; index < draft_.endpoints.size(); ++index) {
		const auto &entry = draft_.endpoints[index] ;
		// 未登録候補を他接続の登録済み情報と誤認させない
		if( !entry.approved) {
			continue ;
		}

		auto purpose =
			Text( entry.purpose == settings::AgentEndpointPurpose::Unspecified
					 ? IDS_AGENT_SETTINGS_UNSPECIFIED
				 : entry.purpose == settings::AgentEndpointPurpose::Svn ? IDS_AGENT_SETTINGS_SVN
																		: IDS_AGENT_SETTINGS_TRAC) ;
		std::array<std::wstring, 3U> columns{settings::FormatAgentSettingsIpv4( entry.target),
											 std::to_wstring( entry.target.port),
											 std::move( purpose)} ;
		LVITEMW						 item{} ;
		item.mask	  = LVIF_TEXT | LVIF_PARAM ;
		item.lParam	  = static_cast<LPARAM>( index) ;
		item.iItem	  = ListView_GetItemCount( list) ;
		item.pszText  = columns[0].data() ;
		const int row = ListView_InsertItem( list, &item) ;
		// resource不足で生成できないrowへ別entryの文字列を流さない
		if( row < 0) {
			continue ;
		}

		// portと用途も所有文字列の寿命内でcopyする
		for( std::size_t column = 1U; column < columns.size(); ++column) {
			ListView_SetItemText( list, row, static_cast<int>( column), columns[column].data()) ;
		}
	}

	restoring_ = false ;
}

std::optional<std::size_t>
AgentSettingsWindow::RegisteredEndpointIndex( const int row) const noexcept
{
	LVITEMW item{} ;
	item.mask  = LVIF_PARAM ;
	item.iItem = row ;
	// 表示rowと元indexの範囲・登録状態を同時に検証する
	if( row < 0 || !ListView_GetItem( GetDlgItem( window_, IDC_AGENT_ENDPOINTS), &item) ||
	   item.lParam < 0 || static_cast<std::size_t>( item.lParam) >= draft_.endpoints.size() ||
	   !draft_.endpoints[static_cast<std::size_t>( item.lParam)].approved) {
		return std::nullopt ;
	}

	return static_cast<std::size_t>( item.lParam) ;
}

void AgentSettingsWindow::SelectEndpoint( const std::size_t index)
{
	const auto &entry = draft_.endpoints[index] ;
	restoring_		  = true ;
	SetDlgItemTextW( window_, IDC_AGENT_IPV4,
					settings::FormatAgentSettingsIpv4( entry.target).c_str()) ;
	SetDlgItemTextW( window_, IDC_AGENT_PORT, std::to_wstring( entry.target.port).c_str()) ;
	ComboBox_SetCurSel( GetDlgItem( window_, IDC_AGENT_PURPOSE),
					   entry.purpose == settings::AgentEndpointPurpose::Unspecified ? -1
					   : entry.purpose == settings::AgentEndpointPurpose::Svn		? 0
																					: 1) ;
	restoring_ = false ;
}

void AgentSettingsWindow::ClearEndpointFields()
{
	restoring_ = true ;
	SetDlgItemTextW( window_, IDC_AGENT_IPV4, L"") ;
	SetDlgItemTextW( window_, IDC_AGENT_PORT, L"80") ;
	ComboBox_SetCurSel( GetDlgItem( window_, IDC_AGENT_PURPOSE), 0) ;
	restoring_ = false ;
}

void AgentSettingsWindow::RegisterEndpoint()
{
	settings::AgentEndpointSetting entry ;
	const auto parsed = settings::ParseAgentSettingsTarget( Input( IDC_AGENT_IPV4),
														   Input( IDC_AGENT_PORT), entry.target) ;
	// 不正IP・portを承認や永続保存へ渡さない
	if( !parsed.succeeded()) {
		ShowError( parsed) ;
		return ;
	}

	const int purpose = ComboBox_GetCurSel( GetDlgItem( window_, IDC_AGENT_PURPOSE)) ;
	// 未選択用途をSVNやTracへ暗黙分類しない
	if( purpose != 0 && purpose != 1) {
		ShowError( {settings::AgentSettingsError::InvalidData}) ;
		return ;
	}

	entry.purpose =
		purpose == 0 ? settings::AgentEndpointPurpose::Svn : settings::AgentEndpointPurpose::Trac ;
	entry.approved		 = true ;
	auto	   candidate = draft_ ;
	const auto found =
		std::find_if( candidate.endpoints.begin(), candidate.endpoints.end(),
					 /**
					  * @cond LOCAL_CALLBACK_CONTRACT
					  * @brief 同じIP・portの登録を既存entryの更新へまとめる。
					  * @param[in] value 変更前の登録済み宛先。比較中だけ借用する。
					  * @return targetが完全一致する場合はtrue。
					  * @endcond
					  */
					 [&entry]( const auto &value) { return value.target == entry.target ; }) ;
	// 同じ完全一致宛先の再登録は重複を作らず匿名IDを保つ
	if( found != candidate.endpoints.end()) {
		entry.endpoint_id = found->endpoint_id ;
		*found			  = entry ;
	} else {
		// 新規登録だけに最大128件を適用する
		if( candidate.endpoints.size() >= agent::network::kMaximumAllowlistEntries) {
			ShowError( {settings::AgentSettingsError::InvalidEndpoint,
					   agent::network::AllowlistError::TooManyEntries}) ;
			return ;
		}

		entry.endpoint_id = 1001U ;
		// 有限一覧で既存INIの匿名IDを上書きしない最小空きIDを選ぶ
		while( std::any_of(
			candidate.endpoints.begin(), candidate.endpoints.end(),
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief 登録IDの重複を避けて最小空きIDを選ぶ。
			 * @param[in] value 同じprofileの既存entry。宛先ではなく匿名IDを比較する。
			 * @return 今回の候補IDが既に使用されている場合はtrue。
			 * @endcond
			 */
			[&entry]( const auto &value) { return value.endpoint_id == entry.endpoint_id ; })) {
			++entry.endpoint_id ;
		}

		candidate.endpoints.push_back( entry) ;
	}

	SaveCandidate( std::move( candidate)) ;
}

void AgentSettingsWindow::RemoveEndpoint()
{
	const int selected =
		ListView_GetNextItem( GetDlgItem( window_, IDC_AGENT_ENDPOINTS), -1, LVNI_SELECTED) ;
	// 選択のない削除で他の宛先を推定しない
	const auto index = RegisteredEndpointIndex( selected) ;
	// 登録済み表示row以外の候補を削除しない
	if( !index) {
		ShowText( IDS_AGENT_SETTINGS_SELECT_ENTRY) ;
		return ;
	}

	auto candidate = draft_ ;
	candidate.endpoints.erase( candidate.endpoints.begin() + *index) ;
	SaveCandidate( std::move( candidate)) ;
}

void AgentSettingsWindow::ShowText( const UINT identifier) const
{
	SetDlgItemTextW( window_, IDC_AGENT_STATUS, Text( identifier).c_str()) ;
}

void AgentSettingsWindow::ShowError( const settings::AgentSettingsResult result) const noexcept
{
	try {
		std::array<wchar_t, 1024U> message{} ;
		const auto				   format = Text( IDS_AGENT_SETTINGS_ERROR_FORMAT) ;
		// 固定error数値だけを表示し、実endpointやfile pathを診断に含めない
		if( !format.empty() && swprintf_s( message.data(), message.size(), format.c_str(),
										 static_cast<unsigned int>( result.error),
										 static_cast<unsigned int>( result.allowlist_error),
										 static_cast<unsigned int>( result.line)) >= 0) {
			SetDlgItemTextW( window_, IDC_AGENT_STATUS, message.data()) ;
		}
	} catch( ...) {
		std::array<wchar_t, 128U> fallback{} ;
		static_cast<void>( LoadStringW( instance_, IDS_AGENT_SETTINGS_FAILURE, fallback.data(),
									  static_cast<int>( fallback.size()))) ;
		SetDlgItemTextW( window_, IDC_AGENT_STATUS, fallback.data()) ;
	}
}

std::wstring AgentSettingsWindow::Text( const UINT identifier) const
{
	std::array<wchar_t, 2048U> buffer{} ;
	const int				   count =
		LoadStringW( instance_, identifier, buffer.data(), static_cast<int>( buffer.size())) ;
	return count > 0 ? std::wstring( buffer.data(), static_cast<std::size_t>( count))
					 : std::wstring{} ;
}

std::wstring AgentSettingsWindow::Input( const int identifier) const
{
	const HWND control = GetDlgItem( window_, identifier) ;
	const int  length  = GetWindowTextLengthW( control) ;
	// owned controlでもbounded copyを徹底する
	if( length < 0 || length > static_cast<int>( settings::kMaximumAgentProfileName)) {
		return {} ;
	}

	std::array<wchar_t, settings::kMaximumAgentProfileName + 1U> buffer{} ;
	const int count = GetWindowTextW( control, buffer.data(), static_cast<int>( buffer.size())) ;
	return count > 0 ? std::wstring( buffer.data(), static_cast<std::size_t>( count))
					 : std::wstring{} ;
}

} // namespace zrdp::client::app
