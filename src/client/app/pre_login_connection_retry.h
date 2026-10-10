//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file pre_login_connection_retry.h
 * @brief ログイン前の既知一時切断だけを有限再試行へ分類する。
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

#include <array>
#include <cstddef>
#include <cstdint>

namespace zrdp::client::app {

// Windows event logでもSSL handshake失敗と一致したActiveX切断reason
inline constexpr long						   kTransientPreLoginDisconnectReason = 263L ;

// ActiveXが追加情報を返さなかった場合のextended reason
inline constexpr long						   kNoPreLoginExtendedDisconnectReason = 0L ;

// Microsoft RDP内部状態をcallback終了後に解放する段階的な有限待機時間
inline constexpr std::array<std::uint32_t, 2U> kPreLoginConnectionRetryDelaysMilliseconds{
	{2000U, 5000U}} ;

// credential誤り等を無制限に再送しないための接続意図ごとの上限
inline constexpr std::size_t kMaximumPreLoginConnectionRetries =
	kPreLoginConnectionRetryDelaysMilliseconds.size() ;

/**
 * @brief ログイン前切断後に実施する処理を表す。
 */
enum class PreLoginConnectionRetryAction {
	/// 対象外のため既存切断処理を続行する。
	None,
	/// 有限delay後の次回再試行を予約する。
	Schedule,
	/// 対象signatureだが既に上限へ到達している。
	Exhausted,
} ;

/**
 * @brief ログイン前接続再試行の純粋な選択結果。
 */
struct PreLoginConnectionRetryDecision {
	/// 呼出し側が実行する処理。
	PreLoginConnectionRetryAction action{PreLoginConnectionRetryAction::None} ;

	/// `Schedule`の場合だけ使用する待機時間。
	std::uint32_t				  delay_milliseconds{0U} ;
} ;

/**
 * @brief 切断情報と現在試行状態から有限再試行の可否を選択する。
 * @param[in] login_completed 現在の接続意図でlogin完了済みならtrue。
 * @param[in] locally_requested ZRdp自身が切断を要求した場合はtrue。
 * @param[in] credential_prompt_cancelled 利用者が認証UIを取り消した場合はtrue。
 * @param[in] authentication_dialog_displayed 現在試行でMicrosoft認証UIを表示した場合はtrue。
 * @param[in] closing Window終了処理中の場合はtrue。
 * @param[in] disconnect_reason ActiveX `OnDisconnected` reason。
 * @param[in] extended_disconnect_reason ActiveX拡張切断reason。
 * @param[in] retries_scheduled 現在の接続意図で予約済みの再試行数。
 * @return 対象外、予約、上限到達のいずれか。
 *
 * 認証の利用者判断と成立済みsessionを尊重し、観測済みsignature以外を
 * 一時障害として推測しない。
 */
[[nodiscard]] constexpr PreLoginConnectionRetryDecision
SelectPreLoginConnectionRetry( const bool login_completed, const bool locally_requested,
							  const bool credential_prompt_cancelled,
							  const bool authentication_dialog_displayed, const bool closing,
							  const long disconnect_reason, const long extended_disconnect_reason,
							  const std::size_t retries_scheduled) noexcept
{
	// 成立済みsession、利用者判断を伴う認証UI、終了処理では自動接続を開始しない
	if( login_completed || locally_requested || credential_prompt_cancelled ||
	   authentication_dialog_displayed || closing) {
		return {} ;
	}

	// ローカルlogとWindows SSL eventが一致した完全な数値組だけを対象にする
	if( disconnect_reason != kTransientPreLoginDisconnectReason ||
	   extended_disconnect_reason != kNoPreLoginExtendedDisconnectReason) {
		return {} ;
	}

	// password再入力やaccount lockoutを誘発しないよう有限表の終端で必ず停止する
	if( retries_scheduled >= kMaximumPreLoginConnectionRetries) {
		return {
			PreLoginConnectionRetryAction::Exhausted,
			0U,
		} ;
	}

	return {
		PreLoginConnectionRetryAction::Schedule,
		kPreLoginConnectionRetryDelaysMilliseconds[retries_scheduled],
	} ;
}

} // namespace zrdp::client::app
