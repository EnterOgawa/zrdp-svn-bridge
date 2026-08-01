/**
 * @file dynamic_resolution_retry_test.cpp
 * @brief 動的解像度の有限retryとWindow／全画面target選択を検証する。
 */

#include "client/app/dynamic_resolution_retry.h"
#include "client/app/dynamic_resolution_target.h"

#include <Windows.h>

#include <array>
#include <cstddef>

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief 動的解像度の一時失敗だけを有限回再試行するpolicyを検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::client::app::DynamicResolutionTarget;
    using zrdp::client::app::SelectDynamicResolutionRetry;
    using zrdp::client::app::SelectDynamicResolutionTarget;

    constexpr std::array<UINT, 3U> expected_delays{
        500U,
        1000U,
        1500U,
    };

    // 初回を含む三回の一時失敗へ、増加する有限delayを順番に割り当てる。
    for (std::size_t retry = 0U;
         retry < expected_delays.size();
         ++retry) {
        const auto decision =
            SelectDynamicResolutionRetry(
                E_UNEXPECTED,
                retry);
        if (!decision.should_retry ||
            decision.delay_milliseconds !=
                expected_delays[retry]) {
            return 1;
        }
    }

    // 上限後は無期限にtimerを再予約せず、利用者へ恒久失敗を通知する。
    const auto exhausted =
        SelectDynamicResolutionRetry(
            E_UNEXPECTED,
            expected_delays.size());
    if (exhausted.should_retry ||
        exhausted.delay_milliseconds != 0U) {
        return 2;
    }

    // interface不在は待機で解決しないため、即座に既存fallbackへ委ねる。
    if (SelectDynamicResolutionRetry(
            E_NOINTERFACE,
            0U).should_retry) {
        return 3;
    }

    // 不正な表示寸法を再送し続けず、入力検証errorとして維持する。
    if (SelectDynamicResolutionRetry(
            E_INVALIDARG,
            0U).should_retry) {
        return 4;
    }

    // 成功値に誤って再試行を割り当てない。
    if (SelectDynamicResolutionRetry(
            S_OK,
            0U).should_retry) {
        return 5;
    }

    // Window表示ではtoolbarを除いたRDP child領域を追従対象にする。
    if (SelectDynamicResolutionTarget(false) !=
        DynamicResolutionTarget::WindowDisplayArea) {
        return 6;
    }

    // ActiveX全画面では元Window寸法を維持せず、monitor全体を追従対象にする。
    if (SelectDynamicResolutionTarget(true) !=
        DynamicResolutionTarget::FullScreenMonitor) {
        return 7;
    }

    return 0;
}
