//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file rdp_run_input_test.h
 * @brief 配置を行わず、確認済みの固定入力を一回だけ検証する。
 * @version 01.00.04
 * @date 2026/10/09
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/09  未署名でも使える固定入力test境界を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/09  確認・focus・送信・結果表示を有限待機で分離。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  単独キー診断も同じ一回入力の契約へ限定。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  明示したR比較にも既存の有限入力境界を適用。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  配置なしの固定Ctrl+V／Enter診断と失効確認を追加。
 * </pre>
 * @note callbackは同じ所有STAからだけ呼ぶ。clipboard内容、file、任意commandを扱わない。
 */
//------------------------------------------------------------------------------
#pragma once
#include <Windows.h>
#include <cstdint>

namespace zrdp::client::rdp {
/** @brief clipboard内容を扱わず、Pasteの非0世代だけを照合する。
 * @param[in] expected 明示確認を開始した世代番号。
 * @param[in] current 送信直前に観測した世代番号。
 * @return 同じ非0世代の場合だけtrue。内容やRDP同期の保証ではない。
 */
[[nodiscard]] constexpr bool SameInputTestClipboard( std::uint32_t expected,
													std::uint32_t current) noexcept
{
	return expected != 0U && expected == current ;
}

/** @brief 開発用固定入力testの段階。自動配置の進行とは独立する。 */
enum class RunInputTestStage : std::uint8_t {
	/// 所有STA、認証世代、他操作との競合を確認する。
	Validation	 = 1,
	/// 利用者が選択した固定キー列一回を承認する。
	Confirmation = 2,
	/// 標準キー転送設定とControl focusを確認する。
	Preparation	 = 3,
	/// 確認した方式の固定キー列だけを一回要求する。
	SendRun		 = 4,
} ;
/** @brief 入力testの固定停止分類。 */
enum class RunInputTestReason : std::uint8_t {
	/// Microsoft APIが入力要求を受付けた。
	None				= 0,
	/// 同じ非0認証世代を確認できない。
	SessionChanged		= 1,
	/// 他入力操作、警告、終了等と競合する。
	Unavailable			= 2,
	/// 所有STA以外から呼ばれた。
	WrongThread			= 3,
	/// 利用者が確認を取消した。
	Declined			= 4,
	/// 入力準備APIで失敗した。
	PreparationFailure	= 5,
	/// 確認済み固定キー列の送信APIで失敗した。
	InputFailure		= 6,
	/// 例外を安全な停止へ変換した。
	UnexpectedException = 7,
	/// timer予約失敗または入力前の有限期限へ到達した。
	TimerFailure		= 8,
	/// 貼付け診断のclipboard世代が0または確認後に変わった。
	ClipboardChanged	= 9,
} ;
/** @brief 同じ所有STAが観測した、機密を含まない安全条件。 */
struct RunInputTestObservation {
	/// 呼出しthreadが対象sessionの所有STAである。
	bool		  owner_thread ;
	/// 現在の認証世代。未認証・切断中は0。
	std::uint64_t epoch ;
	/// 他の固定入力、認証警告、終了等と競合しない。
	bool		  idle ;
	/// 貼付けでは確認開始時と同じclipboard世代。他の診断はtrue。
	bool		  clipboard_unchanged{true} ;
} ;
/** @brief 入力受付と接続先の実表示を区別する数値結果。 */
struct RunInputTestResult {
	/// 最後に実行または拒否した固定段階。
	RunInputTestStage  stage{RunInputTestStage::Validation} ;
	/// 固定分類。Noneだけが入力受付を示す。
	RunInputTestReason reason{RunInputTestReason::SessionChanged} ;
	/// 失敗した境界の元HRESULTまたは固定分類値。
	HRESULT			   result{E_UNEXPECTED} ;
} ;
/** @brief 固定した世代と現在の安全条件を照合する。
 * @param[in] epoch 確認開始時の認証世代。
 * @param[in] observed 現在の所有STAによる観測値。
 * @return 全条件成立はS_OK。失効時は固定分類。
 */
[[nodiscard]] inline RunInputTestResult
CheckRunInputTest( std::uint64_t epoch, const RunInputTestObservation &observed) noexcept
{
	// 別threadでは確認UIやCOM callbackへ進まない
	if( !observed.owner_thread) {
		return {RunInputTestStage::Validation, RunInputTestReason::WrongThread, RPC_E_WRONG_THREAD} ;
	}

	// 未認証と再接続後の旧確認を拒否する
	if( epoch == 0U || observed.epoch != epoch) {
		return {RunInputTestStage::Validation, RunInputTestReason::SessionChanged,
				HRESULT_FROM_WIN32( ERROR_INVALID_STATE)} ;
	}

	// 他入力、警告、終了中にRun dialogを開かない
	if( !observed.idle) {
		return {RunInputTestStage::Validation, RunInputTestReason::Unavailable,
				HRESULT_FROM_WIN32( ERROR_BUSY)} ;
	}

	// 内容を読まず、貼付けの確認と同じ世代だけを許可する
	if( !observed.clipboard_unchanged) {
		return {RunInputTestStage::Validation, RunInputTestReason::ClipboardChanged,
				HRESULT_FROM_WIN32( ERROR_RETRY)} ;
	}

	return {RunInputTestStage::Validation, RunInputTestReason::None, S_OK} ;
}

/** @brief 所有STAのtimerで分離する入力testの内部phase。 */
enum class RunInputTestPhase : std::uint8_t {
	/// 試行を開始していない。
	Idle,
	/// 明示確認のmodal境界にいる。
	Confirmation,
	/// 確認終了後のUI処理復帰を待つ。
	WaitingPreparation,
	/// focus準備callback中。timer再入は無視する。
	Preparing,
	/// focus移動後のUI処理復帰を待つ。
	WaitingSend,
	/// focusを変更せず送信直前条件を読む。
	Verifying,
	/// 一回の固定入力callback中。追加送信しない。
	Sending,
	/// remote入力の処理時間を残し、結果dialogを待つ。
	WaitingResult,
	/// 完了または停止。残留timerでは何も行わない。
	Complete,
} ;

/** @brief callbackを保持せず、確認と一回入力を有限timerへ分離する。
 * @note 同じ所有STAだけで使用する。clockはGetTickCount64相当の単調ms値。
 */
class RunInputTest final {
public:
	/// 確認終了からfocus準備までの最小待機ms。
	static constexpr std::uint64_t kConfirmationSettleMilliseconds = 100U ;
	/// focus準備終了から再確認と送信までの最小待機ms。
	static constexpr std::uint64_t kFocusSettleMilliseconds		   = 250U ;
	/// 固定入力API終了から結果表示までの最小待機ms。
	static constexpr std::uint64_t kResultSettleMilliseconds	   = 1000U ;
	/// 確認終了から送信までに許す上限ms。遅延したtimerで入力しない。
	static constexpr std::uint64_t kInputExpiryMilliseconds		   = 3000U ;
	/** @brief 未開始の状態機械を生成する。 */
	RunInputTest() noexcept										   = default ;
	/** @brief 明示確認だけを同期実行し、準備を将来のtimerへ予約する。
	 * @param[in] epoch 確認開始時に固定する認証世代。
	 * @param[in] observe 所有STA・世代・競合を観測する境界。
	 * @param[in] confirm 既定取消の明示確認。
	 * @param[in] clock 確認終了時の単調ms値を返す境界。
	 * @return 準備待ちへ進めた場合true。再開始・失敗・取消はfalse。
	 */
	template<class Observe, class Confirm, class Clock>
	[[nodiscard]] bool Begin( std::uint64_t epoch, Observe &&observe, Confirm &&confirm,
							 Clock &&clock) noexcept
	{
		// 同じ試行を確認や結果待ち中に再開始しない
		if( phase_ != RunInputTestPhase::Idle) {
			return false ;
		}

		epoch_ = epoch ;
		try {
			const auto before = CheckRunInputTest( epoch_, observe()) ;
			// 初期条件が成立しなければ確認もfocusも操作しない
			if( before.reason != RunInputTestReason::None) {
				Stop( before) ;
				return false ;
			}

			phase_		  = RunInputTestPhase::Confirmation ;
			result_.stage = RunInputTestStage::Confirmation ;
			// 明示承認なしではtimerへ進まない
			if( !confirm()) {
				Stop( {result_.stage, RunInputTestReason::Declined,
					  HRESULT_FROM_WIN32( ERROR_CANCELLED)}) ;
				return false ;
			}

			// nested終了処理で取消済みなら、確認結果で待機を復活させない
			if( phase_ != RunInputTestPhase::Confirmation) {
				return false ;
			}

			const auto after = CheckRunInputTest( epoch_, observe()) ;
			// modal境界で失効した世代や競合を再確認する
			if( after.reason != RunInputTestReason::None) {
				Stop( after) ;
				return false ;
			}

			phase_tick_ = input_start_tick_ = clock() ;
			result_.stage					= RunInputTestStage::Preparation ;
			phase_							= RunInputTestPhase::WaitingPreparation ;
			return true ;
		} catch( ...) {
			Stop( {result_.stage, RunInputTestReason::UnexpectedException, E_UNEXPECTED}) ;
			return false ;
		}
	}

