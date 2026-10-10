//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file update_service.h
 * @brief 同意とsession終了を条件とするbounded Client更新workerを定義する。
 * @version 01.00.00
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  同意付きClient更新のworker境界を追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "client/update/update_manifest.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace zrdp::client::update {

/** @brief UIが観測する更新の有限状態。 */
enum class UpdateStage {
	/// 保留中の更新はない。
	Idle,
	/// manifestだけを取得中。
	Checking,
	/// 新版が見つかり、同意待ち。
	Offered,
	/// 同意済みEXEを取得・検証中。
	Downloading,
	/// 検証済みで全session終了を待つ。
	ReadyToApply,
	/// 新規sessionを止め、helperの事前検証を待つ。
	Applying,
	/// helperが親終了後の差替えを引き受けた。
	HelperReady,
} ;

/** @brief backendが返すmanifest確認の所有結果。 */
struct UpdateCheckResult {
	/// 未設定時はfalseで、外部通信を行わない。
	bool						   configured{false} ;
	/// 内容を含まない確認結果。
	UpdateError					   error{UpdateError::None} ;
	/// 実行中ClientのWindows version。
	UpdateVersion				   current_version ;
	/// 現在より新しくprotocol互換な完全manifestだけを保持する。
	std::optional<ReleaseManifest> release ;
} ;

/** @brief workerからUIへ一件だけ渡す所有通知。 */
struct UpdateNotice {
	/// 完了した操作の状態。
	UpdateStage					   stage{UpdateStage::Idle} ;
	/// 固定更新error。
	UpdateError					   error{UpdateError::None} ;
	/// source設定が有効な場合だけtrue。
	bool						   configured{false} ;
	/// 手動確認では「最新版／失敗／未設定」も表示する。
	bool						   manual_check{false} ;
	/// 現在の数値version。
	UpdateVersion				   current_version ;
	/// 同意または検証の対象となるRelease。
	std::optional<ReleaseManifest> release ;
} ;

/** @brief HTTPS、署名検証、差替えをworker上だけで実行する境界。 */
class IClientUpdateBackend {
public:
	/** @brief backendの資源を解放する。 */
	virtual ~IClientUpdateBackend()														= default ;
	/**
	 * @brief manifestだけを取得し、新版と互換性を確認する。
	 * @param[in] stop worker終了要求。
	 * @return 内容を含まない結果と検証済みmanifest。
	 */
	[[nodiscard]] virtual UpdateCheckResult Check( std::stop_token stop) noexcept		= 0 ;
	/**
	 * @brief 同意済みmanifestのEXEをstageへ取得し、完全検証する。
	 * @param[in] release 同意したReleaseの完全値。
	 * @param[in] stop worker終了要求。
	 * @return 完全検証できた場合だけNone。
	 */
	[[nodiscard]] virtual UpdateError		Prepare( const ReleaseManifest &release,
													std::stop_token		   stop) noexcept		= 0 ;
	/**
	 * @brief 検証済みstageのhelperを開始し、親終了待ちへの受付を確認する。
	 * @param[in] stop worker終了要求。
	 * @return helperが安全な差替えを引き受けた場合だけNone。
	 */
	[[nodiscard]] virtual UpdateError		LaunchHelper( std::stop_token stop) noexcept = 0 ;
	/** @brief worker終了時に未移転stageを回収する。UI threadでは呼出さない。 */
	virtual void							Shutdown() noexcept
	{
	}
} ;

