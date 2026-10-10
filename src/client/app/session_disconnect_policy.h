//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file session_disconnect_policy.h
 * @brief RDP session Windowの終了開始と切断後の維持方針を判定する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include <cstddef>

namespace zrdp::client::app {

// credential UIに続くcertificate UIまで表示済みとみなす最小warning回数
constexpr std::size_t kSecurityWarningCancellationMinimumDisplayCount = 2U ;

// certificate警告を拒否した実機でMicrosoft Controlが返す切断reason
constexpr long		  kSecurityWarningCancellationDisconnectReason = 263L ;

// certificate警告拒否時に追加情報がないことを示す拡張reason
constexpr long		  kSecurityWarningCancellationExtendedReason = 0L ;

/**
 * @brief session Window終了要求を開始する時点の処理を表す。
 */
enum class SessionCloseStartAction {
	/// 有効なRDP接続がないため直ちにWindowを破棄する。
	DestroyImmediately,
	/// 進行中の切断完了eventを待ってからWindowを破棄する。
	WaitForDisconnect,
	/// ActiveXへ明示切断を要求し、完了eventを待つ。
	RequestDisconnect,
} ;

/**
 * @brief 現在のRDP状態からsession Window終了開始処理を選択する。
 * @param[in] connection_active RDP接続または接続遷移が有効な場合はtrue。
 * @param[in] disconnecting ActiveXが既に切断処理中の場合はtrue。
 * @return 二重切断と接続中ActiveXの即時破棄を避ける終了開始処理。
 *
 * 接続中のActiveXを直接破棄すると、Microsoft Control内部のOLE終了が長時間
 * 待機する場合がある。通常の切断buttonと同じDisconnect／OnDisconnected経路へ
 * 統一し、既に切断中なら同じ完了eventだけを待つ。
 */
[[nodiscard]] constexpr SessionCloseStartAction
SelectSessionCloseStartAction( const bool connection_active, const bool disconnecting) noexcept
{
	// 有効な接続がなければ、非同期eventを待たずWindowを破棄できる
	if( !connection_active) {
		return SessionCloseStartAction::DestroyImmediately ;
	}

	// 進行中のDisconnectへ二重要求せず、最初のOnDisconnectedへ収束させる
	if( disconnecting) {
		return SessionCloseStartAction::WaitForDisconnect ;
	}

	return SessionCloseStartAction::RequestDisconnect ;
}

/**
 * @brief 成立済みsessionの外部切断後にWindowを無通知で閉じるか判定する。
 * @param[in] login_completed 現在の接続試行でログイン完了済みならtrue。
 * @param[in] locally_requested_disconnect ZRdp自身が切断を要求した場合はtrue。
 * @param[in] credential_prompt_cancelled Microsoft認証画面の取消ならtrue。
 * @return 接続先再起動等の成立済みsession外部切断ならtrue。
 *
 * 初回接続失敗は原因確認のためWindowとerror表示を維持する。一方、成立済み
 * sessionが外部要因で最終切断された場合は、利用不能なsession Windowを残さない。
 */
[[nodiscard]] constexpr bool
ShouldCloseWindowAfterExternalDisconnect( const bool login_completed,
										 const bool locally_requested_disconnect,
										 const bool credential_prompt_cancelled) noexcept
{
	return login_completed && !locally_requested_disconnect && !credential_prompt_cancelled ;
}

/**
 * @brief Microsoft certificate警告の拒否後に生じた切断か完全一致で判定する。
 * @param[in] login_completed 現在試行でログイン完了済みならtrue。
 * @param[in] locally_requested_disconnect ZRdp自身の切断要求ならtrue。
 * @param[in] credential_prompt_cancelled credential UIの取消ならtrue。
 * @param[in] authentication_warning_display_count 現在試行の認証warning表示回数。
 * @param[in] disconnect_reason `OnDisconnected`が通知したreason。
 * @param[in] extended_disconnect_reason ActiveXから取得した拡張reason。
 * @return 二回目のMicrosoft警告後に観測した取消条件だけtrue。
 *
 * ActiveXは警告buttonの選択値を公開しないため、reasonだけでは分類しない。
 * credential UIに続く二回目のwarning表示と未ログイン状態を必須にし、通常の
 * SSL handshake失敗を広く無通知化しない。
 */
[[nodiscard]] constexpr bool IsSecurityWarningCancellation(
	const bool login_completed, const bool locally_requested_disconnect,
	const bool credential_prompt_cancelled, const std::size_t authentication_warning_display_count,
	const long disconnect_reason, const long extended_disconnect_reason) noexcept
{
	return !login_completed && !locally_requested_disconnect && !credential_prompt_cancelled &&
		   authentication_warning_display_count >=
			   kSecurityWarningCancellationMinimumDisplayCount &&
		   disconnect_reason == kSecurityWarningCancellationDisconnectReason &&
		   extended_disconnect_reason == kSecurityWarningCancellationExtendedReason ;
}

} // namespace zrdp::client::app
