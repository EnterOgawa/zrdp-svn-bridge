//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file session_agent_controls_test.cpp
 * @brief 接続しない実session UIでAgent入口の表示と配置を検証する。
 * @version 01.00.14
 * @date 2026/10/09
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/09  自動接続messageを除去したUI fixtureを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/09  未認証での初期配置の停止snapshotを確認。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  配置から独立した開発用Win+R testを追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/09  timer分離・期限・取消・再入・再確認を検証。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  三種類の固定列とmenu・個別確認を検証。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  RのWindows入力比較をfakeと未接続UIで検証。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  固定Win+RのSendInput比較と停止境界を追加。
 * Ver.01.00.07   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  配置なしの固定Ctrl+V／Enter診断と失効確認を追加。
 * Ver.01.00.09   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.10   Shunji Ogawa  2026/10/10  完了入力sequenceの限定初期化と二回目起動を修正。
 * Ver.01.00.11   Shunji Ogawa  2026/10/10  認証前のWindow focusが非表示入力へ移らないことを検証。
 * Ver.01.00.12   Shunji Ogawa  2026/10/10  接続別設定・自動Agent管理の回帰検証を追加。
 * Ver.01.00.13   Shunji Ogawa  2026/10/10  認証前のAgent停止表示と非modal起動guardを検証。
 * Ver.01.00.14   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 * @note Proxyを設定errorで停止し、自動接続messageをdispatch前に除去する。
 *       実RDP・clipboard・履歴・Agent設定の読込みは行わない。
 */
//------------------------------------------------------------------------------
#include "client/app/main_window.h"
#include "client/rdp/rdp_input_test_keys.h"
#include "client/app/resource.h"
#include "client/rdp/rdp_run_input_test.h"
#include <objbase.h>
#include <CommCtrl.h>
#include <atlbase.h>
#include <atlhost.h>
#include <iostream>
#include <string_view>
#include <array>
#include <limits>
#include "client/rdp/rdp_fixed_input_sequence.h"

namespace {
/** @brief 固定列をgolden値で照合し、未知種別に入力を生成しないことを確認する。
 * @return 要素数、順序、押下解放、padding、未知種別が正しい場合true。
 */
bool TestInputTestKeys()
{
	namespace rdp	  = zrdp::client::rdp ;
	const auto run	  = rdp::FixedInputTestKeys( rdp::InputTestKind::WinR) ;
	const auto win	  = rdp::FixedInputTestKeys( rdp::InputTestKind::WindowsKey) ;
	const auto letter = rdp::FixedInputTestKeys( rdp::InputTestKind::LetterR) ;
	const std::array<VARIANT_BOOL, 4U> run_up{VARIANT_FALSE, VARIANT_FALSE, VARIANT_TRUE,
											  VARIANT_TRUE} ;
	const std::array<LONG, 4U> run_data{0x015B0001L, 0x00130001L, static_cast<LONG>( 0xC0130001UL),
										static_cast<LONG>( 0xC15B0001UL)} ;
	const std::array<VARIANT_BOOL, 4U> single_up{VARIANT_FALSE, VARIANT_TRUE, VARIANT_FALSE,
												 VARIANT_FALSE} ;
	const std::array<LONG, 4U> win_data{0x015B0001L, static_cast<LONG>( 0xC15B0001UL), 0L, 0L} ;
	const std::array<LONG, 4U> r_data{0x00130001L, static_cast<LONG>( 0xC0130001UL), 0L, 0L} ;
	// Win+Rの既存dataを維持し、単独入力へ別modifier等を混ぜない
	if( run.count != 4L || run.key_up != run_up || run.key_data != run_data || win.count != 2L ||
	   win.key_up != single_up || win.key_data != win_data || letter.count != 2L ||
	   letter.key_up != single_up || letter.key_data != r_data) {
		return false ;
	}

	// SendInput比較と未知種別には空列を返し、SendKeysの対象を増やさない
	for( const auto unknown : {0U, 4U, 5U, 6U, 7U, 8U, 255U}) {
		const auto invalid = rdp::FixedInputTestKeys( static_cast<rdp::InputTestKind>( unknown)) ;
		// 未知種別へ任意dataを残さない
		if( invalid.count != 0L || invalid.key_up != std::array<VARIANT_BOOL, 4U>{} ||
		   invalid.key_data != std::array<LONG, 4U>{}) {
			return false ;
		}

		// 四種類のSendInput比較だけを識別し、未知値を診断へ通さない
		if( rdp::IsKeyboardEmulationTest( static_cast<rdp::InputTestKind>( unknown)) !=
		   (unknown >= 4U && unknown <= 7U)) {
			return false ;
		}
	}

	return true ;
}

/** @brief Windows入力比較のfixed列とguard・部分受付を実SendInputなしで検証する。
 * @param[in] windows_r trueはWin+R四要素、falseはR二要素。
 * @return backendは許可された場合だけ一回呼ばれ、数値結果が一致する場合true。
 */
bool TestKeyEmulation( bool windows_r)
{
	namespace rdp = zrdp::client::rdp ;
	std::array<rdp::RKeyEmulationInsertion, 14U> native{{{2U, 0U},
														 {2U, 0U},
														 {2U, 0U},
														 {2U, 0U},
														 {2U, 0U},
														 {2U, 0U},
														 {0U, ERROR_ACCESS_DENIED},
														 {0U, 0U},
														 {1U, 0U},
														 {1U, ERROR_ACCESS_DENIED},
														 {0U, ERROR_NOT_SUPPORTED},
														 {3U, 0U},
														 {2U, 0U},
														 {2U, ERROR_ACCESS_DENIED}}} ;
	std::array<rdp::RKeyEmulationResult, 14U>	 expected{
		   {{S_OK, rdp::RKeyEmulationReason::None, 2U, 0U},
			{RPC_E_WRONG_THREAD, rdp::RKeyEmulationReason::WrongThread, 0U, 0U},
			{E_UNEXPECTED, rdp::RKeyEmulationReason::Unavailable, 0U, 0U},
			{HRESULT_FROM_WIN32( ERROR_INVALID_STATE), rdp::RKeyEmulationReason::FocusMismatch, 0U, 0U},
			{HRESULT_FROM_WIN32( ERROR_BUSY), rdp::RKeyEmulationReason::KeysHeld, 0U, 0U},
			{E_NOINTERFACE, rdp::RKeyEmulationReason::FocusMismatch, 0U, 0U},
			{HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED), rdp::RKeyEmulationReason::InsertionFailure, 0U,
			 ERROR_ACCESS_DENIED},
			{E_FAIL, rdp::RKeyEmulationReason::InsertionFailure, 0U, 0U},
			{HRESULT_FROM_WIN32( ERROR_PARTIAL_COPY), rdp::RKeyEmulationReason::PartialInsertion, 1U,
			 0U},
			{HRESULT_FROM_WIN32( ERROR_ACCESS_DENIED), rdp::RKeyEmulationReason::PartialInsertion, 1U,
			 ERROR_ACCESS_DENIED},
			{HRESULT_FROM_WIN32( ERROR_NOT_SUPPORTED), rdp::RKeyEmulationReason::InsertionFailure, 0U,
			 ERROR_NOT_SUPPORTED},
			{E_UNEXPECTED, rdp::RKeyEmulationReason::UnexpectedFailure, 3U, 0U},
			{E_UNEXPECTED, rdp::RKeyEmulationReason::UnexpectedFailure, 0U, 0U},
			{S_OK, rdp::RKeyEmulationReason::None, 2U, 0U}}} ;
	// Win+Rでは全件4と不正件数5へ置き換え、同じguardを検証する
	if( windows_r) {
		// nativeと期待値の全件受付だけを四要素へ対応させる
		for( std::size_t index = 0U; index < native.size(); ++index) {
			// fakeの全件結果をWin+Rへ対応させる
			if( native[index].inserted == 2U) {
				native[index].inserted = 4U ;
			}

			// guardで未送信の期待件数は変更しない
			if( expected[index].inserted == 2U) {
				expected[index].inserted = 4U ;
			}
		}

		native[11].inserted	  = 5U ;
		expected[11].inserted = 5U ;
	}

