//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_application.cpp
 * @brief 接続管理UI、session専用STA、Proxy slot割当を実装する。
 * @version 01.00.15
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Clientの同意付き更新と設定の分離を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/08  Agent設定画面と非同期保存を独立所有。
 * Ver.01.00.03   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/09  session画面のAgent配置・確認付き起動を追加。
 * Ver.01.00.05   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.06   Shunji Ogawa  2026/10/09  初期配置の停止段階とHRESULT診断を追加。
 * Ver.01.00.07   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.09   Shunji Ogawa  2026/10/10  トレイ終了後の同じRDP内Agent再起動を修正。
 * Ver.01.00.10   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.11   Shunji Ogawa  2026/10/10  接続別設定と自動Agent管理・診断削除を反映。
 * Ver.01.00.12   Shunji Ogawa  2026/10/10  managerメニューを撤廃し自動更新確認へ統一。
 * Ver.01.00.13   Shunji Ogawa  2026/10/10  Agent自動管理から設定画面の自動表示を除去。
 * Ver.01.00.14   Shunji Ogawa  2026/10/10  接続変更と登録済み宛先だけの表示・起動を統一。
 * Ver.01.00.15   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/app/client_application.h"

#include "common/diagnostic_log.h"
#include "client/app/resource.h"

#include <objbase.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <new>
#include <limits>
#include <optional>
#include <thread>
#include <utility>

namespace zrdp::client::app {
namespace {

// 固定数値snapshotの公開はUI threadを待機させないx64 atomicだけを使う
static_assert( std::atomic<std::uint64_t>::is_always_lock_free) ;

// session終了または履歴追加でmanager message loopを起床させるthread message
constexpr UINT kMessageSessionStateChanged		= WM_APP + 0x100U ;
// 更新workerの所有結果だけをmanager threadへ配送するmessage
constexpr UINT kMessageClientUpdateReady		= WM_APP + 0x101U ;
// Agent設定workerの所有結果だけをmanager STAへ配送するmessage
constexpr UINT kMessageAgentSettingsReady		= WM_APP + 0x102U ;
// 固定Ctrl+V要求を対象session所有STAへ配送するthread message
// 接続別設定を所有STAへ移転する限定message
constexpr UINT kMessageAgentConfiguration		= WM_APP + 0x104U ;
constexpr UINT kMessageDeploymentPaste			= WM_APP + 0x103U ;
// 稼働中の更新確認間隔millisecond。無限に短いpollingは行わない
constexpr UINT kUpdateCheckIntervalMilliseconds = 24U * 60U * 60U * 1000U ;

} // namespace

/**
 * @brief 一つのMainWindowとRDP ActiveXを専用COM STA threadで所有する。
 *
 * ActiveXの切断・購読解除・COM解放がMicrosoft内部で待機しても、その待機を
 * 接続管理Windowや他sessionのmessage loopへ波及させない。
 */
class SessionWindowThread final {
public:
	/**
	 * @brief session設定と非所有observerをthread開始前に保持する。
	 * @param[in] instance Client module instance。
	 * @param[in] observer 接続履歴をmanager STAへmarshalするapplication。
	 * @param[in] settings 一session専用の接続条件とProxy port。
	 * @param[in] manager_thread_id 完了通知先のWin32 thread ID。
	 */
	SessionWindowThread( HINSTANCE instance, ISessionWindowObserver &observer,
						SessionLaunchSettings settings, DWORD manager_thread_id)
		: instance_( instance), observer_( observer), settings_( std::move( settings)),
		  manager_thread_id_( manager_thread_id)
	{
		deployment_label_ =
			settings_.connection.connection_name.empty()
				? settings_.connection.server
				: settings_.connection.connection_name + L" (" + settings_.connection.server + L")" ;
	}

	/**
	 * @brief 生存threadがあればWM_QUITで同一STA上の解放を要求してjoinする。
	 */
	~SessionWindowThread()
	{
		StopAndJoin() ;
	}

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	SessionWindowThread( const SessionWindowThread &)			= delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	SessionWindowThread &operator=( const SessionWindowThread &) = delete ;

	/**
	 * @brief 専用STAを開始し、MainWindow生成結果だけを同期的に待つ。
	 * @param[in] show_command session Windowの初期表示command。
	 * @return COM、thread、MainWindowを利用可能にできた場合はtrue。
	 */
	[[nodiscard]] bool	 Start( const int show_command) noexcept
	{
		try {
			worker_ = std::thread( &SessionWindowThread::ThreadMain, this, show_command) ;
		} catch( ...) {
			return false ;
		}

		bool succeeded = false ;
		{
			std::unique_lock lock( startup_mutex_) ;
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief session STAの初期化完了を待つpredicateを提供する。
			 * @return 起動結果を確定済みの場合はtrue。
			 * @note startup_mutex_保持中だけstartup_complete_を参照する。
			 * @endcond
			 */
			startup_condition_.wait( lock, [this]() noexcept { return startup_complete_ ; }) ;
			succeeded = startup_succeeded_ ;
		}

		// 生成失敗threadをvectorへ公開せず、COM解放完了までここで回収する
		if( !succeeded) {
			JoinCompleted() ;
		}

		return succeeded ;
	}

	/**
	 * @brief session STAがMainWindowとCOMをすべて解放済みか返す。
	 * @return 全所有resourceを解放してthread終了直前ならtrue。
	 */
	[[nodiscard]] bool finished() const noexcept
	{
		return finished_.load( std::memory_order_acquire) ;
	}

	/** @brief owner STAが公開した認証世代を返す。 @return 未認証では0。 */
	[[nodiscard]] std::uint64_t deployment_epoch() const noexcept
	{
		return deployment_epoch_.load( std::memory_order_acquire) ;
	}

	/** @brief runtimeだけに表示する接続名を返す。 @return 開始時に固定した非所有view。 */
	[[nodiscard]] std::wstring_view deployment_label() const noexcept
	{
		return deployment_label_ ;
	}

	/** @brief 接続前に固定した配布driveを返す。 @return 失敗時はnullptr。 */
	[[nodiscard]] std::shared_ptr<common::agent_setup::DeploymentDrive>
	deployment_drive() const noexcept
	{
		return deployment_drive_ ;
	}

	/** @brief 自動起動の入力と応答状態を返す。 @return 0は待機、1は応答、-1は入力失敗。 */
	[[nodiscard]] agent_deployment::AutomaticDeploymentSnapshot automatic_status() const noexcept
	{
		return agent_deployment::DecodeAutomaticDeploymentSnapshot(
			automatic_status_.load( std::memory_order_acquire)) ;
	}

	/** @brief 同じ認証世代のINIを最大一件だけowner STAへ渡す。
	 * @param[in] epoch 受付時の認証世代。
	 * @param[in] ini 承認済みINI。空は停止。
	 * @param[in] bootstrap 固定EXE更新を設定確認より先に行う場合true。
	 * @return queueと通知を予約できた場合true。
	 */
	[[nodiscard]] bool QueueConfiguration( std::uint64_t epoch, std::string ini,
										  bool bootstrap = false)
	{
		std::scoped_lock lock( deployment_mutex_) ;
		// 終了、世代失効、既存予約、巨大INIで既存要求を上書きしない
		if( finished() || epoch == 0U || epoch != deployment_epoch() || configuration_request_ ||
		   ini.size() > 65536U) {
			return false ;
		}

		configuration_request_ = ConfigurationRequest{epoch, std::move( ini), bootstrap} ;
		// post失敗を実行済みと扱わない
		if( !PostThreadMessageW( thread_id_.load(), kMessageAgentConfiguration, 0U, 0L)) {
			configuration_request_.reset() ;
			return false ;
		}

		return true ;
	}

