//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file rdp_input_focus.h
 * @brief 固定入力時に他アプリのforegroundを奪わないfocus条件を定義する。
 * @version 01.00.00
 * @date 2026/10/09
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/09  foregroundと入力focusの前後確認を追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once
namespace zrdp::client::rdp {
/** @brief 自process内の可視sessionにだけfocus取得を許可する。
 * @param[in] foreground_in_process 前景WindowがこのClient processに属する。
 * @param[in] target_ready 所有STAの可視・有効なsessionとcontrolである。
 * @param[in] target_minimized 対象sessionが最小化している。
 * @return 明示操作の対象を標準APIでactivateしてよい場合だけtrue。
 */
[[nodiscard]] constexpr bool CanAcquireDeploymentInputFocus( bool foreground_in_process,
															bool target_ready,
															bool target_minimized) noexcept
{
	return foreground_in_process && target_ready && !target_minimized ;
}

/** @brief focus移動後に対象sessionとcontrolへ入力が戻ったことを確認する。
 * @param[in] foreground_target 前景Windowが所有sessionそのものである。
 * @param[in] active_target 所有STAのactive Windowが同じsessionである。
 * @param[in] control_focused focusがActiveX hostまたはその子にある。
 * @return 三条件が全て成立した場合だけtrue。
 */
[[nodiscard]] constexpr bool HasDeploymentInputFocus( bool foreground_target, bool active_target,
													 bool control_focused) noexcept
{
	return foreground_target && active_target && control_focused ;
}
} // namespace zrdp::client::rdp