	// 各停止境界と全件・部分・拒否、不正件数、例外、成功時error無視を確認する
	for( std::size_t mode = 0U; mode < expected.size(); ++mode) {
		rdp::RKeyEmulationObservation observed{true, true, true, true, S_OK} ;
		// 他STAはCOMや入力APIへ進まない
		if( mode == 1U) {
			observed.owner_thread = false ;
		}

		// 未認証または警告中の試行を拒否する
		if( mode == 2U) {
			observed.authenticated = false ;
		}

		// 別Window focusやActiveX Window照会失敗を拒否する
		if( mode == 3U || mode == 5U) {
			observed.focused	  = false ;
			observed.focus_result = mode == 5U ? E_NOINTERFACE : S_OK ;
		}

		// 利用者の押下を補正する入力は生成しない
		if( mode == 4U) {
			observed.keys_released = false ;
		}

		UINT	   calls	  = 0U ;
		bool	   keys_match = true ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 固定二／四要素だけを照合し、fakeの受付数とerrorを返す。
		 * @param[in,out] inputs helperが作った固定列。内容を変更せず検証する。
		 * @return 選択したfakeのnative結果。

		 * @endcond
		 */
		const auto insert	  = [&]( auto &inputs) {
			++calls ;
			keys_match = keys_match && inputs.size() == (windows_r ? 4U : 2U) ;
			// 固定R／Win+Rの順序だけで、VK・UNICODE・extra情報等を混ぜない
			for( std::size_t index = 0U; index < inputs.size(); ++index) {
				const auto &input = inputs[index] ;
				const bool	win	  = windows_r && (index == 0U || index == 3U) ;
				keys_match =
					keys_match && input.type == INPUT_KEYBOARD && input.ki.wVk == 0U &&
					input.ki.wScan == (win ? 0x5BU : 0x13U) &&
					input.ki.dwFlags == (KEYEVENTF_SCANCODE | (win ? KEYEVENTF_EXTENDEDKEY : 0U) |
										 (index >= (windows_r ? 2U : 1U) ? KEYEVENTF_KEYUP : 0U)) &&
					input.ki.time == 0U && input.ki.dwExtraInfo == 0U ;
			}

			// 境界例外でも追加入力は要求しない
			if( mode == 12U) {
				throw 1 ;
			}

			return native[mode] ;
		} ;
		const auto result		  = windows_r ? rdp::RunWinRKeyEmulation( observed, insert)
											  : rdp::RunRKeyEmulation( observed, insert) ;
		const UINT expected_calls = mode >= 1U && mode <= 5U ? 0U : 1U ;
		// 不完全受付でもbackendは再呼出しせず、元数値と停止分類を保持する
		if( calls != expected_calls || !keys_match || result.result != expected[mode].result ||
		   result.reason != expected[mode].reason || result.inserted != expected[mode].inserted ||
		   result.error != expected[mode].error) {
			return false ;
		}
	}

	// Win+Rの2／3要素部分受付も元errorと一回要求を保持する
	if( windows_r) {
		// modifier解放前の各部分受付をerror有無の両方で検証する
		for( UINT count : {2U, 3U}) {
			// Win32の0でも成功へ変えず、非0の元値も失わない
			for( DWORD error : {ERROR_SUCCESS, ERROR_ACCESS_DENIED}) {
				UINT	   calls = 0U ;
				const auto result =
					rdp::RunWinRKeyEmulation( {true, true, true, true, S_OK},
											 /**
											  * @cond LOCAL_CALLBACK_CONTRACT @brief 部分受付だけを返し、追加解放・再送を要求しない。
											  * @param[in,out] inputs 固定四要素。実APIへ渡さない。
											  * @return fakeの部分件数と元error。

											  * @endcond
											  */
											 [&]( std::array<INPUT, 4U> &inputs) {
												 static_cast<void>( inputs) ;
												 ++calls ;
												 return rdp::RKeyEmulationInsertion{count, error} ;
											 }) ;
				// 四件未満は必ず失敗し、数値結果と一回要求を保持する
				if( calls != 1U || result.reason != rdp::RKeyEmulationReason::PartialInsertion ||
				   result.result != HRESULT_FROM_WIN32( error == 0U ? ERROR_PARTIAL_COPY : error) ||
				   result.inserted != count || result.error != error) {
					return false ;
				}
			}
		}
	}

	return true ;
}

/** @brief 配置・起動の固定列と全受付・部分受付・各guardを実SendInputなしで検証する。
 * @return golden列、数値結果、一回要求と未承認拒否が全て成立した場合true。
 */