	/** @brief 固定失敗表示を最大一件のowner queueへ予約する。
	 * @param[in] epoch 失敗を記録した認証世代。
	 * @return 同じ世代へ予約できた場合true。
	 */
	[[nodiscard]] bool QueueLifecycleFailure( std::uint64_t epoch)
	{
		std::scoped_lock lock( deployment_mutex_) ;
		// 未処理設定と別世代の失敗を上書きしない
		if( finished() || epoch == 0U || epoch != deployment_epoch() || configuration_request_) {
			return false ;
		}

		configuration_request_ = ConfigurationRequest{epoch, {}, false, true} ;
		// 通知失敗では次の観測で再予約する
		if( !PostThreadMessageW( thread_id_.load(), kMessageAgentConfiguration, 0U, 0L)) {
			configuration_request_.reset() ;
			return false ;
		}

		return true ;
	}

	/** @brief 設定確認の数値snapshotを返す。 @return owner STAで公開した固定状態。 */
	[[nodiscard]] unsigned lifecycle_status() const noexcept
	{
		return lifecycle_status_.load() ;
	}

	/** @brief 前面対象の固定入力開始条件を返す。 @return owner STAが確認した場合true。 */
	[[nodiscard]] bool input_ready() const noexcept
	{
		return input_ready_.load() ;
	}

	/** @brief 一回の固定キー要求の所有結果。 */
	struct PasteResult {
		/// 準備snapshotと一致させる受付ID。
		std::uint64_t ticket{0U} ;
		/// Microsoft入力要求のHRESULT。配置完了ではない。
		HRESULT		  result{E_FAIL} ;
	} ;
	/** @brief bounded queueへ一件だけ貼付けを予約する。
	 * @param[in] epoch 準備時の認証世代。
	 * @param[in] sequence 確認したclipboard世代。
	 * @param[in] ticket 準備snapshotの受付ID。
	 * @return 所有STAのmessage queueへ予約できた場合だけtrue。
	 */
	[[nodiscard]] bool QueuePaste( std::uint64_t epoch, DWORD sequence,
								  std::uint64_t ticket) noexcept
	{
		std::scoped_lock lock( deployment_mutex_) ;
		// 対象終了・古い世代・既存要求・未取得結果では新規要求を上書きしない
		if( finished() || epoch == 0U || epoch != deployment_epoch() || paste_request_ ||
		   paste_result_ || paste_inflight_) {
			return false ;
		}

		paste_request_ = PasteRequest{epoch, sequence, ticket} ;
		// post失敗では予約状態だけを戻し、別経路からキーを送らない
		if( !PostThreadMessageW( thread_id_.load( std::memory_order_acquire), kMessageDeploymentPaste,
							   0U, 0L)) {
			paste_request_.reset() ;
			return false ;
		}

		return true ;
	}

	/** @brief 最大一件の結果をmanagerへ渡す。
	 * @param[out] result キー要求の所有結果。
	 * @return 結果を移転できた場合だけtrue。
	 */
	[[nodiscard]] bool TakePasteResult( PasteResult &result) noexcept
	{
		std::scoped_lock lock( deployment_mutex_) ;
		// 未完了を成功結果と扱わない
		if( !paste_result_) {
			return false ;
		}

		result = *paste_result_ ;
		paste_result_.reset() ;
		return true ;
	}

	/**
	 * @brief finished確認済みthreadをblockなしで回収する。
	 */
	void JoinCompleted() noexcept
	{
		// 所有workerが起動済みなら終了を待ち、threadを残したまま破棄しない
		if( worker_.joinable()) {
			worker_.join() ;
		}
	}

private:
	/**
	 * @brief MainWindow生成結果を待機中managerへ一度だけ公開する。
	 * @param[in] succeeded COMとMainWindowを利用可能にできた場合はtrue。
	 */
	void PublishStartupResult( const bool succeeded) noexcept
	{
		{
			std::scoped_lock lock( startup_mutex_) ;
			// 例外経路と通常経路から同じ結果を二重公開しない
			if( startup_complete_) {
				return ;
			}

			startup_succeeded_ = succeeded ;
			startup_complete_  = true ;
		}

		startup_condition_.notify_one() ;
	}

	/**
	 * @brief session専用COM STA、Window、message loopを順序付きで実行する。
	 * @param[in] show_command session Windowの初期表示command。
	 */
	void ThreadMain( const int show_command) noexcept
	{
		// UIとActiveX生成前のstartup workerで空の専用driveを準備する
		deployment_drive_ = common::agent_setup::DeploymentDrive::Create(
			common::agent_setup::DeploymentDrive::DefaultBase()) ;
		settings_.deployment_drive	  = deployment_drive_ ;
		const DWORD current_thread_id = GetCurrentThreadId() ;
		thread_id_.store( current_thread_id, std::memory_order_release) ;

		// destructorからWM_QUITを必ずpostできるよう、開始直後にmessage queueを作る
		MSG queue_probe{} ;
		static_cast<void>( PeekMessageW( &queue_probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE)) ;

		const HRESULT com_result = CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED) ;
		// ActiveXをMTAまたは未初期化threadへ生成しない
		if( FAILED( com_result)) {
			PublishStartupResult( false) ;
			finished_.store( true, std::memory_order_release) ;
			NotifyManager() ;
			return ;
		}

		try {
			{
				MainWindow window( instance_, observer_, std::move( settings_)) ;
				const bool created = window.Create( show_command) ;
				PublishStartupResult( created) ;
				// Agent停止中も所有STAから前面条件を一秒周期で公開する
				if( created) {
					static_cast<void>( SetTimer( window.window(), 0x702U, 1000U, nullptr)) ;
				}

				// このSTAが所有する一つのWindowだけをdispatchする
				while( created && window.window() != nullptr) {
					MSG		   message{} ;
					const BOOL result = GetMessageW( &message, nullptr, 0U, 0U) ;
					// message loopの終了または取得失敗でsession STAの後処理へ進む
					if( result <= 0) {
						break ;
					}

					// dialog navigationが消費しなかったmessageだけを通常dispatchへ渡す
					if( message.hwnd == nullptr && message.message == kMessageDeploymentPaste) {
						std::optional<PasteRequest> request ;
						{
							std::scoped_lock lock( deployment_mutex_) ;
							request.swap( paste_request_) ;
							paste_inflight_ = request.has_value() ;
						}

						// COMはqueue lockの外で、実際のowner STA上からだけ呼ぶ
						if( request) {
							const auto paste_status =
								request->sequence == 0U
									? window.StartAutomaticDeployment( request->epoch)
									: window.PasteDeploymentFiles( request->epoch,
																  request->sequence) ;
							// managerへの結果通知より先に詳細を整合した一値で公開する
							automatic_status_.store(
								agent_deployment::EncodeAutomaticDeploymentSnapshot(
									window.AutomaticDeploymentStatus()),
								std::memory_order_release) ;
							{
								std::scoped_lock lock( deployment_mutex_) ;
								paste_result_	= PasteResult{request->ticket, paste_status} ;
								paste_inflight_ = false ;
							}

							NotifyManager() ;
						}
					}

					// 設定は所有STAで世代を再確認し、COMをmanagerから呼ばない
					else if( message.hwnd == nullptr &&
							message.message == kMessageAgentConfiguration) {
						std::optional<ConfigurationRequest> request ;
						{
							std::scoped_lock lock( deployment_mutex_) ;
							request.swap( configuration_request_) ;
						}

						// stale予約は別の認証世代へ移さない
						if( request) {
							// 停止表示は承認INIを別接続へ付け替えない
							if( request->failure) {
								window.MarkAgentLifecycleFailure( request->epoch) ;
							} else {
								static_cast<void>( window.ApplyAgentConfiguration(
									request->epoch, std::move( request->ini), request->bootstrap)) ;
							}
						}
					}
					// dialog navigation未処理の通常messageだけをdispatchする
					else if( !window.ProcessDialogMessage( message)) {
						TranslateMessage( &message) ;
						DispatchMessageW( &message) ;
					}

					lifecycle_status_.store( window.AgentLifecycleStatus()) ;
					input_ready_.store( window.CanBeginAutomaticAgentInput()) ;
					deployment_epoch_.store( window.DeploymentEpoch(), std::memory_order_release) ;
					automatic_status_.store( agent_deployment::EncodeAutomaticDeploymentSnapshot(
												window.AutomaticDeploymentStatus()),
											std::memory_order_release) ;
				}
			}
		} catch( ...) {
			// session thread境界から例外を漏らさず、他sessionを継続する
			PublishStartupResult( false) ;
			common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
									   common::DiagnosticEvent::UnexpectedException) ;
		}