	/** @brief 一回のtimerで準備、送信、結果待ちのうち一段だけを進める。
	 * @param[in] observe 所有STA・同じ世代・前景・競合を確認する境界。
	 * @param[in] prepare focusを一回準備する境界。
	 * @param[in] verify focusを変更せず再確認する境界。
	 * @param[in] send_run 確認済み固定キー列を一回だけ要求する境界。
	 * @param[in] clock 単調ms値を返す境界。
	 * @note callback中のtimer再入、早着通知、取消後、完了後に追加入力しない。
	 */
	template<class Observe, class Prepare, class Verify, class SendRun, class Clock>
	void Advance( Observe &&observe, Prepare &&prepare, Verify &&verify, SendRun &&send_run,
				 Clock &&clock) noexcept
	{
		// 所有STAのcallback境界の例外を固定停止へ変換する
		try {
			// 結果待ちでは入力条件の確認やfocusの変更を追加しない
			if( phase_ == RunInputTestPhase::WaitingResult) {
				// unsigned差分でdeadline加算のoverflowを避ける
				if( clock() - phase_tick_ >= kResultSettleMilliseconds) {
					phase_ = RunInputTestPhase::Complete ;
				}

				return ;
			}

			// callback中の再入と残留timerは全境界を実行せず終了する
			if( phase_ != RunInputTestPhase::WaitingPreparation &&
			   phase_ != RunInputTestPhase::WaitingSend) {
				return ;
			}

			const auto checked = CheckRunInputTest( epoch_, observe()) ;
			// 待機中に失効した条件ではfocusを奪い返さない
			if( checked.reason != RunInputTestReason::None) {
				Stop( checked) ;
				return ;
			}

			const auto now = clock() ;
			// 大幅に遅れたtimerで古い明示確認の入力を実行しない
			if( now - input_start_tick_ >= kInputExpiryMilliseconds) {
				Stop( {RunInputTestStage::Preparation, RunInputTestReason::TimerFailure,
					  HRESULT_FROM_WIN32( WAIT_TIMEOUT)}) ;
				return ;
			}

			// 準備は確認終了後の別dispatchで一回だけ行う
			if( phase_ == RunInputTestPhase::WaitingPreparation) {
				// 早着した旧timerで最小待機を省略しない
				if( now - phase_tick_ < kConfirmationSettleMilliseconds) {
					return ;
				}

				phase_				   = RunInputTestPhase::Preparing ;
				const HRESULT prepared = prepare() ;
				// callbackからの取消を後続処理で上書きしない
				if( phase_ != RunInputTestPhase::Preparing) {
					return ;
				}

				// 元HRESULTを保持し、S_FALSEも準備成功と扱わない
				if( prepared != S_OK) {
					Stop( {RunInputTestStage::Preparation, RunInputTestReason::PreparationFailure,
						  prepared}) ;
					return ;
				}

				const auto after = CheckRunInputTest( epoch_, observe()) ;
				// focus中のCOM再入で世代や競合が変わった場合も停止する
				if( after.reason != RunInputTestReason::None) {
					Stop( after) ;
					return ;
				}

				phase_tick_ = clock() ;
				phase_		= RunInputTestPhase::WaitingSend ;
				return ;
			}

			// focus準備後の別dispatchまで固定入力APIを呼ばない
			if( now - phase_tick_ < kFocusSettleMilliseconds) {
				return ;
			}

			phase_				   = RunInputTestPhase::Verifying ;
			const HRESULT verified = verify() ;
			// 非active化等の取消をreadback成功で取り消さない
			if( phase_ != RunInputTestPhase::Verifying) {
				return ;
			}

			// focusを取り戻さず、読み取りだけで成立しない場合は停止する
			if( verified != S_OK) {
				Stop( {RunInputTestStage::Preparation, RunInputTestReason::PreparationFailure,
					  verified}) ;
				return ;
			}

			const auto after = CheckRunInputTest( epoch_, observe()) ;
			// readbackのCOM再入後にも同じsessionへだけ送る
			if( after.reason != RunInputTestReason::None) {
				Stop( after) ;
				return ;
			}

			// verifyの処理時間も入力期限に含める
			if( clock() - input_start_tick_ >= kInputExpiryMilliseconds) {
				Stop( {RunInputTestStage::Preparation, RunInputTestReason::TimerFailure,
					  HRESULT_FROM_WIN32( WAIT_TIMEOUT)}) ;
				return ;
			}

			phase_			   = RunInputTestPhase::Sending ;
			result_.stage	   = RunInputTestStage::SendRun ;
			const HRESULT sent = send_run() ;
			result_			   = {RunInputTestStage::SendRun,
						  sent == S_OK ? RunInputTestReason::None : RunInputTestReason::InputFailure,
								  sent} ;
			phase_tick_		   = clock() ;
			phase_			   = RunInputTestPhase::WaitingResult ;
		} catch( ...) {
			Stop( {result_.stage, RunInputTestReason::UnexpectedException, E_UNEXPECTED}) ;
		}
	}