bool TestProductionKeyboardInput()
{
	namespace rdp = zrdp::client::rdp ;
	using Kind	  = rdp::FixedKeyboardKind ;
	// 三つの製品入力ごとに全guardと各受付数を検証する
	for( const auto kind : {Kind::WinR, Kind::Paste, Kind::Enter}) {
		const UINT				   count = kind == Kind::Enter ? 2U : 4U ;
		const std::array<WORD, 4U> scans =
			kind == Kind::WinR ? std::array<WORD, 4U>{0x5BU, 0x13U, 0x13U, 0x5BU}

			: kind == Kind::Paste ? std::array<WORD, 4U>{0x1DU, 0x2FU, 0x2FU, 0x1DU}

								  : std::array<WORD, 4U>{0x1CU, 0x1CU, 0U, 0U} ;
		const std::array<DWORD, 4U> flags =
			kind == Kind::WinR ? std::array<DWORD, 4U>{9U, 8U, 10U, 11U}

			: kind == Kind::Paste ? std::array<DWORD, 4U>{8U, 8U, 10U, 10U}

								  : std::array<DWORD, 4U>{8U, 10U, 0U, 0U} ;
		// guard失効の優先順位と入力境界の未呼出しを確認する
		for( unsigned mode = 0U; mode < 7U; ++mode) {
			rdp::RKeyEmulationObservation observed{true, true, true, true, S_OK, true} ;
			const std::array<HRESULT, 7U> results{S_OK,
												  RPC_E_WRONG_THREAD,
												  E_UNEXPECTED,
												  HRESULT_FROM_WIN32( ERROR_INVALID_STATE),
												  HRESULT_FROM_WIN32( ERROR_BUSY),
												  HRESULT_FROM_WIN32( ERROR_CANCELLED),
												  E_NOINTERFACE} ;
			// 別STAでbackendへ進まない
			if( mode == 1U) {
				observed.owner_thread = false ;
			}

			// 未認証・警告ではbackendへ進まない
			if( mode == 2U) {
				observed.authenticated = false ;
			}

			// 前景不一致と元COMエラーを区別する
			if( mode == 3U || mode == 6U) {
				observed.focused	  = false ;
				observed.focus_result = mode == 6U ? E_NOINTERFACE : S_OK ;
			}

			// 利用者のキー押下を勝手に解放しない
			if( mode == 4U) {
				observed.keys_released = false ;
			}

			// COM照会後の世代・clipboard・取消失効を拒否する
			if( mode == 5U) {
				observed.authorized = false ;
			}

			UINT	   calls  = 0U ;
			bool	   golden = true ;
			const auto result = rdp::RunFixedKeyboardInput(
				kind, observed,
				/**
				 * @cond LOCAL_CALLBACK_CONTRACT @brief 固定golden列を照合し全件受付を返すfake境界。
				 * @param[in,out] inputs 固定factoryの所有列。実APIへ渡さない。
				 * @return fake全件受付。

				 * @endcond
				 */
				[&]( auto &inputs) {
					++calls ;
					golden = golden && inputs.size() == count ;
					// 物理scanと押下解放順を独立したgolden値へ照合する
					for( std::size_t index = 0U; index < inputs.size(); ++index) {
						golden = golden && inputs[index].type == INPUT_KEYBOARD &&
								 inputs[index].ki.wVk == 0U &&
								 inputs[index].ki.wScan == scans[index] &&
								 inputs[index].ki.dwFlags == flags[index] &&
								 inputs[index].ki.time == 0U && inputs[index].ki.dwExtraInfo == 0U ;
					}

					return rdp::RKeyEmulationInsertion{count, 0U} ;
				}) ;
			// 全guard失効ではbackendを呼ばず、元HRESULTを保持する
			if( !golden || calls != (mode == 0U ? 1U : 0U) || result.result != results[mode] ||
			   result.inserted != (mode == 0U ? count : 0U) ||
			   (mode == 5U && result.reason != rdp::RKeyEmulationReason::AuthorizationLost)) {
				return false ;
			}
		}

		// 0から全件超過まで、元errorの有無を両方検証する
		for( UINT inserted = 0U; inserted <= count + 1U; ++inserted) {
			// Win32 errorが0でも不完全受付は失敗にする
			for( DWORD error : {ERROR_SUCCESS, ERROR_ACCESS_DENIED}) {
				UINT	   calls  = 0U ;
				const auto result = rdp::RunFixedKeyboardInput(
					kind, {true, true, true, true, S_OK, true},
					/**
					 * @cond LOCAL_CALLBACK_CONTRACT @brief 数値結果だけを返し、補正キー・再送を要求しない。
					 * @param[in,out] inputs 固定列。変更も実API呼出しもしない。
					 * @return fake受付数と元error。

					 * @endcond
					 */
					[&]( auto &inputs) {
						static_cast<void>( inputs) ;
						++calls ;
						return rdp::RKeyEmulationInsertion{inserted, error} ;
					}) ;
				const HRESULT expected = inserted == count	? S_OK
										 : inserted > count ? E_UNEXPECTED
										 : error != 0U		? HRESULT_FROM_WIN32( error)
										 : inserted == 0U	? E_FAIL
														  : HRESULT_FROM_WIN32( ERROR_PARTIAL_COPY) ;
				// 元件数・元errorと一回要求を保持し、全件だけ成功にする
				if( calls != 1U || result.result != expected || result.inserted != inserted ||
				   result.error != (inserted == count ? 0U : error) ||
				   (inserted > 0U && inserted < count &&
					result.reason != rdp::RKeyEmulationReason::PartialInsertion)) {
					return false ;
				}
			}
		}
	}

	// 未知種別で既知入力へfallbackしない
	for( unsigned kind : {0U, 5U, 255U}) {
		UINT	   calls  = 0U ;
		const auto result = rdp::RunFixedKeyboardInput(
			static_cast<Kind>( kind), {true, true, true, true, S_OK, true},
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT @brief 未知種別では呼ばれないことを検証する。
			 * @param[in,out] inputs 仮の固定列。実APIを呼ばない。
			 * @return 呼出し検出用fake値。

			 * @endcond
			 */
			[&]( auto &inputs) {
				static_cast<void>( inputs) ;
				++calls ;
				return rdp::RKeyEmulationInsertion{0U, 0U} ;
			}) ;
		// 未知種別には入力列を一回も公開しない
		if( calls != 0U || result.result != E_INVALIDARG || result.inserted != 0U) {
			return false ;
		}
	}

	return true ;
}

/** @brief 固定入力待機の早着・再入・取消・期限・overflowを仮想時刻で検証する。
 * @return 一回要求と停止後非復活、各最小待機が成立した場合true。
 */
bool TestFixedInputSequence()
{
	using zrdp::client::rdp::FixedInputSequence ;
	using Step = zrdp::client::rdp::FixedInputStep ;
	FixedInputSequence sequence ;
	// 未開始timer、途中段階開始、二重開始、早着・再入通知を拒否する
	if( sequence.Claim( 1000U) != Step::Idle || sequence.Begin( 1000U, Step::Paste) ||
	   !sequence.Begin( 1000U) || sequence.Begin( 1001U) || sequence.Claim( 1349U) != Step::Idle ||
	   sequence.Claim( 1350U) != Step::Run || sequence.Claim( 1400U) != Step::Idle ||
	   !sequence.Complete( 1400U, true) || sequence.Complete( 1400U, true) ||
	   sequence.Claim( 2099U) != Step::Idle || sequence.Claim( 2100U) != Step::Paste ||
	   !sequence.Complete( 2100U, true) || sequence.Claim( 2799U) != Step::Idle ||
	   sequence.Claim( 2800U) != Step::Enter || !sequence.Complete( 2800U, true) ||
	   sequence.pending() || sequence.Claim( 10000U) != Step::Idle || sequence.Begin( 10000U)) {
		return false ;
	}

	sequence.Cancel() ;
	// 受付済み操作を取消で巻き戻したと扱わない
	if( sequence.step() != Step::Complete) {
		return false ;
	}

	// 完了だけを明示初期化でき、取消とは異なり次の新規開始を許可する
	if( !sequence.ResetCompleted() || sequence.ResetCompleted() || sequence.step() != Step::Idle ||
	   sequence.pending() || sequence.in_flight() || sequence.Claim( 10000U) != Step::Idle ||
	   !sequence.Begin( 10000U) || sequence.Claim( 10349U) != Step::Idle ||
	   sequence.ResetCompleted()) {
		return false ;
	}

	sequence.Cancel() ;
	// 取消済みを完了の初期化で復活させない
	if( sequence.ResetCompleted() || sequence.step() != Step::Stopped || sequence.Begin( 11000U)) {
		return false ;
	}

	FixedInputSequence enter ;
	// 手動起動はEnterだけを遅延し、Win+Rやpasteへ進まない
	if( !enter.Begin( 1000U, Step::Enter) || enter.Claim( 1349U) != Step::Idle ||
	   enter.Claim( 1350U) != Step::Enter || !enter.Complete( 1350U, true) || enter.pending()) {
		return false ;
	}

	// 各入力段階の取消と部分受付相当失敗を仮想callbackで検証する
	for( unsigned target = 0U; target < 3U; ++target) {
		// 送信前取消・送信中取消・API失敗をそれぞれ検証する
		for( unsigned mode = 0U; mode < 3U; ++mode) {
			FixedInputSequence stopped ;
			static_cast<void>( stopped.Begin( 1000U)) ;
			std::uint64_t now = 1350U ;
			// 目的の固定段階までだけ正常受付を進める
			for( unsigned previous = 0U; previous < target; ++previous) {
				static_cast<void>( stopped.Claim( now)) ;
				static_cast<void>( stopped.Complete( now, true)) ;
				now += 700U ;
			}

			// 送信前の取消では予約さえ取得しない
			if( mode == 0U) {
				stopped.Cancel() ;
			}

			static_cast<void>( stopped.Claim( now)) ;
			// backend中の取消から正常復帰しても復活させない
			if( mode == 1U) {
				stopped.Cancel() ;
			}

			const bool completed = stopped.Complete( now, mode != 2U) ;
			// 取消・失敗後は再予約・再送・次段階を拒否する
			if( completed || stopped.pending() || stopped.ResetCompleted() || stopped.Begin( now) ||
			   stopped.Claim( now + 5000U) != Step::Idle) {
				return false ;
			}
		}
	}

	// 期限ちょうどとclock逆転、時刻overflowを失効へ変換する
	for( std::uint64_t tick : {999ULL, 4350ULL}) {
		FixedInputSequence expired ;
		static_cast<void>( expired.Begin( 1000U)) ;
		// 期限直前は有効、逆転と期限到達は入力せず失効する
		if( expired.expired( 4349U) || !expired.expired( tick) || expired.Claim( tick) != Step::Idle ||
		   expired.step() != Step::Stopped || expired.Begin( 5000U)) {
			return false ;
		}
	}

	FixedInputSequence overflow ;
	// wrapした時刻を即時入力にしない
	if( overflow.Begin( std::numeric_limits<std::uint64_t>::max()) || overflow.pending()) {
		return false ;
	}

	// callback中のclock逆転と期限超過でも後続入力を予約しない
	for( std::uint64_t ended : {1349ULL, 4350ULL}) {
		FixedInputSequence late ;
		static_cast<void>( late.Begin( 1000U)) ;
		static_cast<void>( late.Claim( 1350U)) ;
		// 全件受付を返しても失効した予約を復活させない
		if( late.Complete( ended, true) || late.pending()) {
			return false ;
		}
	}

	FixedInputSequence next_overflow ;
	const auto		   near_max = std::numeric_limits<std::uint64_t>::max() - 3351U ;
	static_cast<void>( next_overflow.Begin( near_max)) ;
	static_cast<void>( next_overflow.Claim( near_max + 350U)) ;
	// 次段階の予約overflowでも停止する
	if( next_overflow.Complete( near_max + 350U, true) || next_overflow.pending()) {
		return false ;
	}

	return true ;
}

