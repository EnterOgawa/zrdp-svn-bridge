//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_update_service_test.cpp
 * @brief 同意前download禁止、session生存中apply禁止、bounded queueを確認する。
 * @version 01.00.02
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  更新workerの同意と寿命境界testを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  同意対象のAgent sizeとdigestも完全manifestに含める。
 * Ver.01.00.02   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/update/update_service.h"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace {

/** @brief 更新worker境界を短く参照するtest用namespace。 */
namespace update = zrdp::client::update ;

/** @brief fakeの設定と操作数を保持する有限共有状態。 */
struct FakeState {
	/** @brief 実sourceと独立した数値versionの架空新版を用意する。 */
	FakeState()
	{
		checked.configured				   = true ;
		checked.current_version.components = {0U, 7U, 20U, 0U} ;
		update::ReleaseManifest release ;
		release.version.components = {0U, 7U, 21U, 0U} ;
		release.tag				   = "v0.7.21" ;
		release.size_bytes		   = 4096U ;
		release.agent_size_bytes   = 2048U ;
		release.agent_sha256[0]	   = std::byte{1U} ;
		release.bridge_protocol	   = 1U ;
		checked.release			   = std::move( release) ;
	}

	/// backendが返す架空manifest。次のrequest前だけtest側で変更する。
	update::UpdateCheckResult	checked ;
	/// 検証失敗を注入する固定Prepare結果。
	update::UpdateError			prepare_error{update::UpdateError::None} ;
	/// helper起動失敗を注入する固定結果。
	update::UpdateError			apply_error{update::UpdateError::None} ;
	/// manifest確認の回数。
	std::atomic<unsigned int>	checks{0U} ;
	/// EXE取得・検証の回数。
	std::atomic<unsigned int>	prepares{0U} ;
	/// helper要求の回数。
	std::atomic<unsigned int>	applies{0U} ;
	/// worker側Shutdownの回数。
	std::atomic<unsigned int>	shutdowns{0U} ;
	/// stop tokenで取消可能なfake確認待機。
	std::atomic<bool>			block_check{false} ;
	/// stop tokenで取消可能なfake取得待機。
	std::atomic<bool>			block_prepare{false} ;
	/// block解除とUI起床だけを保護するtest mutex。
	std::mutex					mutex ;
	/// fake I/Oとnoticeを有限に待つcondition。
	std::condition_variable_any condition ;
	/// Prepareが同意済み完全manifestを受けた場合だけtrue。
	std::atomic<bool>			approved_manifest_seen{false} ;
	/// Shutdownを実行したworker。join後だけtest側で参照する。
	std::thread::id				shutdown_thread ;
} ;

/** @brief network、file、実processを使わない更新backend。 */
class FakeBackend final : public update::IClientUpdateBackend {
public:
	/**
	 * @brief fake共有状態を保持する。
	 * @param[in] state testが所有する有限状態。
	 */
	explicit FakeBackend( std::shared_ptr<FakeState> state) : state_( std::move( state))
	{
	}

	/**
	 * @brief fake manifestだけを返し、同意前EXE取得を行わない。
	 * @param[in] stop worker終了要求。
	 * @return 注入した確認結果。
	 */
	[[nodiscard]] update::UpdateCheckResult Check( const std::stop_token stop) noexcept override
	{
		try {
			state_->checks.fetch_add( 1U) ;
			state_->condition.notify_all() ;
			std::unique_lock lock( state_->mutex) ;
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief 更新確認backendをfixtureが解放するまで待機させる。
			 * @return block_checkが解除された場合はtrue。
			 * @note state_->mutexを保持するpredicateで、外部GitHubへ接続しない。
			 * @endcond
			 */
			state_->condition.wait( lock, stop, [this] { return !state_->block_check.load() ; }) ;
			// 停止したfake I/Oを有効な更新確認として返さない
			if( stop.stop_requested()) {
				update::UpdateCheckResult cancelled ;
				cancelled.error = update::UpdateError::Cancelled ;
				return cancelled ;
			}

			return state_->checked ;
		} catch( ...) {
			update::UpdateCheckResult failed ;
			failed.error = update::UpdateError::ResourceExhausted ;
			return failed ;
		}
	}

