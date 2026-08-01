/**
 * @file session_disconnect_policy_test.cpp
 * @brief RDP接続段階と切断起点ごとのsession Window終了方針を検証する。
 */

#include "client/app/session_disconnect_policy.h"
#include "client/app/pre_login_connection_retry.h"

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief 外部切断と利用者起因切断に対するWindow終了policyを検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::client::app::kMaximumPreLoginConnectionRetries;
    using zrdp::client::app::kPreLoginConnectionRetryDelayMilliseconds;
    using zrdp::client::app::PreLoginConnectionRetryAction;
    using zrdp::client::app::SelectPreLoginConnectionRetry;
    using zrdp::client::app::SelectSessionCloseStartAction;
    using zrdp::client::app::SessionCloseStartAction;
    using zrdp::client::app::
        ShouldCloseWindowAfterExternalDisconnect;

    // 未接続WindowはActiveX eventを待たず直ちに破棄する。
    if (SelectSessionCloseStartAction(false, false) !=
        SessionCloseStartAction::DestroyImmediately) {
        return 1;
    }

    // 成立中接続は即時破棄せず、通常buttonと同じDisconnectを要求する。
    if (SelectSessionCloseStartAction(true, false) !=
        SessionCloseStartAction::RequestDisconnect) {
        return 2;
    }

    // 既に切断中のsessionへ二重Disconnectを発行しない。
    if (SelectSessionCloseStartAction(true, true) !=
        SessionCloseStartAction::WaitForDisconnect) {
        return 3;
    }

    // ログイン完了後の接続先再起動等では、利用不能なWindowを無通知終了する。
    if (!ShouldCloseWindowAfterExternalDisconnect(
            true,
            false,
            false)) {
        return 4;
    }

    // 初回接続失敗は原因を確認できるよう、Windowとerror表示を維持する。
    if (ShouldCloseWindowAfterExternalDisconnect(
            false,
            false,
            false)) {
        return 5;
    }

    // ZRdp自身の切断要求は既存のclose／切断完了経路へ委ねる。
    if (ShouldCloseWindowAfterExternalDisconnect(
            true,
            true,
            false)) {
        return 6;
    }

    // Microsoft認証画面の取消は従来どおり追加errorなしで未接続へ戻す。
    if (ShouldCloseWindowAfterExternalDisconnect(
            true,
            false,
            true)) {
        return 7;
    }

    const auto retry = SelectPreLoginConnectionRetry(
        false,
        false,
        false,
        false,
        263L,
        0L,
        0U);
    // 観測済みのログイン前signatureだけは2秒後の一回再試行を選ぶ。
    if (retry.action !=
            PreLoginConnectionRetryAction::Schedule ||
        retry.delay_milliseconds !=
            kPreLoginConnectionRetryDelayMilliseconds) {
        return 8;
    }

    // 一度login完了したsessionを新規Connectで置き換えない。
    if (SelectPreLoginConnectionRetry(
            true,
            false,
            false,
            false,
            263L,
            0L,
            0U).action !=
        PreLoginConnectionRetryAction::None) {
        return 9;
    }

    // ZRdp自身の切断要求では利用者意図に反して再接続しない。
    if (SelectPreLoginConnectionRetry(
            false,
            true,
            false,
            false,
            263L,
            0L,
            0U).action !=
        PreLoginConnectionRetryAction::None) {
        return 10;
    }

    // Microsoft認証UIの取消後に同じcredential入力を再要求しない。
    if (SelectPreLoginConnectionRetry(
            false,
            false,
            true,
            false,
            263L,
            0L,
            0U).action !=
        PreLoginConnectionRetryAction::None) {
        return 11;
    }

    // Window終了中に遅延Connectを新しく予約しない。
    if (SelectPreLoginConnectionRetry(
            false,
            false,
            false,
            true,
            263L,
            0L,
            0U).action !=
        PreLoginConnectionRetryAction::None) {
        return 12;
    }

    // 通常reasonまたは拡張reasonのどちらかが異なる場合は推測で再試行しない。
    if (SelectPreLoginConnectionRetry(
            false,
            false,
            false,
            false,
            264L,
            0L,
            0U).action !=
            PreLoginConnectionRetryAction::None ||
        SelectPreLoginConnectionRetry(
            false,
            false,
            false,
            false,
            263L,
            7L,
            0U).action !=
            PreLoginConnectionRetryAction::None) {
        return 13;
    }

    // 一回予約済みの同一signatureは上限到達として明示し、再予約しない。
    if (SelectPreLoginConnectionRetry(
            false,
            false,
            false,
            false,
            263L,
            0L,
            kMaximumPreLoginConnectionRetries).action !=
        PreLoginConnectionRetryAction::Exhausted) {
        return 14;
    }

    return 0;
}
