//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file agent_tray_window.h
 * @brief WTS ownerと独立したAgent通知領域UIのRAII所有者を定義する。
 * @version 01.00.01
 * @date 2026/10/10
 * @author Shunji Ogawa
 * @details 専用UI threadだけがHWND、menu、notification iconを操作する。
 *          通信ownerからはatomic分類だけを受け、終了は協調停止flagへ返す。
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/10  Agentの通知領域UIを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once
#include "agent/app/agent_tray_resources.h"
#include "common/resource_ids.h"
#include <Windows.h>
#include <shellapi.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace zrdp::agent::app {
/** @brief payloadやendpointを含まない通知領域の固定分類。 */
enum class AgentTrayStatus {
	/// Client HELLO待機中。
	Waiting,
	/// SVN Bridge利用可能。
	Ready,
	/// 一時RDP切断の復帰を待機。
	Recovering,
	/// 設定不正またはSVN Bridge利用不能。
	Unavailable,
} ;
/** @brief fixtureでは実notification領域へ触れないWindows境界。 */
using AgentTrayNotify = std::function<bool( DWORD, NOTIFYICONDATAW &)> ;

/** @brief 所有hidden HWNDと一つのiconだけを独立UI threadへ閉じ込める。 */
class AgentTrayWindow final {
public:
	/** @brief ownerより長く生存する協調停止flagを受け取る。
	 * @param[in] instance resource所有module。
	 * @param[in,out] stop_requested I/O ownerへの協調停止要求。
	 * @param[in] notify fixtureだけのShell境界。空ならShell_NotifyIconW。
	 */
	AgentTrayWindow( HINSTANCE instance, std::atomic_bool &stop_requested,
					AgentTrayNotify notify = {})
		: instance_( instance), stop_requested_( stop_requested), notify_( std::move( notify))
	{
		// 空のtest境界だけを標準Shellへ接続する
		if( !notify_) {
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief 所有UI threadから標準Shellへ一つのnotification操作を委譲する。
			 * @param[in] action Shellの追加・更新・削除などの固定操作。
			 * @param[in] data caller所有のicon構成。内容や寿命を外部へ保持しない。
			 * @return Shellが操作を受理した場合はtrue。
			 * @endcond
			 */
			notify_ = []( DWORD action, NOTIFYICONDATAW &data) {
				return Shell_NotifyIconW( action, &data) != FALSE ;
			} ;
		}
	}

	/** @brief icon削除とUI thread回収を所有者の終了より先に完了する。 */
	~AgentTrayWindow()
	{
		Stop() ;
	}

	/** @brief HWNDとthread所有権の複製を禁止する。 */
	AgentTrayWindow( const AgentTrayWindow &)			  = delete ;
	/** @brief HWNDとthread所有権の複製を禁止する。 @return 呼出し禁止。 */
	AgentTrayWindow	  &operator=( const AgentTrayWindow &) = delete ;
	/** @brief 非UI I/O ownerからUI生成を最大二秒待つ。
	 * @return iconとtimerの生成を確認できた場合だけtrue。
	 */
	[[nodiscard]] bool Start() noexcept
	{
		try {
			// 同じownerから二つのiconを生成しない
			if( thread_.joinable() || startup_complete_) {
				return false ;
			}

			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief Agent I/O ownerから分離したUI threadのmessage loopを開始する。
			 * @param[in] stop jthread所有者からの協調停止要求。
			 * @note thisはownerのStopとjoin完了まで生存する。例外はRunの境界で処理する。
			 * @endcond
			 */
			thread_ = std::jthread( [this]( std::stop_token stop) noexcept { Run( stop) ; }) ;
			std::unique_lock lock( startup_mutex_) ;
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief 起動結果が公開されるまでのcondition-variable待機を制御する。
			 * @return startup_mutex_保護下で起動結果が確定していればtrue。
			 * @note startup_mutex_を保持したwait predicateからだけ読む。
			 * @endcond
			 */
			const bool		 ready = startup_cv_.wait_for( lock, std::chrono::seconds( 2), [this] {
				  return startup_complete_ ;
			  }) && startup_succeeded_ ;
			lock.unlock() ;
			// 不可視で終了操作のないAgentを継続しない
			if( !ready) {
				Stop() ;
			}

			return ready ;
		} catch( ...) {
			error_.store( true, std::memory_order_release) ;
			Stop() ;
			return false ;
		}
	}

	/** @brief 数値状態だけをUI timerへ渡す。
	 * @param[in] status 実endpointを含まない固定状態。
	 */
	void SetStatus( AgentTrayStatus status) noexcept
	{
		status_.store( status, std::memory_order_release) ;
	}

	/** @brief Shell失敗を通信ownerへ返す。 @return UIを維持できない場合true。 */
	[[nodiscard]] bool failed() const noexcept
	{
		return error_.load( std::memory_order_acquire) ;
	}