		CoUninitialize() ;
		deployment_epoch_.store( 0U, std::memory_order_release) ;
		finished_.store( true, std::memory_order_release) ;
		NotifyManager() ;
	}

	/**
	 * @brief manager threadを起床し、session回収または履歴反映を促す。
	 */
	void NotifyManager() const noexcept
	{
		// main message queueが終了済みでも、通知失敗をsession異常へ昇格しない
		if( manager_thread_id_ != 0U) {
			static_cast<void>(
				PostThreadMessageW( manager_thread_id_, kMessageSessionStateChanged, 0U, 0L)) ;
		}
	}

	/**
	 * @brief application rollback時に、COM所有STAへ直接触れず終了を要求する。
	 */
	void StopAndJoin() noexcept
	{
		// worker未起動なら停止通知とjoinを行わない
		if( !worker_.joinable()) {
			return ;
		}

		// 終了済みでないsessionだけへWM_QUITを送り、Window破棄は所有STAで行う
		if( !finished()) {
			const DWORD target_thread_id = thread_id_.load( std::memory_order_acquire) ;
			// STA thread IDが確定したsessionだけへ終了messageを送る
			if( target_thread_id != 0U) {
				static_cast<void>( PostThreadMessageW( target_thread_id, WM_QUIT, 0U, 0L)) ;
			}
		}

		worker_.join() ;
	}

	// -------------------------------------------------------------------------
	// session専用STAの所有状態
	// -------------------------------------------------------------------------

	/** @brief managerからowner STAへ渡す一件の固定キー要求。 */
	struct PasteRequest {
		/// 準備時の認証世代。
		std::uint64_t epoch ;
		/// 公開内容を差替えられた場合に拒否するclipboard世代。
		DWORD		  sequence ;
		/// 設定workerの受付ID。
		std::uint64_t ticket ;
	} ;

	/** @brief 内容をmessage引数へ出さない所有INI予約。 */
	struct ConfigurationRequest {
		/// 対象の認証世代。
		std::uint64_t epoch ;
		/// 完全INI。
		std::string	  ini ;
		/// 設定確認前の固定bootstrap。
		bool		  bootstrap ;
		/// INIの変更ではなく、同じsessionへの固定失敗表示。
		bool		  failure{false} ;
	} ;
	/// 最大一件の未実行INI。deployment_mutex_だけで保護する。
	std::optional<ConfigurationRequest>					  configuration_request_ ;
	/// 機密を含まない設定確認状態。
	std::atomic<unsigned>								  lifecycle_status_{0U} ;
	/// owner STAの前面入力条件。操作直前にも再確認する。
	std::atomic<bool>									  input_ready_{false} ;
	/// runtimeの接続先表示。thread開始前に固定する。
	std::wstring										  deployment_label_ ;
	/// startup完了前に固定し、sessionの終了まで同じdriveを所有する。
	std::shared_ptr<common::agent_setup::DeploymentDrive> deployment_drive_ ;
	/// managerへ内容を含めず公開する自動入力の状態。
	std::atomic<std::uint64_t>							  automatic_status_{
		   agent_deployment::EncodeAutomaticDeploymentSnapshot( {})} ;
	/// managerへ公開する認証完了世代。
	std::atomic<std::uint64_t>	deployment_epoch_{0U} ;
	/// 一件要求と一件応答の所有だけを保護する。
	std::mutex					deployment_mutex_ ;
	/// 最大一件の未実行キー要求。
	std::optional<PasteRequest> paste_request_ ;
	/// 最大一件の未取得結果。
	std::optional<PasteResult>	paste_result_ ;
	/// COM実行中に第二要求を受け付けない状態。
	bool						paste_inflight_{false} ;
	/// Client module instanceへの非所有参照。
	HINSTANCE					instance_ ;

	/// manager STAで生存する履歴通知先への非所有参照。
	ISessionWindowObserver	   &observer_ ;

	/// thread開始後にMainWindowへ一度だけmoveするsession設定。
	SessionLaunchSettings		settings_ ;

	/// session終了と履歴追加を通知するmanagerのWin32 thread ID。
	DWORD						manager_thread_id_{0U} ;

	/// 一つのCOM STAとmessage loopを所有するjoin対象thread。
	std::thread					worker_ ;

	/// WM_QUITの通知先としてmanager threadから参照するsession thread ID。
	std::atomic<DWORD>			thread_id_{0U} ;

	/// MainWindow、ActiveX、COMを全て解放したことを公開するflag。
	std::atomic<bool>			finished_{false} ;

	/// 起動結果の一回公開だけを保護するmutex。
	std::mutex					startup_mutex_ ;

	/// MainWindow生成完了までStartだけを待機させるcondition variable。
	std::condition_variable		startup_condition_ ;

	/// 正常経路と例外経路から起動結果を二重公開しないためのflag。
	bool						startup_complete_{false} ;

	/// Startへ返すCOM／MainWindow生成結果。
	bool						startup_succeeded_{false} ;
} ;

// -----------------------------------------------------------------------------
// 接続管理application
// -----------------------------------------------------------------------------

ClientApplication::ClientApplication(
	const HINSTANCE instance, std::shared_ptr<update::ClientUpdateStartupState> update_startup)
	: instance_( instance), manager_thread_id_( GetCurrentThreadId()),
	  update_startup_( std::move( update_startup))
{
}

ClientApplication::~ClientApplication()
{
	// 全Window終了後にだけtimerと更新workerを回収し、UI callback中はjoinしない
	if( update_timer_ != 0U) {
		static_cast<void>( KillTimer( nullptr, update_timer_)) ;
	}

	// 自動Agent観測timerも全画面終了後に回収する
	if( agent_timer_ != 0U) {
		KillTimer( nullptr, agent_timer_) ;
	}

	update_service_.reset() ;
	agent_settings_window_.reset() ;
	agent_settings_service_.reset() ;
	// 各sessionを所有STA上で解放してからmanager、ATL、COMの順序へ戻す
	sessions_.clear() ;
	manager_.reset() ;
	proxy_port_allocator_.reset() ;
}

