//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_settings_service_test.cpp
 * @brief Agent設定workerの上限、終了、世代照合と再検証を確認する。
 * @version 01.00.05
 * @date 2026/10/08
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  設定workerのfake I/O検証を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  トレイ終了後の同じRDP内Agent再起動を修正。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.05   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/agent_settings/agent_settings_service.h"

#include <atomic>
#include <chrono>
#include <iostream>

namespace {
// 設定境界のtest用alias
namespace settings = zrdp::client::agent_settings ;

/** @brief backendとtestが共有するnetworkなしの有限状態。 */
struct FakeState {
	/// fake backendを停止可能に待たせるmutex。
	std::mutex						mutex ;
	/// 起床通知と停止の待機先。
	std::condition_variable_any		condition ;
	/// Loadを解除するまで待たせる状態。
	std::atomic<bool>				block{false} ;
	/// backendが実際に呼ばれた回数。
	std::atomic<unsigned int>		calls{0U} ;
	/// UIへの完了通知回数。
	std::atomic<unsigned int>		notifications{0U} ;
	/// fake Loadで返す一覧。要求がない時にだけtest側が変更する。
	settings::AgentSettingsProfiles loaded ;
	/// fake Importで返す一件。
	settings::AgentSettingsProfile	imported ;
	/// 配置fakeが異なるINIを返す失敗条件。
	bool							mismatched_package{false} ;
	/// backendが呼ばれたthread。完了通知後だけtest側で参照する。
	std::thread::id					backend_thread ;
} ;

/** @brief file、RDP、Socketを使わない設定backend。 */
class FakeBackend final : public settings::IAgentSettingsBackend {
public:
	/**
	 * @brief 所有fixtureの共有参照を保持する。
	 * @param[in] state testが作成した状態。
	 */
	explicit FakeBackend( std::shared_ptr<FakeState> state) : state_( std::move( state))
	{
	}

	/**
	 * @brief 注入一覧だけを返し、取消可能な待機を模す。
	 * @param[out] profiles 成功時のfake一覧。
	 * @param[in] stop worker終了要求。
	 * @return 停止ならCancelled、それ以外は成功。
	 */
	[[nodiscard]] settings::AgentSettingsResult Load( settings::AgentSettingsProfiles &profiles,
													 std::stop_token stop) noexcept override
	{
		try {
			Called() ;
			std::unique_lock lock( state_->mutex) ;
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief backend処理をfixtureが解除するまで停止し、有限queueを検証する。
			 * @return fixtureのblockが解除された場合はtrue。
			 * @note state_->mutex保持下のcondition-variable predicateから読む。
			 * @endcond
			 */
			state_->condition.wait( lock, stop, [this] { return !state_->block.load() ; }) ;
			// 終了要求後のfake結果を新規保存状態として渡さない
			if( stop.stop_requested()) {
				return {settings::AgentSettingsError::Cancelled} ;
			}

			profiles = state_->loaded ;
			return {} ;
		} catch( ...) {
			return {settings::AgentSettingsError::ResourceExhausted} ;
		}
	}

	/**
	 * @brief 呼出しだけを記録し、実fileへ保存しない。
	 * @param[in] profiles 検証済み一覧。内容はこのfakeでは参照しない。
	 * @param[in] stop 検証済みworker要求。待機しないため参照しない。
	 * @return fake成功。
	 */
	[[nodiscard]] settings::AgentSettingsResult
	Save( const settings::AgentSettingsProfiles &profiles, std::stop_token stop) noexcept override
	{
		static_cast<void>( profiles) ;
		static_cast<void>( stop) ;
		Called() ;
		return {} ;
	}

	/**
	 * @brief 注入一件だけを返し、INI実fileを使わない。
	 * @param[in] path fakeでは参照しない選択path。
	 * @param[in] name fakeでは参照しない要求名。
	 * @param[out] profile testが注入した一件。
	 * @param[in] stop 待機しないため参照しない。
	 * @return copy成功または資源失敗。
	 */
	[[nodiscard]] settings::AgentSettingsResult Import( const std::filesystem::path	  &path,
													   std::wstring_view			   name,
													   settings::AgentSettingsProfile &profile,
													   std::stop_token stop) noexcept override
	{
		static_cast<void>( path) ;
		static_cast<void>( name) ;
		static_cast<void>( stop) ;
		try {
			Called() ;
			profile = state_->imported ;
			return {} ;
		} catch( ...) {
			return {settings::AgentSettingsError::ResourceExhausted} ;
		}
	}