/** @brief 実RDPなしで確認・準備・固定入力の停止条件を検証する。
 * @param[in] kind 同じcontrollerに適用する承認済みの固定入力種別。
 * @return 全guard、取消、元HRESULT保持、一回送信が正しい場合true。
 */
bool TestRunInputController( zrdp::client::rdp::InputTestKind kind)
{
	namespace rdp = zrdp::client::rdp ;
	using Stage	  = rdp::RunInputTestStage ;
	using Reason  = rdp::RunInputTestReason ;
	// 正常、取消、未認証、別STA、競合、確認中失効、準備失敗、focus中失効、
	// focus中競合、送信E_FAIL、送信S_FALSE、確認例外の順で検証する
	const std::array<Stage, 12U> stages{
		Stage::SendRun,	   Stage::Confirmation, Stage::Validation,	Stage::Validation,
		Stage::Validation, Stage::Validation,	Stage::Preparation, Stage::Validation,
		Stage::Validation, Stage::SendRun,		Stage::SendRun,		Stage::Confirmation} ;
	const std::array<Reason, 12U>		reasons{Reason::None,
											Reason::Declined,
											Reason::SessionChanged,
											Reason::WrongThread,
											Reason::Unavailable,
											Reason::SessionChanged,
											Reason::PreparationFailure,
											Reason::SessionChanged,
											Reason::Unavailable,
											Reason::InputFailure,
											Reason::InputFailure,
											Reason::UnexpectedException} ;
	const std::array<HRESULT, 12U>		results{S_OK,
											HRESULT_FROM_WIN32( ERROR_CANCELLED),
											HRESULT_FROM_WIN32( ERROR_INVALID_STATE),
											RPC_E_WRONG_THREAD,
											HRESULT_FROM_WIN32( ERROR_BUSY),
											HRESULT_FROM_WIN32( ERROR_INVALID_STATE),
											E_ACCESSDENIED,
											HRESULT_FROM_WIN32( ERROR_INVALID_STATE),
											HRESULT_FROM_WIN32( ERROR_BUSY),
											E_FAIL,
											S_FALSE,
											E_UNEXPECTED} ;
	const std::array<unsigned int, 12U> expected_confirmations{1, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1} ;
	const std::array<unsigned int, 12U> expected_preparations{1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0} ;
	const std::array<unsigned int, 12U> expected_sends{1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0} ;
	// 各caseは独立したfakeだけを使い、実キー・clipboardへ一切接続しない
	for( std::size_t mode = 0U; mode < stages.size(); ++mode) {
		rdp::RunInputTestObservation observed{mode != 3U, mode == 2U ? 0U : 7U, mode != 4U} ;
		unsigned int confirmations = 0U, preparations = 0U, sends = 0U, observations = 0U ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 現在のfake条件だけを観測する。 @return 数値条件のsnapshot。
		 * @endcond
		 */
		const auto	 observe = [&]() {
			  ++observations ;
			  return observed ;
		} ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 確認中の取消・世代失効・例外を注入する。 @return 明示承認のfake結果。
		 * @endcond
		 */
		const auto confirm = [&]() {
			++confirmations ;
			// 確認境界の例外でも入力準備へ進まない
			if( mode == 11U) {
				throw 1 ;
			}

			// modal確認からの復帰前に認証世代が変化するcase
			if( mode == 5U) {
				++observed.epoch ;
			}

			return mode != 1U ;
		} ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief focus移動中の再入と準備失敗を注入する。 @return 元HRESULTのfake値。
		 * @endcond
		 */
		const auto prepare = [&]() {
			++preparations ;
			// focus移動中の再接続を送信前に検出するcase
			if( mode == 7U) {
				++observed.epoch ;
			}

			// 同じ認証世代でも他入力の開始を拒否するcase
			if( mode == 8U) {
				observed.idle = false ;
			}

			return mode == 6U ? E_ACCESSDENIED : S_OK ;
		} ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 実キーを送らず、要求件数と元結果だけを返す。 @return fake API結果。
		 * @endcond
		 */
		const auto send = [&]() {
			++sends ;
			// 選択済みの固定列だけを生成し、未知・空列なら停止する
			if( !rdp::IsKeyboardEmulationTest( kind) && rdp::FixedInputTestKeys( kind).count == 0L) {
				return E_INVALIDARG ;
			}

			return mode == 9U ? E_FAIL : mode == 10U ? S_FALSE : S_OK ;
		} ;
		std::uint64_t	  now	 = 1000U ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 仮想clockだけを返す。 @return 現在のfake ms値。
		 * @endcond
		 */
		const auto		  clock	 = [&]() { return now ; } ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 正常なfocus再確認を注入する。 @return S_OK。
		 * @endcond
		 */
		const auto		  verify = []() { return S_OK ; } ;
		rdp::RunInputTest controller ;
		static_cast<void>( controller.Begin( 7U, observe, confirm, clock)) ;
		now += rdp::RunInputTest::kConfirmationSettleMilliseconds ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		now += rdp::RunInputTest::kFocusSettleMilliseconds ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		now += rdp::RunInputTest::kResultSettleMilliseconds ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		const auto result = controller.result() ;
		// 停止段階と元HRESULTを保持し、失敗後や取消後に入力を追加しない
		if( result.stage != stages[mode] || result.reason != reasons[mode] ||
		   result.result != results[mode] || confirmations != expected_confirmations[mode] ||
		   preparations != expected_preparations[mode] || sends != expected_sends[mode] ||
		   controller.phase() != rdp::RunInputTestPhase::Complete ||
		   (mode == 0U && observations != 6U)) {
			return false ;
		}
	}

	return true ;
}

