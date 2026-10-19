//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file agent_configuration_policy.h
 * @brief 保存済みINIと稼働allowlistの両方から限定設定ACKを判定する。
 * @version 01.00.00
 * @date 2026/10/10
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/10  稼働設定とINIの不一致を再起動で解消する。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once
#include <cstdint>

namespace zrdp::agent::app {

/** @brief disk一致だけを稼働設定の確認済み応答として扱わない。
 * @param[in] updated backup付きINI更新が成功した場合true。
 * @param[in] disk_matches 現在のINIが完全一致した場合true。
 * @param[in] active_matches 検証したINIが稼働allowlistと完全一致する場合true。
 * @param[in] relay_enabled Winsockとallowlist relayが有効な場合true。
 * @return 1は利用可、2は固定自身の再起動、3は拒否。
 */
[[nodiscard]] constexpr std::uint8_t SelectAgentConfigurationStatus( bool updated, bool disk_matches,
																	bool active_matches,
																	bool relay_enabled) noexcept
{
	return disk_matches && active_matches && relay_enabled ? 1U
		   : updated || (disk_matches && !active_matches)  ? 2U
														   : 3U ;
}
} // namespace zrdp::agent::app
