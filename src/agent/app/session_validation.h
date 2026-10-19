//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file session_validation.h
 * @brief Agentを現在の対話RDP sessionだけへ限定する検証契約を定義する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details Session 0、console session、切断済みsessionではSVCを開かず、
 *          service化や別ユーザーsession探索を行わない。
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
#include <WtsApi32.h>

#include <cstdint>

namespace zrdp::agent::app {

/**
 * @brief current process sessionのAgent起動可否分類。
 */
enum class SessionValidationCode {
	/// 現在processがactiveなRDPユーザーsessionに所属する。
	Valid,
	/// 現在processのSession IDを取得できず、channelを開かない。
	ProcessSessionQueryFailed,
	/// 対話RDP sessionではないSession 0のため起動を拒否する。
	SessionZero,
	/// WTS接続状態を取得できず、activeと推測しない。
	ConnectStateQueryFailed,
	/// RDPユーザーsessionがactiveではなく、channelを開かない。
	SessionNotActive,
	/// WTS接続方式を取得できず、RDPと推測しない。
	ProtocolQueryFailed,
	/// 現在sessionがRDP接続ではないため使用を拒否する。
	SessionNotRdp,
} ;

/**
 * @brief session検証結果と秘密情報を含まないWin32 error。
 */
struct SessionValidationResult {
	/// 起動可否を表す固定分類。
	SessionValidationCode  code{SessionValidationCode::ProcessSessionQueryFailed} ;

	/// Windows API失敗時のGetLastError。policy拒否では0。
	DWORD				   win32_error{ERROR_SUCCESS} ;

	/// WTSから取得済みのcurrent session state。未取得時はWTSInit。
	WTS_CONNECTSTATE_CLASS connect_state{WTSInit} ;

	/// WTSClientProtocolType値。未取得時は0。
	std::uint16_t		   protocol_type{0U} ;
} ;

/**
 * @brief 取得済みsession属性がAgent実行条件を満たすか判定する。
 * @param[in] session_id current processのSession ID。
 * @param[in] connect_state WTSが返した接続状態。
 * @param[in] protocol_type WTSClientProtocolType値。RDPは2。
 * @return policy判定結果。Windows API呼出しは行わない。
 */
[[nodiscard]] SessionValidationResult
ValidateSessionAttributes( DWORD session_id, WTS_CONNECTSTATE_CLASS connect_state,
						  std::uint16_t protocol_type) noexcept ;

/**
 * @brief current processのSession ID、active状態、RDP protocolを検証する。
 * @return AgentがSVCを開ける条件または分類済み失敗。
 *
 * current session以外を列挙・探索せず、実ユーザー名やclient addressを取得しない。
 */
[[nodiscard]] SessionValidationResult ValidateCurrentSession() noexcept ;

} // namespace zrdp::agent::app