/** @brief fake clockで遅延入力の待機・取消・期限・再入を検証する。
 * @param[in] kind 同じcontrollerに適用する承認済み固定入力種別。
 * @return 早着・残留timerで送信せず、正常時だけ一回入力する場合true。
 */
bool TestDeferredRunInputController( zrdp::client::rdp::InputTestKind kind)
{
	namespace rdp							   = zrdp::client::rdp ;
	using Phase								   = rdp::RunInputTestPhase ;
	using Stage								   = rdp::RunInputTestStage ;
	using Reason							   = rdp::RunInputTestReason ;
	const HRESULT				   busy		   = HRESULT_FROM_WIN32( ERROR_BUSY) ;
	const HRESULT				   invalid	   = HRESULT_FROM_WIN32( ERROR_INVALID_STATE) ;
	const HRESULT				   timeout	   = HRESULT_FROM_WIN32( WAIT_TIMEOUT) ;
	const HRESULT				   timer_error = HRESULT_FROM_WIN32( ERROR_NO_SYSTEM_RESOURCES) ;
	// 正常境界、準備前取消／世代／競合、準備後世代／競合／STA、再確認失敗、
	// 再確認中世代／競合／取消、予約失敗、準備前／後期限、各callback例外、
	// 準備中取消／再入、送信中再入、送信済み取消、再確認中期限、clock wrapの順
	const std::array<Reason, 23U>  reasons{Reason::None,
										   Reason::Unavailable,
										   Reason::SessionChanged,
										   Reason::Unavailable,
										   Reason::SessionChanged,
										   Reason::Unavailable,
										   Reason::WrongThread,
										   Reason::PreparationFailure,
										   Reason::SessionChanged,
										   Reason::Unavailable,
										   Reason::Unavailable,
										   Reason::TimerFailure,
										   Reason::TimerFailure,
										   Reason::TimerFailure,
										   Reason::UnexpectedException,
										   Reason::UnexpectedException,
										   Reason::UnexpectedException,
										   Reason::Unavailable,
										   Reason::None,
										   Reason::None,
										   Reason::None,
										   Reason::TimerFailure,
										   Reason::None} ;
	const std::array<HRESULT, 23U> results{
		S_OK,			busy,		  invalid,		busy, invalid,	   busy,	RPC_E_WRONG_THREAD,
		E_ACCESSDENIED, invalid,	  busy,			busy, timer_error, timeout, timeout,
		E_UNEXPECTED,	E_UNEXPECTED, E_UNEXPECTED, busy, S_OK,		   S_OK,	S_OK,
		timeout,		S_OK} ;
	const std::array<Stage, 23U> stages{
		Stage::SendRun,		Stage::Preparation, Stage::Validation,	Stage::Validation,
		Stage::Validation,	Stage::Validation,	Stage::Validation,	Stage::Preparation,
		Stage::Validation,	Stage::Validation,	Stage::Preparation, Stage::Preparation,
		Stage::Preparation, Stage::Preparation, Stage::Preparation, Stage::Preparation,
		Stage::SendRun,		Stage::Preparation, Stage::SendRun,		Stage::SendRun,
		Stage::SendRun,		Stage::Preparation, Stage::SendRun} ;
	// 各caseは実時間・実入力に依存しない独立した状態機械で検証する
	for( std::size_t mode = 0U; mode < reasons.size(); ++mode) {
		rdp::RunInputTest			 controller ;
		rdp::RunInputTestObservation observed{true, 7U, true} ;
		std::uint64_t				 now		  = mode == 22U ? UINT64_MAX - 50U : 1000U ;
		unsigned int				 preparations = 0U, verifications = 0U, sends = 0U ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 現在のfake条件を返す。 @return 安全条件snapshot。
		 * @endcond
		 */
		const auto					 observe  = [&]() { return observed ; } ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 明示承認を注入する。 @return true。
		 * @endcond
		 */
		const auto					 confirm  = []() { return true ; } ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 単調fake clockを返す。 @return 仮想ms値。
		 * @endcond
		 */
		const auto					 clock	  = [&]() { return now ; } ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 入力を行わない再入検証用境界。 @return S_OK。
		 * @endcond
		 */
		const auto					 no_input = []() { return S_OK ; } ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 準備中の例外・取消・timer再入を注入する。 @return S_OK。
		 * @endcond
		 */
		const auto					 prepare  = [&]() {
			   ++preparations ;
			   // 準備の例外は固定停止にする
			   if( mode == 14U) {
				   throw 1 ;
			   }

			   // 準備中の取消を成功で復活させない
			   if( mode == 17U) {
				   controller.Cancel( Reason::Unavailable, busy) ;
			   }

			   // 準備callback中のnested timerで入力callbackを追加しない
			   if( mode == 18U) {
				   controller.Advance( observe, no_input, no_input, no_input, clock) ;
			   }

			   return S_OK ;
		} ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief focus readback中の失効・取消・例外・長時間処理を注入する。
		 * @return focus不成立caseは元E_ACCESSDENIED。それ以外はS_OK。

		 * @endcond
		 */
		const auto verify = [&]() {
			++verifications ;
			// 再確認後に認証世代を必ず読み直す
			if( mode == 8U) {
				++observed.epoch ;
			}

			// 再確認後に他入力との競合も読み直す
			if( mode == 9U) {
				observed.idle = false ;
			}

			// COM再入の取消をreadback成功で上書きしない
			if( mode == 10U) {
				controller.Cancel( Reason::Unavailable, busy) ;
			}

			// readback例外では送信しない
			if( mode == 15U) {
				throw 1 ;
			}

			// readbackの処理時間も古い入力の期限に含める
			if( mode == 21U) {
				now += rdp::RunInputTest::kInputExpiryMilliseconds ;
			}

			return mode == 7U ? E_ACCESSDENIED : S_OK ;
		} ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 固定入力要求の件数だけを数え、例外と再入を注入する。
		 * @return 通常はS_OK。実入力は行わない。

		 * @endcond
		 */
		const auto send = [&]() {
			++sends ;
			// fakeでも承認済みの固定入力だけを生成する
			if( !rdp::IsKeyboardEmulationTest( kind) && rdp::FixedInputTestKeys( kind).count == 0L) {
				return E_INVALIDARG ;
			}

			// API境界の例外でも二度送らない
			if( mode == 16U) {
				throw 1 ;
			}

			// SendKeys中のnested timerで第二の入力を開始しない
			if( mode == 19U) {
				controller.Advance( observe, no_input, no_input, no_input, clock) ;
			}

			return S_OK ;
		} ;
		// 確認完了ではfocusもキーも未実行である
		if( !controller.Begin( 7U, observe, confirm, clock) || preparations != 0U || sends != 0U ||
		   controller.phase() != Phase::WaitingPreparation ||
		   controller.Begin( 7U, observe, confirm, clock)) {
			return false ;
		}

		// 待機前の取消・切断・競合とtimer予約失敗を模擬する
		if( mode == 1U || mode == 11U) {
			controller.Cancel( mode == 1U ? Reason::Unavailable : Reason::TimerFailure,
							  mode == 1U ? busy : timer_error) ;
		}

		// 準備前に接続世代が変化する
		if( mode == 2U) {
			++observed.epoch ;
		}

		// 準備前に別アプリ前景等で競合する
		if( mode == 3U) {
			observed.idle = false ;
		}

		now += 99U ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		// 早着timerで準備・送信を省略して実行しない
		if( preparations != 0U || verifications != 0U || sends != 0U) {
			return false ;
		}

		now += mode == 12U ? rdp::RunInputTest::kInputExpiryMilliseconds : 1U ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		// 準備後の待機中にも世代・前景・STA失効を拒否する
		if( mode == 4U) {
			++observed.epoch ;
		}

		// 他アプリへの切替を安全条件の失効として模擬する
		if( mode == 5U) {
			observed.idle = false ;
		}

		// 別STAからtimer処理を呼んだ場合はCOMへ進まない
		if( mode == 6U) {
			observed.owner_thread = false ;
		}

		now += 249U ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		// focus待機中の早着timerではreadbackも送信もしない
		if( verifications != 0U || sends != 0U) {
			return false ;
		}

		now += mode == 13U ? rdp::RunInputTest::kInputExpiryMilliseconds : 1U ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		// 正常送信後の結果dialogを同じdispatchへ戻さない
		if( reasons[mode] == Reason::None && controller.phase() != Phase::WaitingResult) {
			return false ;
		}

		// 送信済みの受付を終了処理の取消で書き換えない
		if( mode == 20U) {
			controller.Cancel( Reason::Unavailable, busy) ;
		}

		now += 999U ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		// 通常の結果待ちを最小時間前に完了させない
		if( reasons[mode] == Reason::None && mode != 20U &&
		   controller.phase() != Phase::WaitingResult) {
			return false ;
		}

		++now ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		const auto result = controller.result() ;
		const bool prepare_expected =
			mode != 1U && mode != 2U && mode != 3U && mode != 11U && mode != 12U ;
		const bool verify_expected = prepare_expected && mode != 4U && mode != 5U && mode != 6U &&
									 mode != 13U && mode != 14U && mode != 17U ;
		const bool send_expected = reasons[mode] == Reason::None || mode == 16U ;
		// 最終数値と各callbackの回数を確認し、残留通知の再送を検出する
		if( controller.phase() != Phase::Complete || result.stage != stages[mode] ||
		   result.reason != reasons[mode] || result.result != results[mode] ||
		   preparations != (prepare_expected ? 1U : 0U) ||
		   verifications != (verify_expected ? 1U : 0U) || sends != (send_expected ? 1U : 0U)) {
			std::cout << "Deferred input case failed: " << mode << '\n' ;
			return false ;
		}
	}

	return true ;
}

