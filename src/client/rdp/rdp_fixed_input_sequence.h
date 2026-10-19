//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file rdp_fixed_input_sequence.h
 * @brief 固定Agent入力の有限待機と一回要求を実APIなしで管理する。
 * @version 01.00.01
 * @date 2026/10/10
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/10  固定三段階と単独Enterの取消・期限を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  完了入力sequenceの限定初期化と二回目起動を修正。
 * </pre>
 * @note 所有STA専用。callback、HWND、clipboard、文字列を保持しない。
 */
//------------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <limits>

namespace zrdp::client::rdp {
/** @brief 任意commandへ拡張しない固定入力段階。 */
enum class FixedInputStep : std::uint8_t {
	/// 未開始または早着通知の無処理値。
	Idle	 = 0,
	/// 固定Win+R。
	Run		 = 1,
	/// 固定Ctrl+V。
	Paste	 = 2,
	/// 固定Enter。
	Enter	 = 3,
	/// 入力受付済み。Agent成功とは別である。
	Complete = 4,
	/// 失効。Beginでも再開しない。
	Stopped	 = 5,
} ;

/** @brief 固定入力だけを時刻・一回要求・再入拒否で進める非I/O境界。 */
class FixedInputSequence final {
public:
	/// 初回focus準備から入力までの最小ms。
	static constexpr std::uint64_t kFirstDelay = 350U ;
	/// 後続入力までの最小ms。
	static constexpr std::uint64_t kNextDelay  = 700U ;
	/// 予定時刻を過ぎた入力を捨てる有限ms。
	static constexpr std::uint64_t kExpiry	   = 3000U ;
	/** @brief 初回のRunまたは確認済みEnterだけを予約する。
	 * @param[in] now focus準備完了後の単調ms。
	 * @param[in] first RunまたはEnter。任意途中段階は拒否する。
	 * @return 初回予約を成立させた場合だけtrue。
	 */
	[[nodiscard]] bool Begin( std::uint64_t now, FixedInputStep first = FixedInputStep::Run) noexcept
	{
		// 取消・完了後と未承認途中段階の再利用を拒否する
		if( step_ != FixedInputStep::Idle ||
		   (first != FixedInputStep::Run && first != FixedInputStep::Enter)) {
			return false ;
		}

		step_ = first ;
		return Schedule( now, kFirstDelay) ;
	}

	/** @brief 期限内の予定段階を一回だけ予約し、再入通知を無処理にする。
	 * @param[in] now 現在の単調ms。
	 * @return 入力予定段階。早着・再入・終了・失効ではIdle。
	 */
	[[nodiscard]] FixedInputStep Claim( std::uint64_t now) noexcept
	{
		// 終了後と再入の通知を入力要求へ変換しない
		if( !pending() || in_flight_) {
			return FixedInputStep::Idle ;
		}

		// clock逆転と遅延しすぎた入力は現在の画面へ送らない
		if( expired( now)) {
			Cancel() ;
			return FixedInputStep::Idle ;
		}

		// 最小待機前の早着通知を無視する
		if( now < due_) {
			return FixedInputStep::Idle ;
		}

		in_flight_ = true ;
		claimed_   = now ;
		return step_ ;
	}

	/** @brief 一回要求の結果を反映し、取消中のcallback復帰でも復活させない。
	 * @param[in] now API終了後の単調ms。
	 * @param[in] accepted 固定列の全件受付だけtrue。
	 * @return 次段階予約または入力完了を成立させた場合true。
	 */
	[[nodiscard]] bool Complete( std::uint64_t now, bool accepted) noexcept
	{
		// 未予約・取消後・二重完了の通知で次の入力を追加しない
		if( !pending() || !in_flight_) {
			return false ;
		}

		in_flight_ = false ;
		// API境界中のclock逆転や期限到達でも後続入力を予約しない
		if( now < claimed_ || now >= deadline_) {
			Cancel() ;
			return false ;
		}

		// 部分受付・API失敗は追加解放も再送もしない
		if( !accepted) {
			Cancel() ;
			return false ;
		}

		// Enter後はAgent応答確認をownerへ戻す
		if( step_ == FixedInputStep::Enter) {
			step_ = FixedInputStep::Complete ;
			return true ;
		}

		step_ = step_ == FixedInputStep::Run ? FixedInputStep::Paste : FixedInputStep::Enter ;
		return Schedule( now, kNextDelay) ;
	}