bool ClientApplication::Create( const int show_command)
{
	// manager公開前に有限64 KiBの固定local設定を一度だけ確定し、session間で共有する
	proxy_configuration_result_ =
		proxy::LoadClientProxyConfigurationFromExecutableDirectory( proxy_configuration_) ;

	try {
		sessions_.reserve( kMaximumConcurrentRdpSessions) ;
		pending_successful_connections_.reserve( kMaximumConcurrentRdpSessions) ;
		proxy_port_allocator_ =
			std::make_unique<SessionProxyPortAllocator>( proxy_configuration_.port) ;
		manager_ = std::make_unique<ConnectionManagerWindow>( instance_, *this) ;
		// manager生成失敗時は部分所有objectを公開せずrollbackする
		if( !manager_->Create( show_command)) {
			manager_.reset() ;
			proxy_port_allocator_.reset() ;
			return false ;
		}

		UpdateManagerStatus() ;
		EnsureAgentSettingsService() ;
		launch_load_request_ = agent_settings_service_->Request( {}) ;
		agent_timer_		 = SetTimer( nullptr, 0U, 1000U, nullptr) ;
		// timerなしで自動監視可能とは扱わない
		if( agent_timer_ == 0U) {
			throw std::bad_alloc{} ;
		}

		try {
			update_service_ = std::make_unique<update::ClientUpdateService>(
				update::MakeWindowsUpdateBackend( update_startup_),
				/**
				 * @cond LOCAL_CALLBACK_CONTRACT
				 * @brief 更新workerの所有結果をmanager STAへ非blockingで通知する。
				 * @note captured thread IDだけを使い、workerからWindow/COMを直接操作しない。
				 * @endcond
				 */
				[thread = manager_thread_id_]() noexcept {
					static_cast<void>(
						PostThreadMessageW( thread, kMessageClientUpdateReady, 0U, 0L)) ;
				}) ;
			update_timer_ = SetTimer( nullptr, 0U, kUpdateCheckIntervalMilliseconds, nullptr) ;
			static_cast<void>( update_service_->RequestCheck( false)) ;
			UpdateManagerStatus() ;
		} catch( ...) {
			// 更新の補助workerを作れなくても接続管理UIとRDPを継続する
			update_service_.reset() ;
			common::WriteDiagnosticLog( common::DiagnosticLevel::Warning,
									   common::DiagnosticEvent::UnexpectedException) ;
		}

		return true ;
	} catch( ...) {
		// startup allocation例外をWinMainへ固定失敗として返す
		manager_.reset() ;
		proxy_port_allocator_.reset() ;
		sessions_.clear() ;
		return false ;
	}
}

int ClientApplication::RunMessageLoop()
{
	MSG message{} ;

	// manager STAはmanager UIとsessionからのthread messageだけを直列化する
	while( true) {
		const BOOL result = GetMessageW( &message, nullptr, 0U, 0U) ;
		// message取得失敗を正常終了と区別してWin32 errorを返す
		if( result == -1) {
			return static_cast<int>( GetLastError()) ;
		}

		// WM_QUITを受けた場合は終了codeを保持してprocess共通loopを終える
		if( result == 0) {
			return static_cast<int>( message.wParam) ;
		}

		const bool internal_notification =
			message.hwnd == nullptr &&
			(message.message == kMessageSessionStateChanged ||
			 message.message == kMessageClientUpdateReady ||
			 message.message == kMessageAgentSettingsReady ||
			 (message.message == WM_TIMER && agent_timer_ != 0U && message.wParam == agent_timer_)) ;
		const bool update_tick = message.hwnd == nullptr && message.message == WM_TIMER &&
								 update_timer_ != 0U && message.wParam == update_timer_ ;
		// 生存中managerがある場合だけ周期確認を予約し、消えたUIへ通知を蓄積しない
		if( update_tick && update_service_ != nullptr && manager_ != nullptr &&
		   manager_->window() != nullptr) {
			static_cast<void>( update_service_->RequestCheck( false)) ;
		}

		// thread messageをUSER32既定dispatchへ渡さず、下のqueue drainだけに使用する
		if( !internal_notification && !update_tick && !ProcessDialogMessage( message)) {
			TranslateMessage( &message) ;
			DispatchMessageW( &message) ;
		}

		DrainSuccessfulConnections() ;
		CleanupDestroyedWindows() ;
		DrainSessionAgentSettings() ;
		DrainAgentSettings() ;
		DrainAgentDeployment() ;
		AdvanceAgentLifecycle() ;
		DrainClientUpdates() ;
		// 更新dialogのmodal loop中に閉じられたWindowも、次のGetMessage待機前に回収する
		CleanupDestroyedWindows() ;
		AdvanceClientUpdate() ;
		// helper受付後もsessionを強制切断せず、全session終了済みの状態だけで終了する
		if( exit_for_update_ && sessions_.empty()) {
			return 0 ;
		}

		// managerと全sessionがなくなった場合だけprocess message loopを終了する
		if( manager_ == nullptr && sessions_.empty() &&
		   (agent_settings_window_ == nullptr || agent_settings_window_->window() == nullptr)) {
			// 最後の画面を閉じても受付済み設定保存の完了までは有限workerを保持する
			if( agent_settings_service_ != nullptr && agent_settings_service_->Busy()) {
				continue ;
			}

			const auto stage =
				update_service_ == nullptr ? update::UpdateStage::Idle : update_service_->stage() ;
			// 同意済みdownload／適用の結果だけは全Window終了後も有限workerへ任せる
			if( stage == update::UpdateStage::Downloading ||
			   stage == update::UpdateStage::ReadyToApply ||
			   stage == update::UpdateStage::Applying) {
				continue ;
			}

			return 0 ;
		}
	}
}

SessionOpenResult ClientApplication::OpenSession( SessionConnectionRequest request) noexcept
{
	// 固定入力のclipboard公開中だけ追加接続を延期する
	if( deployment_target_ != 0U) {
		return {SessionOpenError::DeploymentInProgress, 0U} ;
	}

	// 同意後のhelper受付中だけ新規sessionを止め、既存sessionは変更しない
	if( update_service_ != nullptr &&
	   (update_service_->stage() == update::UpdateStage::Applying ||
		update_service_->stage() == update::UpdateStage::HelperReady)) {
		return {SessionOpenError::UpdateInProgress, 0U} ;
	}

	// session数を有限上限へ固定し、UI連打でActiveX／threadを無制限生成しない
	if( sessions_.size() >= kMaximumConcurrentRdpSessions) {
		return {
			SessionOpenError::SessionLimitReached,
			0U,
		} ;
	}

	// port allocatorを用意できなければ専用listenerを偽装せずsession生成を拒否する
	if( proxy_port_allocator_ == nullptr) {
		return {
			SessionOpenError::ResourceExhausted,
			0U,
		} ;
	}

	const std::optional<SessionProxyPortLease> lease = proxy_port_allocator_->Acquire() ;
	// base末尾のoverflowまたは全slot使用中では新規sessionだけを拒否する
	if( !lease.has_value()) {
		return {
			SessionOpenError::ProxyPortUnavailable,
			0U,
		} ;
	}

	try {
		// 古い対象IDを新しいsessionへ割り当てない
		if( session_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
			static_cast<void>( proxy_port_allocator_->Release( lease->slot)) ;
			return {SessionOpenError::ResourceExhausted, 0U} ;
		}

		const auto binding = agent_settings::BuildAgentConnectionKey( request.server, request.domain,
																	 request.user_name) ;
		// 不正な接続組を共通設定へfallbackしない
		if( binding.empty()) {
			static_cast<void>( proxy_port_allocator_->Release( lease->slot)) ;
			return {SessionOpenError::WindowCreationFailed, 0U} ;
		}

		const auto			  connection = request ;
		const auto			  session_id = ++session_sequence_ ;
		SessionLaunchSettings settings{
			std::move( request), proxy_configuration_result_.succeeded() ? lease->port : 0U,
			lease->slot,		proxy_configuration_result_.error,
			session_id,
		} ;
		auto session = std::make_unique<SessionWindowThread>( instance_, *this, std::move( settings),
															 manager_thread_id_) ;
		// COMまたはActiveX生成失敗時はsession一覧とport leaseへ公開しない
		if( !session->Start( SW_SHOWNORMAL)) {
			static_cast<void>( proxy_port_allocator_->Release( lease->slot)) ;
			return {
				SessionOpenError::WindowCreationFailed,
				0U,
			} ;
		}

		sessions_.push_back( SessionRecord{
			session_id,
			std::move( session),
			lease->slot,
			connection,
			binding,
		}) ;
		UpdateManagerStatus() ;
		return {
			SessionOpenError::None,
			proxy_configuration_result_.succeeded() ? lease->port : 0U,
		} ;
	} catch( const std::bad_alloc &) {
		static_cast<void>( proxy_port_allocator_->Release( lease->slot)) ;
		return {
			SessionOpenError::ResourceExhausted,
			0U,
		} ;
	} catch( ...) {
		static_cast<void>( proxy_port_allocator_->Release( lease->slot)) ;
		return {
			SessionOpenError::WindowCreationFailed,
			0U,
		} ;
	}
}