/** @brief clipboardの数値世代と各待機境界の置換拒否を実clipboardなしで検証する。
 * @return 未取得・置換・確認中・準備中・再確認中の失効で入力しない場合true。
 */
bool TestClipboardInputController()
{
	namespace rdp = zrdp::client::rdp ;
	// 同じ非0世代だけを許可し、0、差異、wrap先を拒否する
	if( rdp::SameInputTestClipboard( 0U, 0U) || rdp::SameInputTestClipboard( 0U, 7U) ||
	   rdp::SameInputTestClipboard( 7U, 0U) || rdp::SameInputTestClipboard( 7U, 8U) ||
	   !rdp::SameInputTestClipboard( 7U, 7U) ||
	   !rdp::SameInputTestClipboard( UINT32_MAX, UINT32_MAX) ||
	   rdp::SameInputTestClipboard( UINT32_MAX, 0U)) {
		return false ;
	}

	// 初期、確認中、準備前、準備中、送信待ち、再確認中を独立したfakeで検証する
	for( unsigned int mode = 0U; mode < 6U; ++mode) {
		rdp::RunInputTest			 controller ;
		rdp::RunInputTestObservation observed{true, 7U, true, mode != 0U} ;
		std::uint64_t				 now		   = 1000U ;
		unsigned int				 confirmations = 0U, preparations = 0U, sends = 0U ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief clipboard内容ではなく数値条件だけを返す。 @return fakeの安全条件。
		 * @endcond
		 */
		const auto					 observe = [&]() { return observed ; } ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 確認中のclipboard置換だけを模擬する。 @return 明示承認。
		 * @endcond
		 */
		const auto					 confirm = [&]() {
			  ++confirmations ;
			  // modal確認で変わったclipboardにも送信しない
			  if( mode == 1U) {
				  observed.clipboard_unchanged = false ;
			  }

			  return true ;
		} ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief focus準備中のclipboard変更を模擬する。 @return S_OK。
		 * @endcond
		 */
		const auto prepare = [&]() {
			++preparations ;
			// COM準備から復帰した時も元世代を再確認する
			if( mode == 3U) {
				observed.clipboard_unchanged = false ;
			}

			return S_OK ;
		} ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief readback中のclipboard置換を模擬する。 @return S_OK。
		 * @endcond
		 */
		const auto verify = [&]() {
			// 送信前の再確認から復帰しても元世代へ限定する
			if( mode == 5U) {
				observed.clipboard_unchanged = false ;
			}

			return S_OK ;
		} ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 実キーを送らず、誤った到達を数える。 @return S_OK。
		 * @endcond
		 */
		const auto send = [&]() {
			++sends ;
			return S_OK ;
		} ;
		/**
		 * @cond LOCAL_CALLBACK_CONTRACT @brief 有限fake時刻を返す。 @return 仮想ms値。
		 * @endcond
		 */
		const auto clock = [&]() { return now ; } ;
		static_cast<void>( controller.Begin( 7U, observe, confirm, clock)) ;
		// 確認後の準備待ちでもclipboard変更を検出する
		if( mode == 2U) {
			observed.clipboard_unchanged = false ;
		}

		now += 100U ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		// focus準備後の待機中にもclipboardの置換を拒否する
		if( mode == 4U) {
			observed.clipboard_unchanged = false ;
		}

		now += 250U ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		controller.Advance( observe, prepare, verify, send, clock) ;
		// 失効は明示分類で停止し、準備／送信をやり直さない
		if( controller.phase() != rdp::RunInputTestPhase::Complete ||
		   controller.result().reason != rdp::RunInputTestReason::ClipboardChanged ||
		   controller.result().result != HRESULT_FROM_WIN32( ERROR_RETRY) || sends != 0U ||
		   confirmations != (mode == 0U ? 0U : 1U) || preparations != (mode >= 3U ? 1U : 0U)) {
			return false ;
		}
	}

	return true ;
}

/** @brief 固定Paste／Enterの送信callback内の再承認をfake native境界で検証する。
 * @return 正常だけ一回挿入し、COM再入の失効・期限・取消を拒否する場合true。
 */