	/** @brief 未送信入力を失効し、戻っても再開しない。 */
	void Cancel() noexcept
	{
		// 受付完了は取消で巻き戻したと表示しない
		if( step_ != FixedInputStep::Complete) {
			step_ = FixedInputStep::Stopped ;
		}

		in_flight_ = false ;
	}

	/** @brief 完了済み入力だけを次の明示操作用に初期化する。
	 * @return CompleteかつAPI要求中でない場合だけtrue。未開始・途中・停止は変更しない。
	 * @note ownerは同認証世代で応答確認済みAgentの終了後だけ呼ぶ。
	 *       初期化だけでは入力を予約せず、次の明示確認後にBeginを必要とする。
	 */
	[[nodiscard]] bool ResetCompleted() noexcept
	{
		// 途中・失敗・取消を新しい操作として再送しない
		if( step_ != FixedInputStep::Complete || in_flight_) {
			return false ;
		}

		*this = FixedInputSequence{} ;
		return true ;
	}

	/** @brief 未送信または要求中の固定入力が存在するかを返す。 @return 存在時true。 */
	[[nodiscard]] bool pending() const noexcept
	{
		return step_ == FixedInputStep::Run || step_ == FixedInputStep::Paste ||
			   step_ == FixedInputStep::Enter ;
	}

	/** @brief 現在の固定段階を返す。 @return 入力受付済みもremote成功とは別である。 */
	[[nodiscard]] FixedInputStep step() const noexcept
	{
		return step_ ;
	}

	/** @brief 現在のAPI予約を識別する。 @return Claim後でComplete前の場合true。 */
	[[nodiscard]] bool in_flight() const noexcept
	{
		return in_flight_ ;
	}

	/** @brief 期限またはclock逆転を安全側へ分類する。
	 * @param[in] now 現在の単調ms。
	 * @return 予約時刻より逆転、または予定時刻から3秒以上ならtrue。
	 */
	[[nodiscard]] bool expired( std::uint64_t now) const noexcept
	{
		return pending() && (now < started_ || now >= deadline_) ;
	}

private:
	/** @brief 有限の次時刻をoverflowなしで予約する。
	 * @param[in] now 現在の単調ms。
	 * @param[in] delay 固定待機ms。
	 * @return 期限を表現できる場合true。失敗時は失効する。
	 */
	[[nodiscard]] bool Schedule( std::uint64_t now, std::uint64_t delay) noexcept
	{
		// wrapした時刻を即時入力として扱わない
		if( now > std::numeric_limits<std::uint64_t>::max() - delay - kExpiry) {
			Cancel() ;
			return false ;
		}

		started_  = now ;
		due_	  = now + delay ;
		deadline_ = due_ + kExpiry ;
		return true ;
	}

	/// 所有STAだけが変更する固定段階。
	FixedInputStep step_{FixedInputStep::Idle} ;
	/// callback再入による同段階の二重要求を拒否する。
	bool		   in_flight_{false} ;
	/// clock逆転を検出する予約時刻。
	std::uint64_t  started_{0U} ;
	/// API予約時刻。callback復帰時のclock逆転を拒否する。
	std::uint64_t  claimed_{0U} ;
	/// 早着timerを無視する最小送信時刻。
	std::uint64_t  due_{0U} ;
	/// 遅すぎるtimerを拒否する期限。
	std::uint64_t  deadline_{0U} ;
} ;
} // namespace zrdp::client::rdp