void ClientApplication::RememberSuccessfulConnection(
	std::wstring connection_name, std::wstring server, std::wstring domain, std::wstring user_name,
	const rdp::RdpAudioRedirectionSettings audio) noexcept
{
	try {
		{
			std::scoped_lock lock( successful_connections_mutex_) ;
			pending_successful_connections_.push_back( PendingSuccessfulConnection{
				std::move( connection_name),
				std::move( server),
				std::move( domain),
				std::move( user_name),
				audio,
			}) ;
		}

		// manager objectへsession threadから触れず、main queueだけを起床する
		static_cast<void>(
			PostThreadMessageW( manager_thread_id_, kMessageSessionStateChanged, 0U, 0L)) ;
	} catch( ...) {
		// 履歴保存失敗で成立済みRDP sessionを切断しない
		common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
								   common::DiagnosticEvent::UnexpectedException) ;
	}
}

void ClientApplication::CleanupDestroyedWindows() noexcept
{
	try {
		// managerのWM_NCDESTROY後だけ所有controllerを解放する
		if( manager_ != nullptr && manager_->window() == nullptr) {
			manager_.reset() ;
		}

		auto iterator = sessions_.begin() ;
		// 終了済みSTAだけをjoinし、対応するport slotを一度だけ解放する
		while( iterator != sessions_.end()) {
			// 生存中sessionは保持し、完了済みsessionだけを回収対象にする
			if( iterator->thread != nullptr && !iterator->thread->finished()) {
				++iterator ;
				continue ;
			}

			// 終了が確認されたworkerだけをjoinしてthread所有を解放する
			if( iterator->thread != nullptr) {
				iterator->thread->JoinCompleted() ;
			}

			// 対応するport slotだけを解放し、他sessionの割当には触れない
			if( proxy_port_allocator_ != nullptr) {
				static_cast<void>( proxy_port_allocator_->Release( iterator->proxy_slot)) ;
			}

			iterator = sessions_.erase( iterator) ;
		}

		UpdateManagerStatus() ;
	} catch( ...) {
		// cleanup境界で例外が出ても生存Windowのmessage dispatchを継続する
		common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
								   common::DiagnosticEvent::UnexpectedException) ;
	}
}

void ClientApplication::DrainSuccessfulConnections() noexcept
{
	try {
		std::vector<PendingSuccessfulConnection> pending ;
		{
			std::scoped_lock lock( successful_connections_mutex_) ;
			pending.swap( pending_successful_connections_) ;
		}

		// manager終了後の履歴通知は破棄し、生存workerや無効HWNDへ渡さない
		if( manager_ == nullptr || manager_->window() == nullptr) {
			return ;
		}

		// 各sessionの成功順を維持して共通DPAPI履歴workerへ渡す
		for( PendingSuccessfulConnection &connection : pending) {
			manager_->RememberSuccessfulConnection(
				std::move( connection.connection_name), std::move( connection.server),
				std::move( connection.domain), std::move( connection.user_name), connection.audio) ;
		}
	} catch( ...) {
		// 履歴queueのallocation失敗でsession message loopを停止しない
		common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
								   common::DiagnosticEvent::UnexpectedException) ;
	}
}

void ClientApplication::UpdateManagerStatus() noexcept
{
	try {
		// managerまたはallocator未生成時は未確定状態をUIへ通知しない
		if( manager_ == nullptr || manager_->window() == nullptr ||
		   proxy_port_allocator_ == nullptr) {
			return ;
		}

		const std::optional<std::uint16_t> next_port =
			proxy_configuration_result_.succeeded() ? proxy_port_allocator_->NextAvailablePort()
													: std::nullopt ;
		manager_->UpdateRuntimeStatus( sessions_.size(), next_port,
									  proxy_configuration_result_.error) ;
		UINT identifier = 0U ;
		// 更新の詳細や取得URLをtitleへ混入させず、固定の短い状態だけを表示する
		if( update_service_ != nullptr) {
			// workerが公開する有限状態から日本語resourceを選ぶ
			switch( update_service_->stage()) {
			// manifestだけを確認中で、RDP入力を止めない
			case update::UpdateStage::Checking:
				identifier = IDS_UPDATE_CHECKING ;
				break ;

			// 同意したEXEを取得・検証中でも既存／新規RDPを維持する
			case update::UpdateStage::Downloading:
				identifier = IDS_UPDATE_DOWNLOADING ;
				break ;

			// 全session Window終了まで適用しない
			case update::UpdateStage::ReadyToApply:
				identifier = IDS_UPDATE_WAITING ;
				break ;

			// helper受付中だけ新規sessionを拒否する
			case update::UpdateStage::Applying:
			// helper引受済みは親終了まで適用状態を維持する
			case update::UpdateStage::HelperReady:
				identifier = IDS_UPDATE_APPLYING ;
				break ;

			// 未保留／同意dialog表示は通常session状態だけを示す
			case update::UpdateStage::Idle:
			// 同意前は通常状態を維持し、modal dialogだけで更新情報を示す
			case update::UpdateStage::Offered:
				break ;
			}
		}

		manager_->UpdateClientUpdateStatus( identifier == 0U ? std::wstring{}

															: LoadUpdateString( identifier)) ;
	} catch( ...) {
		// 表示用文字列の確保失敗でcommit済みsessionのport slotを解放しない
		common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
								   common::DiagnosticEvent::UnexpectedException) ;
	}
}

bool ClientApplication::ProcessDialogMessage( MSG &message) const noexcept
{
	// 配置画面はmanager STA上で処理し、session STAへ同期SendMessageしない

	// 独立設定dialogもmanagerと同じSTAの標準Tab処理へ通す
	if( agent_settings_window_ != nullptr && agent_settings_window_->ProcessDialogMessage( message)) {
		return true ;
	}

	// session Windowは各専用STAで処理するため、manager階層だけを対象にする
	return manager_ != nullptr && manager_->ProcessDialogMessage( message) ;
}

std::wstring ClientApplication::LoadUpdateString( const UINT identifier) const
{
	std::array<wchar_t, 1536U> text{} ;
	const int				   length =
		LoadStringW( instance_, identifier, text.data(), static_cast<int>( text.size())) ;
	return length > 0 ? std::wstring( text.data(), static_cast<std::size_t>( length))
					  : std::wstring{} ;
}

