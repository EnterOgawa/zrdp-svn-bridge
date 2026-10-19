//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file rdp_input_test_keys.h
 * @brief 明示承認した三種類の固定scan-code列だけを定義する。
 * @version 01.00.03
 * @date 2026/10/10
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/10  Win+RからWin単独とR単独を分離する。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  SendKeys列を増やさずRのWindows入力比較を識別。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  固定Win+RのSendInput比較を別種別に追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  配置なしの固定Ctrl+V／Enter診断と失効確認を追加。
 * </pre>
 * @note 任意キーや文字列を受け付けず、SendKeysやWindow操作も実行しない。
 */
//------------------------------------------------------------------------------
#pragma once
#include <Windows.h>
#include <OleAuto.h>
#include <array>
#include <cstdint>

namespace zrdp::client::rdp {
/** @brief 利用者が一回ごとに選択・承認する固定入力種別と比較方式。 */
enum class InputTestKind : std::uint8_t {
	/// 従来の左WindowsキーとRの組合せ。
	WinR			 = 1,
	/// 左Windowsキーの押下と解放だけ。
	WindowsKey		 = 2,
	/// Rキーの押下と解放だけ。事前にメモ帳の空欄を利用者が選ぶ。
	LetterR			 = 3,
	/// RだけのWindows SendInput比較。SendKeys列には変換しない。
	LetterREmulation = 4,
	/// 固定Win+RだけのWindows SendInput比較。配置には使用しない。
	WinREmulation	 = 5,
	/// 固定Ctrl+VだけのSendInput診断。無害なclipboardとメモ帳を利用者が準備する。
	PasteEmulation	 = 6,
	/// 主EnterだけのSendInput診断。メモ帳で改行を確認する。
	EnterEmulation	 = 7,
} ;

/** @brief 固定SendInput診断だけを識別し、未知値を拒否する。
 * @param[in] kind 選択して確認する固定種別。
 * @return 固定R／Win+R／Ctrl+V／Enter比較の場合true。
 */
[[nodiscard]] constexpr bool IsKeyboardEmulationTest( InputTestKind kind) noexcept
{
	return kind == InputTestKind::LetterREmulation || kind == InputTestKind::WinREmulation ||
		   kind == InputTestKind::PasteEmulation || kind == InputTestKind::EnterEmulation ;
}

/** @brief Microsoft SendKeysへ渡す最大四要素の固定列。 */
struct InputTestKeys {
	/// 各要素の押下FALSE、解放TRUE。count未満だけを使用する。
	std::array<VARIANT_BOOL, 4U> key_up{} ;
	/// scan、extended、repeat、previous、transitionの固定lParam形式。
	std::array<LONG, 4U>		 key_data{} ;
	/// 有効要素数。未知種別は0として入力を拒否する。
	LONG						 count{0L} ;
} ;

/** @brief 承認済みの入力種別を、従来と同じ固定key dataへ変換する。
 * @param[in] kind 固定三種類または変換を拒否する比較enum。
 * @return 未知種別とSendInput比較はcount=0。Win+Rは4、既存単独は2要素。
 */
[[nodiscard]] constexpr InputTestKeys FixedInputTestKeys( InputTestKind kind) noexcept
{
	// Win+Rの構成要素を変えず、単独入力も同じ形式に固定する
	switch( kind) {
	// 従来の一括Win+R入力を変更しない
	case InputTestKind::WinR:
		return {{VARIANT_FALSE, VARIANT_FALSE, VARIANT_TRUE, VARIANT_TRUE},
				{0x015B0001L, 0x00130001L, static_cast<LONG>( 0xC0130001UL),
				 static_cast<LONG>( 0xC15B0001UL)},
				4L} ;
	// 左Windowsキー以外のmodifierやletterを混ぜない
	case InputTestKind::WindowsKey:
		return {{VARIANT_FALSE, VARIANT_TRUE, VARIANT_FALSE, VARIANT_FALSE},
				{0x015B0001L, static_cast<LONG>( 0xC15B0001UL), 0L, 0L},
				2L} ;
	// Rキー以外のmodifierやEnterを混ぜない
	case InputTestKind::LetterR:
		return {{VARIANT_FALSE, VARIANT_TRUE, VARIANT_FALSE, VARIANT_FALSE},
				{0x00130001L, static_cast<LONG>( 0xC0130001UL), 0L, 0L},
				2L} ;
	// Windows入力比較をSendKeysへ変換せず、専用methodへ限定する
	case InputTestKind::LetterREmulation:
		return {} ;
	// 固定Win+R比較も従来の配置SendKeysへ変換しない
	case InputTestKind::WinREmulation:
		return {} ;
	// 貼付け診断はSendInput専用とし、SendKeysへ渡さない
	case InputTestKind::PasteEmulation:
		return {} ;
	// Enter診断も旧SendKeys診断へ変換しない
	case InputTestKind::EnterEmulation:
		return {} ;
	// 未知種別をSendKeysへ無断で渡さない
	default:
		return {} ;
	}
}
} // namespace zrdp::client::rdp
