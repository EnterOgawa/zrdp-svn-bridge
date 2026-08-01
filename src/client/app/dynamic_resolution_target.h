/**
 * @file dynamic_resolution_target.h
 * @brief ActiveXのWindow／全画面状態から動的解像度の寸法取得元を選択する。
 */
#pragma once

namespace zrdp::client::app {

/**
 * @brief 成立済みremote desktopへ適用する寸法の取得元。
 */
enum class DynamicResolutionTarget {
    WindowDisplayArea,  ///< セッションWindow内のRDP child client領域。
    FullScreenMonitor,  ///< ActiveX全画面を表示するmonitor全体。
};

/**
 * @brief ActiveXの現在表示状態に対応する寸法取得元を返す。
 * @param[in] full_screen Microsoft ActiveXが全画面中の場合はtrue。
 * @return 全画面ではmonitor、それ以外ではRDP child表示領域。
 *
 * 等倍表示とSmart Sizingは呼出側で除外し、接続先desktop寸法を維持する。
 */
[[nodiscard]] constexpr DynamicResolutionTarget
SelectDynamicResolutionTarget(
    const bool full_screen) noexcept {
    return full_screen
        ? DynamicResolutionTarget::FullScreenMonitor
        : DynamicResolutionTarget::WindowDisplayArea;
}

}  // namespace zrdp::client::app
