//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file dynamic_resolution_retry.h
 * @brief RDPログイン直後の動的解像度更新を有限回だけ再試行する方針を定義する。
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
#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>

namespace zrdp::client::app {

/**
 * @brief 一回の動的解像度失敗に対する次回試行を表す。
 */
struct DynamicResolutionRetryDecision {
	/// 一時失敗として次回試行を予約する場合はtrue。
	bool should_retry{false} ;

	/// 次回試行までの有限待機時間。
	UINT delay_milliseconds{0U} ;
} ;

/**
 * @brief HRESULTと予約済み回数から、限定的な再試行だけを選択する。
 * @param[in] error Microsoft RDP ActiveXが返したHRESULT。
 * @param[in] retries_already_scheduled 現在要求に対して既に予約した再試行数。
 * @return 次回試行の要否と待機時間。
 *
 * `E_UNEXPECTED`は、`OnLoginComplete`直後でも接続先の表示更新準備が完了して
 * いない場合に一時的に返る。interface不在、不正寸法、thread違反等は再試行で
 * 解決しないため対象にせず、従来どおりfail-closedで表示modeを縮退させる。
 */
[[nodiscard]] constexpr DynamicResolutionRetryDecision
SelectDynamicResolutionRetry( const HRESULT	   error,
							 const std::size_t retries_already_scheduled) noexcept
{
	constexpr std::array<UINT, 3U> kRetryDelaysMilliseconds{
		500U,
		1000U,
		1500U,
	} ;

	// 観測済みの一時的HRESULTだけを再試行し、恒久的な設定errorを隠さない
	if( error != E_UNEXPECTED || retries_already_scheduled >= kRetryDelaysMilliseconds.size()) {
		return {} ;
	}

	return {
		true,
		kRetryDelaysMilliseconds[retries_already_scheduled],
	} ;
}

} // namespace zrdp::client::app
