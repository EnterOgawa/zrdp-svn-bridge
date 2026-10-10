//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file rdp_keyboard_emulation_test.h
 * @brief 固定Agent入力とR／Win+R比較の数値判定を実APIと分離する。
 * @version 01.00.02
 * @date 2026/10/10
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/10  固定二要素と一回要求の数値境界を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  Win+R四要素と共通の不完全受付判定を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * </pre>
 * @note 実SendInput、COM、Window操作は行わず、注入したbackendだけを一回呼ぶ。
 */
//------------------------------------------------------------------------------
#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>
#include <utility>

namespace zrdp::client::rdp {
/** @brief 公開する任意キー入力とは独立した固定四種類。 */
enum class FixedKeyboardKind : std::uint8_t {
	/// 開発用R単独比較。
	LetterR = 1,
	/// 開発用比較または初回配置の固定Win+R。
	WinR	= 2,
	/// 確認済み配置だけの固定Ctrl+V。
	Paste	= 3,
	/// 固定Agent起動だけのEnter。
	Enter	= 4,
} ;
/** @brief 固定入力が共有する数値分類。既存R識別子と値を維持する。 */
enum class RKeyEmulationReason : std::uint8_t {
	/// 固定列の全要素をWindows入力streamが受け付けた。remote入力成功ではない。
	None			  = 0,
	/// 所有STA以外から要求された。
	WrongThread		  = 1,
	/// 未認証、警告、切断中である。
	Unavailable		  = 2,
	/// 対象ActiveX自身のWindowと前景・focusを確認できない。
	FocusMismatch	  = 3,
	/// 関連するR／V／Enterまたは固定修飾キーが押されている。
	KeysHeld		  = 4,
	/// 入力streamが一件も受け付けなかった。
	InsertionFailure  = 5,
	/// 固定列の一部だけ受け付けた。追加入力で補正しない。
	PartialInsertion  = 6,
	/// 不正な受付数またはbackend例外を検出した。
	UnexpectedFailure = 7,
	/// controllerの確認・準備等で停止し、専用methodへ到達していない。
	NotAttempted	  = 8,
	/// COM照会後にownerの世代・clipboard・取消条件が失効した。
	AuthorizationLost = 9,
} ;
/** @brief 送信直前に所有STAが数値化した必要条件。 */
struct RKeyEmulationObservation {
	/// 当該sessionの所有STAである。
	bool	owner_thread{false} ;
	/// 警告のない認証済みの同じControlである。
	bool	authenticated{false} ;
	/// 前景・active Windowとfocusが対象ActiveX Windowに一致する。
	bool	focused{false} ;
	/// 当該操作に関連するキーと修飾キーが押されていない。
	bool	keys_released{false} ;
	/// ActiveX Window取得で失敗した場合の元HRESULT。
	HRESULT focus_result{S_OK} ;
	/// ownerがCOM照会後にも固定操作を承認している。
	bool	authorized{true} ;
} ;
/** @brief native挿入の元結果。全件成功の場合のerrorは0とする。 */
struct RKeyEmulationInsertion {
	/// SendInputが返した受付event数。
	UINT  inserted{0U} ;
	/// 不完全受付直後のGetLastError。0もそのまま保持する。
	DWORD error{ERROR_SUCCESS} ;
} ;
/** @brief API受付数と実入力の確認を分ける数値結果。 */
struct RKeyEmulationResult {
	/// guardまたは受付判定のHRESULT。固定列の全件だけS_OK。
	HRESULT				result{E_UNEXPECTED} ;
	/// 接続先やkey状態を含まない固定分類。
	RKeyEmulationReason reason{RKeyEmulationReason::NotAttempted} ;
	/// 受付event数。入力しなかった場合は0。
	UINT				inserted{0U} ;
	/// 不完全受付の元Win32 error。
	DWORD				error{ERROR_SUCCESS} ;
} ;

/** @brief Rの押下・解放だけをscan-code指定の固定二要素で生成する。
 * @return VK、時刻、extra情報は0。他のキーやUNICODE文字を含まない。
 */
[[nodiscard]] inline std::array<INPUT, 2U> RKeyEmulationInputs() noexcept
{
	std::array<INPUT, 2U> inputs{} ;
	// 二要素とも同じ物理Rだけを指定し、modifierや文字列を受け付けない
	for( auto &input : inputs) {
		input.type		 = INPUT_KEYBOARD ;
		input.ki.wScan	 = 0x13U ;
		input.ki.dwFlags = KEYEVENTF_SCANCODE ;
	}

	inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP ;
	return inputs ;
}

/** @brief 左Win押下、R押下、R解放、左Win解放だけを固定四要素で生成する。
 * @return 左Winはextended scan 0x5B、Rはscan 0x13。VK、時刻、extra情報は0。
 */
[[nodiscard]] inline std::array<INPUT, 4U> WinRKeyEmulationInputs() noexcept
{
	std::array<INPUT, 4U> inputs{} ;
	// 固定の順序でWinとRだけを指定し、他modifierや文字列を受け付けない
	for( std::size_t index = 0U; index < inputs.size(); ++index) {
		const bool win			 = index == 0U || index == 3U ;
		inputs[index].type		 = INPUT_KEYBOARD ;
		inputs[index].ki.wScan	 = win ? 0x5BU : 0x13U ;
		inputs[index].ki.dwFlags = KEYEVENTF_SCANCODE | (win ? KEYEVENTF_EXTENDEDKEY : 0U) |
								   (index >= 2U ? KEYEVENTF_KEYUP : 0U) ;
	}

	return inputs ;
}

/** @brief 左Ctrl押下、V押下、V解放、左Ctrl解放だけを固定生成する。
 * @return scan 0x1D／0x2Fの四要素。文字列、Win、Enterを含まない。
 */
[[nodiscard]] inline std::array<INPUT, 4U> PasteKeyEmulationInputs() noexcept
{
	std::array<INPUT, 4U> inputs{} ;
	// 固定のCtrl+V順序だけを生成し、任意の貼付け内容は受け付けない
	for( std::size_t index = 0U; index < inputs.size(); ++index) {
		inputs[index].type		 = INPUT_KEYBOARD ;
		inputs[index].ki.wScan	 = index == 0U || index == 3U ? 0x1DU : 0x2FU ;
		inputs[index].ki.dwFlags = KEYEVENTF_SCANCODE | (index >= 2U ? KEYEVENTF_KEYUP : 0U) ;
	}

	return inputs ;
}

/** @brief 主Enterの押下と解放だけを固定生成する。
 * @return scan 0x1Cの二要素。keypad Enterや他キーを含まない。
 */
[[nodiscard]] inline std::array<INPUT, 2U> EnterKeyEmulationInputs() noexcept
{
	std::array<INPUT, 2U> inputs{} ;
	// 固定Agentの起動要求だけに使い、modifierや文字列を受け付けない
	for( auto &input : inputs) {
		input.type		 = INPUT_KEYBOARD ;
		input.ki.wScan	 = 0x1CU ;
		input.ki.dwFlags = KEYEVENTF_SCANCODE ;
	}

	inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP ;
	return inputs ;
}

namespace detail {
/** @brief 固定factoryが生成した列を、共通guard後に一回だけ要求する。
 * @param[in] observed 送信直前の所有STA・認証・focus・キー解放条件。
 * @param[in] insert 固定列だけを受けるnativeまたはfake挿入境界。
 * @param[in,out] inputs 固定factoryの所有列。二要素または四要素に限定する。
 * @return 全件だけS_OK。Win32 error=0の不完全受付も失敗にする。
 * @note 内部の数値判定だけであり、実APIを呼ぶ公開の任意入力経路ではない。
 * @note 条件確認と実APIの間の前景切替競合は、Windows標準APIで完全には排除できない。
 */
template<std::size_t Count, class Insert>
[[nodiscard]] RKeyEmulationResult RunFixedKeyEmulation( const RKeyEmulationObservation &observed,
													   Insert						 &&insert,
													   std::array<INPUT, Count> inputs) noexcept
{
	static_assert( Count == 2U || Count == 4U) ;
	// 別STAではbackendを呼ばない
	if( !observed.owner_thread) {
		return {RPC_E_WRONG_THREAD, RKeyEmulationReason::WrongThread} ;
	}

	// 認証前・警告中・切断後へ入力しない
	if( !observed.authenticated) {
		return {E_UNEXPECTED, RKeyEmulationReason::Unavailable} ;
	}

	// hostだけのfocus成功や他Window前景を入力成功と扱わない
	if( !observed.focused) {
		return {FAILED( observed.focus_result) ? observed.focus_result
											  : HRESULT_FROM_WIN32( ERROR_INVALID_STATE),
				RKeyEmulationReason::FocusMismatch} ;
	}

	// 利用者の押下状態を解除・変更せず、競合した試行を停止する
	if( !observed.keys_released) {
		return {HRESULT_FROM_WIN32( ERROR_BUSY), RKeyEmulationReason::KeysHeld} ;
	}

	// COM照会後に失効した世代・clipboardへ固定キーでも送らない
	if( !observed.authorized) {
		return {HRESULT_FROM_WIN32( ERROR_CANCELLED), RKeyEmulationReason::AuthorizationLost} ;
	}

	try {
		const RKeyEmulationInsertion native = insert( inputs) ;
		// 固定列の全件挿入だけを受付とし、remoteの反応までは判定しない
		if( native.inserted == inputs.size()) {
			return {S_OK, RKeyEmulationReason::None, native.inserted, ERROR_SUCCESS} ;
		}

		// API契約外の件数を成功へ丸めない
		if( native.inserted > inputs.size()) {
			return {E_UNEXPECTED, RKeyEmulationReason::UnexpectedFailure, native.inserted,
					native.error} ;
		}

		const bool partial = native.inserted != 0U ;
		return {native.error != ERROR_SUCCESS ? HRESULT_FROM_WIN32( native.error)
				: partial					  ? HRESULT_FROM_WIN32( ERROR_PARTIAL_COPY)
											  : E_FAIL,
				partial ? RKeyEmulationReason::PartialInsertion
						: RKeyEmulationReason::InsertionFailure,
				native.inserted, native.error} ;
	} catch( ...) {
		// backend例外でも再送・追加のkey解放を実行しない
		return {E_UNEXPECTED, RKeyEmulationReason::UnexpectedFailure} ;
	}
}
} // namespace detail

/** @brief 条件成立時だけ固定R二要素を一回要求し、部分受付でも再送しない。
 * @param[in] observed 送信直前の必要条件。
 * @param[in] insert 固定R列の挿入境界。
 * @return 二件だけS_OK。不完全受付では元数値を保持する。
 */
template<class Insert>
[[nodiscard]] RKeyEmulationResult RunRKeyEmulation( const RKeyEmulationObservation &observed,
												   Insert						 &&insert) noexcept
{
	return detail::RunFixedKeyEmulation( observed, std::forward<Insert>( insert),
										RKeyEmulationInputs()) ;
}

/** @brief 条件成立時だけ固定Win+R四要素を一回要求し、部分受付でも再送しない。
 * @param[in] observed 送信直前の必要条件。
 * @param[in] insert 固定Win+R列の挿入境界。
 * @return 四件だけS_OK。不完全受付では元数値を保持する。
 */
template<class Insert>
[[nodiscard]] RKeyEmulationResult RunWinRKeyEmulation( const RKeyEmulationObservation &observed,
													  Insert &&insert) noexcept
{
	return detail::RunFixedKeyEmulation( observed, std::forward<Insert>( insert),
										WinRKeyEmulationInputs()) ;
}

/** @brief 内部の固定四種類だけを数値判定へ渡す非I/O境界。
 * @param[in] kind 固定種別。未知値はbackendを呼ばない。
 * @param[in] observed ownerが観測した直前条件。
 * @param[in] insert 固定列のnativeまたはfake挿入境界。
 * @return 全件だけS_OK。部分受付では再送せず元数値を保持する。
 */
template<class Insert>
[[nodiscard]] RKeyEmulationResult RunFixedKeyboardInput( FixedKeyboardKind				kind,
														const RKeyEmulationObservation &observed,
														Insert &&insert) noexcept
{
	// 固定factory以外の列や未知種別を入力境界へ渡さない
	switch( kind) {
	// R診断の既存列を維持する
	case FixedKeyboardKind::LetterR:
		return RunRKeyEmulation( observed, std::forward<Insert>( insert)) ;
	// 利用者確認済みのWin+R四要素だけを使う
	case FixedKeyboardKind::WinR:
		return RunWinRKeyEmulation( observed, std::forward<Insert>( insert)) ;
	// 固定の貼付け四要素だけを使う
	case FixedKeyboardKind::Paste:
		return detail::RunFixedKeyEmulation( observed, std::forward<Insert>( insert),
											PasteKeyEmulationInputs()) ;
	// 主Enterの押下・解放だけを使う
	case FixedKeyboardKind::Enter:
		return detail::RunFixedKeyEmulation( observed, std::forward<Insert>( insert),
											EnterKeyEmulationInputs()) ;
	// 未知値を別の固定キーへfallbackしない
	default:
		return {E_INVALIDARG, RKeyEmulationReason::UnexpectedFailure} ;
	}
}
} // namespace zrdp::client::rdp
