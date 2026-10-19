//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_deployment_window_test.cpp
 * @brief 実clipboardとRDPなしで配置UIの同意・所有・拒否条件を確認する。
 * @version 01.00.01
 * @date 2026/10/08
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  fake clipboardとowner要求で配置画面を検証。
 * Ver.01.00.01   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/app/agent_deployment_window.h"
#include "client/app/resource.h"
#include <iostream>

/** @brief fake clipboardと偽接続先だけで配置操作を確認する。
 * @param[in] argc preview有無の引数件数。
 * @param[in] argv --previewだけを確認し、remote commandは受け取らない。
 * @return 全拒否条件を満たす場合は0。
 */
int wmain( int argc, wchar_t **argv)
{
	namespace app		 = zrdp::client::app ;
	namespace deployment = zrdp::client::agent_deployment ;
	SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ;
	bool		 approved = false, current = true, clear_fails = false, current_error = false ;
	unsigned int published = 0U, cleared = 0U, requests = 0U ;
	app::DeploymentClipboardOps clipboard{
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT
		 * @brief 公開要求回数だけを数え、実clipboardを使わない。
		 * @return 固定S_OK。
		 * @note 無名IDataObjectは公開件数の観測に不要で、保持もCOM呼出しも行わない。
		 * @endcond
		 */
		[&]( IDataObject *) {
			++published ;
			return S_OK ;
		},
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT
		 * @brief clipboardの所有・変更・照会失敗をflagで再現する。
		 * @return 所有時はS_OK、変更時はS_FALSE、照会失敗はE_FAIL。
		 * @note 無名IDataObjectはfake状態選択に不要で、実OLEへアクセスしない。
		 * @endcond
		 */
		[&]( IDataObject *) { return current_error ? E_FAIL
									: current	  ? S_OK
												  : S_FALSE ; },
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT
		 * @brief clipboard回収の一回性と失敗分類を再現する。
		 * @return clear_failsならE_FAIL、それ以外はS_OK。
		 * @note 無名IDataObjectは回収件数だけを検証するため使わず、他appのclipboardへ触れない。
		 * @endcond
		 */
		[&]( IDataObject *) {
			++cleared ;
			return clear_fails ? E_FAIL : S_OK ;
		}} ;
	app::AgentDeploymentWindow window( GetModuleHandleW( nullptr), clipboard,
									  /**
									   * @cond LOCAL_CALLBACK_CONTRACT
									   * @brief 利用者確認をfixtureの承認flagへ置き換える。
									   * @return approvedの現在値。
									   * @note 無名HWND・resource ID・本文は固定確認結果のため不要で、実dialogを開かない。
									   * @endcond
									   */
									  [&]( HWND, UINT, std::wstring_view) { return approved ; }) ;
	// 同意なしではclipboardを公開せず、追加接続制限も残さない
	if( window.ConfirmPreparation( L"test (192.0.2.1)") || window.active() || published != 0U) {
		return 1 ;
	}

	approved = true ;
	// 一件の準備状態だけを受け付ける
	if( !window.ConfirmPreparation( L"test (192.0.2.1)") || !window.active() ||
	   window.ConfirmPreparation( L"other")) {
		return 2 ;
	}

	window.CancelPreparation() ;
	// worker失敗では元clipboardを消さない
	if( window.active() || cleared != 0U) {
		return 3 ;
	}

	auto package   = std::make_shared<deployment::Package>() ;
	package->agent = {std::byte{0x4d}} ;
	package->ini   = "[allowlist]\r\nendpoint.1001=192.0.2.10:80\r\n" ;
	// UIとOLE参照を同時に公開できた場合だけ操作可能にする
	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief 貼付け要求をfake件数だけに変換し、再送の有無を検証する。
	 * @return 固定true。実RDPへキーを送らない。
	 * @note 無名clipboard世代はこの件数試験では不要で、外部のclipboardを読まない。
	 * @endcond
	 */
	if( !window.ConfirmPreparation( L"test (192.0.2.1)") || !window.Show( package, [&]( DWORD) {
		   ++requests ;
		   return true ;
	   })) {
		return 4 ;
	}

	const auto dialog = window.window() ;
	// previewはtest所有画面だけを表示し、実clipboardは一切変更しない
	if( argc == 2 && std::wstring_view( argv[1]) == L"--preview") {
		// Hiddenで開始したtest consoleの初回ShowWindow制約だけを解除する
		ShowWindow( dialog, SW_SHOW) ;
		const auto end = GetTickCount64() + 20000U ;
		// 明示previewを有限時間だけ維持する
		while( window.window() != nullptr && GetTickCount64() < end) {
			MSG message{} ;
			// test自身のmessageだけを通常dispatchする
			while( PeekMessageW( &message, nullptr, 0U, 0U, PM_REMOVE)) {
				// dialog navigation未処理messageだけを通常処理へ渡す
				if( !window.ProcessDialogMessage( message)) {
					TranslateMessage( &message) ;
					DispatchMessageW( &message) ;
				}
			}

			Sleep( 10U) ;
		}

		return 0 ;
	}

	SendMessageW( dialog, WM_COMMAND, MAKEWPARAM( IDC_DEPLOY_PASTE, BN_CLICKED), 0L) ;
	// disabled buttonの偽WM_COMMANDでもfolder確認前には送信しない
	if( requests != 0U || IsWindowEnabled( GetDlgItem( dialog, IDC_DEPLOY_PASTE))) {
		return 5 ;
	}

	CheckDlgButton( dialog, IDC_DEPLOY_FOLDER_READY, BST_CHECKED) ;
	SendMessageW( dialog, WM_COMMAND, MAKEWPARAM( IDC_DEPLOY_FOLDER_READY, BN_CLICKED), 0L) ;
	SendMessageW( dialog, WM_COMMAND, MAKEWPARAM( IDC_DEPLOY_PASTE, BN_CLICKED), 0L) ;
	SendMessageW( dialog, WM_COMMAND, MAKEWPARAM( IDC_DEPLOY_PASTE, BN_CLICKED), 0L) ;
	// queued要求を二重に送らず、キー応答前には完了操作を許可しない
	if( requests != 1U || IsWindowEnabled( GetDlgItem( dialog, IDC_DEPLOY_COMPLETE))) {
		return 6 ;
	}

	window.OnPasteResult( S_OK) ;
	// キー送信後にだけ利用者による最終確認を可能にする
	if( !IsWindowEnabled( GetDlgItem( dialog, IDC_DEPLOY_COMPLETE))) {
		return 7 ;
	}

	current = false ;
	SendMessageW( dialog, WM_COMMAND, MAKEWPARAM( IDC_DEPLOY_PASTE, BN_CLICKED), 0L) ;
	// clipboard変更では次のキーを送らない
	if( requests != 1U || IsWindowEnabled( GetDlgItem( dialog, IDC_DEPLOY_PASTE))) {
		return 8 ;
	}

	SendMessageW( dialog, WM_CLOSE, 0U, 0L) ;
	// 別アプリ所有clipboardを終了時に消さない
	if( window.active() || cleared != 0U) {
		return 9 ;
	}

	current		  = true ;
	current_error = true ;
	// 再準備は以前の完了状態を継承しない
	if( !window.ConfirmPreparation( L"test (192.0.2.1)") ||
	   /**
	    * @cond LOCAL_CALLBACK_CONTRACT
	    * @brief 再準備後の最初の貼付け要求失敗を再現する。
	    * @return 固定false。追加入力は行わない。
	    * @note 無名clipboard世代は固定失敗を注入するため不要で、状態を外部へ反映しない。
	    * @endcond
	    */
	   !window.Show( package, []( DWORD) { return false ; })) {
		return 10 ;
	}

	SendMessageW( window.window(), WM_CLOSE, 0U, 0L) ;
	// 所有判定の失敗を「他アプリ所有」と近似して公開終了しない
	if( !window.active() || cleared != 0U) {
		return 13 ;
	}

	current_error = false ;
	clear_fails	  = true ;
	SendMessageW( window.window(), WM_CLOSE, 0U, 0L) ;
	// clipboard clear失敗では公開状態を保持して追加接続を許可しない
	if( !window.active() || window.window() == nullptr) {
		return 11 ;
	}

	clear_fails = false ;
	SendMessageW( window.window(), WM_CLOSE, 0U, 0L) ;
	// 明示再試行で自分のclipboardだけを終了する
	if( window.active() || cleared != 2U) {
		return 12 ;
	}

	std::cout << "Agent deployment UI passed\n" ;
	return 0 ;
}
