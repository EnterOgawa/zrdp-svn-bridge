//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file session_disconnect_policy_test.cpp
 * @brief RDP接続段階と切断起点ごとのsession Window終了方針を検証する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  認証UIの9991/0を自動retryと取消から除外。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/app/session_disconnect_policy.h"
#include "client/app/pre_login_connection_retry.h"

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief 外部切断と利用者起因切断に対するWindow終了policyを検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain()
{
	using zrdp::client::app::kMaximumPreLoginConnectionRetries ;
	using zrdp::client::app::kSecurityWarningCancellationMinimumDisplayCount ;
	using zrdp::client::app::IsSecurityWarningCancellation ;
	using zrdp::client::app::kPreLoginConnectionRetryDelaysMilliseconds ;
	using zrdp::client::app::PreLoginConnectionRetryAction ;
	using zrdp::client::app::SelectPreLoginConnectionRetry ;
	using zrdp::client::app::SelectSessionCloseStartAction ;
	using zrdp::client::app::SessionCloseStartAction ;
	using zrdp::client::app::ShouldCloseWindowAfterExternalDisconnect ;

	// 未接続WindowはActiveX eventを待たず直ちに破棄する
	if( SelectSessionCloseStartAction( false, false) != SessionCloseStartAction::DestroyImmediately) {
		return 1 ;
	}

	// 成立中接続は即時破棄せず、通常buttonと同じDisconnectを要求する
	if( SelectSessionCloseStartAction( true, false) != SessionCloseStartAction::RequestDisconnect) {
		return 2 ;
	}

	// 既に切断中のsessionへ二重Disconnectを発行しない
	if( SelectSessionCloseStartAction( true, true) != SessionCloseStartAction::WaitForDisconnect) {
		return 3 ;
	}

	// ログイン完了後の接続先再起動等では、利用不能なWindowを無通知終了する
	if( !ShouldCloseWindowAfterExternalDisconnect( true, false, false)) {
		return 4 ;
	}

	// 初回接続失敗は原因を確認できるよう、Windowとerror表示を維持する
	if( ShouldCloseWindowAfterExternalDisconnect( false, false, false)) {
		return 5 ;
	}

	// ZRdp自身の切断要求は既存のclose／切断完了経路へ委ねる
	if( ShouldCloseWindowAfterExternalDisconnect( true, true, false)) {
		return 6 ;
	}

	// Microsoft認証画面の取消は従来どおり追加errorなしで未接続へ戻す
	if( ShouldCloseWindowAfterExternalDisconnect( true, false, true)) {
		return 7 ;
	}

	const auto first_retry =
		SelectPreLoginConnectionRetry( false, false, false, false, false, 263L, 0L, 0U) ;
	// 観測済みのログイン前signatureは、最初に2秒後の再試行を選ぶ
	if( first_retry.action != PreLoginConnectionRetryAction::Schedule ||
	   first_retry.delay_milliseconds != kPreLoginConnectionRetryDelaysMilliseconds[0U]) {
		return 8 ;
	}

	const auto second_retry =
		SelectPreLoginConnectionRetry( false, false, false, false, false, 263L, 0L, 1U) ;
	// 同じ一時signatureが続いた場合だけ、次は5秒待って最終試行へ進む
	if( second_retry.action != PreLoginConnectionRetryAction::Schedule ||
	   second_retry.delay_milliseconds != kPreLoginConnectionRetryDelaysMilliseconds[1U]) {
		return 9 ;
	}

	// 一度login完了したsessionを新規Connectで置き換えない
	if( SelectPreLoginConnectionRetry( true, false, false, false, false, 263L, 0L, 0U).action !=
	   PreLoginConnectionRetryAction::None) {
		return 10 ;
	}

	// ZRdp自身の切断要求では利用者意図に反して再接続しない
	if( SelectPreLoginConnectionRetry( false, true, false, false, false, 263L, 0L, 0U).action !=
	   PreLoginConnectionRetryAction::None) {
		return 11 ;
	}

	// Microsoft認証UIの取消後に同じcredential入力を再要求しない
	if( SelectPreLoginConnectionRetry( false, false, true, false, false, 263L, 0L, 0U).action !=
	   PreLoginConnectionRetryAction::None) {
		return 12 ;
	}

	// Window終了中に遅延Connectを新しく予約しない
	if( SelectPreLoginConnectionRetry( false, false, false, false, true, 263L, 0L, 0U).action !=
	   PreLoginConnectionRetryAction::None) {
		return 13 ;
	}

	// 通常reasonまたは拡張reasonのどちらかが異なる場合は推測で再試行しない
	if( SelectPreLoginConnectionRetry( false, false, false, false, false, 264L, 0L, 0U).action !=
		   PreLoginConnectionRetryAction::None ||
	   SelectPreLoginConnectionRetry( false, false, false, false, false, 263L, 7L, 0U).action !=
		   PreLoginConnectionRetryAction::None) {
		return 14 ;
	}

	// Microsoft認証UIを一度でも表示した試行は、利用者判断を別Connectで反復しない
	if( SelectPreLoginConnectionRetry( false, false, false, true, false, 263L, 0L, 0U).action !=
	   PreLoginConnectionRetryAction::None) {
		return 15 ;
	}

	// 二回予約済みの同一signatureは上限到達として明示し、再予約しない
	if( SelectPreLoginConnectionRetry( false, false, false, false, false, 263L, 0L,
									 kMaximumPreLoginConnectionRetries)
		   .action != PreLoginConnectionRetryAction::Exhausted) {
		return 16 ;
	}

	// 二回目のMicrosoft警告を拒否した263/0だけを追加error抑止対象にする
	if( !IsSecurityWarningCancellation( false, false, false,
									  kSecurityWarningCancellationMinimumDisplayCount, 263L, 0L)) {
		return 17 ;
	}

	// warning一回、成立済み、local切断、credential取消は拒否操作と推測しない
	if( IsSecurityWarningCancellation( false, false, false, 1U, 263L, 0L) ||
	   IsSecurityWarningCancellation( true, false, false,
									 kSecurityWarningCancellationMinimumDisplayCount, 263L, 0L) ||
	   IsSecurityWarningCancellation( false, true, false,
									 kSecurityWarningCancellationMinimumDisplayCount, 263L, 0L) ||
	   IsSecurityWarningCancellation( false, false, true,
									 kSecurityWarningCancellationMinimumDisplayCount, 263L, 0L)) {
		return 18 ;
	}

	// reasonまたは拡張reasonが異なる失敗は従来どおりerror表示へ残す
	if( IsSecurityWarningCancellation( false, false, false,
									 kSecurityWarningCancellationMinimumDisplayCount, 264L, 0L) ||
	   IsSecurityWarningCancellation( false, false, false,
									 kSecurityWarningCancellationMinimumDisplayCount, 263L, 1L)) {
		return 19 ;
	}

	// Broker失敗はUI表示有無・回数によらずretryも利用者取消への近似も行わない
	for( const bool displayed : {false, true}) {
		// 有限retryの各段階でも9991を対象へ拡大しない
		for( unsigned int retries = 0U; retries <= kMaximumPreLoginConnectionRetries; ++retries) {
			// RPC系認証失敗を接続成功や単なるcertificate拒否として隠さない
			if( SelectPreLoginConnectionRetry( false, false, false, displayed, false, 9991L, 0L,
											 retries)
					   .action != PreLoginConnectionRetryAction::None ||
			   IsSecurityWarningCancellation( false, false, false,
											 kSecurityWarningCancellationMinimumDisplayCount, 9991L,
											 0L)) {
				return 20 ;
			}
		}
	}

	return 0 ;
}
