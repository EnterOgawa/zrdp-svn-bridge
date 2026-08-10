/**
 * @file channel_recovery_policy.h
 * @brief AgentのSVC再open可否、retry間隔、診断間引きpolicyを定義する。
 *
 * @details Windows APIを呼ばないpure policyとし、一時RDP切断ではprocessを維持する
 *          一方、終了済みsessionや非RDP sessionでは再openを停止する。
 */
#pragma once

#include "agent/app/session_validation.h"

#include <cstdint>

namespace zrdp::agent::app {

/// channel回復loopがcurrent sessionに対して行う次の操作。
enum class ChannelRecoveryAction {
    OpenChannel,  ///< active RDP sessionでSVC openを試行する。
    Wait,         ///< 一時切断または照会失敗のためprocessを維持して待つ。
    Stop,         ///< session終了または非RDP化のためAgentを停止する。
};

/// 最初のSVC再openまで旧RDP transportの解放を待つ時間。
inline constexpr std::uint32_t
    kChannelRecoveryInitialDelayMilliseconds = 250U;

/// Ctrl+CとWTS stateを再確認するpoll間隔。
inline constexpr std::uint32_t
    kChannelRecoveryPollMilliseconds = 100U;

/// 長時間のRDP切断でも再試行頻度を有限に抑える上限間隔。
inline constexpr std::uint32_t
    kChannelRecoveryMaximumDelayMilliseconds = 2000U;

/**
 * @brief current session検証結果をSVC回復loopの操作へ分類する。
 * @param[in] result current processだけを照会したsession検証結果。
 * @return SVC open、待機、Agent停止のいずれか。
 *
 * @note WTS照会失敗だけではsession終了を推測せず、fail-closedでopenを待機する。
 */
[[nodiscard]] constexpr ChannelRecoveryAction
SelectChannelRecoveryAction(
    const SessionValidationResult& result) noexcept {
    switch (result.code) {
        case SessionValidationCode::Valid:
            return ChannelRecoveryAction::OpenChannel;

        case SessionValidationCode::SessionNotActive:
            switch (result.connect_state) {
                case WTSConnected:
                case WTSConnectQuery:
                case WTSShadow:
                case WTSDisconnected:
                case WTSIdle:
                    // 再接続可能なuser session stateではSVCを開かず復帰を待つ。
                    return ChannelRecoveryAction::Wait;

                case WTSListen:
                case WTSReset:
                case WTSDown:
                case WTSInit:
                    return ChannelRecoveryAction::Stop;

                case WTSActive:
                    // validation codeとstateの矛盾ではSVCを推測して開かない。
                    return ChannelRecoveryAction::Wait;
            }
            return ChannelRecoveryAction::Wait;

        case SessionValidationCode::ProtocolQueryFailed:
            // state取得後のprotocol照会失敗でも明示的な終了stateは尊重する。
            if (result.connect_state == WTSListen ||
                result.connect_state == WTSReset ||
                result.connect_state == WTSDown ||
                result.connect_state == WTSInit) {
                return ChannelRecoveryAction::Stop;
            }
            return ChannelRecoveryAction::Wait;

        case SessionValidationCode::ProcessSessionQueryFailed:
        case SessionValidationCode::ConnectStateQueryFailed:
            return ChannelRecoveryAction::Wait;

        case SessionValidationCode::SessionZero:
        case SessionValidationCode::SessionNotRdp:
            return ChannelRecoveryAction::Stop;
    }

    return ChannelRecoveryAction::Wait;
}

/**
 * @brief 完了済みopen試行回数から次の有限backoffを求める。
 * @param[in] completed_attempts 現在の回復期間で既に実行したopen回数。
 * @return 250 msから2秒までの指数backoff。
 */
[[nodiscard]] constexpr std::uint32_t
ChannelRecoveryDelayMilliseconds(
    const std::uint64_t completed_attempts) noexcept {
    if (completed_attempts >= 3U) {
        return kChannelRecoveryMaximumDelayMilliseconds;
    }

    return kChannelRecoveryInitialDelayMilliseconds <<
        static_cast<std::uint32_t>(completed_attempts);
}

/**
 * @brief 長時間断のopen失敗を対数件数だけ診断記録するか判定する。
 * @param[in] attempt_count 1始まりのSVC open試行回数。
 * @return 1、2、4、8、...回目だけtrue。
 */
[[nodiscard]] constexpr bool
ShouldLogChannelRecoveryAttempt(
    const std::uint64_t attempt_count) noexcept {
    return attempt_count != 0U &&
        (attempt_count & (attempt_count - 1U)) == 0U;
}

}  // namespace zrdp::agent::app