void ClientApplication::ShowClientUpdateError( const update::UpdateError error) const noexcept
{
	try {
		const UINT				   identifier = error == update::UpdateError::InvalidSignature ||
										error == update::UpdateError::SignerMismatch
													? IDS_UPDATE_SIGNATURE_FAILURE_FORMAT
													: IDS_UPDATE_FAILURE_FORMAT ;
		const auto				   format	  = LoadUpdateString( identifier) ;
		std::array<wchar_t, 1536U> text{} ;
		static_cast<void>(
			swprintf_s( text.data(), text.size(), format.c_str(), static_cast<unsigned int>( error))) ;
		MessageBoxW( manager_ == nullptr ? nullptr : manager_->window(), text.data(),
					L"ZRdp SVN Bridge", MB_OK | MB_ICONWARNING) ;
	} catch( ...) {
		// 補助dialogの確保失敗でRDPやworker状態を変更しない
		common::WriteDiagnosticLog( common::DiagnosticLevel::Warning,
								   common::DiagnosticEvent::UnexpectedException) ;
	}
}

void ClientApplication::DrainClientUpdates() noexcept
{
	try {
		// 補助worker未生成時はRDPのmessage loopだけを維持する
		if( update_service_ == nullptr) {
			return ;
		}

		update::UpdateNotice notice ;
		// 最大一件の所有結果だけをUIへ移す
		if( !update_service_->TakeNotice( notice)) {
			return ;
		}

		// 明示確認／同意済み操作の失敗だけをdialog化し、周期確認失敗で作業を妨げない
		if( notice.error != update::UpdateError::None) {
			// 同意前の自動確認では失敗dialogを連続表示しない
			if( notice.manual_check || notice.release.has_value()) {
				ShowClientUpdateError( notice.error) ;
			}
		}
		// 有効な新版だけを提示し、明示同意を取得する
		else if( notice.stage == update::UpdateStage::Offered && notice.release.has_value()) {
			// 接続管理Windowが消えた場合は同意を取得できないためdownloadを開始しない
			if( manager_ == nullptr || manager_->window() == nullptr) {
				update_service_->Dismiss() ;
			} else {
				const auto old_version = update::FormatUpdateVersion( notice.current_version) ;
				const auto new_version = update::FormatUpdateVersion( notice.release->version) ;
				const std::wstring		   old_text( old_version.begin(), old_version.end()) ;
				const std::wstring		   new_text( new_version.begin(), new_version.end()) ;
				const auto				   format = LoadUpdateString( IDS_UPDATE_OFFER_FORMAT) ;
				std::array<wchar_t, 1536U> text{} ;
				// 完全な説明を表示できない状態では同意を取得したことにしない
				if( format.empty() || swprintf_s( text.data(), text.size(), format.c_str(),
												old_text.c_str(), new_text.c_str()) < 0) {
					update_service_->Dismiss() ;
					ShowClientUpdateError( update::UpdateError::ResourceExhausted) ;
				}
				// 初期選択は「いいえ」とし、利用者の「はい」だけを受付ける
				else if( MessageBoxW( manager_->window(), text.data(), L"ZRdp SVN Bridge",
									MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2) == IDYES) {
					// 「はい」の明示応答だけがEXE取得を許可する
					static_cast<void>( update_service_->Approve()) ;
				} else {
					update_service_->Dismiss() ;
				}
			}
		}
		// 検証済みhelperが引受けた場合だけ親Client終了へ進める
		else if( notice.stage == update::UpdateStage::HelperReady) {
			// 全session終了後だけmanagerを終了し、helperへ旧版process終了を渡す
			if( sessions_.empty()) {
				// managerが生存している場合だけそのWindowを通常破棄する
				if( manager_ != nullptr && manager_->window() != nullptr) {
					DestroyWindow( manager_->window()) ;
				}

				exit_for_update_ = true ;
			}
		}
		// 利用者の明示確認には「新版なし／無効」を一件通知する
		else if( notice.manual_check && notice.stage == update::UpdateStage::Idle) {
			// 利用者の手動確認には最新版／無効のどちらかを明示する
			const auto text =
				LoadUpdateString( notice.configured ? IDS_UPDATE_LATEST : IDS_UPDATE_DISABLED) ;
			MessageBoxW( manager_ == nullptr ? nullptr : manager_->window(), text.c_str(),
						L"ZRdp SVN Bridge", MB_OK | MB_ICONINFORMATION) ;
		}

		UpdateManagerStatus() ;
	} catch( ...) {
		// dialog準備の失敗をdownload同意として扱わず、RDPを継続する
		if( update_service_ != nullptr) {
			update_service_->Dismiss() ;
		}

		ShowClientUpdateError( update::UpdateError::ResourceExhausted) ;
	}
}

void ClientApplication::AdvanceClientUpdate() noexcept
{
	// 未完了の配置公開を更新helperによるClient終了で中断しない

	// 未保存の設定画面や受付済み保存を自動更新で終了・中断しない
	if( (agent_settings_window_ != nullptr && agent_settings_window_->window() != nullptr) ||
	   (agent_settings_service_ != nullptr && agent_settings_service_->Busy())) {
		return ;
	}

	// 全session Windowの破棄・COM解放が完了した場合だけhelperを予約する
	if( update_service_ != nullptr && update_service_->ApplyIfIdle( sessions_.size())) {
		UpdateManagerStatus() ;
	}
}

bool ClientApplication::RequestSessionAgentSettings( std::uint64_t session_id,
													std::uint64_t epoch) noexcept
{
	try {
		std::scoped_lock lock( session_agent_mutex_) ;
		// 未認証要求と二重要求は既存queueへ上書きしない
		if( session_id == 0U || epoch == 0U || session_agent_request_) {
			return false ;
		}

		session_agent_request_ = std::pair{session_id, epoch} ;
		// queueの実データはmessage引数へ渡さず、同じmanagerだけを起床する
		if( !PostThreadMessageW( manager_thread_id_, kMessageAgentSettingsReady, 0U, 0L)) {
			session_agent_request_.reset() ;
			return false ;
		}

		return true ;
	} catch( ...) {
		return false ;
	}
}

void ClientApplication::DrainSessionAgentSettings() noexcept
{
	std::optional<std::pair<std::uint64_t, std::uint64_t>> request ;
	{
		std::scoped_lock lock( session_agent_mutex_) ;
		request.swap( session_agent_request_) ;
	}

	// 旧内部入口も設定表示だけへ限定し、起動承認には使わない
	if( !request) {
		return ;
	}

	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief session入口要求の再利用しないIDへ対象を固定する。
	 * @param[in] record managerが所有するsession。別sessionの接続情報を採用しない。
	 * @return 要求IDに対応する生存recordならtrue。
	 * @endcond
	 */
	const auto found = std::find_if( sessions_.begin(), sessions_.end(), [&]( const auto &record) {
		return record.id == request->first ;
	}) ;
	// 閉じた対象を他の生存sessionへ付け替えない
	if( found != sessions_.end() && found->thread->deployment_epoch() == request->second) {
		ShowConnectionAgentSettings( found->connection) ;
	}
}

void ClientApplication::EnsureAgentSettingsService()
{
	// 接続managerが保存一覧と有限workerを一つだけ所有する
	if( agent_settings_service_ == nullptr) {
		agent_settings_service_ = std::make_unique<agent_settings::AgentSettingsService>(
			agent_settings::MakeWindowsAgentSettingsBackend(),
			/**
			 * @cond LOCAL_CALLBACK_CONTRACT
			 * @brief Agent設定workerの完了をmanager STAへ配送する。
			 * @note 通知先thread IDだけを保持し、workerから設定画面を直接操作しない。
			 * @endcond
			 */
			[thread = manager_thread_id_]() noexcept {
				static_cast<void>( PostThreadMessageW( thread, kMessageAgentSettingsReady, 0U, 0L)) ;
			}) ;
	}
}