	/**
	 * @brief 実fileへ出力せず到達回数だけを確認する。
	 * @param[in] path fakeで参照しない選択path。
	 * @param[in] profile serviceが検証した一件。内容を保存しない。
	 * @param[in] overwrite_confirmed fakeでは保存しないため参照しない。
	 * @param[in] stop fakeでは待機しないため参照しない。
	 * @return fake成功。
	 */
	[[nodiscard]] settings::AgentSettingsResult
	Export( const std::filesystem::path &path, const settings::AgentSettingsProfile &profile,
		   bool overwrite_confirmed, std::stop_token stop) noexcept override
	{
		static_cast<void>( path) ;
		static_cast<void>( profile) ;
		static_cast<void>( overwrite_confirmed) ;
		static_cast<void>( stop) ;
		Called() ;
		return {} ;
	}

	/** @brief 実Agent EXEを使わずworkerの配置再検証を確認する。
	 * @param[in] profile 完全承認済みsnapshot。
	 * @param[out] package fake二ファイル。
	 * @param[in] stop 有限workerの取消要求。fakeは待機しない。
	 * @return memory fixtureの生成結果。
	 */
	[[nodiscard]] settings::AgentSettingsResult
	PrepareDeployment( const settings::AgentSettingsProfile							 &profile,
					  std::shared_ptr<const zrdp::client::agent_deployment::Package> &package,
					  std::stop_token stop) noexcept override
	{
		static_cast<void>( stop) ;
		Called() ;
		try {
			auto result			 = std::make_shared<zrdp::client::agent_deployment::Package>() ;
			result->agent		 = {std::byte{0x4d}} ;
			result->agent_setup	 = true ;
			const auto generated = settings::GenerateAgentSettingsIni( profile, result->ini) ;
			// fake結果でもINI生成が失敗したら部分packageを渡さない
			if( !generated.succeeded()) {
				return generated ;
			}

			// 誤ったbackendの成功応答を再現する
			if( state_->mismatched_package) {
				result->ini = "wrong" ;
			}

			package = std::move( result) ;
			return {} ;
		} catch( ...) {
			return {settings::AgentSettingsError::ResourceExhausted} ;
		}
	}

private:
	/** @brief backend呼出しの回数とthreadを内容なしで記録する。 */
	void Called() noexcept
	{
		state_->backend_thread = std::this_thread::get_id() ;
		state_->calls.fetch_add( 1U) ;
		state_->condition.notify_all() ;
	}

	/// serviceより長く生存するfake状態。
	std::shared_ptr<FakeState> state_ ;
} ;

/**
 * @brief 完了通知を最大二秒だけ待ち、一件の所有結果を取得する。
 * @param[in,out] service 結果の単独所有者。
 * @param[in,out] state fake通知状態。
 * @param[in] count 期待する通知累積回数。
 * @param[out] notice 完了した一件。
 * @return 時間内に結果を取得できた場合はtrue。
 */
[[nodiscard]] bool WaitNotice( settings::AgentSettingsService &service, FakeState &state,
							  unsigned int count, settings::AgentSettingsNotice &notice)
{
	std::unique_lock lock( state.mutex) ;
	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief 指定数のworker完了通知をfixtureだけで待つ。
	 * @return 通知件数が要求count以上ならtrue。
	 * @note state.mutex保護下で待ち、実設定を読み込まない。
	 * @endcond
	 */
	const bool ready = state.condition.wait_for( lock, std::chrono::seconds( 2), [&state, count] {
		return state.notifications.load() >= count ;
	}) ;
	lock.unlock() ;
	return ready && service.TakeNotice( notice) ;
}
} // namespace

// -----------------------------------------------------------------------------
// queueとservice境界の契約
// -----------------------------------------------------------------------------

/**
 * @brief 一workerの有限queueとbackend再検証をfakeだけで確認する。
 * @return 全契約が成立する場合は0。失敗段階は固定code。
 */