	/** @brief 未送信の操作を失効させ、送信済みの場合は元結果だけを保持する。
	 * @param[in] reason 停止の固定分類。
	 * @param[in] result 元HRESULTまたは固定拒否値。
	 */
	void Cancel( RunInputTestReason reason, HRESULT result) noexcept
	{
		// SendInput前のCOM再入でも失効し、既に送ったAPI結果は後でそのまま保持する
		if( phase_ == RunInputTestPhase::Sending) {
			send_invalidated_ = true ;
		}

		// API呼出し後の受付結果を取消・timer失敗で上書きしない
		if( phase_ == RunInputTestPhase::WaitingResult) {
			phase_ = RunInputTestPhase::Complete ;
		}

		// 既完了と送信中には停止結果を重ねない
		if( phase_ != RunInputTestPhase::Complete && phase_ != RunInputTestPhase::Sending) {
			// 初期失敗と未送信の取消だけを固定結果へ変換する
			Stop( {result_.stage, reason, result}) ;
		}
	}

	/** @brief 現在のphaseを返す。 @return timerの進行状態。 */
	[[nodiscard]] RunInputTestPhase phase() const noexcept
	{
		return phase_ ;
	}

	/** @brief 元結果を返す。 @return remote表示成功を含まないsnapshot。 */
	[[nodiscard]] RunInputTestResult result() const noexcept
	{
		return result_ ;
	}

