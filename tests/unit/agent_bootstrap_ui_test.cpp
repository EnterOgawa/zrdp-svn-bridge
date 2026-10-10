//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file agent_bootstrap_ui_test.cpp
 * @brief 偽起動queueと応答で初回配置UIの受付と完了を分離して検証する。
 * @version 01.00.10
 * @date 2026/10/09
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/09  自動初期配置の状態試験を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/09  初期配置の停止段階とHRESULT診断を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  自動入力のfocusとWindowsキー転送を確認。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  トレイ終了後の同じRDP内Agent再起動を修正。
 * Ver.01.00.07   Shunji Ogawa  2026/10/10  完了入力sequenceの限定初期化と二回目起動を修正。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.09   Shunji Ogawa  2026/10/10  接続別設定・自動Agent管理の回帰検証を追加。
 * Ver.01.00.10   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/app/agent_deployment_window.h"
#include "client/app/resource.h"
#include <array>
#include <string>
#include <iostream>
#include <windowsx.h>

/** @brief 偽接続先とworkspace fixtureだけで自動UIを確認する。
 * @param[in] argc fixture baseと任意previewの件数。
 * @param[in] argv baseと--previewだけを受け付ける。
 * @return UIの全拒否・完了条件を満たす場合0。
 */
int wmain( int argc, wchar_t **argv)
{
	namespace app		 = zrdp::client::app ;
	namespace deployment = zrdp::client::agent_deployment ;
	using Stage			 = deployment::AutomaticDeploymentStage ;
	using Reason		 = deployment::AutomaticDeploymentReason ;
	using zrdp::common::agent_setup::DeploymentDrive ;
	// 実ユーザー保存先をfixtureに使わない
	if( argc < 2 || argc > 3) {
		return 1 ;
	}

	// 実clipboard・drive・RDPに到達する前に確認resourceだけをpreviewする
	if( argc == 3 && std::wstring_view( argv[2]) == L"--preview-confirmation") {
		std::array<wchar_t, 2048U> format{} ;
		std::array<wchar_t, 3072U> message{} ;
		LoadStringW( GetModuleHandleW( nullptr), IDS_AUTO_DEPLOY_CONFIRM, format.data(),
					static_cast<int>( format.size())) ;
		// 接続先は文書例示用の固定値だけを表示する
		if( swprintf_s( message.data(), message.size(), format.data(), L"UI test (192.0.2.1)") < 0) {
			return 17 ;
		}

		MessageBoxW( nullptr, message.data(), L"ZRdp SVN Bridge",
					MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) ;
		return 0 ;
	}

	// 独立起動確認のpreviewは保存先・clipboard・driveへ触れない
	if( argc == 3 && std::wstring_view( argv[2]) == L"--preview-launch") {
		zrdp::client::agent_settings::AgentSettingsProfile profile ;
		// 不完全fixtureでendpoint indexを参照しない
		if( !zrdp::client::agent_settings::ParseAgentSettingsIni(
				"[allowlist]\nendpoint.1001=192.0.2.10:80\n", L"UI Fixture", profile)
				.succeeded()) {
			return 30 ;
		}

		profile.endpoints[0].purpose  = zrdp::client::agent_settings::AgentEndpointPurpose::Svn ;
		profile.endpoints[0].approved = true ;
		app::AgentDeploymentWindow preview( GetModuleHandleW( nullptr)) ;
		static_cast<void>( preview.ConfirmLaunch( L"UI test (192.0.2.1)", {profile},
												/**
												 * @cond LOCAL_CALLBACK_CONTRACT
												 * @brief 旧内部起動確認resourceだけをpreviewし、実RDPへの入力を行わない。
												 * @param[in] launch fixtureが所有する確認dialog。callback中だけ借用する。
												 * @endcond
												 */
												[&]( HWND launch) {
													// hidden起動したfixtureも自作確認画面だけを明示表示する
													ShowWindow( launch, SW_SHOWNORMAL) ;
												})) ;
		return 0 ;
	}

	// 各安全条件の失敗を本番と同じ分類関数で確認する
	// 非同期Loadの結果は同じ一sessionと認証世代にしか使用しない
	if( !app::CanUseSavedAgentLaunch( 7U, 4U, 7U, 4U, 1U) ||
	   app::CanUseSavedAgentLaunch( 7U, 4U, 8U, 4U, 1U) ||
	   app::CanUseSavedAgentLaunch( 7U, 4U, 7U, 5U, 1U) ||
	   app::CanUseSavedAgentLaunch( 7U, 0U, 7U, 0U, 1U) ||
	   app::CanUseSavedAgentLaunch( 7U, 4U, 7U, 4U, 2U)) {
		return 31 ;
	}

	const std::array<deployment::AutomaticDeploymentGuard, 5U> guards{
		{{false, true, true, true, true},
		 {true, false, true, true, true},
		 {true, true, false, true, true},
		 {true, true, true, false, true},
		 {true, true, true, true, false}}} ;
	// 完了後の同認証世代のAgent終了だけをidleへ戻す
	if( !deployment::CanResetCompletedAgentLaunch( 4, true, true, true, true) ||
	   deployment::CanResetCompletedAgentLaunch( 4, false, true, true, true) ||
	   deployment::CanResetCompletedAgentLaunch( 4, true, false, true, true) ||
	   deployment::CanResetCompletedAgentLaunch( 4, true, true, false, true) ||
	   deployment::CanResetCompletedAgentLaunch( 4, true, true, true, false)) {
		return 32 ;
	}

	// 途中・初期・失敗段階を自動再試行の対象にしない
	for( const int phase : {-1, 0, 1, 2, 3}) {
		// 完了以外のphaseを再起動可能と判定したら後続fixtureへ進まない
		if( deployment::CanResetCompletedAgentLaunch( phase, true, true, true, true)) {
			return 33 ;
		}
	}

	zrdp::client::rdp::FixedInputSequence restart_sequence ;
	using InputStep = zrdp::client::rdp::FixedInputStep ;
	// 本番と同じ固定入力状態とreset境界で初回から四回の起動を繰り返す
	for( unsigned int attempt = 0U; attempt < 4U; ++attempt) {
		const std::uint64_t started = 1000U + attempt * 10000U ;
		// 初期化そのものや前回の残留timerで入力を勝手に予約しない
		if( restart_sequence.pending() || restart_sequence.Claim( started) != InputStep::Idle ||
		   !restart_sequence.Begin( started) ||
		   restart_sequence.Claim( started + 349U) != InputStep::Idle ||
		   deployment::ResetCompletedAgentLaunch( restart_sequence, 4, true, true, true, true)) {
			return 38 ;
		}

		std::uint64_t tick = started + 350U ;
		// 仮想時刻で実際のRun・Paste・Enterを一回ずつ完了させる
		for( const auto step : {InputStep::Run, InputStep::Paste, InputStep::Enter}) {
			// 同段階再入と二重完了を受付けず、次の待機を本番と同じ規則で予約する
			if( restart_sequence.Claim( tick) != step ||
			   restart_sequence.Claim( tick) != InputStep::Idle ||
			   !restart_sequence.Complete( tick, true) || restart_sequence.Complete( tick, true)) {
				return 39 ;
			}

			tick += 700U ;
		}

		// 完了だけでは再開せず、応答・終了・同世代・成功の一条件でも欠ければ保持する
		if( restart_sequence.step() != InputStep::Complete || restart_sequence.Begin( tick) ||
		   deployment::ResetCompletedAgentLaunch( restart_sequence, 4, false, true, true, true) ||
		   deployment::ResetCompletedAgentLaunch( restart_sequence, 4, true, false, true, true) ||
		   deployment::ResetCompletedAgentLaunch( restart_sequence, 4, true, true, false, true) ||
		   deployment::ResetCompletedAgentLaunch( restart_sequence, 4, true, true, true, false) ||
		   deployment::ResetCompletedAgentLaunch( restart_sequence, 3, true, true, true, true) ||
		   restart_sequence.step() != InputStep::Complete ||
		   !deployment::ResetCompletedAgentLaunch( restart_sequence, 4, true, true, true, true) ||
		   restart_sequence.step() != InputStep::Idle ||
		   deployment::ResetCompletedAgentLaunch( restart_sequence, 4, true, true, true, true)) {
			return 40 ;
		}
	}

	restart_sequence.Cancel() ;
	// 失敗相当のStoppedはownerの他guardが揃っても復活させない
	if( deployment::ResetCompletedAgentLaunch( restart_sequence, 4, true, true, true, true) ||
	   restart_sequence.step() != InputStep::Stopped || restart_sequence.Begin( 50000U)) {
		return 41 ;
	}

	const std::array<Reason, 5U> reasons{Reason::SessionChanged, Reason::AlreadyStarted,
										 Reason::DriveUnavailable, Reason::BridgeUnavailable,
										 Reason::ClipboardChanged} ;
	// snapshotの分類は条件位置に一致し、全成立だけ許可する
	for( std::size_t i = 0U; i < guards.size(); ++i) {
		// 別の停止理由に分類された場合は後続入力のtestを行わない
		if( deployment::AutomaticDeploymentGuardReason( guards[i]) != reasons[i]) {
			return 9 ;
		}
	}

	// clock逆転、期限直前、期限ちょうどを区別する
	if( deployment::AutomaticDeploymentGuardReason( {true, true, true, true, true}) != Reason::None ||
	   deployment::AutomaticDeploymentTimedOut( 1000U, 999U) ||
	   deployment::AutomaticDeploymentTimedOut( 1000U, 45999U) ||
	   !deployment::AutomaticDeploymentTimedOut( 1000U, 46000U)) {
		return 10 ;
	}

	auto drive = DeploymentDrive::Create( std::filesystem::path( argv[1]) / L"bootstrap-ui-test") ;
	const std::vector<std::byte> agent{std::byte{0x4d}} ;
	// fake EXEと文書用endpointだけを公開する
	if( drive == nullptr || !drive->Publish( agent, "[allowlist]\nendpoint.1001=192.0.2.10:80\n")) {
		return 2 ;
	}

	unsigned int							clipboard_calls = 0U, launches = 0U ;
	bool									approved = false ;
	deployment::AutomaticDeploymentSnapshot status ;
	app::DeploymentClipboardOps				clipboard{/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief 配布物公開要求の回数だけを観測するfake OLE境界。
	 * @return 固定のS_OK。実clipboardを変更しない。
	 * @note 無名IDataObjectはCOM内容の読取り・保持が不要なため無視し、呼出し件数だけ増やす。
	 * @endcond
	 */
										  [&]( IDataObject *) {
											  ++clipboard_calls ;
											  return S_OK ;
										  },
										  /**
										   * @cond LOCAL_CALLBACK_CONTRACT
										   * @brief clipboard所有照会の回数だけを記録して公開状態を再現する。
										   * @return 固定のS_OK。実OLEへ問い合わせない。
										   * @note 無名IDataObjectはfake件数の検証に不要で、参照保持も出力変更も行わない。
										   * @endcond
										   */
										  [&]( IDataObject *) {
											  ++clipboard_calls ;
											  return S_OK ;
										  },
										  /**
										   * @cond LOCAL_CALLBACK_CONTRACT
										   * @brief 配布取消時の回収要求だけを数えるfake境界。
										   * @return 固定のS_OK。実clipboardを消さない。
										   * @note 無名IDataObjectは回収件数の検証に不要で、内容へアクセスしない。
										   * @endcond
										   */
										  [&]( IDataObject *) {
											  ++clipboard_calls ;
											  return S_OK ;
										  }} ;
	app::AgentDeploymentWindow				window( GetModuleHandleW( nullptr), clipboard,
												   /**
												    * @cond LOCAL_CALLBACK_CONTRACT
												    * @brief 初回配置の承認・拒否をfixtureのflagで決定する。
												    * @param[in] id 表示要求の固定resource ID。
												    * @return 初回確認はapproved、それ以外の内部確認はtrue。
												    * @note 無名HWNDと本文は既知fixtureのため使わず、実確認dialogを開かない。
												    * @endcond
												    */
												   [&]( HWND, UINT id, std::wstring_view) {
										   return id == IDS_AUTO_DEPLOY_CONFIRM ? approved : true ;
									   }) ;
	// 承認前は何も公開・起動しない
	if( window.ConfirmPreparation( L"example (192.0.2.1)", true) || window.active() ||
	   clipboard_calls != 0U) {
		return 3 ;
	}

	zrdp::client::agent_settings::AgentSettingsProfile profile ;
	// 文書例示用設定だけを起動候補へ変換する
	if( !zrdp::client::agent_settings::ParseAgentSettingsIni(
			"[allowlist]\nendpoint.1001=192.0.2.10:80\n", L"Fixture", profile)
			.succeeded() ||
	   app::IsAgentLaunchProfileUsable( profile)) {
		return 23 ;
	}

	profile.endpoints[0].purpose  = zrdp::client::agent_settings::AgentEndpointPurpose::Svn ;
	profile.endpoints[0].approved = true ;
	// 空設定と未承認を保存済み起動候補として扱わない
	if( !app::IsAgentLaunchProfileUsable( profile) ||
	   app::IsAgentLaunchProfileUsable( {L"Empty", {}}) ||
	   window.ConfirmLaunch( L"Fixture", {}).profile_index) {
		return 24 ;
	}

	bool initial_valid = false ;
	auto choice		   = window.ConfirmLaunch(
		   L"Fixture (192.0.2.1)", {profile},
		   /**
		    * @cond LOCAL_CALLBACK_CONTRACT
		    * @brief 旧内部確認器の初期選択・既定取消・承認後状態を検証する。
		    * @param[in] launch testが所有する確認dialog。実接続先ではない。
		    * @endcond
		    */
		   [&]( HWND launch) {
			   initial_valid = ComboBox_GetCurSel( GetDlgItem( launch, IDC_LAUNCH_PROFILE)) == 0 &&
							   GetDlgItem( launch, 1115U) == nullptr &&
							   LOWORD( SendMessageW( launch, DM_GETDEFID, 0U, 0L)) == IDCANCEL ;
			   std::array<wchar_t, 3072U> warning{} ;
			   GetDlgItemTextW( launch, IDC_LAUNCH_WARNING, warning.data(),
								   static_cast<int>( warning.size())) ;
			   initial_valid =
				   initial_valid &&
				   std::wstring_view( warning.data()).find( L"\r\n\r\n") != std::wstring_view::npos &&
				   std::wstring_view( warning.data()).find( L"バックアップ") != std::wstring_view::npos ;
			   PostMessageW( launch, WM_COMMAND, MAKEWPARAM( IDOK, BN_CLICKED), 0L) ;
		   }) ;
	// 一件なら選択済みだが、起動は明示click後だけ受付ける
	if( !initial_valid || choice.profile_index != 0U || choice.open_settings || !window.active()) {
		return 25 ;
	}

	window.CancelPreparation() ;
	choice = window.ConfirmLaunch( L"Fixture", {profile},
								  /**
								   * @cond LOCAL_CALLBACK_CONTRACT
								   * @brief 確認器を再利用したときの廃止control不在と取消を検証する。
								   * @param[in] launch 同じtest内で再生成した確認dialog。
								   * @endcond
								   */
								  [&]( HWND launch) {
									  initial_valid = GetDlgItem( launch, 1115U) == nullptr ;
									  PostMessageW( launch, WM_COMMAND, IDCANCEL, 0L) ;
								  }) ;
	// 取消と次回表示でも廃止checkboxを公開しない
	if( !initial_valid || choice.profile_index || window.active()) {
		return 27 ;
	}

	auto second = profile ;
	second.name = L"Second fixture" ;
	choice		= window.ConfirmLaunch(
		 L"Fixture", {profile, second},
		 /**
		  * @cond LOCAL_CALLBACK_CONTRACT
		  * @brief 複数候補の未選択拒否と利用者選択後の一件承認を検証する。
		  * @param[in] launch 自作profileだけを持つ確認dialog。
		  * @endcond
		  */
		 [&]( HWND launch) {
			 initial_valid = ComboBox_GetCurSel( GetDlgItem( launch, IDC_LAUNCH_PROFILE)) == CB_ERR &&
							 !IsWindowEnabled( GetDlgItem( launch, IDOK)) ;
			 SendMessageW( launch, WM_COMMAND, MAKEWPARAM( IDOK, BN_CLICKED), 0L) ;
			 ComboBox_SetCurSel( GetDlgItem( launch, IDC_LAUNCH_PROFILE), 1) ;
			 SendMessageW( launch, WM_COMMAND, MAKEWPARAM( IDC_LAUNCH_PROFILE, CBN_SELCHANGE), 0L) ;
			 PostMessageW( launch, WM_COMMAND, MAKEWPARAM( IDOK, BN_CLICKED), 0L) ;
		 }) ;
	// 複数設定は暗黙選択せず、明示した一件だけを起動する
	if( !initial_valid || choice.profile_index != 1U || !window.active()) {
		return 28 ;
	}

	window.CancelPreparation() ;
	choice = window.ConfirmLaunch( L"Fixture", {profile},
								  /**
								   * @cond LOCAL_CALLBACK_CONTRACT
								   * @brief 旧内部設定入口が起動承認ではなく設定編集結果を返すよう選択する。
								   * @param[in] launch 設定入口を検証する自作dialog。
								   * @endcond
								   */
								  []( HWND launch) {
									  PostMessageW( launch, WM_COMMAND,
												   MAKEWPARAM( IDC_LAUNCH_SETTINGS, BN_CLICKED), 0L) ;
								  }) ;
	// 設定表示からは起動準備へ進まない
	if( !choice.open_settings || choice.profile_index || window.active()) {
		return 29 ;
	}

	approved = true ;
	// 承認後もownerへの固定起動一件だけを要求する
	if( !window.ConfirmPreparation( L"example (192.0.2.1)", true) || !window.ShowAutomatic(
																	   drive,
																	   /**
																	    * @cond LOCAL_CALLBACK_CONTRACT
																	    * @brief 自動起動要求の回数と固定clipboard世代をfakeで確認する。
																	    * @param[in] sequence ownerが確認したclipboard世代。実clipboardへ問い合わせない。
																	    * @return 世代がfixtureの0の場合だけtrue。
																	    * @endcond
																	    */
																	   [&]( DWORD sequence) {
																		   ++launches ;
																		   return sequence == 0U ;
																	   },
																	   /**
																	    * @cond LOCAL_CALLBACK_CONTRACT
																	    * @brief 実Agentを起動せずfixtureの進行snapshotを返す。
																	    * @return その時点のstatusのコピー。
																	    * @endcond
																	    */
																	   [&]() { return status ; })) {
		return 4 ;
	}

	const auto dialog = window.window() ;
	// フォルダ確認と貼付けは不要だが、まだ完了していない
	if( launches != 1U || clipboard_calls != 0U ||
	   IsWindowVisible( GetDlgItem( dialog, IDC_DEPLOY_FOLDER_READY)) ||
	   IsWindowVisible( GetDlgItem( dialog, IDC_DEPLOY_PASTE)) ||
	   IsWindowEnabled( GetDlgItem( dialog, IDC_DEPLOY_COMPLETE))) {
		return 5 ;
	}

	window.OnPasteResult( S_OK) ;
	SendMessageW( dialog, WM_TIMER, 1U, 0L) ;
	// 入力受付だけで完了を表示しない
	if( IsWindowEnabled( GetDlgItem( dialog, IDC_DEPLOY_COMPLETE)) || window.CloseCompleted()) {
		return 6 ;
	}

	// previewは偽の応答待ち画面だけを有限表示する
	if( argc == 3 && (std::wstring_view( argv[2]) == L"--preview" ||
					 std::wstring_view( argv[2]) == L"--preview-failure" ||
					 std::wstring_view( argv[2]) == L"--preview-partial")) {
		// previewの停止画面も実clipboard・RDPを使わない
		if( std::wstring_view( argv[2]) == L"--preview-failure" ||
		   std::wstring_view( argv[2]) == L"--preview-partial") {
			status = std::wstring_view( argv[2]) == L"--preview-partial"
						 ? deployment::AutomaticDeploymentSnapshot{-1, Stage::Paste,
																   Reason::PartialInput,
																   HRESULT_FROM_WIN32(
																	   ERROR_PARTIAL_COPY)}

						 : deployment::AutomaticDeploymentSnapshot{-1, Stage::InputPreparation,
																   Reason::InputPreparationFailure,
																   E_ACCESSDENIED} ;
			SendMessageW( dialog, WM_TIMER, 1U, 0L) ;
		}

		ShowWindow( dialog, SW_SHOW) ;
		const auto deadline = GetTickCount64() + 20000U ;
		// test所有Windowのmessageだけをdispatchする
		while( GetTickCount64() < deadline && window.window() != nullptr) {
			MSG message{} ;
			// 実RDPへ接続するmessageは生成しない
			while( PeekMessageW( &message, nullptr, 0U, 0U, PM_REMOVE)) {
				// test dialogが処理しなかったmessageを通常dispatchする
				if( !window.ProcessDialogMessage( message)) {
					TranslateMessage( &message) ;
					DispatchMessageW( &message) ;
				}
			}

			Sleep( 10U) ;
		}

		return 0 ;
	}

	status = {1, Stage::AgentResponse, Reason::None, S_OK} ;
	SendMessageW( dialog, WM_TIMER, 1U, 0L) ;
	// 同じ対象の応答確認後だけ閉じるbuttonを有効にする
	if( !IsWindowEnabled( GetDlgItem( dialog, IDC_DEPLOY_COMPLETE)) || launches != 1U ||
	   clipboard_calls != 0U) {
		return 7 ;
	}

	// 前回成功画面を手動で閉じていなくても次の明示要求で通常終了できる
	if( !window.CloseCompleted()) {
		return 34 ;
	}

	// 終了で未送信入力を失効し、他clipboardへ触れない
	if( window.active() || drive->sealed() || clipboard_calls != 0U) {
		return 8 ;
	}

	// 同じ配布driveで応答と終了を繰返し、前回成功状態を今回の応答に流用しない
	for( unsigned int attempt = 0U; attempt < 3U; ++attempt) {
		status = {} ;
		// 明示確認と同一packageの再検証なしでは新しい要求を作らない
		if( !window.ConfirmPreparation( L"example (192.0.2.1)", true) ||
		   !drive->PrepareLaunch( agent, "[allowlist]\nendpoint.1001=192.0.2.10:80\n") ||
		   !window.ShowAutomatic(
			   drive,
			   /**
			    * @cond LOCAL_CALLBACK_CONTRACT
			    * @brief 二回目の内部起動要求を同じfixture世代で数える。
			    * @param[in] sequence 起動要求時のclipboard世代。外部入力は発行しない。
			    * @return fixtureの世代0である場合だけtrue。
			    * @endcond
			    */
			   [&]( DWORD sequence) {
				   ++launches ;
				   return sequence == 0U ;
			   },
			   /**
			    * @cond LOCAL_CALLBACK_CONTRACT
			    * @brief 再起動試験へfakeのAgent応答状態を公開する。
			    * @return その時点のstatusのコピー。
			    * @endcond
			    */
			   [&] { return status ; })) {
			return 35 ;
		}

		window.OnPasteResult( S_OK) ;
		SendMessageW( window.window(), WM_TIMER, 1U, 0L) ;
		// 旧成功画面のlatchを今回へ持ち越さず、まだ閉じられない
		if( window.CloseCompleted() ||
		   IsWindowEnabled( GetDlgItem( window.window(), IDC_DEPLOY_COMPLETE))) {
			return 36 ;
		}

		status = {1, Stage::AgentResponse, Reason::None, S_OK} ;
		SendMessageW( window.window(), WM_TIMER, 1U, 0L) ;
		// 今回の応答後だけ成功画面を閉じ、同じfileを次回のため保持する
		if( !window.CloseCompleted() || window.active() || drive->sealed() || !drive->published() ||
		   launches != attempt + 2U || clipboard_calls != 0U) {
			return 37 ;
		}
	}

	drive.reset() ;
	std::filesystem::remove( std::filesystem::path( argv[1]) / L"bootstrap-ui-test") ;
	const std::array<deployment::AutomaticDeploymentSnapshot, 7U> failures{
		{{-1, Stage::Run, Reason::InputFailure, E_ACCESSDENIED},
		 {-1, Stage::Paste, Reason::ClipboardChanged, HRESULT_FROM_WIN32( ERROR_RETRY)},
		 {-1, Stage::Request, Reason::QueueFailure, HRESULT_FROM_WIN32( ERROR_BUSY)},
		 {-1, Stage::AgentResponse, Reason::AgentTimeout, HRESULT_FROM_WIN32( WAIT_TIMEOUT)},
		 {-1, Stage::Timer, Reason::InputTimeout, HRESULT_FROM_WIN32( WAIT_TIMEOUT)},
		 {-1, Stage::InputPreparation, Reason::InputPreparationFailure, E_ACCESSDENIED},
		 {-1, Stage::Paste, Reason::PartialInput, HRESULT_FROM_WIN32( ERROR_PARTIAL_COPY)}}} ;
	// 64-bit公開、入力失敗、queue拒否、timeoutを実clipboardなしで注入する
	for( std::size_t i = 0U; i < failures.size(); ++i) {
		const auto encoded = deployment::EncodeAutomaticDeploymentSnapshot( failures[i]) ;
		const auto decoded = deployment::DecodeAutomaticDeploymentSnapshot( encoded) ;
		// HRESULTの上位bitと同じ段階の分類を保持する
		if( decoded.status != failures[i].status || decoded.stage != failures[i].stage ||
		   decoded.reason != failures[i].reason || decoded.result != failures[i].result) {
			return 11 ;
		}

		const auto base = std::filesystem::path( argv[1]) /
						  (L"bootstrap-failure-test-" + std::to_wstring( GetCurrentProcessId()) +
						   L"-" + std::to_wstring( i)) ;
		auto failed_drive = DeploymentDrive::Create( base) ;
		// このtestのfake packageだけを専用fixtureへ公開する
		if( failed_drive == nullptr ||
		   !failed_drive->Publish( agent, "[allowlist]\nendpoint.1001=192.0.2.10:80\n")) {
			return 12 ;
		}

		deployment::AutomaticDeploymentSnapshot observed ;
		// timeoutはownerが最後に公開した入力段階と応答待ちを区別する
		if( i == 3U || i == 4U) {
			observed.stage = failures[i].stage ;
		}

		ULONGLONG				   now = 1000U ;
		app::AgentDeploymentWindow failed(
			GetModuleHandleW( nullptr), clipboard,
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief 停止経路試験の独自確認を実dialogなしで受理する。
			 * @return 固定true。Windows警告の自動承認ではない。
			 * @note 無名HWND・resource ID・本文は固定fake応答に不要で、表示や入力は行わない。
			 * @endcond
			 */
			[]( HWND, UINT, std::wstring_view) { return true ; },
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief 停止試験へ実時計ではなくfixtureの仮想時刻を供給する。
			 * @return fixtureのmillisecond単位の現在時刻。
			 * @endcond
			 */
			[&] { return now ; }) ;
		// queue拒否も画面生成時に即時停止へ進む
		if( !failed.ConfirmPreparation( L"example (192.0.2.1)", true) ||
		   !failed.ShowAutomatic(
			   /**
			    * @cond LOCAL_CALLBACK_CONTRACT
			    * @brief 指定段階の失敗を外部入力なしで再現する。
			    * @return fixture indexが失敗対象2以外ならtrue。
			    * @note 無名clipboard世代はこの停止分類試験では比較不要で、読み書きや外部送信をしない。
			    * @endcond
			    */
			   failed_drive, [&]( DWORD) { return i != 2U ; },
			   /**
			    * @cond LOCAL_CALLBACK_CONTRACT
			    * @brief 段階失敗試験の観測snapshotだけを返す。
			    * @return その時点のobservedのコピー。
			    * @endcond
			    */
			   [&] { return observed ; })) {
			return 13 ;
		}

		// ownerのAPI失敗は通知と同じsnapshotで即時表示する
		if( i == 0U) {
			observed = failures[i] ;
			failed.OnPasteResult( observed.result) ;
		}
		// clipboard guardの失敗と45秒timeoutはtimerで別々に確認する
		else if( i != 2U) {
			failed.OnPasteResult( S_OK) ;
			// 時間切れ前は完了buttonを有効にしない
			if( i == 3U || i == 4U) {
				now = 45999U ;
				SendMessageW( failed.window(), WM_TIMER, 1U, 0L) ;
				// 期限直前をtimeoutへ丸めない
				if( IsWindowEnabled( GetDlgItem( failed.window(), IDC_DEPLOY_COMPLETE))) {
					return 14 ;
				}

				now = 46000U ;
			} else {
				observed = failures[i] ;
			}

			SendMessageW( failed.window(), WM_TIMER, 1U, 0L) ;
		}

		std::array<wchar_t, 1024U> text{}, after{} ;
		GetDlgItemTextW( failed.window(), IDC_DEPLOY_STATUS, text.data(),
						static_cast<int>( text.size())) ;
		std::array<wchar_t, 32U> expected_code{} ;
		swprintf_s( expected_code.data(), expected_code.size(), L"0x%08X",
				   static_cast<unsigned int>( failures[i].result)) ;
		// 失敗は閉じられるが、応答成功とは表示しない
		if( failed.CloseCompleted() ||
		   !IsWindowEnabled( GetDlgItem( failed.window(), IDC_DEPLOY_COMPLETE)) ||
		   failed_drive->sealed() ||
		   (failures[i].stage == Stage::InputPreparation &&
			std::wstring_view( text.data()).find( L"入力フォーカス・キー転送の確認") ==
				std::wstring_view::npos) ||
		   std::wstring_view( text.data()).find( expected_code.data()) == std::wstring_view::npos ||
		   std::wstring_view( text.data())
				   .find( L"段階" + std::to_wstring( static_cast<unsigned int>( failures[i].stage))) ==
			   std::wstring_view::npos ||
		   std::wstring_view( text.data())
				   .find( L"分類" + std::to_wstring( static_cast<unsigned int>(
									   failures[i].reason))) == std::wstring_view::npos) {
			return 15 ;
		}

		failed.Invalidate() ;
		GetDlgItemTextW( failed.window(), IDC_DEPLOY_STATUS, after.data(),
						static_cast<int>( after.size())) ;
		// 後続取消は最初の詳細を上書きしない
		if( std::wstring_view( text.data()) != after.data()) {
			return 16 ;
		}

		SendMessageW( failed.window(), WM_COMMAND, MAKEWPARAM( IDC_DEPLOY_COMPLETE, BN_CLICKED), 0L) ;
		failed_drive.reset() ;
		std::filesystem::remove( base) ;
	}

	// 期限とbackoffは実時計やRDPなしで境界値を検証する
	if( deployment::AgentRecoveryDelay( 0U) != 5000U ||
	   deployment::AgentRecoveryDelay( 1U) != 10000U ||
	   deployment::AgentRecoveryDelay( 2U) != 20000U ||
	   deployment::AgentRecoveryDelay( 4U) != 60000U ||
	   deployment::AgentRecoveryDelay( 0xffffffffU) != 60000U ||
	   deployment::AgentLaunchExpired( 0U, 90000U) || deployment::AgentLaunchExpired( 100U, 45099U) ||
	   !deployment::AgentLaunchExpired( 100U, 45100U) ||
	   !deployment::AgentLaunchExpired( 100U, 99U) ||
	   deployment::kMaximumAgentRecoveryFailures != 3U) {
		return 40 ;
	}

	std::cout << "Automatic bootstrap UI and failure diagnostics passed\n" ;
	return 0 ;
}
