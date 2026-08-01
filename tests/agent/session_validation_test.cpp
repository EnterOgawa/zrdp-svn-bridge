/**
 * @file session_validation_test.cpp
 * @brief Agent session policyをWindows session探索なしのpure inputで検証する。
 */

#include "agent/app/session_validation.h"

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Agentが対話RDP user sessionだけを許可する判定を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::agent::app::SessionValidationCode;
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

    // disconnected RDP sessionへAgentを常駐させない。
    if (ValidateSessionAttributes(
            1U,
            WTSDisconnected,
            2U).code !=
        SessionValidationCode::SessionNotActive) {
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

    return 0;
}
