/**
 * @file rdp_container_full_screen.h
 * @brief Container-handled RDP全画面で使用するWindow styleと表示領域policyを定義する。
 */
#pragma once

#include <Windows.h>

namespace zrdp::client::app {

/**
 * @brief 通常のtop-level Window styleからborderless全画面styleを構築する。
 * @param[in] window_style Window modeで使用しているstyle。
 * @return captionとsize frameを除去し、既存の表示／clip属性を維持したstyle。
 *
 * @note 呼出し元はtop-level Windowだけを渡す。元styleはWindow復帰用に別途保存する。
 */
[[nodiscard]] constexpr LONG_PTR MakeContainerFullScreenStyle(
    const LONG_PTR window_style) noexcept {
    constexpr LONG_PTR kOverlappedWindowStyle =
        static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW);
    constexpr LONG_PTR kPopupWindowStyle =
        static_cast<LONG_PTR>(WS_POPUP);
    return (window_style & ~kOverlappedWindowStyle) |
        kPopupWindowStyle;
}

/**
 * @brief connection barを覆わずcontainerだけをmonitorへ再配置するflagを返す。
 * @return frame変更を反映し、現在のtop-level Z-orderを維持する`SetWindowPos` flag。
 */
[[nodiscard]] constexpr UINT ContainerFullScreenPositionFlags() noexcept {
    return SWP_FRAMECHANGED | SWP_NOZORDER;
}

/**
 * @brief RDP ActiveX表示領域のclient座標上端を表示状態から選択する。
 * @param[in] container_full_screen container Windowが全画面中の場合はtrue。
 * @param[in] toolbar_height Window modeで表示する設定行のDPI適用済み高さ。
 * @return 全画面では0、Window modeでは0以上の設定行高さ。
 */
[[nodiscard]] constexpr int SelectRdpContentTop(
    const bool container_full_screen,
    const int toolbar_height) noexcept {
    return container_full_screen
        ? 0
        : toolbar_height > 0
            ? toolbar_height
            : 0;
}

}  // namespace zrdp::client::app