/** @brief 一request・一noticeの有限queueと同意状態を所有する。 */
class ClientUpdateService final {
public:
	/**
	 * @brief 更新専用workerを開始する。
	 * @param[in] backend Windows実装またはtest fakeの単独所有権。
	 * @param[in] notify workerからUI messageをpostする非blocking callback。
	 * @note callbackではUI／COMを直接操作しない。
	 */
	ClientUpdateService( std::unique_ptr<IClientUpdateBackend> backend,
						std::function<void()>				  notify) ;
	/** @brief workerを停止し、backendを解放する。 */
	~ClientUpdateService() ;
	/** @brief 稼働workerの複製を禁止する。 */
	ClientUpdateService( const ClientUpdateService &)				 = delete ;
	/**
	 * @brief 稼働workerの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	ClientUpdateService		 &operator=( const ClientUpdateService &) = delete ;
	/**
	 * @brief manifest確認を一件だけ予約する。
	 * @param[in] manual_check 利用者が明示的に確認した場合はtrue。
	 * @return idle状態で受付けた場合はtrue。
	 */
	[[nodiscard]] bool		  RequestCheck( bool manual_check) noexcept ;
	/**
	 * @brief 現在提示中の新版に同意し、EXE取得を予約する。
	 * @return 同意待ち状態の一件を受付けた場合はtrue。
	 */
	[[nodiscard]] bool		  Approve() noexcept ;
	/** @brief 同意待ちの新版を取消し、このprocess中の同じ自動通知を抑止する。 */
	void					  Dismiss() noexcept ;
	/**
	 * @brief 同意済み・完全検証済み・全session終了時だけ適用を予約する。
	 * @param[in] active_sessions applicationが所有する生存session Window数。
	 * @return 条件を満たして受付けた場合はtrue。
	 */
	[[nodiscard]] bool		  ApplyIfIdle( std::size_t active_sessions) noexcept ;
	/**
	 * @brief UIが所有通知を一件取り出す。
	 * @param[out] notice 完了した結果。
	 * @return noticeが存在した場合はtrue。
	 */
	[[nodiscard]] bool		  TakeNotice( UpdateNotice &notice) noexcept ;
	/**
	 * @brief 現在の非機密更新状態を返す。
	 * @return mutexで保護された状態。
	 */
	[[nodiscard]] UpdateStage stage() const noexcept ;
	/** @brief requestを取消してworkerを終了する。全Window終了後だけjoinする。 */
	void					  Stop() noexcept ;

private:
	/** @brief workerが実行する有限操作種別。 */
	enum class Operation {
		/// manifest確認。
		Check,
		/// 同意したEXEの取得と検証。
		Prepare,
		/// 検証済みstageのhelper開始。
		Apply,
	} ;
	/** @brief mutexで受渡す一件の操作要求。 */
	struct Request {
		/// 実行する操作。
		Operation operation{Operation::Check} ;
		/// 手動確認の表示方針。
		bool	  manual{false} ;
	} ;
	/**
	 * @brief 同意とsession境界を満たした操作をworkerで直列実行する。
	 * @param[in] stop jthread終了要求。
	 */
	void								  Run( std::stop_token stop) noexcept ;
	/**
	 * @brief 完了結果を有限noticeへcommitし、UIだけを起床する。
	 * @param[in] notice 所有結果。
	 */
	void								  Publish( UpdateNotice notice) noexcept ;
	/// request、notice、同意状態だけを保護する。I/O中は保持しない。
	mutable std::mutex					  mutex_ ;
	/// workerだけが待つ条件変数。
	std::condition_variable_any			  condition_ ;
	/// workerが単独所有するplatform実装。
	std::unique_ptr<IClientUpdateBackend> backend_ ;
	/// 非blockingなUI起床callback。
	std::function<void()>				  notify_ ;
	/// 最大一件の未実行要求。
	std::optional<Request>				  request_ ;
	/// 最大一件の未取得通知。
	std::optional<UpdateNotice>			  notice_ ;
	/// 同意対象の完全manifest。
	std::optional<ReleaseManifest>		  offered_ ;
	/// 自動通知を取消した数値version。
	std::optional<UpdateVersion>		  dismissed_ ;
	/// backendから得た現在のversion。
	UpdateVersion						  current_ ;
	/// UIとworkerで共有する有限状態。
	UpdateStage							  stage_{UpdateStage::Idle} ;
	/// 明示的な同意を得た場合だけtrue。
	bool								  approved_{false} ;
	/// Stop後の再予約を防ぐ。
	bool								  stopping_{false} ;
	/// すべてのmember構築後に開始し、backend解放前に終了する所有thread。
	std::jthread						  worker_ ;
} ;

} // namespace zrdp::client::update
