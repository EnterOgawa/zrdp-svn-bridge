/**
 * @file session_validation_test.cpp
 * @brief Agent session policyをWindows session探索なしのpure inputで検証する。
 */

#include "agent/app/session_validation.h"
#include "agent/app/channel_recovery_policy.h"

#include <array>

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Agentが対話RDP user sessionだけを許可する判定を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::agent::app::SessionValidationCode;
    using zrdp::agent::app::ChannelRecoveryAction;
    using zrdp::agent::app::ChannelRecoveryDelayMilliseconds;
    using zrdp::agent::app::SelectChannelRecoveryAction;
    using zrdp::agent::app::ShouldLogChannelRecoveryAttempt;
    using zrdp::agent::app::ValidateSessionAttributes;

    // 非0、active、RDP protocol 2の完全一致だけを許可する。
    if (ValidateSessionAttributes(
            1U,
            WTSActive,
            2U).code != SessionValidationCode::Valid) {
        return 1;
    }

    // service用Session 0を対話sessionとして扱わない。
    if (ValidateSessionAttributes(
            0U,
            WTSActive,
            2U).code !=
        SessionValidationCode::SessionZero) {
        return 2;
    }

    // startup時はdisconnected RDP sessionを従来どおり拒否する。
    const auto disconnected = ValidateSessionAttributes(
        1U,
        WTSDisconnected,
        2U);
    if (disconnected.code !=
            SessionValidationCode::SessionNotActive ||
        SelectChannelRecoveryAction(disconnected) !=
            ChannelRecoveryAction::Wait) {
        return 3;
    }

    // console protocol 0をRDPとして近似しない。
    if (ValidateSessionAttributes(
            1U,
            WTSActive,
            0U).code !=
        SessionValidationCode::SessionNotRdp) {
        return 4;
    }

    // legacy protocol値1も初期製品では拒否する。
    if (ValidateSessionAttributes(
            1U,
            WTSActive,
            1U).code !=
        SessionValidationCode::SessionNotRdp) {
        return 5;
    }

    // active RDP sessionだけがSVC再openへ進める。
    if (SelectChannelRecoveryAction(
            ValidateSessionAttributes(
                1U,
                WTSActive,
                2U)) !=
        ChannelRecoveryAction::OpenChannel) {
        return 6;
    }

    constexpr std::array transient_states{
        WTSConnected,
        WTSConnectQuery,
        WTSShadow,
        WTSDisconnected,
        WTSIdle,
    };
    // 自動再接続中に現れ得るstateではprocessを残し、active復帰を待つ。
    for (const auto state : transient_states) {
        if (SelectChannelRecoveryAction(
                ValidateSessionAttributes(
                    1U,
                    state,
                    2U)) !=
            ChannelRecoveryAction::Wait) {
            return 7;
        }
    }

    constexpr std::array ended_states{
        WTSListen,
        WTSReset,
        WTSDown,
        WTSInit,
    };
    // current user sessionの明示的な終了stateでは再openを停止する。
    for (const auto state : ended_states) {
        if (SelectChannelRecoveryAction(
                ValidateSessionAttributes(
                    1U,
                    state,
                    2U)) !=
            ChannelRecoveryAction::Stop) {
            return 8;
        }
    }

    // WTS照会の一時失敗からsession終了を推測せず回復待機する。
    if (SelectChannelRecoveryAction({
            SessionValidationCode::ConnectStateQueryFailed,
            ERROR_GEN_FAILURE,
            WTSInit,
            0U,
        }) != ChannelRecoveryAction::Wait) {
        return 9;
    }

    // state取得後のprotocol照会失敗では取得済み終了stateを保持する。
    if (SelectChannelRecoveryAction({
            SessionValidationCode::ProtocolQueryFailed,
            ERROR_GEN_FAILURE,
            WTSDown,
            0U,
        }) != ChannelRecoveryAction::Stop) {
        return 10;
    }

    // 非RDP化したsessionへ別transportを推測してfallbackしない。
    if (SelectChannelRecoveryAction(
            ValidateSessionAttributes(
                1U,
                WTSActive,
                0U)) !=
        ChannelRecoveryAction::Stop) {
        return 11;
    }

    // backoffは250 msから指数増加し、2秒で必ず上限となる。
    if (ChannelRecoveryDelayMilliseconds(0U) != 250U ||
        ChannelRecoveryDelayMilliseconds(1U) != 500U ||
        ChannelRecoveryDelayMilliseconds(2U) != 1000U ||
        ChannelRecoveryDelayMilliseconds(3U) != 2000U ||
        ChannelRecoveryDelayMilliseconds(100U) != 2000U) {
        return 12;
    }

    // retry logは2の冪だけに間引き、長時間断で線形増加させない。
    if (ShouldLogChannelRecoveryAttempt(0U) ||
        !ShouldLogChannelRecoveryAttempt(1U) ||
        !ShouldLogChannelRecoveryAttempt(2U) ||
        ShouldLogChannelRecoveryAttempt(3U) ||
        !ShouldLogChannelRecoveryAttempt(4U) ||
        ShouldLogChannelRecoveryAttempt(5U)) {
        return 13;
    }

    return 0;
}