	/** @brief UI所有HWNDをfixtureの固定message送信にだけ公開する。
	 * @return 生成前／終了後はnullptr。
	 */
	[[nodiscard]] HWND window() const noexcept
	{
		return window_.load( std::memory_order_acquire) ;
	}

	/** @brief 協調停止後にmenuとiconの所有threadを回収する。 */
	void Stop() noexcept
	{
		// 未生成threadには停止messageを送らない
		if( !thread_.joinable()) {
			return ;
		}

		thread_.request_stop() ;
		const auto window = this->window() ;
		// 開いたpopup menuも終了messageで閉じ、joinを利用者操作へ依存させない
		if( window != nullptr) {
			PostMessageW( window, WM_CANCELMODE, 0U, 0L) ;
			PostMessageW( window, WM_CLOSE, 0U, 0L) ;
		}

		thread_.join() ;
	}

	/** @brief 固定resourceから右click menuを作る。callerがDestroyMenuする。
	 * @param[in] instance resource所有module。
	 * @param[in] status 現在の非機密状態。
	 * @return 完全menuのみ。失敗はnullptr。
	 */
	[[nodiscard]] static HMENU MakeMenu( HINSTANCE instance, AgentTrayStatus status) noexcept
	{
		std::array<wchar_t, 128U> state{}, exit{} ;
		const auto				  menu = CreatePopupMenu() ;
		// 不完全な終了導線を通知領域へ表示しない
		if( menu == nullptr ||
		   LoadStringW( instance, StatusResource( status), state.data(), 128) <= 0 ||
		   LoadStringW( instance, IDS_AGENT_TRAY_EXIT, exit.data(), 128) <= 0 ||
		   !AppendMenuW( menu, MF_STRING | MF_GRAYED, 0U, state.data()) ||
		   !AppendMenuW( menu, MF_SEPARATOR, 0U, nullptr) ||
		   !AppendMenuW( menu, MF_STRING, kExitCommand, exit.data())) {
			// 部分的に生成できたmenuだけを回収する
			if( menu != nullptr) {
				DestroyMenu( menu) ;
			}

			return nullptr ;
		}

		return menu ;
	}

private:
	/** @brief 状態を固定resourceへ対応付ける。
	 * @param[in] status 非機密分類。
	 * @return 未知値は利用不能の固定ID。
	 */
	[[nodiscard]] static UINT StatusResource( AgentTrayStatus status) noexcept
	{
		// 不明な状態を接続済みと偽装しない
		switch( status) {
		// HELLO成立前
		case AgentTrayStatus::Waiting:
			return IDS_AGENT_TRAY_WAITING ;
		// 通信ownerが応答まで確認済み
		case AgentTrayStatus::Ready:
			return IDS_AGENT_TRAY_READY ;
		// RDP復帰待機
		case AgentTrayStatus::Recovering:
			return IDS_AGENT_TRAY_RECOVERING ;
		// 設定または利用条件が不成立
		default:
			return IDS_AGENT_TRAY_UNAVAILABLE ;
		}
	}

	/** @brief UI生成結果だけを起動待機中ownerへ通知する。
	 * @param[in] succeeded iconとtimerを両方生成した状態。
	 */
	void CompleteStartup( bool succeeded) noexcept
	{
		{
			std::scoped_lock lock( startup_mutex_) ;
			startup_succeeded_ = succeeded ;
			startup_complete_  = true ;
		}

		startup_cv_.notify_one() ;
	}

	/** @brief 一つのiconを追加または状態更新する。
	 * @param[in] add Explorer再起動を含む新規登録ならtrue。
	 * @return 全Shell操作が成功した場合だけtrue。
	 */
	[[nodiscard]] bool UpdateIcon( bool add)
	{
		data_.uFlags	  = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP ;
		const auto status = status_.load( std::memory_order_acquire) ;
		// tooltip全文が得られない場合は無表示常駐を継続しない
		if( LoadStringW( instance_, StatusResource( status), data_.szTip,
					   static_cast<int>( std::size( data_.szTip))) <= 0 ||
		   !notify_( add ? NIM_ADD : NIM_MODIFY, data_)) {
			return false ;
		}

		icon_added_ = true ;
		// keyboard対応を含むVERSION_4を追加のたびに指定する
		if( add) {
			data_.uVersion = NOTIFYICON_VERSION_4 ;
			// 未対応を旧callback形式へ暗黙縮退しない
			if( !notify_( NIM_SETVERSION, data_)) {
				return false ;
			}
		}

		displayed_status_ = status ;
		return true ;
	}