	/**
	 * @brief 同意したmanifestのfake取得だけを記録する。
	 * @param[in] release UIが同意した完全値。
	 * @param[in] stop worker終了要求。
	 * @return 注入した署名等の結果。
	 */
	[[nodiscard]] update::UpdateError Prepare( const update::ReleaseManifest &release,
											  const std::stop_token			 stop) noexcept override
	{
		try {
			state_->prepares.fetch_add( 1U) ;
			state_->approved_manifest_seen.store( state_->checked.release.has_value() &&
												 release == *state_->checked.release) ;
			state_->condition.notify_all() ;
			std::unique_lock lock( state_->mutex) ;
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief 更新準備backendの停止・取消をfixture待機で検証する。
			 * @return block_prepareが解除された場合はtrue。
			 * @note state_->mutex保持中だけ評価し、EXEのdownloadや置換は行わない。
			 * @endcond
			 */
			state_->condition.wait( lock, stop, [this] { return !state_->block_prepare.load() ; }) ;
			return stop.stop_requested() ? update::UpdateError::Cancelled : state_->prepare_error ;
		} catch( ...) {
			return update::UpdateError::ResourceExhausted ;
		}
	}

	/**
	 * @brief fake helper起動だけを記録する。
	 * @param[in] stop worker終了要求。
	 * @return 注入した受付結果。
	 */
	[[nodiscard]] update::UpdateError LaunchHelper( const std::stop_token stop) noexcept override
	{
		state_->applies.fetch_add( 1U) ;
		return stop.stop_requested() ? update::UpdateError::Cancelled : state_->apply_error ;
	}

	/** @brief Shutdownをworker thread上で一度だけ記録する。 */
	void Shutdown() noexcept override
	{
		state_->shutdown_thread = std::this_thread::get_id() ;
		state_->shutdowns.fetch_add( 1U) ;
	}

private:
	/// testとbackendの共有状態。serviceより長く保持する。
	std::shared_ptr<FakeState> state_ ;
} ;

/**
 * @brief 最大一秒だけfake noticeを待つ。
 * @param[in,out] service 起床済み結果を取り出す更新worker。
 * @param[in,out] state fake condition。
 * @param[out] notice 完了結果。
 * @return 一件を取得できた場合はtrue。
 */
[[nodiscard]] bool WaitNotice( update::ClientUpdateService &service, FakeState &state,
							  update::UpdateNotice &notice)
{
	// lost wakeupでも一msごとに所有queueを確認し、待機を一秒へ制限する
	for( unsigned int attempt = 0U; attempt < 1000U; ++attempt) {
		// 完成した所有結果だけをtest threadへ渡す
		if( service.TakeNotice( notice)) {
			return true ;
		}

		std::unique_lock lock( state.mutex) ;
		state.condition.wait_for( lock, std::chrono::milliseconds( 1)) ;
	}

	return false ;
}

/**
 * @brief fake backendが操作を開始したことだけを有限に待つ。
 * @param[in] counter atomic操作数。
 * @return 一回以上の開始を確認できた場合はtrue。
 */
[[nodiscard]] bool WaitStarted( const std::atomic<unsigned int> &counter)
{
	// worker schedulerの揺らぎを許し、失敗時も有限で終える
	for( unsigned int attempt = 0U; attempt < 1000U; ++attempt) {
		// fake I/Oへ入った時点が終了要求を試す境界となる
		if( counter.load() != 0U) {
			return true ;
		}

		std::this_thread::sleep_for( std::chrono::milliseconds( 1)) ;
	}

	return false ;
}

} // namespace