int main()
{
	auto state	  = std::make_shared<FakeState>() ;
	state->loaded = {
		{L"Fixture",
		 {{1001U, {{192U, 0U, 2U, 10U}, 80U}, settings::AgentEndpointPurpose::Svn, true}}}} ;
	state->imported = state->loaded[0] ;
	state->block.store( true) ;
	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief worker完了通知をfixture counterとcondition-variableへ変換する。
	 * @note 共有stateをcaptureし、実UIへ通知しない。
	 * @endcond
	 */
	settings::AgentSettingsService service( std::make_unique<FakeBackend>( state), [state] {
		state->notifications.fetch_add( 1U) ;
		state->condition.notify_all() ;
	}) ;
	const auto					   first_id = service.Request( {}) ;
	// 一件処理中に連打された要求で元要求を上書きしない
	if( first_id == 0U || service.Request( {}) != 0U || !service.Busy()) {
		return 1 ;
	}

	state->block.store( false) ;
	state->condition.notify_all() ;
	{
		std::unique_lock lock( state->mutex) ;
		// 完成した未取得結果もqueue一件として保持する
		if( !state->condition.wait_for( lock, std::chrono::seconds( 2),
									  /**
									   * @cond LOCAL_CALLBACK_CONTRACT
									   * @brief 未取得結果も有限一件として残ることを待って検証する。
									   * @return 一件以上の通知を受けた場合はtrue。
									   * @endcond
									   */
									  [&state] { return state->notifications.load() >= 1U ; })) {
			return 2 ;
		}
	}

	// 結果を未取得の間は次の操作を予約しない
	if( service.Request( {}) != 0U) {
		return 3 ;
	}

	settings::AgentSettingsNotice notice ;
	// 所有結果を取得してからだけ次操作を受付け、I/Oはcaller threadで行わない
	if( !service.TakeNotice( notice) || notice.request_id != first_id || !notice.result.succeeded() ||
	   notice.profiles != state->loaded || service.Busy() ||
	   state->backend_thread == std::this_thread::get_id()) {
		return 4 ;
	}

	settings::AgentSettingsRequest request ;
	request.operation					  = settings::AgentSettingsOperation::Export ;
	request.profile						  = state->loaded[0] ;
	request.profile.endpoints[0].approved = false ;
	const auto rejected_id				  = service.Request( request) ;
	// 未承認の出力はbackendへ到達させず、固定失敗として返す
	if( rejected_id <= first_id || !WaitNotice( service, *state, 2U, notice) ||
	   notice.result.error != settings::AgentSettingsError::UnapprovedEndpoint ||
	   state->calls.load() != 1U) {
		return 5 ;
	}

	request.profile.endpoints[0].approved = true ;
	// 明示承認後の出力だけを保存境界へ渡す
	if( service.Request( request) == 0U || !WaitNotice( service, *state, 3U, notice) ||
	   !notice.result.succeeded() || state->calls.load() != 2U) {
		return 6 ;
	}

	request.operation = settings::AgentSettingsOperation::Import ;
	// backendが承認済み値を返してもINIから承認を引き継がない
	if( service.Request( request) == 0U || !WaitNotice( service, *state, 4U, notice) ||
	   !notice.result.succeeded() || notice.profile.endpoints[0].approved ||
	   notice.profile.endpoints[0].purpose != settings::AgentEndpointPurpose::Unspecified) {
		return 7 ;
	}

	state->loaded[0].name.clear() ;
	request = {} ;
	// platform側から返った破損一覧を空成功として採用しない
	if( service.Request( request) == 0U || !WaitNotice( service, *state, 5U, notice) ||
	   notice.result.succeeded() || !notice.profiles.empty()) {
		return 8 ;
	}

	request.operation	   = settings::AgentSettingsOperation::Save ;
	request.profiles	   = state->loaded ;
	const auto prior_calls = state->calls.load() ;
	// 不正保存はplatform書込みへ到達させない
	if( service.Request( request) == 0U || !WaitNotice( service, *state, 6U, notice) ||
	   notice.result.succeeded() || state->calls.load() != prior_calls) {
		return 9 ;
	}

	request.profiles[0].endpoints.resize( 129U) ;
	// 各profileの過大bufferをqueueへ保持しない
	if( service.Request( request) != 0U) {
		return 10 ;
	}

	// 配置でも未承認設定をplatformへ渡さず、既存操作と同じqueueを使う
	request			  = {} ;
	request.operation = settings::AgentSettingsOperation::PrepareDeployment ;
	request.profile	  = {
		  L"Fixture",
		  {{1001U, {{192U, 0U, 2U, 10U}, 80U}, settings::AgentEndpointPurpose::Svn, false}}} ;
	const auto deployment_calls = state->calls.load() ;
	// 未承認draftはbackendに到達しない
	if( service.Request( request) == 0U || !WaitNotice( service, *state, 7U, notice) ||
	   notice.result.succeeded() || notice.package != nullptr ||
	   state->calls.load() != deployment_calls) {
		return 14 ;
	}

	request.profile.endpoints[0].approved = true ;
	// 承認snapshotの二ファイルだけをworkerから移転する
	if( service.Request( request) == 0U || !WaitNotice( service, *state, 8U, notice) ||
	   !notice.result.succeeded() || notice.package == nullptr ||
	   state->backend_thread == std::this_thread::get_id()) {
		return 15 ;
	}

	state->mismatched_package = true ;
	// backend差替えでINIをすり替えても部分packageを公開しない
	if( service.Request( request) == 0U || !WaitNotice( service, *state, 9U, notice) ||
	   notice.result.succeeded() || notice.package != nullptr) {
		return 16 ;
	}

	state->mismatched_package = false ;
	// 通常の配置要求が承認済みINIを保持し、保存一覧を変更しない
	if( service.Request( request) == 0U || !WaitNotice( service, *state, 10U, notice) ||
	   !notice.result.succeeded() || notice.package == nullptr ||
	   notice.package->ini.find( "192.0.2.10:80") == std::string::npos) {
		return 17 ;
	}

	// 次の通常要求も独立した成功結果を返す
	if( service.Request( request) == 0U || !WaitNotice( service, *state, 11U, notice) ||
	   !notice.result.succeeded() || notice.package == nullptr) {
		return 18 ;
	}

	const auto drive_base = std::filesystem::current_path() /
							(L"worker-restart-fixture-" + std::to_wstring( GetCurrentProcessId())) ;
	auto drive = zrdp::common::agent_setup::DeploymentDrive::Create( drive_base) ;
	// 実RDPやclipboardなしで、workerからの初回公開を確認する
	if( drive == nullptr) {
		return 19 ;
	}

	request.deployment_drive		 = drive ;
	std::uint64_t last_id			 = 0U ;
	unsigned int  notification_count = 11U ;
	// 毎回backendで再検証し、新しい要求IDで同じreadonly packageだけを再許可する
	for( unsigned int attempt = 0U; attempt < 4U; ++attempt) {
		const auto calls_before = state->calls.load() ;
		const auto id			= service.Request( request) ;
		// 古い結果を今回へ流用せず、今回のdrive所有権を照合する
		if( id <= last_id || !WaitNotice( service, *state, ++notification_count, notice) ||
		   !notice.result.succeeded() || notice.request_id != id ||
		   notice.deployment_drive != drive || !drive->sealed() || !drive->published() ||
		   state->calls.load() != calls_before + 1U) {
			return 20 ;
		}

		last_id = id ;
		drive->Cancel() ;
	}

	request.profile.endpoints[0].target.port = 81U ;
	// 保存済み設定が変わった場合は配布fileへ上書きせず準備失敗とする
	if( service.Request( request) == 0U ||
	   !WaitNotice( service, *state, ++notification_count, notice) || notice.result.succeeded() ||
	   notice.package != nullptr || notice.deployment_drive != nullptr || drive->sealed() ||
	   !drive->published()) {
		return 21 ;
	}

	request.deployment_drive.reset() ;
	notice = {} ;
	drive.reset() ;
	std::filesystem::remove( drive_base) ;
	service.Stop() ;
	// 停止後は新しいI/Oや結果を予約しない
	if( service.Request( {}) != 0U) {
		return 11 ;
	}

	// 停止可能なbackend待機を長時間hangさせずjoinする
	state = std::make_shared<FakeState>() ;
	state->block.store( true) ;
	settings::AgentSettingsService stopped( std::make_unique<FakeBackend>( state),
										   /**
										    * @cond LOCAL_CALLBACK_CONTRACT
										    * @brief 停止済みserviceの通知有無をfixture counterへ記録する。
										    * @note 実UIや設定保存には到達しない。
										    * @endcond
										    */
										   [state] { state->notifications.fetch_add( 1U) ; }) ;
	// queueから取り出される前でもStopで安全に取消す
	if( stopped.Request( {}) == 0U) {
		return 12 ;
	}

	stopped.Stop() ;
	// 終了後は閉じた画面へ結果を公開しない
	if( stopped.TakeNotice( notice) || state->notifications.load() != 0U) {
		return 13 ;
	}

	std::cout << "Agent settings service passed\n" ;
	return 0 ;
}
