//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file channel_recovery_policy.h
 * @brief AgentのSVC再open可否、retry間隔、診断間引きpolicyを定義する。
 * @version 01.00.00
 * @date 2026/08/10
 * @author Shunji Ogawa
 *
 * @details Windows APIを呼ばないpure policyとし、一時RDP切断ではprocessを維持する
 *          一方、終了済みsessionや非RDP sessionでは再openを停止する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "agent/app/session_validation.h"

#include <cstdint>

namespace zrdp::agent::app {

/**
 * @brief channel回復loopがcurrent sessionに対して行う次の操作。
 */
enum class ChannelRecoveryAction {
	/// active RDP sessionでSVC openを試行する。
	OpenChannel,
	/// 一時切断または照会失敗のためprocessを維持して待つ。
	Wait,
	/// session終了または非RDP化のためAgentを停止する。
	Stop,
} ;

// 最初のSVC再openまで旧RDP transportの解放を待つ時間
inline constexpr std::uint32_t kChannelRecoveryInitialDelayMilliseconds = 250U ;

// Ctrl+CとWTS stateを再確認するpoll間隔
inline constexpr std::uint32_t kChannelRecoveryPollMilliseconds = 100U ;

// 長時間のRDP切断でも再試行頻度を有限に抑える上限間隔
inline constexpr std::uint32_t kChannelRecoveryMaximumDelayMilliseconds = 2000U ;

/**
 * @brief current session検証結果をSVC回復loopの操作へ分類する。
 * @param[in] result current processだけを照会したsession検証結果。
 * @return SVC open、待機、Agent停止のいずれか。
 *
 * @note WTS照会失敗だけではsession終了を推測せず、fail-closedでopenを待機する。
 */
[[nodiscard]] constexpr ChannelRecoveryAction
SelectChannelRecoveryAction( const SessionValidationResult &result) noexcept
{
	// session検証の失敗種別から、SVCを開くか復帰を待つか停止するかを選ぶ
	switch( result.code) {
	// activeなRDP sessionと確認できた場合だけSVC openを許可する
	case SessionValidationCode::Valid:
		return ChannelRecoveryAction::OpenChannel ;

	// 非active sessionはWTSの接続状態を確認して待機と終了を区別する
	case SessionValidationCode::SessionNotActive:
		// user sessionの復帰可能性に応じてchannel回復の継続可否を選ぶ
		switch( result.connect_state) {
		// 接続済みでもactive未確認の間はSVCを開かず待つ
		case WTSConnected:
		// 接続確立途中では認証・session復帰を待つ
		case WTSConnectQuery:
		// shadow状態からの復帰を待ち、現在のchannelを推測して開かない
		case WTSShadow:
		// 一時切断ではAgentを終了せず再接続を待つ
		case WTSDisconnected:
		// idleなuser sessionではactiveへの復帰を待つ
		case WTSIdle:
			// 再接続可能なuser session stateではSVCを開かず復帰を待つ
			return ChannelRecoveryAction::Wait ;

		// listener状態は対象のuser sessionとして使用せず停止する
		case WTSListen:
		// reset中のsessionを再利用せずAgentを停止する
		case WTSReset:
		// 終了済みsessionにはSVC再openを試みない
		case WTSDown:
		// 初期化状態は利用中のuser sessionとみなさず停止する
		case WTSInit:
			return ChannelRecoveryAction::Stop ;

		// 検証結果とactive状態が矛盾する場合は再照会まで待つ
		case WTSActive:
			// validation codeとstateの矛盾ではSVCを推測して開かない
			return ChannelRecoveryAction::Wait ;
		}

		return ChannelRecoveryAction::Wait ;

	// protocol照会だけの失敗では取得済みの終了状態を尊重する
	case SessionValidationCode::ProtocolQueryFailed:
		// state取得後のprotocol照会失敗でも明示的な終了stateは尊重する
		if( result.connect_state == WTSListen || result.connect_state == WTSReset ||
		   result.connect_state == WTSDown || result.connect_state == WTSInit) {
			return ChannelRecoveryAction::Stop ;
		}

		return ChannelRecoveryAction::Wait ;

	// processのsession照会失敗だけでuser sessionの終了を推測しない
	case SessionValidationCode::ProcessSessionQueryFailed:
	// 接続状態の照会失敗ではSVCを開かず再確認を待つ
	case SessionValidationCode::ConnectStateQueryFailed:
		return ChannelRecoveryAction::Wait ;

	// service用session 0ではchannel回復を行わない
	case SessionValidationCode::SessionZero:
	// RDP以外のsessionへ変化した場合はAgentを停止する
	case SessionValidationCode::SessionNotRdp:
		return ChannelRecoveryAction::Stop ;
	}

	return ChannelRecoveryAction::Wait ;
}

/**
 * @brief 完了済みopen試行回数から次の有限backoffを求める。
 * @param[in] completed_attempts 現在の回復期間で既に実行したopen回数。
 * @return 250 msから2秒までの指数backoff。
 */
[[nodiscard]] constexpr std::uint32_t
ChannelRecoveryDelayMilliseconds( const std::uint64_t completed_attempts) noexcept
{
	// 試行回数が増えてもbackoffを2秒の上限に固定して無制限に延ばさない
	if( completed_attempts >= 3U) {
		return kChannelRecoveryMaximumDelayMilliseconds ;
	}

	return kChannelRecoveryInitialDelayMilliseconds
		   << static_cast<std::uint32_t>( completed_attempts) ;
}

/**
 * @brief 長時間断のopen失敗を対数件数だけ診断記録するか判定する。
 * @param[in] attempt_count 1始まりのSVC open試行回数。
 * @return 1、2、4、8、...回目だけtrue。
 */
[[nodiscard]] constexpr bool
ShouldLogChannelRecoveryAttempt( const std::uint64_t attempt_count) noexcept
{
	return attempt_count != 0U && (attempt_count & (attempt_count - 1U)) == 0U ;
}

} // namespace zrdp::agent::app