void ClientApplication::ShowAgentSettings( std::wstring preferred_name) noexcept
{
	// 旧単体入口は未関連付け設定の保全だけに残し、製品managerは接続組入口を使う
	try {
		EnsureAgentSettingsService() ;
		// 同じ画面の未保存draftを保持する
		if( agent_settings_window_ == nullptr) {
			agent_settings_window_ =
				std::make_unique<AgentSettingsWindow>( instance_, *agent_settings_service_) ;
		}

		static_cast<void>( agent_settings_window_->Show( nullptr, std::move( preferred_name))) ;
	} catch( ...) {
		common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
								   common::DiagnosticEvent::UnexpectedException) ;
	}
}

void ClientApplication::ShowConnectionAgentSettings( SessionConnectionRequest request) noexcept
{
	try {
		const auto key = agent_settings::BuildAgentConnectionKey( request.server, request.domain,
																 request.user_name) ;
		// 未入力の接続先へ共通のAgent設定を作らない
		if( key.empty()) {
			MessageBoxW( manager_ == nullptr ? nullptr : manager_->window(),
						LoadUpdateString( IDS_SERVER_REQUIRED).c_str(), L"ZRdp SVN Bridge",
						MB_OK | MB_ICONWARNING) ;
			return ;
		}

		EnsureAgentSettingsService() ;
		// 生存画面の対象と未保存draftを黙って切り替えない
		if( agent_settings_window_ == nullptr) {
			agent_settings_window_ =
				std::make_unique<AgentSettingsWindow>( instance_, *agent_settings_service_) ;
		}

		auto name = request.connection_name.empty()
						? request.server
						: request.connection_name + L" (" + request.server + L")" ;
		name.resize( std::min( name.size(), agent_settings::kMaximumAgentProfileName)) ;
		static_cast<void>( agent_settings_window_->Show( nullptr, std::move( name), key)) ;
	} catch( ...) {
		common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
								   common::DiagnosticEvent::UnexpectedException) ;
	}
}

void ClientApplication::UpdateOpenAgentSettings( SessionConnectionRequest request) noexcept
{
	try {
		// 閉じた設定画面を接続入力の変更だけで自動表示しない
		if( agent_settings_window_ == nullptr || agent_settings_window_->window() == nullptr) {
			return ;
		}

		auto name = request.connection_name.empty()
						? request.server
						: request.connection_name + L" (" + request.server + L")" ;
		name.resize( std::min( name.size(), agent_settings::kMaximumAgentProfileName)) ;
		const auto key = agent_settings::BuildAgentConnectionKey( request.server, request.domain,
																 request.user_name) ;
		agent_settings_window_->ChangeConnection( std::move( name), key) ;
	} catch( ...) {
		common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
								   common::DiagnosticEvent::UnexpectedException) ;
	}
}

void ClientApplication::DrainAgentSettings() noexcept
{
	try {
		// service不在では保存済みを推定しない
		if( agent_settings_service_ == nullptr) {
			return ;
		}

		agent_settings::AgentSettingsNotice notice ;
		// 最大一件のworker結果を取得する
		if( !agent_settings_service_->TakeNotice( notice)) {
			return ;
		}

		const bool profiles = notice.operation == agent_settings::AgentSettingsOperation::Load ||
							  notice.operation == agent_settings::AgentSettingsOperation::Save ;
		// 完全Load／Save成功だけを全sessionの判断用snapshotへ採用する
		if( profiles && notice.result.succeeded()) {
			agent_profiles_		   = notice.profiles ;
			agent_profiles_loaded_ = true ;
			// 明示保存は失敗停止後の再試行も許可するが、部分入力の自動再送はしない
			if( notice.operation == agent_settings::AgentSettingsOperation::Save) {
				// 対応する設定が変わったsessionだけを後続の比較で更新する
				for( auto &record : sessions_) {
					const auto profile = agent_settings::FindRegisteredAgentSettingsProfile(
						agent_profiles_, record.connection_key) ;
					std::string ini ;
					// 拒否・削除を含む差分は旧設定で継続しない
					if( profile) {
						static_cast<void>( agent_settings::GenerateAgentSettingsIni( *profile, ini)) ;
					}

					// 内容差分だけを再起動対象とする
					if( ini != record.applied_ini) {
						record.blocked			= false ;
						record.failure_reported = false ;
						record.failures			= 0U ;
						record.next_attempt		= 0U ;
					}

					// 同じ設定でも明示保存は停止後の新しい起動要求として扱う
					if( record.blocked && !ini.empty() && agent_settings_window_ != nullptr &&
					   agent_settings::SameAgentConnectionKey(
						   record.connection_key, agent_settings_window_->connection_key()) &&
					   record.thread->QueueConfiguration( record.thread->deployment_epoch(), ini,
														 true)) {
						record.blocked			= false ;
						record.failure_reported = false ;
						record.failures			= 0U ;
						record.attempt_tick		= 0U ;
						record.next_attempt		= GetTickCount64() + 5000U ;
					}
				}
			}
		}

		// manager初期Loadを設定画面の別世代へ誤配送しない
		if( notice.request_id == launch_load_request_ && launch_load_request_ != 0U) {
			launch_load_request_ = 0U ;
			// 初期Load中に開いた設定画面の遅延Loadも再予約する
			if( agent_settings_window_ != nullptr) {
				agent_settings_window_->OnNotice( std::move( notice)) ;
			}

			return ;
		}

		// 配布結果を準備時の同じRDP世代へ限定する
		if( notice.operation == agent_settings::AgentSettingsOperation::PrepareDeployment) {
			const auto found =
				/**
				 * @cond LOCAL_CALLBACK_CONTRACT
				 * @brief 準備済み配布物を同じ対象sessionへだけ適用する。
				 * @param[in] record manager所有のsession候補。世代照合は後続処理で行う。
				 * @return 現在のdeployment対象IDと一致する場合はtrue。
				 * @endcond
				 */
				std::find_if( sessions_.begin(), sessions_.end(), [this]( const auto &record) {
					return record.id == deployment_target_ ;
				}) ;
			// stale結果から固定入力を開始しない
			if( found != sessions_.end() && found->thread->deployment_epoch() == deployment_epoch_ &&
			   notice.request_id == deployment_request_) {
				// package準備が完全成功した場合だけ所有STAへ予約する
				if( notice.result.succeeded() && notice.deployment_drive &&
				   found->thread->QueuePaste( deployment_epoch_, 0U, deployment_request_)) {
					found->attempt_tick = GetTickCount64() ;
				} else {
					found->blocked	   = true ;
					deployment_target_ = 0U ;
				}
			} else {
				deployment_target_ = 0U ;
			}

			// 配布worker中に開いた設定画面の遅延Loadを再予約する
			if( agent_settings_window_ != nullptr) {
				agent_settings_window_->OnNotice( std::move( notice)) ;
			}

			return ;
		}

		// modeless設定画面が所有する結果だけを照合して反映する
		if( agent_settings_window_ != nullptr) {
			agent_settings_window_->OnNotice( std::move( notice)) ;
		}
	} catch( ...) {
		common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
								   common::DiagnosticEvent::UnexpectedException) ;
	}
}