/**
 * @brief 同意、取消、queue上限、検証失敗、session終了、停止を確認する。
 * @return 成功は0、失敗境界を示す固定非0 code。
 */
int main()
{
	auto						state = std::make_shared<FakeState>() ;
	update::ClientUpdateService service( std::make_unique<FakeBackend>( state),
										/**
										 * @cond LOCAL_CALLBACK_CONTRACT
										 * @brief 更新worker完了をtest condition-variableへだけ通知する。
										 * @note captureしたfixtureの寿命だけを共有し、Window・GitHub・更新配置に触れない。
										 * @endcond
										 */
										[state] { state->condition.notify_all() ; }) ;
	// idleでも同意や完全検証なしにdownload／applyを開始しない
	if( service.Approve() || service.ApplyIfIdle( 0U) || !service.RequestCheck( false) ||
	   service.RequestCheck( true)) {
		return 1 ;
	}

	update::UpdateNotice notice ;
	// manifestの提示だけではEXEを取得せず、UIがnoticeを取得するまで同意を受けない
	if( !WaitNotice( service, *state, notice) || notice.stage != update::UpdateStage::Offered ||
	   state->prepares.load() != 0U || state->applies.load() != 0U || !service.Approve()) {
		return 2 ;
	}

	// 完全な同意manifestだけがPrepareへ渡り、同時の操作をqueueへ増やさない
	if( service.Approve() || service.RequestCheck( true) || !WaitNotice( service, *state, notice) ||
	   notice.stage != update::UpdateStage::ReadyToApply || state->prepares.load() != 1U ||
	   !state->approved_manifest_seen.load() || service.ApplyIfIdle( 1U) ||
	   service.ApplyIfIdle( 16U) || state->applies.load() != 0U) {
		return 3 ;
	}

	// 全session Windowがなくなったときだけhelperへ一回移転する
	if( !service.ApplyIfIdle( 0U) || service.ApplyIfIdle( 0U) ||
	   !WaitNotice( service, *state, notice) || notice.stage != update::UpdateStage::HelperReady ||
	   state->applies.load() != 1U) {
		return 4 ;
	}

	service.Stop() ;
	// stage回収はUI threadでなく停止済みworker上だけで行う
	if( state->shutdowns.load() != 1U || state->shutdown_thread == std::this_thread::get_id() ||
	   service.RequestCheck( true) || service.Approve() || service.ApplyIfIdle( 0U)) {
		return 5 ;
	}

	auto						declined = std::make_shared<FakeState>() ;
	update::ClientUpdateService refusal( std::make_unique<FakeBackend>( declined),
										/**
										 * @cond LOCAL_CALLBACK_CONTRACT
										 * @brief 更新worker完了をtest condition-variableへだけ通知する。
										 * @note captureしたfixtureの寿命だけを共有し、Window・GitHub・更新配置に触れない。
										 * @endcond
										 */
										[declined] { declined->condition.notify_all() ; }) ;
	// 「いいえ」相当の取消を確認し、同じ版を自動再提示しない
	if( !refusal.RequestCheck( false) || !WaitNotice( refusal, *declined, notice)) {
		return 6 ;
	}

	refusal.Dismiss() ;
	// 取消済みの同意を後から再利用しない
	if( refusal.Approve() || !refusal.RequestCheck( false) ||
	   !WaitNotice( refusal, *declined, notice) || notice.stage != update::UpdateStage::Idle ||
	   notice.release.has_value() || declined->prepares.load() != 0U) {
		return 7 ;
	}

	// 手動確認は取消版も再提示するが、再度の明示同意が必要である
	if( !refusal.RequestCheck( true) || !WaitNotice( refusal, *declined, notice) ||
	   notice.stage != update::UpdateStage::Offered || declined->prepares.load() != 0U) {
		return 8 ;
	}

	refusal.Dismiss() ;
	refusal.Stop() ;

	const std::array<update::UpdateError, 4U> failures{
		update::UpdateError::InvalidSignature, update::UpdateError::SignerMismatch,
		update::UpdateError::HashMismatch, update::UpdateError::MetadataMismatch} ;
	// 同意済みでも検証に失敗した候補へhelperを起動しない
	for( const auto failure : failures) {
		auto invalid		   = std::make_shared<FakeState>() ;
		invalid->prepare_error = failure ;
		update::ClientUpdateService rejected( std::make_unique<FakeBackend>( invalid),
											 /**
											  * @cond LOCAL_CALLBACK_CONTRACT
											  * @brief 更新worker完了をtest condition-variableへだけ通知する。
											  * @note captureしたfixtureの寿命だけを共有し、Window・GitHub・更新配置に触れない。
											  * @endcond
											  */
											 [invalid] { invalid->condition.notify_all() ; }) ;
		// 失敗後に全session終了条件だけで再開しないことを確認する
		if( !rejected.RequestCheck( false) || !WaitNotice( rejected, *invalid, notice) ||
		   !rejected.Approve() || !WaitNotice( rejected, *invalid, notice) ||
		   notice.error != failure || notice.stage != update::UpdateStage::Idle ||
		   rejected.ApplyIfIdle( 0U) || invalid->applies.load() != 0U) {
			return 9 ;
		}
	}

	// version、互換性、設定のいずれかが不適切なら提示しない
	for( unsigned int kind = 0U; kind < 4U; ++kind) {
		auto invalid = std::make_shared<FakeState>() ;
		// 同版／旧版／wire非互換／無効設定を個別に注入する
		if( kind == 0U) {
			invalid->checked.release->version = invalid->checked.current_version ;
		}
		// downgrade候補は同意待ちへ移行しない
		else if( kind == 1U) {
			invalid->checked.release->version.components = {0U, 7U, 19U, 0U} ;
		}
		// Agentと非互換なwire versionをbackendから注入する
		else if( kind == 2U) {
			invalid->checked.release->bridge_protocol = 2U ;
		} else {
			invalid->checked.configured = false ;
		}

		update::ClientUpdateService incompatible( std::make_unique<FakeBackend>( invalid),
												 /**
												  * @cond LOCAL_CALLBACK_CONTRACT
												  * @brief 更新worker完了をtest condition-variableへだけ通知する。
												  * @note captureしたfixtureの寿命だけを共有し、Window・GitHub・更新配置に触れない。
												  * @endcond
												  */
												 [invalid] { invalid->condition.notify_all() ; }) ;
		// 無効候補ではdownloadとapplyの両方を拒否する
		if( !incompatible.RequestCheck( false) || !WaitNotice( incompatible, *invalid, notice) ||
		   notice.stage != update::UpdateStage::Idle || incompatible.Approve() ||
		   incompatible.ApplyIfIdle( 0U) || invalid->prepares.load() != 0U) {
			return 10 ;
		}
	}

	auto blocked = std::make_shared<FakeState>() ;
	blocked->block_check.store( true) ;
	update::ClientUpdateService cancellation( std::make_unique<FakeBackend>( blocked),
											 /**
											  * @cond LOCAL_CALLBACK_CONTRACT
											  * @brief 更新worker完了をtest condition-variableへだけ通知する。
											  * @note captureしたfixtureの寿命だけを共有し、Window・GitHub・更新配置に触れない。
											  * @endcond
											  */
											 [blocked] { blocked->condition.notify_all() ; }) ;
	// 停止要求がfake I/O待機を解除し、未完了結果でdownloadを開始しない
	if( !cancellation.RequestCheck( false) || !WaitStarted( blocked->checks)) {
		return 11 ;
	}

	cancellation.Stop() ;
	// worker停止後も未実行要求やnoticeを処理しない
	if( blocked->shutdowns.load() != 1U || blocked->prepares.load() != 0U ||
	   blocked->applies.load() != 0U || cancellation.TakeNotice( notice)) {
		return 12 ;
	}

	return 0 ;
}