	/** @brief icon操作不能ではI/O ownerへ協調停止を要求する。 */
	void Fail() noexcept
	{
		error_.store( true, std::memory_order_release) ;
		stop_requested_.store( true, std::memory_order_release) ;
		const auto window = this->window() ;
		// 起動完了前に無効HWNDへ終了を要求しない
		if( window != nullptr) {
			PostMessageW( window, WM_CLOSE, 0U, 0L) ;
		}
	}

	/** @brief tray clickから状態行と協調終了だけのmenuを表示する。
	 * @param[in] window UI threadの所有hidden HWND。
	 */
	void ShowMenu( HWND window) noexcept
	{
		const auto menu = MakeMenu( instance_, status_.load( std::memory_order_acquire)) ;
		// 終了menu生成不能では操作不能な常駐を続けない
		if( menu == nullptr) {
			Fail() ;
			return ;
		}

		POINT position{} ;
		GetCursorPos( &position) ;
		SetForegroundWindow( window) ;
		const auto chosen = TrackPopupMenu( menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
										   position.x, position.y, 0, window, nullptr) ;
		DestroyMenu( menu) ;
		PostMessageW( window, WM_NULL, 0U, 0L) ;
		// 固定終了command以外から通信ownerを停止しない
		if( chosen == kExitCommand) {
			stop_requested_.store( true, std::memory_order_release) ;
			PostMessageW( window, WM_CLOSE, 0U, 0L) ;
		}
	}

	/** @brief UI threadへWin32通知をrouteし、例外を境界外へ漏らさない。
	 * @param[in] window 所有hidden HWND。
	 * @param[in] message Win32 message。
	 * @param[in] wparam 固定command値。
	 * @param[in] lparam 作成時だけ非所有this。
	 * @return 処理済み値または標準処理。
	 */
	static LRESULT CALLBACK WindowProcedure( HWND window, UINT message, WPARAM wparam,
											LPARAM lparam) noexcept
	{
		auto *self = reinterpret_cast<AgentTrayWindow *>( GetWindowLongPtrW( window, GWLP_USERDATA)) ;
		// UI thread生成時だけ非所有thisをHWNDへ関連付ける
		if( message == WM_NCCREATE) {
			const auto *creation = reinterpret_cast<const CREATESTRUCTW *>( lparam) ;
			self				 = static_cast<AgentTrayWindow *>( creation->lpCreateParams) ;
			SetWindowLongPtrW( window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( self)) ;
		}

		// 作成前／破棄後の通知はcontrollerを参照しない
		if( self == nullptr) {
			return DefWindowProcW( window, message, wparam, lparam) ;
		}

		try {
			// Explorer再起動後だけ自分のiconを登録し直す
			if( message == self->taskbar_created_) {
				self->icon_added_ = false ;
				// 復帰不能では不可視の常駐を続けない
				if( !self->UpdateIcon( true)) {
					self->Fail() ;
				}

				return 0 ;
			}

			// 状態変更だけをUI ownerからShellへ反映する
			if( message == WM_TIMER) {
				// I/O ownerからの終了を通知領域へ遅延なく反映する
				if( self->thread_.get_stop_token().stop_requested()) {
					DestroyWindow( window) ;
				}
				// 同じtooltipの繰返し更新を避ける
				else if( self->displayed_status_ != self->status_.load( std::memory_order_acquire) &&
						!self->UpdateIcon( false)) {
					self->Fail() ;
				}

				return 0 ;
			}

			// VERSION_4のlow wordだけを固定tray eventとして解釈する
			if( message == kTrayMessage) {
				const auto event = LOWORD( lparam) ;
				// 右click、左選択、keyboard選択を同じ有限menuへ渡す
				if( event == WM_CONTEXTMENU || event == NIN_SELECT || event == NIN_KEYSELECT) {
					self->ShowMenu( window) ;
				}

				return 0 ;
			}

			// 固定menu終了commandだけを協調停止へ変換する
			if( message == WM_COMMAND && LOWORD( wparam) == kExitCommand) {
				self->stop_requested_.store( true, std::memory_order_release) ;
				DestroyWindow( window) ;
				return 0 ;
			}

			// owner終了時はpopupも閉じて、hidden HWNDだけを破棄する
			if( message == WM_CLOSE) {
				EndMenu() ;
				DestroyWindow( window) ;
				return 0 ;
			}

			// thread message pump終了はI/O ownerの停止と別に回収する
			if( message == WM_DESTROY) {
				self->window_.store( nullptr, std::memory_order_release) ;
				PostQuitMessage( 0) ;
				return 0 ;
			}
		} catch( ...) {
			self->Fail() ;
		}

		return DefWindowProcW( window, message, wparam, lparam) ;
	}

