//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_settings_service.cpp
 * @brief Agent設定の有限要求を直列実行し、所有結果だけをUIへ渡す。
 * @version 01.00.06
 * @date 2026/10/08
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  設定I/Oを一件queue workerへ分離。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  トレイ終了後の同じRDP内Agent再起動を修正。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.06   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/agent_settings/agent_settings_service.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace zrdp::client::agent_settings {

AgentSettingsService::AgentSettingsService( std::unique_ptr<IAgentSettingsBackend> backend,
										   std::function<void()>				  notify)
	: backend_( std::move( backend)), notify_( std::move( notify))
{
	// 保存先と起床先の欠落したworkerを開始しない
	if( backend_ == nullptr || !notify_) {
		throw std::invalid_argument( "Agent settings backend is required") ;
	}

	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief 設定file操作を専用workerで直列実行する。
	 * @param[in] stop jthreadの協調停止要求。
	 * @note ownerはStopとjoinまで生存し、UI/COM操作はworkerで行わない。
	 * @endcond
	 */
	worker_ = std::jthread( [this]( const std::stop_token stop) { Run( stop) ; }) ;
}

AgentSettingsService::~AgentSettingsService()
{
	Stop() ;
}

std::uint64_t AgentSettingsService::Request( AgentSettingsRequest request) noexcept
{
	try {
		// 所有要求も全上限を満たす場合だけqueueへ渡す
		if( request.path.native().size() > 32767U ||
		   request.profile.name.size() > kMaximumAgentProfileName ||
		   request.profile.endpoints.size() > agent::network::kMaximumAllowlistEntries ||
		   request.profiles.size() > kMaximumAgentProfiles) {
			return 0U ;
		}

		// 外側の件数だけでなく各profileの所有bufferも受付前に制限する
		for( const auto &profile : request.profiles) {
			// 過大な一件を有限queueへ保持しない
			if( profile.name.size() > kMaximumAgentProfileName ||
			   profile.endpoints.size() > agent::network::kMaximumAllowlistEntries) {
				return 0U ;
			}
		}

		std::scoped_lock lock( mutex_) ;
		// 操作中、未取得結果、終了後、ID overflowでは既存状態を上書きしない
		if( stopping_ || busy_ || request_.has_value() || notice_.has_value() ||
		   sequence_ == std::numeric_limits<std::uint64_t>::max()) {
			return 0U ;
		}

		request_ = std::move( request) ;
		++sequence_ ;
		busy_ = true ;
		condition_.notify_one() ;
		return sequence_ ;
	} catch( ...) {
		return 0U ;
	}
}

bool AgentSettingsService::TakeNotice( AgentSettingsNotice &notice) noexcept
{
	try {
		std::scoped_lock lock( mutex_) ;
		// 所有結果がある場合だけ最大一件をUIへ移転する
		if( !notice_.has_value()) {
			return false ;
		}

		notice = std::move( *notice_) ;
		notice_.reset() ;
		return true ;
	} catch( ...) {
		return false ;
	}
}

bool AgentSettingsService::Busy() const noexcept
{
	try {
		std::scoped_lock lock( mutex_) ;
		return busy_ || request_.has_value() || notice_.has_value() || stopping_ ;
	} catch( ...) {
		return true ;
	}
}

void AgentSettingsService::Stop() noexcept
{
	try {
		{
			std::scoped_lock lock( mutex_) ;
			stopping_ = true ;
			request_.reset() ;
		}

		worker_.request_stop() ;
		condition_.notify_all() ;
		// 全UI終了後のapplication ownerだけがworkerをjoinする
		if( worker_.joinable()) {
			worker_.join() ;
		}
	} catch( ...) {
		worker_.request_stop() ;
	}
}

void AgentSettingsService::Run( const std::stop_token stop) noexcept
{
	// I/OとUI通知を一件ずつ実行し、並行保存や無制限queueを作らない
	while( !stop.stop_requested()) {
		AgentSettingsNotice notice ;
		try {
			AgentSettingsRequest request ;
			{
				std::unique_lock lock( mutex_) ;
				/**
				 * @cond LOCAL_CALLBACK_CONTRACT
				 * @brief 停止要求または一件の処理要求が来るまでworkerを待機させる。
				 * @return 停止中または処理すべき要求があればtrue。
				 * @note mutex_保護下でrequest_とstopping_を読む。
				 * @endcond
				 */
				condition_.wait( lock, stop, [this] { return stopping_ || request_.has_value() ; }) ;
				// 終了要求後の残った保存を新たに実行しない
				if( stopping_ || stop.stop_requested()) {
					break ;
				}

				request = std::move( *request_) ;
				request_.reset() ;
				notice.request_id = sequence_ ;
				notice.operation  = request.operation ;
			}

			// platform実装をmutexなしで呼び、UIによる終了要求を妨げない
			switch( request.operation) {
			// 保存fileの欠落は正常な空一覧として扱う
			case AgentSettingsOperation::Load:
				notice.result = backend_->Load( notice.profiles, stop) ;
				// platform実装からの結果も完全に検証してからUIへ渡す
				if( notice.result.succeeded()) {
					notice.result = ValidateAgentSettingsProfiles( notice.profiles) ;
				}

				// 検証できない結果を部分一覧として公開しない
				if( !notice.result.succeeded()) {
					notice.profiles.clear() ;
				}

				break ;

			// 保存候補全体をplatform側でも再検証してから公開する
			case AgentSettingsOperation::Save:
				notice.result = ValidateAgentSettingsProfiles( request.profiles) ;
				// 不正な要求をfile保存境界へ渡さない
				if( notice.result.succeeded()) {
					notice.result = backend_->Save( request.profiles, stop) ;
				}

				// 成功した完全一覧だけをUIへ返す
				if( notice.result.succeeded()) {
					notice.profiles = std::move( request.profiles) ;
				}

				break ;

			// 明示選択したlocal INIだけを未承認draftへ変換する
			case AgentSettingsOperation::Import:
				notice.result = ValidateAgentSettingsProfile( {request.profile.name, {}}) ;
				// 不正な設定名でfile入力を開始しない
				if( notice.result.succeeded()) {
					notice.result =
						backend_->Import( request.path, request.profile.name, notice.profile, stop) ;
				}

				// backendの差替えでも不正な部分一覧を画面へ公開しない
				if( notice.result.succeeded()) {
					notice.result = ValidateAgentSettingsProfile( notice.profile) ;
					// Agent INIの空allowlistを有効なimportとしない
					if( notice.result.succeeded() && notice.profile.endpoints.empty()) {
						notice.result.error = AgentSettingsError::EmptyEndpoints ;
					}
				}

				// INIには承認・用途がないため、受信値を推定して継承しない
				if( notice.result.succeeded()) {
					// 完全読込み後のすべての宛先を再承認待ちへ戻す
					for( auto &entry : notice.profile.endpoints) {
						entry.approved = false ;
						entry.purpose  = AgentEndpointPurpose::Unspecified ;
					}
				} else {
					notice.profile = {} ;
				}

				break ;

			// Agent parserと全entryの承認に成功した設定だけを出力する
			// 配置もINI出力と同じ承認gateで再検証する
			case AgentSettingsOperation::PrepareDeployment: {
				std::string ini ;
				notice.result = GenerateAgentSettingsIni( request.profile, ini) ;
				// 未承認snapshotはbackendへ渡さない
				if( notice.result.succeeded()) {
					notice.result =
						backend_->PrepareDeployment( request.profile, notice.package, stop) ;
					// backend差替えでも別設定や上限外packageを採用しない
					if( notice.result.succeeded() &&
					   (notice.package == nullptr || notice.package->ini != ini ||
						notice.package->agent.empty() ||
						notice.package->agent.size() > agent_deployment::kMaximumAgentBytes)) {
						notice.result.error = AgentSettingsError::InvalidData ;
					}

					// 製品情報・設定を検証した後だけ、同じsessionの専用driveへ公開する
					if( notice.result.succeeded() && request.deployment_drive != nullptr) {
						// 初期配置非対応Agentにはsetup argumentを送らない
						if( !notice.package->agent_setup ||
						   !request.deployment_drive->PrepareLaunch( notice.package->agent,
																	notice.package->ini, stop)) {
							notice.result.error = AgentSettingsError::InvalidData ;
						} else {
							notice.deployment_drive = std::move( request.deployment_drive) ;
						}
					}
				}

				// 平文の検証用copyを使い終わったら直ちに消す
				if( !ini.empty()) {
					SecureZeroMemory( ini.data(), ini.size()) ;
				}

				// 失敗時は部分packageをUIへ渡さない
				if( !notice.result.succeeded()) {
					notice.package.reset() ;
				}

				break ;
			}

			// 承認済み設定を指定local fileへ原子的に出力する
			case AgentSettingsOperation::Export: {
				std::string validated ;
				notice.result = GenerateAgentSettingsIni( request.profile, validated) ;
				// backendの差替えでも未承認先を出力境界へ渡さない
				if( notice.result.succeeded()) {
					notice.result = backend_->Export( request.path, request.profile,
													 request.overwrite_confirmed, stop) ;
				}
			} break ;

			// 将来または不正な操作を既存操作へ近似しない
			default:
				notice.result.error = AgentSettingsError::InvalidData ;
				break ;
			}
		} catch( ...) {
			notice.result.error = AgentSettingsError::ResourceExhausted ;
		}

		try {
			{
				std::scoped_lock lock( mutex_) ;
				busy_ = false ;
				// 終了後には消えたUIへ所有結果を新規公開しない
				if( stopping_ || stop.stop_requested()) {
					break ;
				}

				notice_ = std::move( notice) ;
			}

			notify_() ;
		} catch( ...) {
			// callback失敗でも有限結果を保持し、次のUI drainへ委ねる
		}
	}
}

} // namespace zrdp::client::agent_settings