void ClientApplication::DrainAgentDeployment() noexcept
{
	// 全sessionの有限入力応答を回収し、失敗は同じ対象だけで停止する
	for( auto &record : sessions_) {
		SessionWindowThread::PasteResult result ;
		// 入力失敗後は無確認で繰り返さない
		if( record.thread->TakePasteResult( result) && record.id == deployment_target_ &&
		   result.ticket == deployment_request_ && FAILED( result.result)) {
			record.blocked = true ;
		}
	}

	// 固定clipboardはEnter完了または失敗まで全sessionで一つだけ使用する
	if( deployment_target_ != 0U && deployment_request_ != 0U) {
		const auto found =
			std::find_if( sessions_.begin(), sessions_.end(),
						 /**
						  * @cond LOCAL_CALLBACK_CONTRACT
						  * @brief 配布終了通知の対象sessionをIDで照合する。
						  * @param[in] record 生存sessionの候補。別sessionの状態は変更しない。
						  * @return deployment対象IDに一致する場合はtrue。
						  * @endcond
						  */
						 [this]( const auto &record) { return record.id == deployment_target_ ; }) ;
		// 切断と入力完了では他sessionが次の固定操作を予約できる
		if( found == sessions_.end() || found->thread->deployment_epoch() != deployment_epoch_ ||
		   found->blocked ||
		   (found->attempt_tick != 0U &&
			found->thread->automatic_status().stage ==
				agent_deployment::AutomaticDeploymentStage::AgentResponse)) {
			deployment_target_	= 0U ;
			deployment_request_ = 0U ;
		}
	}
}

void ClientApplication::AdvanceAgentLifecycle() noexcept
{
	try {
		// 復号失敗を空一覧として保存・起動しない
		if( !agent_profiles_loaded_ || agent_settings_service_ == nullptr) {
			return ;
		}

		const auto now = GetTickCount64() ;
		// 上限16sessionの所有snapshotだけを観測する
		for( auto &record : sessions_) {
			const auto epoch = record.thread->deployment_epoch() ;
			// 未認証や切断後は固定入力も設定配送も行わない
			if( epoch == 0U) {
				continue ;
			}

			const bool new_epoch = record.observed_epoch != epoch ;
			// 認証世代が変わった場合だけ失敗停止を解除する
			if( new_epoch) {
				record.observed_epoch = epoch ;
				record.applied_ini.clear() ;
				record.failures				 = 0U ;
				record.blocked				 = false ;
				record.attempt_tick			 = 0U ;
				record.healthy_since		 = 0U ;
				record.configuration_pending = true ;
				record.failure_reported		 = false ;
				record.recovery_armed		 = true ;
				record.next_attempt			 = now + 1000U ;
			}

			const auto profile = agent_settings::FindRegisteredAgentSettingsProfile(
				agent_profiles_, record.connection_key) ;
			std::string ini ;
			// 空・未承認設定をINIへ流さない
			if( profile) {
				static_cast<void>( agent_settings::GenerateAgentSettingsIni( *profile, ini)) ;
			}

			// 設定差分は同じ接続のowner STAだけへ渡す
			if( record.configuration_pending || ini != record.applied_ini) {
				// bounded queueがbusyの場合は次tickで差分を再確認する
				if( !record.thread->QueueConfiguration(
					   epoch, ini, record.configuration_pending && !ini.empty())) {
					continue ;
				}

				record.configuration_pending = false ;
				record.applied_ini			 = ini ;
				record.attempt_tick =
					new_epoch || ini.empty() || !record.bootstrap_profile ? 0U : now ;
				record.next_attempt = now + 5000U ;
				continue ;
			}

			// 未設定ではdialogを自動表示せず、手動設定まで入力と起動を待つ
			if( ini.empty()) {
				continue ;
			}

			const auto state = record.thread->lifecycle_status() ;
			// ACKで確認した利用可能状態だけを安定稼働として数える
			if( state == 1U) {
				record.attempt_tick	  = 0U ;
				record.recovery_armed = false ;
				// 一瞬の接続だけで連続失敗counterを解除しない
				if( record.healthy_since == 0U) {
					record.healthy_since = now ;
				}

				// 安定30秒後に連続失敗を解除する
				if( now >= record.healthy_since &&
				   now - record.healthy_since >= agent_deployment::kAgentStableMilliseconds) {
					record.failures = 0U ;
				}

				continue ;
			}

			// 安定期間前の疎通断も連続失敗として有限に数える
			if( record.healthy_since != 0U && now >= record.healthy_since &&
			   now - record.healthy_since < agent_deployment::kAgentStableMilliseconds &&
			   !record.recovery_armed) {
				++record.failures ;
				// 短い疎通を繰返すAgentを無限に起動しない
				if( record.failures >= agent_deployment::kMaximumAgentRecoveryFailures) {
					record.blocked = true ;
				}
			}

			record.healthy_since = 0U ;
			// 死活断では一回だけ固定更新を準備し、設定変更の応答待ちは中断しない
			if( !record.blocked && !record.recovery_armed && record.attempt_tick == 0U &&
			   state == 0U) {
				// bounded queue競合時は次tickで同じ条件を再評価する
				if( record.thread->QueueConfiguration( epoch, ini, true)) {
					record.recovery_armed = true ;
					record.next_attempt	  = now + 5000U ;
				}

				continue ;
			}

			// 拒否・protocol異常と部分入力は無確認で再送しない
			if( state >= 3U || record.thread->automatic_status().status < 0) {
				record.blocked = true ;
			}

			// 起動完了の待機は有限にし、三回の無応答で停止する
			if( record.attempt_tick != 0U) {
				// 起動受付後45秒以内の正常応答を待つ
				if( !agent_deployment::AgentLaunchExpired( record.attempt_tick, now)) {
					continue ;
				}

				record.attempt_tick = 0U ;
				++record.failures ;
				// 完全入力済みの無応答だけを次の有限試行へ戻す
				if( record.failures >= agent_deployment::kMaximumAgentRecoveryFailures) {
					record.blocked = true ;
				}

				// 同じepochの完了済み操作だけをresetしてbackoffする
				if( !record.blocked) {
					static_cast<void>( record.thread->QueueConfiguration( epoch, ini, true)) ;
					record.next_attempt =
						now + agent_deployment::AgentRecoveryDelay( record.failures) ;
				}
			}

			// 自動復旧を止めた場合は同じownerへ一回だけ固定失敗表示する
			if( record.blocked && !record.failure_reported) {
				record.failure_reported = record.thread->QueueLifecycleFailure( epoch) ;
			}

			// 暗黙の前面化や最小化解除をせず、元RDPの安全条件を待つ
			if( record.blocked || now < record.next_attempt || deployment_target_ != 0U ||
			   !record.thread->input_ready() || agent_settings_service_->Busy()) {
				continue ;
			}

			const auto drive = record.thread->deployment_drive() ;
			// 専用driveなしで通常driveや別転送経路へfallbackしない
			if( drive == nullptr) {
				record.blocked = true ;
				continue ;
			}

			// readonly packageは同一RDP中に変更せず、最新INIは限定SVCで渡す
			if( !record.bootstrap_profile) {
				record.bootstrap_profile = *profile ;
			}

			agent_settings::AgentSettingsRequest prepare ;
			prepare.operation		 = agent_settings::AgentSettingsOperation::PrepareDeployment ;
			prepare.profile			 = *record.bootstrap_profile ;
			prepare.deployment_drive = drive ;
			deployment_request_		 = agent_settings_service_->Request( std::move( prepare)) ;
			// 一件worker予約成功時だけglobal入力対象を固定する
			if( deployment_request_ != 0U) {
				deployment_target_ = record.id ;
				deployment_epoch_  = epoch ;
			}
		}
	} catch( ...) {
		common::WriteDiagnosticLog( common::DiagnosticLevel::Error,
								   common::DiagnosticEvent::UnexpectedException) ;
	}
}

} // namespace zrdp::client::app
