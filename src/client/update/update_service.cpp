//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file update_service.cpp
 * @brief 同意前download禁止とsession生存中apply禁止を一workerで強制する。
 * @version 01.00.01
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  bounded更新workerと同意状態を実装。
 * Ver.01.00.01   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/update/update_service.h"
#include "protocol/frame.h"

#include <stdexcept>
#include <utility>

namespace zrdp::client::update {

ClientUpdateService::ClientUpdateService( std::unique_ptr<IClientUpdateBackend> backend,
										 std::function<void()>				   notify)
	: backend_( std::move( backend)), notify_( std::move( notify))
{
	// 実処理と通知先がない不完全workerを開始しない
	if( backend_ == nullptr || !notify_) {
		throw std::invalid_argument( "Client update backend is required") ;
	}

	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief Client/Agent更新の取得・検証を専用workerで直列実行する。
	 * @param[in] stop jthreadの協調停止要求。
	 * @note ownerは停止とjoinまで生存し、UI通知だけをmanagerへ配送する。
	 * @endcond
	 */
	worker_ = std::jthread( [this]( const std::stop_token stop) { Run( stop) ; }) ;
}

ClientUpdateService::~ClientUpdateService()
{
	Stop() ;
}

bool ClientUpdateService::RequestCheck( const bool manual_check) noexcept
{
	try {
		std::scoped_lock lock( mutex_) ;
		// 実行中／保留中／未取得結果を上書きせず一件へ制限する
		if( stopping_ || stage_ != UpdateStage::Idle || request_.has_value() ||
		   notice_.has_value()) {
			return false ;
		}

		stage_	 = UpdateStage::Checking ;
		request_ = Request{Operation::Check, manual_check} ;
		condition_.notify_one() ;
		return true ;
	} catch( ...) {
		return false ;
	}
}

bool ClientUpdateService::Approve() noexcept
{
	try {
		std::scoped_lock lock( mutex_) ;
		// 提示結果をUIが取得し、明示同意を返す前はEXEを取得しない
		if( stopping_ || stage_ != UpdateStage::Offered || !offered_.has_value() ||
		   request_.has_value() || notice_.has_value()) {
			return false ;
		}

		approved_ = true ;
		stage_	  = UpdateStage::Downloading ;
		request_  = Request{Operation::Prepare, false} ;
		condition_.notify_one() ;
		return true ;
	} catch( ...) {
		return false ;
	}
}

void ClientUpdateService::Dismiss() noexcept
{
	try {
		std::scoped_lock lock( mutex_) ;
		// 同意待ちの提示だけを取消し、適用中の状態を取消結果で上書きしない
		if( stage_ == UpdateStage::Offered && offered_.has_value()) {
			dismissed_ = offered_->version ;
			offered_.reset() ;
			notice_.reset() ;
			approved_ = false ;
			stage_	  = UpdateStage::Idle ;
		}
	} catch( ...) {
		// 取消処理の失敗でdownloadやapplyを暗黙に開始しない
	}
}

bool ClientUpdateService::ApplyIfIdle( const std::size_t active_sessions) noexcept
{
	try {
		std::scoped_lock lock( mutex_) ;
		// 同意と完全検証と全session終了を満たさなければhelperを開始しない
		if( stopping_ || active_sessions != 0U || !approved_ ||
		   stage_ != UpdateStage::ReadyToApply || !offered_.has_value() || request_.has_value() ||
		   notice_.has_value()) {
			return false ;
		}

		stage_	 = UpdateStage::Applying ;
		request_ = Request{Operation::Apply, false} ;
		condition_.notify_one() ;
		return true ;
	} catch( ...) {
		return false ;
	}
}

bool ClientUpdateService::TakeNotice( UpdateNotice &notice) noexcept
{
	try {
		std::scoped_lock lock( mutex_) ;
		// 存在する一件だけをcallerへ所有移転する
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

UpdateStage ClientUpdateService::stage() const noexcept
{
	try {
		std::scoped_lock lock( mutex_) ;
		return stage_ ;
	} catch( ...) {
		return UpdateStage::Applying ;
	}
}

void ClientUpdateService::Stop() noexcept
{
	try {
		{
			std::scoped_lock lock( mutex_) ;
			stopping_ = true ;
			request_.reset() ;
		}

		worker_.request_stop() ;
		condition_.notify_all() ;
		// 全Window終了後の所有者だけがworkerをjoinし、UIイベント処理中は呼ばない
		if( worker_.joinable()) {
			worker_.join() ;
		}
	} catch( ...) {
		// 終了要求を受けたjthreadのRAII解放へ任せ、再予約や更新は行わない
	}
}

void ClientUpdateService::Publish( UpdateNotice notice) noexcept
{
	try {
		{
			std::scoped_lock lock( mutex_) ;
			// 終了後はUIへ新規通知を渡さず、未取得結果も上書きしない
			if( stopping_ || notice_.has_value()) {
				return ;
			}

			stage_	= notice.stage ;
			notice_ = std::move( notice) ;
		}

		notify_() ;
	} catch( ...) {
		// 通知callbackの例外をworker境界から漏らさず、結果はUIの次回drainへ残す
	}
}

void ClientUpdateService::Run( const std::stop_token stop) noexcept
{
	// 一requestを処理してから次を待つため、networkとfile操作を並行開始しない
	while( !stop.stop_requested()) {
		try {
			Request						   request ;
			std::optional<ReleaseManifest> release ;
			{
				std::unique_lock lock( mutex_) ;
				/**
				 * @cond LOCAL_CALLBACK_CONTRACT
				 * @brief 停止要求または一件の更新要求が来るまでworkerを待たせる。
				 * @return 停止中または要求が存在する場合はtrue。
				 * @note mutex_を保持したwait predicateからだけ状態を読む。
				 * @endcond
				 */
				condition_.wait( lock, stop, [this] { return request_.has_value() || stopping_ ; }) ;
				// Stop後に残っていた操作を実行しない
				if( stopping_ || stop.stop_requested()) {
					break ;
				}

				request = *request_ ;
				request_.reset() ;
				release = offered_ ;
			}

			UpdateNotice result ;
			result.manual_check = request.manual ;
			// manifest確認はEXEのPrepareを呼ばず、同意待ちへ遷移するだけとする
			if( request.operation == Operation::Check) {
				auto checked		   = backend_->Check( stop) ;
				result.error		   = checked.error ;
				result.configured	   = checked.configured ;
				result.current_version = checked.current_version ;
				std::scoped_lock lock( mutex_) ;
				current_  = checked.current_version ;
				approved_ = false ;
				// platform実装が非互換Releaseを返してもworker境界でも拒否する
				if( checked.release.has_value() &&
				   checked.release->bridge_protocol != protocol::kProtocolVersion) {
					result.error = UpdateError::IncompatibleProtocol ;
				}

				// backendの誤った同version／downgrade提示もworker境界で拒否する
				if( checked.configured && result.error == UpdateError::None &&
				   checked.release.has_value() && checked.release->version > current_ &&
				   (request.manual || !dismissed_.has_value() ||
					checked.release->version != *dismissed_)) {
					offered_	   = std::move( checked.release) ;
					result.release = offered_ ;
					result.stage   = UpdateStage::Offered ;
				} else {
					offered_.reset() ;
				}
			} else {
				result.configured	   = true ;
				result.current_version = current_ ;
				result.release		   = release ;
				// 内部要求でも完全manifestがない操作を許可しない
				if( !release.has_value()) {
					result.error = UpdateError::InvalidManifest ;
				}
				// 明示同意後の一件だけをplatform実装へ渡して取得・検証する
				else if( request.operation == Operation::Prepare) {
					result.error = backend_->Prepare( *release, stop) ;
					result.stage = result.error == UpdateError::None ? UpdateStage::ReadyToApply
																	 : UpdateStage::Idle ;
				} else {
					result.error = backend_->LaunchHelper( stop) ;
					result.stage = result.error == UpdateError::None ? UpdateStage::HelperReady
																	 : UpdateStage::Idle ;
				}
			}

			Publish( std::move( result)) ;
		} catch( ...) {
			UpdateNotice failed ;
			failed.error = UpdateError::ResourceExhausted ;
			Publish( std::move( failed)) ;
		}
	}

	backend_->Shutdown() ;
}

} // namespace zrdp::client::update
