/**
 * @file rdp_window_sizing_test.cpp
 * @brief session Windowの手動アスペクト比制約を座標と最小寸法で検証する。
 */

#include "client/app/rdp_window_sizing.h"

#include <Windows.h>

#include <limits>

namespace {

/**
 * @brief テスト共通の16:9 desktopと非RDP領域を返す。
 */
[[nodiscard]] zrdp::client::app::RdpWindowSizingConstraints
CreateConstraints() noexcept {
    return zrdp::client::app::RdpWindowSizingConstraints{
        .desktop_width = 1920L,
        .desktop_height = 1080L,
        .non_rdp_width = 16,
        .non_rdp_height = 70,
        .minimum_window_width = 300,
        .minimum_window_height = 200,
        .reference_window_width = 1016,
        .reference_window_height = 633,
    };
}

/**
 * @brief RECTの寸法が期待値と一致するか判定する。
 */
[[nodiscard]] bool HasSize(
    const RECT& bounds,
    const LONG width,
    const LONG height) noexcept {
    return bounds.right - bounds.left == width &&
        bounds.bottom - bounds.top == height;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief RDP表示領域のアスペクト比制約と例外配置を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::client::app::ConstrainRdpWindowSizingRect;

    const auto constraints = CreateConstraints();

    RECT right_edge{100L, 200L, 1116L, 900L};
    // 幅1000のRDP領域から高さ563を算出し、上／左を固定する。
    if (!ConstrainRdpWindowSizingRect(
            WMSZ_RIGHT,
            constraints,
            right_edge) ||
        !HasSize(right_edge, 1016L, 633L) ||
        right_edge.left != 100L ||
        right_edge.top != 200L) {
        return 1;
    }

    RECT top_edge{100L, 100L, 1300L, 1070L};
    // 高さ900のRDP領域から幅1600を算出し、下／左を固定する。
    if (!ConstrainRdpWindowSizingRect(
            WMSZ_TOP,
            constraints,
            top_edge) ||
        !HasSize(top_edge, 1616L, 970L) ||
        top_edge.left != 100L ||
        top_edge.bottom != 1070L) {
        return 2;
    }

    RECT top_left{84L, 137L, 1100L, 770L};
    // 左上操作では既存の右下をanchorしたまま両辺を補正する。
    if (!ConstrainRdpWindowSizingRect(
            WMSZ_TOPLEFT,
            constraints,
            top_left) ||
        !HasSize(top_left, 1016L, 633L) ||
        top_left.right != 1100L ||
        top_left.bottom != 770L) {
        return 3;
    }

    RECT left_edge{84L, 200L, 1100L, 900L};
    // 左辺操作では右辺を固定し、補正高さを上辺から下方向へ適用する。
    if (!ConstrainRdpWindowSizingRect(
            WMSZ_LEFT,
            constraints,
            left_edge) ||
        !HasSize(left_edge, 1016L, 633L) ||
        left_edge.right != 1100L ||
        left_edge.top != 200L) {
        return 8;
    }

    RECT bottom_edge{100L, 100L, 1300L, 1070L};
    // 下辺操作では上辺を固定し、高さから導出した幅を右方向へ適用する。
    if (!ConstrainRdpWindowSizingRect(
            WMSZ_BOTTOM,
            constraints,
            bottom_edge) ||
        !HasSize(bottom_edge, 1616L, 970L) ||
        bottom_edge.left != 100L ||
        bottom_edge.top != 100L) {
        return 9;
    }

    RECT top_right{100L, 137L, 1116L, 770L};
    // 右上操作では左下をanchorする。
    if (!ConstrainRdpWindowSizingRect(
            WMSZ_TOPRIGHT,
            constraints,
            top_right) ||
        !HasSize(top_right, 1016L, 633L) ||
        top_right.left != 100L ||
        top_right.bottom != 770L) {
        return 10;
    }

    RECT bottom_left{84L, 100L, 1100L, 733L};
    // 左下操作では右上をanchorする。
    if (!ConstrainRdpWindowSizingRect(
            WMSZ_BOTTOMLEFT,
            constraints,
            bottom_left) ||
        !HasSize(bottom_left, 1016L, 633L) ||
        bottom_left.right != 1100L ||
        bottom_left.top != 100L) {
        return 11;
    }

    RECT height_dominant_corner{100L, 100L, 1000L, 1070L};
    // 角操作では高さ基準の方が変更候補に近い場合、その候補を選ぶ。
    if (!ConstrainRdpWindowSizingRect(
            WMSZ_BOTTOMRIGHT,
            constraints,
            height_dominant_corner) ||
        !HasSize(height_dominant_corner, 1616L, 970L)) {
        return 4;
    }

    auto wide_minimum = constraints;
    wide_minimum.minimum_window_width = 900;
    wide_minimum.minimum_window_height = 520;
    RECT undersized{100L, 100L, 350L, 250L};
    // 最小Window寸法を満たすため、比率を崩さず両軸を拡大する。
    if (!ConstrainRdpWindowSizingRect(
            WMSZ_BOTTOMRIGHT,
            wide_minimum,
            undersized) ||
        undersized.right - undersized.left < 900L ||
        undersized.bottom - undersized.top < 520L) {
        return 5;
    }

    auto invalid_constraints = constraints;
    invalid_constraints.desktop_height = 0L;
    const RECT original{10L, 20L, 1010L, 720L};
    RECT invalid_ratio = original;
    // 不正なremote比率では、利用者の変更候補を一切書き換えない。
    if (ConstrainRdpWindowSizingRect(
            WMSZ_RIGHT,
            invalid_constraints,
            invalid_ratio) ||
        invalid_ratio.left != original.left ||
        invalid_ratio.top != original.top ||
        invalid_ratio.right != original.right ||
        invalid_ratio.bottom != original.bottom) {
        return 6;
    }

    RECT invalid_edge = original;
    // WM_SIZING以外の識別子を任意の辺へ近似しない。
    if (ConstrainRdpWindowSizingRect(
            0U,
            constraints,
            invalid_edge)) {
        return 7;
    }

    RECT reversed{100L, 100L, 50L, 200L};
    // 反転RECTから正規のWindow位置を推測しない。
    if (ConstrainRdpWindowSizingRect(
            WMSZ_RIGHT,
            constraints,
            reversed)) {
        return 12;
    }

    const RECT overflow_original{
        std::numeric_limits<LONG>::max() - 10L,
        100L,
        std::numeric_limits<LONG>::max(),
        200L,
    };
    RECT overflow = overflow_original;
    // RECT座標上限を超える最小寸法補正を切り詰めて適用しない。
    if (ConstrainRdpWindowSizingRect(
            WMSZ_RIGHT,
            constraints,
            overflow) ||
        overflow.left != overflow_original.left ||
        overflow.top != overflow_original.top ||
        overflow.right != overflow_original.right ||
        overflow.bottom != overflow_original.bottom) {
        return 13;
    }

    return 0;
}
