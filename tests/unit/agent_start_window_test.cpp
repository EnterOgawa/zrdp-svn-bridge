//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_start_window_test.cpp
 * @brief fake入力だけで起動確認・二重要求・接続失効を検証する。
 * @version 01.00.02
 * @date 2026/10/09
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/09  実RDP・実clipboardを使わない起動UI確認。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.02   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/app/agent_start_window.h"
#include "client/app/resource.h"
#include <array>
#include <iostream>
#include <string_view>

/** @brief fake callbackと例示用接続先だけで確認画面を検証する。
 * @param[in] argc preview有無の引数件数。
 * @param[in] argv --previewだけを受け付ける。
 * @return すべての拒否・表示条件を満たした場合は0。
 */
int wmain( int argc, wchar_t **argv)
{
	using zrdp::client::app::AgentStartWindow ;
	SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ;
	ULONGLONG		 now = 1000U ;
	AgentStartWindow window( GetModuleHandleW( nullptr),
							/**
							 * @cond LOCAL_CALLBACK_CONTRACT @brief Enterの有限待機を実sleepなしで進める。 @return fake単調ms。
							 * @endcond
							 */
							[&]() { return now ; }) ;
	unsigned int	 requests = 0U ;
	// 未認証・callback不在では表示すらしない
	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief 未認証fixtureで起動callbackが呼ばれないことを検証する。
	 * @return 呼ばれた場合は固定S_OK。実入力は行わない。
	 * @endcond
	 */
	if( window.Show( L"test (192.0.2.1)", 0U, [] { return S_OK ; }) ||
	   window.Show( L"test (192.0.2.1)", 1U, {})) {
		return 1 ;
	}

	// 実RDPの代わりに送信件数だけを観測する
	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief 旧内部確認器の一回だけのEnter要求を件数として観測する。
	 * @return 固定S_OK。Agentを実行しない。
	 * @endcond
	 */
	if( !window.Show( L"test (192.0.2.1)", 1U, [&] {
		   ++requests ;
		   return S_OK ;
	   })) {
		return 2 ;
	}

	const HWND dialog = window.window() ;
	// previewは所有するfake画面だけを表示し、実設定やclipboardを参照しない
	if( argc == 2 && std::wstring_view( argv[1]) == L"--preview") {
		ShowWindow( dialog, SW_SHOW) ;
		MSG message{} ;
		// 利用者が閉じるまでfake画面だけをdispatchする
		while( window.window() != nullptr && GetMessageW( &message, nullptr, 0U, 0U) > 0) {
			// dialogが未処理の場合だけ標準dispatchする
			if( !window.ProcessDialogMessage( message)) {
				TranslateMessage( &message) ;
				DispatchMessageW( &message) ;
			}
		}

		return 0 ;
	}

	SendMessageW( dialog, WM_COMMAND, MAKEWPARAM( IDC_START_SEND, BN_CLICKED), 0L) ;
	// 確認なしの偽commandでも入力を送らない
	if( requests != 0U || IsWindowEnabled( GetDlgItem( dialog, IDC_START_SEND))) {
		return 3 ;
	}

	CheckDlgButton( dialog, IDC_START_CONFIRMED, BST_CHECKED) ;
	SendMessageW( dialog, WM_COMMAND, MAKEWPARAM( IDC_START_CONFIRMED, BN_CLICKED), 0L) ;
	SendMessageW( dialog, WM_COMMAND, MAKEWPARAM( IDC_START_SEND, BN_CLICKED), 0L) ;
	SendMessageW( dialog, WM_COMMAND, MAKEWPARAM( IDC_START_SEND, BN_CLICKED), 0L) ;
	// 確認直後と早着timerではEnterを送らない
	SendMessageW( dialog, WM_TIMER, 0x704U, 0L) ;
	// 既定の350 ms待機を破っていないことを確認する
	if( requests != 0U || !window.pending_input()) {
		return 12 ;
	}

	now += 350U ;
	SendMessageW( dialog, WM_TIMER, 0x704U, 0L) ;
	SendMessageW( dialog, WM_TIMER, 0x704U, 0L) ;
	std::array<wchar_t, 512U> text{} ;
	GetDlgItemTextW( dialog, IDC_START_STATUS, text.data(), static_cast<int>( text.size())) ;
	// 送信成功を起動成功と表示せず、二度目のキーを拒否する
	if( requests != 1U ||
	   std::wstring_view( text.data()).find( L"応答待ち") == std::wstring_view::npos) {
		return 4 ;
	}

	window.Update( 1U, true) ;
	GetDlgItemTextW( dialog, IDC_START_STATUS, text.data(), static_cast<int>( text.size())) ;
	// 実応答の通知だけでAgent接続済みへ変わる
	if( std::wstring_view( text.data()).find( L"応答を確認") == std::wstring_view::npos) {
		return 5 ;
	}

	window.Update( 1U, false) ;
	GetDlgItemTextW( dialog, IDC_START_STATUS, text.data(), static_cast<int>( text.size())) ;
	// 応答喪失後に接続済み表示を維持しない
	if( std::wstring_view( text.data()).find( L"確認できなくなりました") == std::wstring_view::npos) {
		return 10 ;
	}

	SendMessageW( dialog, WM_CLOSE, 0U, 0L) ;
	// 閉じた画面は他の世代へ再利用せず、新規確認を必要とする
	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief 閉じた確認器の新規確認でだけ要求件数を増やす。
	 * @return 固定S_OK。実RDPに入力しない。
	 * @endcond
	 */
	if( window.window() != nullptr || !window.Show( L"test", 2U, [&] {
		   ++requests ;
		   return S_OK ;
	   })) {
		return 6 ;
	}

	window.Update( 3U, true) ;
	CheckDlgButton( window.window(), IDC_START_CONFIRMED, BST_CHECKED) ;
	SendMessageW( window.window(), WM_COMMAND, MAKEWPARAM( IDC_START_SEND, BN_CLICKED), 0L) ;
	// 再接続先が応答済みでも古い画面の入力は失効したままとする
	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief 無効化した世代を新規確認なしで再利用できないことを検証する。
	 * @return 固定S_OK。呼出し許可は確認器側が判断する。
	 * @endcond
	 */
	if( requests != 1U || window.Show( L"other", 3U, [] { return S_OK ; })) {
		return 7 ;
	}

	window.Update( 0U, false) ;
	window.Update( 2U, true) ;
	GetDlgItemTextW( window.window(), IDC_START_STATUS, text.data(), static_cast<int>( text.size())) ;
	// 失効後に元の世代値を通知しても成功確認を復活させない
	if( std::wstring_view( text.data()).find( L"古い起動要求") == std::wstring_view::npos) {
		return 11 ;
	}

	SendMessageW( window.window(), WM_CLOSE, 0U, 0L) ;
	// 入力失敗も自動再送せず、利用者のやり直しを待つ
	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief 入力要求失敗をfakeで返し、自動再送しないことを検証する。
	 * @return 固定E_FAIL。実入力は行わない。
	 * @endcond
	 */
	if( !window.Show( L"test", 4U, [] { return E_FAIL ; })) {
		return 8 ;
	}

	CheckDlgButton( window.window(), IDC_START_CONFIRMED, BST_CHECKED) ;
	SendMessageW( window.window(), WM_COMMAND, MAKEWPARAM( IDC_START_SEND, BN_CLICKED), 0L) ;
	now += 350U ;
	SendMessageW( window.window(), WM_TIMER, 0x704U, 0L) ;
	GetDlgItemTextW( window.window(), IDC_START_STATUS, text.data(), static_cast<int>( text.size())) ;
	// 失敗を起動待ちや接続確認へ読み替えない
	if( std::wstring_view( text.data()).find( L"送信できません") == std::wstring_view::npos ||
	   IsWindowEnabled( GetDlgItem( window.window(), IDC_START_SEND))) {
		return 9 ;
	}

	SendMessageW( window.window(), WM_CLOSE, 0U, 0L) ;
	// 取消と途中close、準備失敗、再入、期限到達で遅延Enterを追加しない
	for( unsigned mode = 0U; mode < 6U; ++mode) {
		unsigned callbacks	  = 0U ;
		unsigned preparations = 0U ;
		// fake入力だけを注入し、本番Windows入力へは接続しない
		if( !window.Show(
			   L"test", 10U + mode,
			   /**
			    * @cond LOCAL_CALLBACK_CONTRACT @brief 同じcallback中のtimer再入も一回要求を維持する。 @return fake受付。
			    * @endcond
			    */
			   [&]() {
				   ++callbacks ;
				   SendMessageW( window.window(), WM_TIMER, 0x704U, 0L) ;
				   return S_OK ;
			   },
			   /**
			    * @cond LOCAL_CALLBACK_CONTRACT @brief 準備失敗・準備中closeを実入力なしで注入する。 @return fake準備結果。
			    * @endcond
			    */
			   [&]() {
				   ++preparations ;
				   // 初回準備中の画面破棄後に入力を予約しない
				   if( mode == 4U) {
					   SendMessageW( window.window(), WM_CLOSE, 0U, 0L) ;
				   }

				   return mode == 3U ? E_ACCESSDENIED : S_OK ;
			   })) {
			return 13 ;
		}

		const HWND trial = window.window() ;
		CheckDlgButton( trial, IDC_START_CONFIRMED, BST_CHECKED) ;
		SendMessageW( trial, WM_COMMAND, MAKEWPARAM( IDC_START_SEND, BN_CLICKED), 0L) ;
		// focus切替相当の取消から戻っても入力を予約し直さない
		if( mode == 0U) {
			window.CancelPendingInput() ;
		}

		// 画面close後の残留timerを拒否する
		if( mode == 1U) {
			SendMessageW( trial, WM_CLOSE, 0U, 0L) ;
		}

		now += mode == 2U ? 3350U : 350U ;
		SendMessageW( trial, WM_TIMER, 0x704U, 0L) ;
		SendMessageW( trial, WM_COMMAND, MAKEWPARAM( IDC_START_SEND, BN_CLICKED), 0L) ;
		// 成功時だけ一回要求し、全停止caseではcallbackを呼ばない
		if( preparations != 1U || callbacks != (mode == 5U ? 1U : 0U)) {
			return 14 ;
		}

		// 各試行の所有画面だけを閉じる
		if( window.window() != nullptr) {
			SendMessageW( window.window(), WM_CLOSE, 0U, 0L) ;
		}
	}

	std::cout << "Agent start UI checks passed\n" ;
	return 0 ;
}
