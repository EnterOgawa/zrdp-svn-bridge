//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_start_window.cpp
 * @brief 選択したAgentへの確認付きEnter要求と応答待ちを実装する。
 * @version 01.00.01
 * @date 2026/10/09
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/09  固定キー・認証世代・利用者確認を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/app/agent_start_window.h"
#include "client/app/resource.h"
#include <array>
#include <utility>

namespace zrdp::client::app {
AgentStartWindow::AgentStartWindow( HINSTANCE instance, std::function<ULONGLONG()> clock)
	: instance_( instance), clock_( std::move( clock))
{
}

AgentStartWindow::~AgentStartWindow()
{
	// 所有STA終了前にmodeless画面を回収する
	if( window_ != nullptr) {
		DestroyWindow( window_) ;
	}
}

bool AgentStartWindow::Show( std::wstring target, std::uint64_t epoch, std::function<HRESULT()> send,
							std::function<HRESULT()> prepare)
{
	// 再表示で別の接続世代へ暗黙に付け替えない
	if( window_ != nullptr) {
		SetForegroundWindow( window_) ;
		return epoch == epoch_ ;
	}

	// 未認証または入力境界なしでは起動操作を公開しない
	if( epoch == 0U || !send) {
		return false ;
	}

	target_		 = std::move( target) ;
	epoch_		 = epoch ;
	send_		 = std::move( send) ;
	prepare_	 = std::move( prepare) ;
	sequence_	 = rdp::FixedInputSequence{} ;
	stopped_	 = false ;
	invalidated_ = false ;
	ready_		 = false ;
	window_		 = CreateDialogParamW( instance_, MAKEINTRESOURCEW( IDD_AGENT_START), nullptr,
									  DialogProcedure, reinterpret_cast<LPARAM>( this)) ;
	// 画面がなければキー要求を残さない
	if( window_ == nullptr) {
		send_ = {} ;
		return false ;
	}

	ShowWindow( window_, SW_SHOW) ;
	SetForegroundWindow( window_) ;
	return true ;
}

void AgentStartWindow::Status( UINT id) const noexcept
{
	std::array<wchar_t, 512U> text{} ;
	static_cast<void>( LoadStringW( instance_, id, text.data(), static_cast<int>( text.size()))) ;
	SetDlgItemTextW( window_, IDC_START_STATUS, text.data()) ;
}

void AgentStartWindow::Update( std::uint64_t epoch, bool ready) noexcept
{
	// 未表示画面には通知を蓄積しない
	if( window_ == nullptr) {
		return ;
	}

	// 切断・再認証した同じserverにも古いキーを送らない
	if( epoch == 0U || epoch != epoch_) {
		invalidated_ = true ;
		sequence_.Cancel() ;
		KillTimer( window_, 0x704U) ;
		ready_	 = false ;
		stopped_ = true ;
		Status( IDS_START_INVALIDATED) ;
	}
	// 実際にAgentから応答を受けた場合だけ接続確認と表示する
	else if( ready && !invalidated_) {
		stopped_ = true ;
		sequence_.Cancel() ;
		KillTimer( window_, 0x704U) ;
		ready_ = true ;
		Status( IDS_START_CONNECTED) ;
	}
	// 応答喪失後に以前の接続済み表示を残さない
	else if( ready_ && !ready) {
		ready_ = false ;
		Status( IDS_START_UNAVAILABLE) ;
	}

	EnableWindow( GetDlgItem( window_, IDC_START_SEND),
				 !stopped_ && IsDlgButtonChecked( window_, IDC_START_CONFIRMED) == BST_CHECKED) ;
}

bool AgentStartWindow::ProcessDialogMessage( MSG &message) const noexcept
{
	return window_ != nullptr && (message.hwnd == window_ || IsChild( window_, message.hwnd)) &&
		   IsDialogMessageW( window_, &message) != FALSE ;
}

HWND AgentStartWindow::window() const noexcept
{
	return window_ ;
}

ULONGLONG AgentStartWindow::Tick() const
{
	return clock_ ? clock_() : GetTickCount64() ;
}

bool AgentStartWindow::pending_input() const noexcept
{
	try {
		return window_ != nullptr && !invalidated_ && sequence_.pending() &&
			   !sequence_.expired( Tick()) ;
	} catch( ...) {
		// clockを観測できない場合はowner承認を与えない
		return false ;
	}
}

void AgentStartWindow::CancelPendingInput() noexcept
{
	// 入力済みEnterは取消で巻き戻せないため、未送信だけを失効する
	if( sequence_.pending()) {
		sequence_.Cancel() ;
		KillTimer( window_, 0x704U) ;
		Status( IDS_START_FAILED) ;
	}
}

void AgentStartWindow::AdvanceInput() noexcept
{
	try {
		// 取消・失効・実応答後の残留通知とnested通知を無視する
		if( window_ == nullptr || invalidated_ || ready_ || !sequence_.pending() ||
		   sequence_.in_flight()) {
			return ;
		}

		const auto now = Tick() ;
		// 待機期限またはclock逆転では元画面へ入力せず停止する
		if( sequence_.expired( now)) {
			CancelPendingInput() ;
			return ;
		}

		// 早着timerでは送信callbackへ進まない
		if( sequence_.Claim( now) == rdp::FixedInputStep::Idle) {
			return ;
		}

		KillTimer( window_, 0x704U) ;
		// COM再入で画面が閉じても同期callbackの所有copyを維持する
		const auto send		 = send_ ;
		const auto result	 = send ? send() : E_UNEXPECTED ;
		const bool completed = sequence_.Complete( Tick(), result == S_OK) ;
		// closeや失効後は無効Windowへ成功表示を追加しない
		if( window_ != nullptr && !invalidated_) {
			Status( ready_						 ? IDS_START_CONNECTED
				   : completed && result == S_OK ? IDS_START_SENT
												 : IDS_START_FAILED) ;
		}
	} catch( ...) {
		// callback・clock失敗でも再送や追加解放をせず停止表示する
		sequence_.Cancel() ;
		KillTimer( window_, 0x704U) ;
		Status( IDS_START_FAILED) ;
	}
}

INT_PTR CALLBACK AgentStartWindow::DialogProcedure( HWND window, UINT message, WPARAM wparam,
												   LPARAM lparam) noexcept
{
	auto *self = reinterpret_cast<AgentStartWindow *>( GetWindowLongPtrW( window, DWLP_USER)) ;
	// USER32の初期化時だけ非所有controllerを関連付ける
	if( message == WM_INITDIALOG) {
		self = reinterpret_cast<AgentStartWindow *>( lparam) ;
		SetWindowLongPtrW( window, DWLP_USER, reinterpret_cast<LONG_PTR>( self)) ;
		self->window_ = window ;
		SetDlgItemTextW( window, IDC_START_TARGET, self->target_.c_str()) ;
		std::array<wchar_t, 1024U> instructions{} ;
		LoadStringW( self->instance_, IDS_START_INSTRUCTIONS, instructions.data(),
					static_cast<int>( instructions.size())) ;
		SetDlgItemTextW( window, IDC_START_INSTRUCTIONS, instructions.data()) ;
		EnableWindow( GetDlgItem( window, IDC_START_SEND), FALSE) ;
		self->Status( IDS_START_READY) ;
		return TRUE ;
	}

	// 初期化前と破棄後のmessageでcontrollerを参照しない
	if( self == nullptr) {
		return FALSE ;
	}

	try {
		// Cancelまたはcloseはremote processを終了せず画面だけ閉じる
		if( message == WM_CLOSE || (message == WM_COMMAND && LOWORD( wparam) == IDCANCEL)) {
			DestroyWindow( window) ;
			return TRUE ;
		}

		// HWND破棄後にcallbackや対象表示を保持しない
		if( message == WM_NCDESTROY) {
			self->window_  = nullptr ;
			self->send_	   = {} ;
			self->prepare_ = {} ;
			self->sequence_.Cancel() ;
			KillTimer( window, 0x704U) ;
			self->target_.clear() ;
			return FALSE ;
		}

		// 所有するEnter待機timerだけを処理し、任意messageを入力へ変換しない
		if( message == WM_TIMER && wparam == 0x704U) {
			self->AdvanceInput() ;
			return TRUE ;
		}

		// 起動操作は明示checkboxとbutton通知だけから受け付ける
		if( message == WM_COMMAND && HIWORD( wparam) == BN_CLICKED) {
			// 確認前や一回要求後にはdisabled buttonの偽通知も拒否する
			if( LOWORD( wparam) == IDC_START_SEND && !self->stopped_ &&
			   IsDlgButtonChecked( window, IDC_START_CONFIRMED) == BST_CHECKED) {
				self->stopped_ = true ;
				EnableWindow( GetDlgItem( window, IDC_START_SEND), FALSE) ;
				// 初回focus準備と実入力を別dispatchにし、準備中のnested通知を送信へ進めない
				const auto prepare = self->prepare_ ;
				const auto result  = prepare ? prepare() : S_OK ;
				// 準備中のclose・世代失効・Agent応答では入力を予約しない
				if( self->window_ != window || self->invalidated_ || self->ready_) {
					return TRUE ;
				}

				// 準備失敗・時刻overflowでは入力を予約しない
				if( result != S_OK ||
				   !self->sequence_.Begin( self->Tick(), rdp::FixedInputStep::Enter)) {
					self->Status( IDS_START_FAILED) ;
					return TRUE ;
				}

				// timer予約失敗でも同じstackでEnterを送らない
				if( SetTimer( window, 0x704U, 50U, nullptr) == 0U) {
					self->CancelPendingInput() ;
					return TRUE ;
				}

				self->Status( IDS_START_PENDING) ;

				return TRUE ;
			}

			EnableWindow( GetDlgItem( window, IDC_START_SEND),
						 !self->stopped_ &&
							 IsDlgButtonChecked( window, IDC_START_CONFIRMED) == BST_CHECKED) ;
		}
	} catch( ...) {
		// callback失敗では自動再送せず画面を閉じてやり直してもらう
		self->stopped_ = true ;
		self->sequence_.Cancel() ;
		KillTimer( window, 0x704U) ;
		EnableWindow( GetDlgItem( window, IDC_START_SEND), FALSE) ;
		self->Status( IDS_START_FAILED) ;
	}

	return FALSE ;
}
} // namespace zrdp::client::app
