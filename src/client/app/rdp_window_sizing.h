/**
 * @file rdp_window_sizing.h
 * @brief 手動変更中のsession WindowをRDP desktopのアスペクト比へ制約する。
 */
#pragma once

#include <Windows.h>

namespace zrdp::client::app {

/**
 * @brief RDP表示領域の比率をtop-level Window矩形へ変換するための固定値。
 *
 * `non_rdp_width`と`non_rdp_height`には、非client領域と表示mode欄等、
 * RDP ActiveXが占有しないpixel数を含める。
 */
struct RdpWindowSizingConstraints final {
    /// 接続開始時にActiveXへ要求したremote desktop幅。
    long desktop_width{0L};

    /// 接続開始時にActiveXへ要求したremote desktop高さ。
    long desktop_height{0L};

    /// top-level Window幅のうちRDP表示領域に含まれないpixel数。
    int non_rdp_width{0};

    /// top-level Window高さのうちRDP表示領域に含まれないpixel数。
    int non_rdp_height{0};

    /// DPI適用済みの最小top-level Window幅。
    int minimum_window_width{1};

    /// DPI適用済みの最小top-level Window高さ。
    int minimum_window_height{1};

    /// 一回の手動変更loop開始時に配置されていたtop-level Window幅。
    int reference_window_width{1};

    /// 一回の手動変更loop開始時に配置されていたtop-level Window高さ。
    int reference_window_height{1};
};

/**
 * @brief 手動変更候補のRDP表示領域を接続時desktopの比率へ補正する。
 *
 * @param[in] sizing_edge `WM_SIZING`の`WMSZ_*`値。
 * @param[in] constraints desktop比率、非RDP領域、最小Window寸法。
 * @param[in,out] bounds 画面座標の変更候補。成功時だけ補正値へ置換する。
 * @return 有効な手動変更を補正できた場合はtrue。不正値ではfalse。
 *
 * 左／上側を操作した場合は反対側を固定する。角操作では現在寸法からの変化率が
 * 大きい方を利用者の主操作軸として扱い、縦方向だけの操作も追従させる。
 */
[[nodiscard]] bool ConstrainRdpWindowSizingRect(
    UINT sizing_edge,
    const RdpWindowSizingConstraints& constraints,
    RECT& bounds) noexcept;

}  // namespace zrdp::client::app
