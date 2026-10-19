//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file session_validation.cpp
 * @brief current processがactive RDP user session内かWTS APIで検証する。
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

#include "agent/app/session_validation.h"

#include <cstddef>

namespace zrdp::agent::app {
namespace {

/**
 * @brief WTSQuerySessionInformationが確保したbufferを一意所有する。
 */
class WtsMemory final {
public:
	/**
	 * @brief null所有状態のWTS memory wrapperを構築する。
	 */
	WtsMemory() = default ;

	/**
	 * @brief WTS出力pointerの受取先を返す。
	 * @return 未所有状態のLPWSTR二重pointer。
	 */
	[[nodiscard]] LPWSTR *Put() noexcept
	{
		Reset() ;
		return &value_ ;
	}

	/**
	 * @brief 所有WTS memoryを解放する。
	 */
	~WtsMemory()
	{
		Reset() ;
	}

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	WtsMemory( const WtsMemory &)						   = delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	WtsMemory				 &operator=( const WtsMemory &) = delete ;

	/**
	 * @brief raw byte列の先頭pointerを取得する。
	 * @return WTS所有buffer。未取得時はnullptr。
	 */
	[[nodiscard]] const void *data() const noexcept
	{
		return static_cast<const void *>( value_) ;
	}

private:
	/**
	 * @brief 現在所有するWTS allocationを解放する。
	 */
	void Reset() noexcept
	{
		// APIがmemoryを返した場合だけWTSFreeMemoryへ渡す
		if( value_ != nullptr) {
			WTSFreeMemory( value_) ;
			value_ = nullptr ;
		}
	}

	/// WTSFreeMemoryで解放する所有pointer。
	LPWSTR value_{nullptr} ;
} ;

} // namespace

SessionValidationResult ValidateSessionAttributes( const DWORD				   session_id,
												  const WTS_CONNECTSTATE_CLASS connect_state,
												  const std::uint16_t protocol_type) noexcept
{
	// serviceのSession 0では対話RDP SVCを開かない
	if( session_id == 0U) {
		return {
			SessionValidationCode::SessionZero,
			ERROR_SUCCESS,
			connect_state,
			protocol_type,
		} ;
	}

	// disconnected、idle等のsessionへAgentを常駐させない
	if( connect_state != WTSActive) {
		return {
			SessionValidationCode::SessionNotActive,
			ERROR_SUCCESS,
			connect_state,
			protocol_type,
		} ;
	}

	// 0のconsoleやlegacy protocolをRDP sessionとして近似しない
	if( protocol_type != 2U) {
		return {
			SessionValidationCode::SessionNotRdp,
			ERROR_SUCCESS,
			connect_state,
			protocol_type,
		} ;
	}

	return {
		SessionValidationCode::Valid,
		ERROR_SUCCESS,
		connect_state,
		protocol_type,
	} ;
}

SessionValidationResult ValidateCurrentSession() noexcept
{
	DWORD session_id = 0U ;
	// current process以外のuser sessionを探索せず、自身のSession IDだけ取得する
	if( !ProcessIdToSessionId( GetCurrentProcessId(), &session_id)) {
		return {
			SessionValidationCode::ProcessSessionQueryFailed,
			GetLastError(),
			WTSInit,
			0U,
		} ;
	}

	WtsMemory state_memory ;
	DWORD	  state_bytes = 0U ;
	// current local sessionの接続stateだけを問い合わせる
	if( !WTSQuerySessionInformationW( WTS_CURRENT_SERVER_HANDLE, session_id, WTSConnectState,
									state_memory.Put(), &state_bytes)) {
		return {
			SessionValidationCode::ConnectStateQueryFailed,
			GetLastError(),
			WTSInit,
			0U,
		} ;
	}

	// WTS出力を型として読む前に必要byte数を完全検証する
	if( state_memory.data() == nullptr || state_bytes < sizeof( WTS_CONNECTSTATE_CLASS)) {
		return {
			SessionValidationCode::ConnectStateQueryFailed,
			ERROR_INVALID_DATA,
			WTSInit,
			0U,
		} ;
	}

	const auto connect_state = *static_cast<const WTS_CONNECTSTATE_CLASS *>( state_memory.data()) ;

	WtsMemory  protocol_memory ;
	DWORD	   protocol_bytes = 0U ;
	// client名やaddressを取得せず、RDPかを示すprotocol typeだけ問い合わせる
	if( !WTSQuerySessionInformationW( WTS_CURRENT_SERVER_HANDLE, session_id, WTSClientProtocolType,
									protocol_memory.Put(), &protocol_bytes)) {
		return {
			SessionValidationCode::ProtocolQueryFailed,
			GetLastError(),
			connect_state,
			0U,
		} ;
	}

	// USHORT値を読む前にWTS allocationとbyte数を検証する
	if( protocol_memory.data() == nullptr || protocol_bytes < sizeof( USHORT)) {
		return {
			SessionValidationCode::ProtocolQueryFailed,
			ERROR_INVALID_DATA,
			connect_state,
			0U,
		} ;
	}

	const auto protocol_type = *static_cast<const USHORT *>( protocol_memory.data()) ;
	return ValidateSessionAttributes( session_id, connect_state, protocol_type) ;
}

} // namespace zrdp::agent::app