bool TestDiagnosticSendAuthorization()
{
	namespace rdp = zrdp::client::rdp ;
	// 送信phase以外の承認と、8種類の送信callback内条件を二つの固定列で確認する
	for( const auto kind : {rdp::FixedKeyboardKind::Paste, rdp::FixedKeyboardKind::Enter}) {
		// 正常、世代、STA、競合、clipboard、取消から復帰、期限、clock逆転を検証する
		for( unsigned int mode = 0U; mode < 8U; ++mode) {
			rdp::RunInputTest			 controller ;
			rdp::RunInputTestObservation observed{true, 7U, true, true} ;
			std::uint64_t				 now		= 1000U ;
			unsigned int				 insertions = 0U ;
			rdp::RKeyEmulationResult	 native{} ;
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT @brief 現在の数値条件だけを返す。 @return fake観測。
			 * @endcond
			 */
			const auto					 observe  = [&]() { return observed ; } ;
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT @brief 明示承認を注入する。 @return true。
			 * @endcond
			 */
			const auto					 confirm  = []() { return true ; } ;
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT @brief focus操作なしの正常準備を返す。 @return S_OK。
			 * @endcond
			 */
			const auto					 prepared = []() { return S_OK ; } ;
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT @brief 仮想clockだけを返す。 @return ms値。
			 * @endcond
			 */
			const auto					 clock	  = [&]() { return now ; } ;
			// 未開始では既知の世代でも入力承認を与えない
			if( controller.CanSend( now, observed) ||
			   !controller.Begin( 7U, observe, confirm, clock) ||
			   controller.CanSend( now, observed)) {
				return false ;
			}

			/**
			 * @cond LOCAL_CALLBACK_CONTRACT @brief COM照会後の失効を模擬し、固定factoryだけをfakeへ渡す。 @return 元HRESULT。
			 * @endcond
			 */
			const auto send = [&]() {
				// 同じ画面でも新しい認証世代へ入力しない
				if( mode == 1U) {
					++observed.epoch ;
				}

				// 別STAの観測へ入力承認を与えない
				if( mode == 2U) {
					observed.owner_thread = false ;
				}

				// 前景・競合が変わった場合も拒否する
				if( mode == 3U) {
					observed.idle = false ;
				}

				// COM照会中のclipboard変更でも固定Pasteを拒否する
				if( mode == 4U) {
					observed.clipboard_unchanged = false ;
				}

				// 取消後に前景が戻っても承認を復活させない
				if( mode == 5U) {
					controller.Cancel( rdp::RunInputTestReason::Unavailable, E_ABORT) ;
					observed.idle = true ;
				}

				// COM照会の処理時間も確認開始からの有限期限に含める
				if( mode == 6U) {
					now = 4000U ;
				}

				// clockの逆転も現在の画面へ送信しない
				if( mode == 7U) {
					now = 999U ;
				}

				/**
				 * @cond LOCAL_CALLBACK_CONTRACT @brief 固定列の受付数だけを返し、実入力しない。 @param[in] inputs 固定列。 @return fake全件受付。
				 * @endcond
				 */
				const auto insert = [&]( const auto &inputs) {
					++insertions ;
					return rdp::RKeyEmulationInsertion{static_cast<UINT>( inputs.size()), 0U} ;
				} ;
				native = rdp::RunFixedKeyboardInput(
					kind, {true, true, true, true, S_OK, controller.CanSend( now, observed)},
					insert) ;
				return native.result ;
			} ;
			now += 100U ;
			controller.Advance( observe, prepared, prepared, send, clock) ;
			now += 250U ;
			controller.Advance( observe, prepared, prepared, send, clock) ;
			// 元受付を記録する段階では再承認を与えず、失効時はbackendが0回である
			if( controller.CanSend( now, observed) || insertions != (mode == 0U ? 1U : 0U) ||
			   native.reason != (mode == 0U ? rdp::RKeyEmulationReason::None
											: rdp::RKeyEmulationReason::AuthorizationLost) ||
			   native.result != (mode == 0U ? S_OK : HRESULT_FROM_WIN32( ERROR_CANCELLED)) ||
			   native.inserted !=
				   (mode == 0U ? (kind == rdp::FixedKeyboardKind::Paste ? 4U : 2U) : 0U)) {
				return false ;
			}
		}
	}

	return true ;
}

/** @brief fixture process全体のATL containment lifetimeを所有する。 */
class FixtureAtlModule final : public ATL::CAtlExeModuleT<FixtureAtlModule> {} ;
// ActiveXの生成前から破棄後までATL moduleを維持する
FixtureAtlModule	   fixture_module ;
/** @brief 実履歴・Agent設定へ一切接続しないUI observer。 */
class Observer		   final : public zrdp::client::app::ISessionWindowObserver {
public:
	/** @brief UI fixtureでは設定queueを生成しない。
	 * @param[in] session_id 未使用の固定fixture ID。
	 * @param[in] epoch 未使用の認証世代。
	 * @return 常にfalse。
	 */
	[[nodiscard]] bool RequestSessionAgentSettings( std::uint64_t session_id,
														   std::uint64_t epoch) noexcept override
	{
		static_cast<void>( session_id) ;
		static_cast<void>( epoch) ;
		return false ;
	}

	/** @brief 実履歴へ書き込まないfake成功通知。
	 * @param[in] connection_name 未使用の接続名。
	 * @param[in] server 未使用の接続先。
	 * @param[in] domain 未使用のdomain。
	 * @param[in] user_name 未使用のユーザー名。
	 * @param[in] audio 未使用の音声設定。
	 */
	void RememberSuccessfulConnection(
				std::wstring connection_name, std::wstring server, std::wstring domain,
				std::wstring								   user_name,
				zrdp::client::rdp::RdpAudioRedirectionSettings audio) noexcept override
	{
		static_cast<void>( connection_name) ;
		static_cast<void>( server) ;
		static_cast<void>( domain) ;
		static_cast<void>( user_name) ;
		static_cast<void>( audio) ;
	}
} ;
}

/** @brief 通信を止めた実session UIの一行を確認する。
 * @param[in] argc preview有無の引数件数。
 * @param[in] argv 固定のtoolbar／確認／数値結果previewだけを受け付ける。
 * @return 表示と無効化が正しい場合は0。
 */