	/** @brief HWNDとShell操作だけを所有する専用UI thread entry。
	 * @param[in] stop ownerが要求したUI終了。
	 */
	void Run( std::stop_token stop) noexcept
	{
		const wchar_t class_name[] = L"ZRdpSvnAgent.Tray.Owner" ;
		WNDCLASSW	  registration{} ;
		registration.lpfnWndProc   = WindowProcedure ;
		const auto class_instance  = GetModuleHandleW( nullptr) ;
		registration.hInstance	   = class_instance ;
		registration.lpszClassName = class_name ;
		const auto registered	   = RegisterClassW( &registration) ;
		try {
			taskbar_created_ = RegisterWindowMessageW( L"TaskbarCreated") ;
			// Shell復帰通知を受けられない状態では常駐を開始しない
			if( registered == 0U || taskbar_created_ == 0U || stop.stop_requested()) {
				CompleteStartup( false) ;
			} else {
				const auto window =
					CreateWindowExW( 0U, class_name, L"ZRdp SVN Bridge - Agent", WS_OVERLAPPED, 0, 0,
									0, 0, nullptr, nullptr, class_instance, this) ;
				window_.store( window, std::memory_order_release) ;
				data_.cbSize		   = sizeof( data_) ;
				data_.hWnd			   = window ;
				data_.uID			   = 1U ;
				data_.uCallbackMessage = kTrayMessage ;
				data_.hIcon			   = LoadIconW( instance_, MAKEINTRESOURCEW( IDI_ZRDP_APP)) ;
				const bool ready	   = window != nullptr && data_.hIcon != nullptr &&
								   UpdateIcon( true) && SetTimer( window, 1U, 250U, nullptr) != 0U ;
				CompleteStartup( ready) ;
				// 初期化成功時だけ独立message pumpを継続する
				if( ready) {
					MSG message{} ;
					int result = 0 ;
					// WTSとSocketを触らず自分のUI通知だけをdispatchする
					while( (result = GetMessageW( &message, nullptr, 0U, 0U)) > 0) {
						TranslateMessage( &message) ;
						DispatchMessageW( &message) ;
					}

					// 予期しないpump終了でも不可視の通信常駐を続けない
					if( result < 0 || (!stop.stop_requested() &&
									  !stop_requested_.load( std::memory_order_acquire))) {
						Fail() ;
					}
				}
			}
		} catch( ...) {
			Fail() ;
			CompleteStartup( false) ;
		}

		// icon所有権はUI thread終了前に必ずShellへ返す
		if( icon_added_) {
			try {
				static_cast<void>( notify_( NIM_DELETE, data_)) ;
			} catch( ...) {
				error_.store( true, std::memory_order_release) ;
			}
		}

		const auto window = this->window() ;
		// 初期化失敗で残った自分のHWNDだけを破棄する
		if( window != nullptr) {
			DestroyWindow( window) ;
		}

		// 自分が登録したclassだけをUI thread回収前に戻す
		if( registered != 0U) {
			UnregisterClassW( class_name, class_instance) ;
		}
	}

	/// 同じUI ownerの固定終了command。任意commandは受けない。
	static constexpr UINT		 kExitCommand = 1U ;
	/// notification eventだけの私有message。
	static constexpr UINT		 kTrayMessage = WM_APP + 1U ;
	/// resource moduleの非所有参照。
	HINSTANCE					 instance_ ;
	/// I/O ownerより短いlifetimeで参照する協調停止flag。
	std::atomic_bool			&stop_requested_ ;
	/// Shellまたは隔離fixtureの操作境界。UI threadだけが呼ぶ。
	AgentTrayNotify				 notify_ ;
	/// 通信ownerからの固定数値状態。
	std::atomic<AgentTrayStatus> status_{AgentTrayStatus::Waiting} ;
	/// UI異常をownerへ返す固定flag。
	std::atomic_bool			 error_{false} ;
	/// Stopのwakeにだけ使用する非所有HWND値。
	std::atomic<HWND>			 window_{nullptr} ;
	/// startup結果の二つのboolだけを保護するmutex。
	std::mutex					 startup_mutex_ ;
	/// UI生成完了の有限待機。
	std::condition_variable		 startup_cv_ ;
	/// startup mutexでだけ参照する完了flag。
	bool						 startup_complete_{false} ;
	/// startup mutexでだけ参照する成否。
	bool						 startup_succeeded_{false} ;
	/// UI threadだけがShellへ渡す固定icon data。
	NOTIFYICONDATAW				 data_{} ;
	/// UI threadだけが保持する最後の表示分類。
	AgentTrayStatus				 displayed_status_{AgentTrayStatus::Waiting} ;
	/// Shellへ一回登録したiconの削除責任。
	bool						 icon_added_{false} ;
	/// Explorer再起動通知の登録済みID。
	UINT						 taskbar_created_{0U} ;
	/// このownerがjoinする唯一のUI thread。
	std::jthread				 thread_ ;
} ;
} // namespace zrdp::agent::app
