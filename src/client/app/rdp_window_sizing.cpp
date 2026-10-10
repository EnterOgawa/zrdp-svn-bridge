//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file rdp_window_sizing.cpp
 * @brief RDP表示領域のアスペクト比を維持するWin32 sizing計算を実装する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/app/rdp_window_sizing.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace zrdp::client::app {
namespace {

/**
 * @brief 一つのアスペクト比候補をtop-level Window寸法で保持する。
 */
struct WindowSizeCandidate final {
	/// 候補Windowの幅（pixel）。アスペクト補正の比較に使用する。
	std::int64_t width{0} ;
	/// 候補Windowの高さ（pixel）。アスペクト補正の比較に使用する。
	std::int64_t height{0} ;
} ;

/**
 * @brief 正の整数を乗算後、最も近い整数へ除算する。
 * @param[in] value 比率を適用する正のpixel数。
 * @param[in] numerator 正の比率分子。
 * @param[in] denominator 正の比率分母。
 * @param[out] result 計算成功時の四捨五入後pixel数。trueの場合だけ使用する。
 * @return overflowせず計算できた場合はtrue。
 */
[[nodiscard]] bool TryScaleRounded( const std::int64_t value, const std::int64_t numerator,
								   const std::int64_t denominator, std::int64_t &result) noexcept
{
	// 不正な比率を除算へ渡すと、Window矩形を予測不能な値へ変更する
	if( value <= 0 || numerator <= 0 || denominator <= 0) {
		return false ;
	}

	const std::int64_t rounding = denominator / 2 ;
	// 画面座標由来の値でも乗算overflowを許さず、元の矩形を維持する
	if( value > (std::numeric_limits<std::int64_t>::max() - rounding) / numerator) {
		return false ;
	}

	result = (value * numerator + rounding) / denominator ;
	return result > 0 ;
}

/**
 * @brief 正の整数を乗算後、切り上げ除算する。
 * @param[in] value 最小寸法として比率を適用する正のpixel数。
 * @param[in] numerator 正の比率分子。
 * @param[in] denominator 正の比率分母。
 * @param[out] result 計算成功時の切上げ後pixel数。trueの場合だけ使用する。
 * @return overflowせず計算できた場合はtrue。
 */
[[nodiscard]] bool TryScaleCeiling( const std::int64_t value, const std::int64_t numerator,
								   const std::int64_t denominator, std::int64_t &result) noexcept
{
	// 不正な比率を最小寸法計算へ使用しない
	if( value <= 0 || numerator <= 0 || denominator <= 0) {
		return false ;
	}

	const std::int64_t rounding = denominator - 1 ;
	// 最小値補正でも乗算overflowをfail-closedにする
	if( value > (std::numeric_limits<std::int64_t>::max() - rounding) / numerator) {
		return false ;
	}

	result = (value * numerator + rounding) / denominator ;
	return result > 0 ;
}

/**
 * @brief 幅を利用者操作の基準として比率固定候補を作る。
 * @param[in] proposed_width USER32候補のtop-level Window幅（pixel）。
 * @param[in] constraints desktop比率、非RDP領域、最小Window寸法。
 * @param[out] candidate 比率と最小寸法を満たす候補。trueの場合だけ使用する。
 * @return overflowせずRECTの表現範囲内で候補を作れた場合はtrue。
 */
[[nodiscard]] bool TryCreateWidthBasedCandidate( const std::int64_t				  proposed_width,
												const RdpWindowSizingConstraints &constraints,
												WindowSizeCandidate &candidate) noexcept
{
	const std::int64_t non_rdp_width	 = constraints.non_rdp_width ;
	const std::int64_t non_rdp_height	 = constraints.non_rdp_height ;
	const std::int64_t minimum_rdp_width = std::max<std::int64_t>(
		1, static_cast<std::int64_t>( constraints.minimum_window_width) - non_rdp_width) ;
	const std::int64_t minimum_rdp_height = std::max<std::int64_t>(
		1, static_cast<std::int64_t>( constraints.minimum_window_height) - non_rdp_height) ;

	std::int64_t minimum_width_for_height = 0 ;
	// 最小高さを満たす幅を安全に導出できなければ候補を公開しない
	if( !TryScaleCeiling( minimum_rdp_height, constraints.desktop_width, constraints.desktop_height,
						minimum_width_for_height)) {
		return false ;
	}

	const std::int64_t proposed_rdp_width =
		std::max<std::int64_t>( 1, proposed_width - non_rdp_width) ;
	const std::int64_t rdp_width  = std::max( {
		 proposed_rdp_width,
		 minimum_rdp_width,
		 minimum_width_for_height,
	 }) ;
	std::int64_t	   rdp_height = 0 ;
	// desktop比率の乗除算に失敗した候補でWindowを変更しない
	if( !TryScaleRounded( rdp_width, constraints.desktop_height, constraints.desktop_width,
						rdp_height)) {
		return false ;
	}

	candidate.width	 = rdp_width + non_rdp_width ;
	candidate.height = rdp_height + non_rdp_height ;
	// RECTへ格納できない寸法は補正せず、USER32の既定処理へ戻す
	return candidate.width > 0 && candidate.height > 0 &&
		   candidate.width <= std::numeric_limits<LONG>::max() &&
		   candidate.height <= std::numeric_limits<LONG>::max() ;
}

/**
 * @brief 高さを利用者操作の基準として比率固定候補を作る。
 * @param[in] proposed_height USER32候補のtop-level Window高さ（pixel）。
 * @param[in] constraints desktop比率、非RDP領域、最小Window寸法。
 * @param[out] candidate 比率と最小寸法を満たす候補。trueの場合だけ使用する。
 * @return overflowせずRECTの表現範囲内で候補を作れた場合はtrue。
 */
[[nodiscard]] bool TryCreateHeightBasedCandidate( const std::int64_t				   proposed_height,
												 const RdpWindowSizingConstraints &constraints,
												 WindowSizeCandidate &candidate) noexcept
{
	const std::int64_t non_rdp_width	 = constraints.non_rdp_width ;
	const std::int64_t non_rdp_height	 = constraints.non_rdp_height ;
	const std::int64_t minimum_rdp_width = std::max<std::int64_t>(
		1, static_cast<std::int64_t>( constraints.minimum_window_width) - non_rdp_width) ;
	const std::int64_t minimum_rdp_height = std::max<std::int64_t>(
		1, static_cast<std::int64_t>( constraints.minimum_window_height) - non_rdp_height) ;

	std::int64_t minimum_height_for_width = 0 ;
	// 最小幅を満たす高さを安全に導出できなければ候補を公開しない
	if( !TryScaleCeiling( minimum_rdp_width, constraints.desktop_height, constraints.desktop_width,
						minimum_height_for_width)) {
		return false ;
	}

	const std::int64_t proposed_rdp_height =
		std::max<std::int64_t>( 1, proposed_height - non_rdp_height) ;
	const std::int64_t rdp_height = std::max( {
		proposed_rdp_height,
		minimum_rdp_height,
		minimum_height_for_width,
	}) ;
	std::int64_t	   rdp_width  = 0 ;
	// desktop比率の乗除算に失敗した候補でWindowを変更しない
	if( !TryScaleRounded( rdp_height, constraints.desktop_width, constraints.desktop_height,
						rdp_width)) {
		return false ;
	}

	candidate.width	 = rdp_width + non_rdp_width ;
	candidate.height = rdp_height + non_rdp_height ;
	// RECTへ格納できない寸法は補正せず、USER32の既定処理へ戻す
	return candidate.width > 0 && candidate.height > 0 &&
		   candidate.width <= std::numeric_limits<LONG>::max() &&
		   candidate.height <= std::numeric_limits<LONG>::max() ;
}

/**
 * @brief 二寸法の符号なしpixel差を返す。
 * @param[in] left 比較する第一のWindow寸法（pixel）。
 * @param[in] right 比較する第二のWindow寸法（pixel）。
 * @return 二寸法の非負な差。入力はWindow座標由来の表現可能範囲とする。
 */
[[nodiscard]] std::int64_t AbsoluteDifference( const std::int64_t left,
											  const std::int64_t right) noexcept
{
	return left >= right ? left - right : right - left ;
}

/**
 * @brief 計算済み寸法を操作辺と反対側へanchorしてRECTへ格納する。
 * @param[in] sizing_edge USER32のWMSZ_*手動操作辺。
 * @param[in] candidate 検証済みtop-level Window寸法（pixel）。
 * @param[in,out] bounds 入力時の候補矩形。座標範囲検証後だけ補正結果へ置換する。
 * @return 補正後の全座標をLONGへ安全に格納できる場合はtrue。
 */
[[nodiscard]] bool TryApplyCandidate( const UINT sizing_edge, const WindowSizeCandidate &candidate,
									 RECT &bounds) noexcept
{
	const bool moves_left =
		sizing_edge == WMSZ_LEFT || sizing_edge == WMSZ_TOPLEFT || sizing_edge == WMSZ_BOTTOMLEFT ;
	const bool moves_top =
		sizing_edge == WMSZ_TOP || sizing_edge == WMSZ_TOPLEFT || sizing_edge == WMSZ_TOPRIGHT ;

	std::int64_t left	= bounds.left ;
	std::int64_t top	= bounds.top ;
	std::int64_t right	= bounds.right ;
	std::int64_t bottom = bounds.bottom ;
	// 左または上を動かす場合は、利用者が動かしていない反対辺を固定する
	if( moves_left) {
		left = right - candidate.width ;
	} else {
		right = left + candidate.width ;
	}

	// 上辺をdragした場合は下辺を固定して補正後の高さを適用する
	if( moves_top) {
		top = bottom - candidate.height ;
	} else {
		bottom = top + candidate.height ;
	}

	constexpr std::int64_t kMinimumLong = std::numeric_limits<LONG>::min() ;
	constexpr std::int64_t kMaximumLong = std::numeric_limits<LONG>::max() ;
	// 画面座標の範囲を超える補正はRECTへ切り詰めず、元候補を維持する
	if( left < kMinimumLong || left > kMaximumLong || top < kMinimumLong || top > kMaximumLong ||
	   right < kMinimumLong || right > kMaximumLong || bottom < kMinimumLong ||
	   bottom > kMaximumLong) {
		return false ;
	}

	bounds.left	  = static_cast<LONG>( left) ;
	bounds.top	  = static_cast<LONG>( top) ;
	bounds.right  = static_cast<LONG>( right) ;
	bounds.bottom = static_cast<LONG>( bottom) ;
	return true ;
}

} // namespace

bool ConstrainRdpWindowSizingRect( const UINT						sizing_edge,
								  const RdpWindowSizingConstraints &constraints,
								  RECT							   &bounds) noexcept
{
	// 接続時desktop比率、固定領域、最小寸法が不正なら矩形を変更しない
	if( constraints.desktop_width <= 0L || constraints.desktop_height <= 0L ||
	   constraints.non_rdp_width < 0 || constraints.non_rdp_height < 0 ||
	   constraints.minimum_window_width <= 0 || constraints.minimum_window_height <= 0 ||
	   constraints.reference_window_width <= 0 || constraints.reference_window_height <= 0) {
		return false ;
	}

	const bool horizontal_edge = sizing_edge == WMSZ_LEFT || sizing_edge == WMSZ_RIGHT ;
	const bool vertical_edge   = sizing_edge == WMSZ_TOP || sizing_edge == WMSZ_BOTTOM ;
	const bool corner		   = sizing_edge == WMSZ_TOPLEFT || sizing_edge == WMSZ_TOPRIGHT ||
						sizing_edge == WMSZ_BOTTOMLEFT || sizing_edge == WMSZ_BOTTOMRIGHT ;
	// WM_SIZING以外の値を辺のanchorとして近似しない
	if( !horizontal_edge && !vertical_edge && !corner) {
		return false ;
	}

	const std::int64_t proposed_width =
		static_cast<std::int64_t>( bounds.right) - static_cast<std::int64_t>( bounds.left) ;
	const std::int64_t proposed_height =
		static_cast<std::int64_t>( bounds.bottom) - static_cast<std::int64_t>( bounds.top) ;
	// 反転または空の矩形から有効なWindow位置を推測しない
	if( proposed_width <= 0 || proposed_height <= 0) {
		return false ;
	}

	WindowSizeCandidate width_candidate{} ;
	WindowSizeCandidate height_candidate{} ;
	// 両軸候補を検証できない場合は、一方だけの近似値を適用しない
	if( !TryCreateWidthBasedCandidate( proposed_width, constraints, width_candidate) ||
	   !TryCreateHeightBasedCandidate( proposed_height, constraints, height_candidate)) {
		return false ;
	}

	const WindowSizeCandidate *selected = nullptr ;
	// 単辺操作は利用者が直接動かした軸を寸法決定の正本にする
	if( horizontal_edge) {
		selected = &width_candidate ;
	}
	// 上下辺のdragでは高さを基準としてRDP領域の比率を補正する
	else if( vertical_edge) {
		selected = &height_candidate ;
	} else {
		const std::int64_t width_change =
			AbsoluteDifference( proposed_width, constraints.reference_window_width) ;
		const std::int64_t height_change =
			AbsoluteDifference( proposed_height, constraints.reference_window_height) ;
		const long double normalized_width_change =
			static_cast<long double>( width_change) /
			static_cast<long double>( constraints.desktop_width) ;
		const long double normalized_height_change =
			static_cast<long double>( height_change) /
			static_cast<long double>( constraints.desktop_height) ;
		// 角操作では変化率が大きい軸を優先し、cursorの主移動方向へ追従する
		if( normalized_width_change > normalized_height_change) {
			selected = &width_candidate ;
		}
		// 角dragで相対的な高さ変更が大きい場合は高さ基準の候補を選ぶ
		else if( normalized_height_change > normalized_width_change) {
			selected = &height_candidate ;
		} else {
			const std::int64_t width_candidate_difference =
				AbsoluteDifference( width_candidate.width, proposed_width) +
				AbsoluteDifference( width_candidate.height, proposed_height) ;
			const std::int64_t height_candidate_difference =
				AbsoluteDifference( height_candidate.width, proposed_width) +
				AbsoluteDifference( height_candidate.height, proposed_height) ;
			selected = width_candidate_difference <= height_candidate_difference
						   ? &width_candidate
						   : &height_candidate ;
		}
	}

	return TryApplyCandidate( sizing_edge, *selected, bounds) ;
}

} // namespace zrdp::client::app