int wmain( int argc, wchar_t **argv)
{
	// pure controllerはWindow生成前に全停止条件を検証する
	if( !TestInputTestKeys() || !TestKeyEmulation( false) || !TestKeyEmulation( true) ||
	   !TestProductionKeyboardInput() || !TestFixedInputSequence() ||
	   !TestClipboardInputController() || !TestDiagnosticSendAuthorization()) {
		return 6 ;
	}

	// 追加した単独入力にも全35停止・待機条件を同じfakeで適用する
	for( const auto kind :
		{zrdp::client::rdp::InputTestKind::WinR, zrdp::client::rdp::InputTestKind::WindowsKey,
		 zrdp::client::rdp::InputTestKind::LetterR,
		 zrdp::client::rdp::InputTestKind::LetterREmulation,
		 zrdp::client::rdp::InputTestKind::WinREmulation,
		 zrdp::client::rdp::InputTestKind::PasteEmulation,
		 zrdp::client::rdp::InputTestKind::EnterEmulation}) {
		// いずれかの種別で安全条件が崩れた場合はWindowを生成しない
		if( !TestRunInputController( kind) || !TestDeferredRunInputController( kind)) {
			return 6 ;
		}
	}

	SetProcessDpiAwarenessContext( DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) ;
	// ActiveXは既存WindowsのSTA上で生成するだけで接続しない
	if( FAILED( CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED))) {
		return 1 ;
	}

	int result = 0 ;
	// USER32標準のbuttonとcomboだけを使い、ATL containmentを登録する
	if( !ATL::AtlAxWinInit()) {
		std::cout << "ATL initialization failed: " << GetLastError() << '\n' ;
		CoUninitialize() ;
		return 2 ;
	}

	{
		Observer								 observer ;
		zrdp::client::app::SessionLaunchSettings settings{} ;
		settings.connection.server			= L"192.0.2.1" ;
		settings.connection.connection_name = L"UI test" ;
		settings.session_id					= 1U ;
		settings.proxy_configuration_error =
			zrdp::client::proxy::ClientProxyConfigurationError::InvalidPort ;
		zrdp::client::app::MainWindow window( GetModuleHandleW( nullptr), observer, settings) ;
		// 生成失敗ではmessage loopを開始しない
		if( !window.Create( SW_SHOWNORMAL)) {
			result = 2 ;
		} else {
			MSG automatic{} ;
			// 本fixtureだけで固定自動接続messageを捨て、実接続を防ぐ
			if( !PeekMessageW( &automatic, window.window(), WM_APP + 7U, WM_APP + 7U, PM_REMOVE)) {
				result = 3 ;
			}

			// 非接続sessionの通常focus要求でも非表示の接続欄へ移さない
			const HWND marker =
				CreateWindowExW( 0U, L"BUTTON", L"Focus marker", WS_CHILD, 0, 0, 100, 24,
								window.window(), nullptr, GetModuleHandleW( nullptr), nullptr) ;
			// fixture内だけのfocusを準備できない場合も未検証として失敗させる
			if( marker == nullptr) {
				result = 9 ;
			} else {
				SetFocus( marker) ;
				const bool prepared = GetFocus() == marker ;
				SendMessageW( window.window(), WM_SETFOCUS, reinterpret_cast<WPARAM>( marker), 0L) ;
				// WM_SETFOCUSが既存のMicrosoft UIのfocusを置き換えないことを確認する
				if( !prepared || GetFocus() != marker) {
					result = 9 ;
				}

				// 過去の認証完了messageが残っても未認証Windowへfocusを渡さない
				SendMessageW( window.window(), WM_APP + 8U, 0U, 0L) ;
				// 遅延messageも通常focusと同じ認証条件を維持する
				if( GetFocus() != marker) {
					result = 9 ;
				}

				DestroyWindow( marker) ;
			}

			std::array<wchar_t, 1024U> initial_title{} ;
			GetWindowTextW( window.window(), initial_title.data(),
						   static_cast<int>( initial_title.size())) ;
			// 認証前の空設定は要設定と誤表示せず、従来の停止表示を維持する
			if( std::wstring_view( initial_title.data()).find( L"Agent:停止") ==
				   std::wstring_view::npos ||
			   std::wstring_view( initial_title.data()).find( L"Agent:要設定") !=
				   std::wstring_view::npos) {
				result = 10 ;
			}

			// 本番ownerの未認証guardはclipboard変更前に詳細を返す
			const auto stopped	= window.StartAutomaticDeployment( 1U) ;
			const auto snapshot = window.AutomaticDeploymentStatus() ;
			// 最初の停止理由と元HRESULTをUIの所有結果として保持する
			if( stopped != HRESULT_FROM_WIN32( ERROR_INVALID_STATE) || snapshot.status != -1 ||
			   snapshot.stage !=
				   zrdp::client::agent_deployment::AutomaticDeploymentStage::Validation ||
			   snapshot.reason !=
				   zrdp::client::agent_deployment::AutomaticDeploymentReason::SessionChanged ||
			   snapshot.result != stopped) {
				result = 5 ;
			}

			RECT bounds{} ;
			// 起動時Hidden指定のpreviewでも所有するfixtureだけを再表示する
			ShowWindow( window.window(), SW_SHOW) ;
			const int dpi = static_cast<int>( GetDpiForWindow( window.window())) ;
			SetWindowPos( window.window(), nullptr, 0, 0, MulDiv( 1140, dpi, 96),
						 MulDiv( 720, dpi, 96), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ;
			GetClientRect( window.window(), &bounds) ;
			RECT previous{} ;
			// 全画面入口は未認証では表示するが無効とし、一行の表示操作欄へ収める
			for( int id : {IDC_FULL_SCREEN}) {
				const HWND control = GetDlgItem( window.window(), id) ;
				RECT	   rect{} ;
				GetWindowRect( control, &rect) ;
				MapWindowPoints( nullptr, window.window(), reinterpret_cast<POINT *>( &rect), 2U) ;
				// 入口欠落・切れ・重なり・認証前有効化を拒否する
				if( control == nullptr || !IsWindowVisible( control) || IsWindowEnabled( control) ||
				   rect.right > bounds.right ||
				   (previous.right != 0L &&
					(rect.left < previous.right || rect.top != previous.top))) {
					result = 4 ;
				}

				previous = rect ;
			}

			// Agent配置・起動および開発用入力の旧入口を製品sessionへ再公開しない
			if( GetDlgItem( window.window(), IDC_SESSION_AGENT_DEPLOY) != nullptr ||
			   GetDlgItem( window.window(), IDC_SESSION_AGENT_START) != nullptr ||
			   GetDlgItem( window.window(), IDC_SESSION_RUN_INPUT_TEST) != nullptr) {
				result = 8 ;
			}

			// 未認証ではmenu入口も各固定入力commandも開始しない
			for( const auto command :
				{IDC_SESSION_RUN_INPUT_TEST, IDM_INPUT_TEST_WIN_R, IDM_INPUT_TEST_WIN_ONLY,
				 IDM_INPUT_TEST_R_ONLY, IDM_INPUT_TEST_R_EMULATION, IDM_INPUT_TEST_WIN_R_EMULATION,
				 IDM_INPUT_TEST_PASTE_EMULATION, IDM_INPUT_TEST_ENTER_EMULATION}) {
				SendMessageW( window.window(), WM_COMMAND, MAKEWPARAM( command, BN_CLICKED), 0L) ;
			}

			// 未予約・過去ID・将来IDのtimerで未認証fixtureを進めない
			for( UINT_PTR id : {0x800U, 0x801U, 0x802U}) {
				SendMessageW( window.window(), WM_TIMER, id, 0L) ;
			}

			const auto after_test = window.AutomaticDeploymentStatus() ;
			// 未認証commandでも配置snapshotを変えず、modal確認や入力を開始しない
			if( after_test.status != snapshot.status || after_test.stage != snapshot.stage ||
			   after_test.reason != snapshot.reason || after_test.result != snapshot.result) {
				result = 7 ;
			}

			// toolbar previewも自動接続messageを除去済みの場合だけ許可する
			if( result == 0 && argc == 2 && std::wstring_view( argv[1]) == L"--preview") {
				MSG message{} ;
				// fixture終了まで実UIだけをdispatchする
				while( window.window() != nullptr && GetMessageW( &message, nullptr, 0U, 0U) > 0) {
					// dialogが未処理の場合だけ標準dispatchする
					if( !window.ProcessDialogMessage( message)) {
						TranslateMessage( &message) ;
						DispatchMessageW( &message) ;
					}
				}
			}

			// 未接続fixtureの画面だけを回収し、任意processを終了しない
			if( window.window() != nullptr) {
				DestroyWindow( window.window()) ;
			}
		}
	}

	CoUninitialize() ;
	std::cout << "Session controls result: " << result << '\n' ;
	return result ;
}
