/**
 * @file session_validation.h
 * @brief Agentを現在の対話RDP sessionだけへ限定する検証契約を定義する。
 *
 * @details Session 0、console session、切断済みsessionではSVCを開かず、
 *          service化や別ユーザーsession探索を行わない。
 */
#pragma once

#include <Windows.h>
#include <WtsApi32.h>

#include <cstdint>

namespace zrdp::agent::app {

/**
 * @brief current process sessionのAgent起動可否分類。
 */
enum class SessionValidationCode {
    Valid,
    ProcessSessionQueryFailed,
    SessionZero,
    ConnectStateQueryFailed,
    SessionNotActive,
    ProtocolQueryFailed,
    SessionNotRdp,
};

/**
 * @brief session検証結果と秘密情報を含まないWin32 error。
 */
struct SessionValidationResult {
    /// 起動可否を表す固定分類。
    SessionValidationCode code{
        SessionValidationCode::ProcessSessionQueryFailed};

    /// Windows API失敗時のGetLastError。policy拒否では0。
    DWORD win32_error{ERROR_SUCCESS};

    /// WTSから取得済みのcurrent session state。未取得時はWTSInit。
    WTS_CONNECTSTATE_CLASS connect_state{WTSInit};

    /// WTSClientProtocolType値。未取得時は0。
    std::uint16_t protocol_type{0U};
};

/**
 * @brief 取得済みsession属性がAgent実行条件を満たすか判定する。
 * @param[in] session_id current processのSession ID。
 * @param[in] connect_state WTSが返した接続状態。
 * @param[in] protocol_type WTSClientProtocolType値。RDPは2。
 * @return policy判定結果。Windows API呼出しは行わない。
 */
[[nodiscard]] SessionValidationResult ValidateSessionAttributes(
    DWORD session_id,
    WTS_CONNECTSTATE_CLASS connect_state,
    std::uint16_t protocol_type) noexcept;

/**
 * @brief current processのSession ID、active状態、RDP protocolを検証する。
 * @return AgentがSVCを開ける条件または分類済み失敗。
 *
 * current session以外を列挙・探索せず、実ユーザー名やclient addressを取得しない。
 */
[[nodiscard]] SessionValidationResult ValidateCurrentSession() noexcept;

}  // namespace zrdp::agent::app