	/** @brief 対象の認証世代を返す。 @return 開始前は0。 */
	[[nodiscard]] std::uint64_t epoch() const noexcept
	{
		return epoch_ ;
	}

	/** @brief 送信callback内のCOM照会後にも元の確認を一回入力へ承認する。
	 * @param[in] now 現在の単調ms値。実clock操作はownerが行う。
	 * @param[in] observed 同じ世代・前景・競合・clipboard世代の現在条件。
	 * @return 送信予約中、失効なし、期限内、全条件成立の場合だけtrue。
	 */
	[[nodiscard]] bool CanSend( std::uint64_t				  now,
							   const RunInputTestObservation &observed) const noexcept
	{
		return phase_ == RunInputTestPhase::Sending && !send_invalidated_ &&
			   now >= input_start_tick_ && now - input_start_tick_ < kInputExpiryMilliseconds &&
			   CheckRunInputTest( epoch_, observed).reason == RunInputTestReason::None ;
	}

	/** @brief focus準備または未送信の操作中かを返す。 @return 取消が入力を防止する場合true。 */
	[[nodiscard]] bool pending_input() const noexcept
	{
		return phase_ == RunInputTestPhase::WaitingPreparation ||
			   phase_ == RunInputTestPhase::Preparing || phase_ == RunInputTestPhase::WaitingSend ||
			   phase_ == RunInputTestPhase::Verifying ;
	}

private:
	/** @brief 最初の停止結果を保持して完了する。
	 * @param[in] result 固定段階、分類、元HRESULT。
	 */
	void Stop( RunInputTestResult result) noexcept
	{
		result_ = result ;
		phase_	= RunInputTestPhase::Complete ;
	}

	/// 所有STAだけが進める有限phase。
	RunInputTestPhase  phase_{RunInputTestPhase::Idle} ;
	/// 確認開始時に固定した認証世代。
	std::uint64_t	   epoch_{0U} ;
	/// 確認終了時の単調ms値。入力期限の基準。
	std::uint64_t	   input_start_tick_{0U} ;
	/// 最後のphase開始ms値。最小待機の基準。
	std::uint64_t	   phase_tick_{0U} ;
	/// 入力受付とremote表示を分離した元結果。
	RunInputTestResult result_{} ;
	/// 送信callbackのCOM再入中に取消された承認は、前景が戻っても復活させない。
	bool			   send_invalidated_{false} ;
} ;
} // namespace zrdp::client::rdp
