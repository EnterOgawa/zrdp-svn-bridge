//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_application.h
 * @brief 接続管理Windowと複数RDP session Windowのprocess内所有者を定義する。
 * @version 01.00.10
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details 接続管理Windowと各RDP sessionを別COM STAへ分離し、sessionごとの
 *          ActiveX、Bridge、Proxy所有権と有限port slotを分離する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Clientの同意付き更新と設定の分離を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/08  Agent設定画面と非同期保存を独立所有。
 * Ver.01.00.03   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/09  session画面のAgent配置・確認付き起動を追加。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.07   Shunji Ogawa  2026/10/10  接続別設定と自動Agent管理・診断削除を反映。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  managerメニューを撤廃し自動更新確認へ統一。
 * Ver.01.00.09   Shunji Ogawa  2026/10/10  自動設定dialogの状態を廃止。
 * Ver.01.00.10   Shunji Ogawa  2026/10/10  接続変更と登録済み宛先だけの表示・起動を統一。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "client/app/connection_manager_window.h"
#include "client/app/agent_settings_window.h"
#include "client/app/main_window.h"
#include "client/app/session_proxy_port_allocator.h"
#include "client/proxy/client_proxy_config.h"
#include "client/update/update_windows.h"

#include <Windows.h>

#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <vector>

namespace zrdp::client::app {

/**
 * @brief implementation file内で一session専用COM STAを所有する内部class。
 */
class SessionWindowThread ;

/**
 * @brief Client processのmanager UI、session STA、有限Proxy slotを所有する。
 */
class ClientApplication final : public IConnectionManagerObserver, public ISessionWindowObserver {
public:
	/**
	 * @brief module instanceを参照する未生成applicationを構築する。
	 * @param[in] instance Client resourceを所有するmodule instance。
	 * @param[in] update_startup 同じClientの再起動確認状態。通常起動では空。
	 */
	explicit ClientApplication(
		HINSTANCE instance, std::shared_ptr<update::ClientUpdateStartupState> update_startup = {}) ;

	/**
	 * @brief 全session Windowをmanagerより先に解放する。
	 */
	~ClientApplication() override ;

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	ClientApplication( const ClientApplication &)						 = delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	ClientApplication			   &operator=( const ClientApplication &) = delete ;

	/**
	 * @brief Proxy base設定を読み、接続管理Windowを生成する。
	 * @param[in] show_command WinMain由来の表示command。
	 * @return managerを利用可能にできた場合はtrue。
	 */
	[[nodiscard]] bool				Create( int show_command) ;

	/**
	 * @brief 接続管理Windowとsession完了通知のmessage loopを実行する。
	 * @return WM_QUIT codeまたはGetMessageW失敗時のWin32 error。
	 */
	[[nodiscard]] int				RunMessageLoop() ;

	/**
	 * @brief manager要求から独立RDP／Bridge session Windowを生成する。
	 * @param[in] request passwordを含まない接続profile。
	 * @return session生成または固定拒否理由。
	 */
	[[nodiscard]] SessionOpenResult OpenSession( SessionConnectionRequest request) noexcept override ;
	/**
	 * @brief Agent設定画面を一つだけ生成または再表示する。
	 * @param[in] preferred_name 開く設定名。RDP接続先やcredentialは受け取らない。
	 */
	void ShowAgentSettings( std::wstring preferred_name) noexcept override ;
	/** @brief 接続managerの固定接続組を設定画面へ渡す。 @param[in] request 非機密の接続組。 */
	void ShowConnectionAgentSettings( SessionConnectionRequest request) noexcept override ;
	/** @brief 接続入力変更で開いている設定画面だけを再読込みする。
	 * @param[in] request 現在の接続入力。空または不正接続は編集不可にする。
	 */
	void UpdateOpenAgentSettings( SessionConnectionRequest request) noexcept override ;
	/** @brief sessionからの対象固定要求を一件だけ非同期受付する。
	 * @param[in] session_id 対象の再利用しないID。
	 * @param[in] epoch 対象の認証世代。
	 * @return 一件queueに予約できた場合だけtrue。
	 */
	[[nodiscard]] bool RequestSessionAgentSettings( std::uint64_t session_id,
												   std::uint64_t epoch) noexcept override ;

	/**
	 * @brief 認証済み接続profileを生存中managerの共通履歴へ渡す。
	 * @param[in] connection_name 任意の接続名。
	 * @param[in] server RDP接続先。
	 * @param[in] domain 任意domain。
	 * @param[in] user_name passwordを含まないユーザー名。
	 * @param[in] audio 接続時に適用した音声リダイレクト設定。
	 */
	void RememberSuccessfulConnection( std::wstring connection_name, std::wstring server,
									  std::wstring domain, std::wstring user_name,
									  rdp::RdpAudioRedirectionSettings audio) noexcept override ;

private:
	/**
	 * @brief session STAからmanager STAへ渡す認証済み接続profile。
	 */
	struct PendingSuccessfulConnection {
		/// 利用者が指定した任意の履歴表示名。
		std::wstring					 connection_name ;

		/// passwordを含まないRDP接続先。
		std::wstring					 server ;

		/// 任意のRDP logon domain。
		std::wstring					 domain ;

		/// passwordを含まないRDP user name。
		std::wstring					 user_name ;

		/// 接続前に確定した音声redirect profile。
		rdp::RdpAudioRedirectionSettings audio{} ;
	} ;

	/**
	 * @brief 一つのsession専用STAと解放対象Proxy slotを所有する。
	 */
	struct SessionRecord {
		/// 同一process内で再利用しない配置対象ID。
		std::uint64_t										id{0U} ;
		/// MainWindow、ActiveX、COM STAを所有するsession thread。
		std::unique_ptr<SessionWindowThread>				thread ;

		/// session終了時にallocatorへ返す有限Proxy slot。
		std::size_t											proxy_slot{0U} ;
		/// 設定を特定する開始時のRDP接続組。
		SessionConnectionRequest							connection ;
		/// 永続設定との完全一致key。
		std::wstring										connection_key ;
		/// 新しい認証世代へ自動操作を持ち越さないための観測値。
		std::uint64_t										observed_epoch{0U} ;
		/// owner STAへ渡した完全INI。
		std::string											applied_ini ;
		/// readonly配布driveの初回snapshot。以降はINIをSVCで更新する。
		std::optional<agent_settings::AgentSettingsProfile> bootstrap_profile ;
		/// 起動完了待機の開始tick。0は要求なし。
		std::uint64_t										attempt_tick{0U} ;
		/// 有限backoffの次回試行tick。
		std::uint64_t										next_attempt{0U} ;
		/// 安定稼働30秒を確認する開始tick。
		std::uint64_t										healthy_since{0U} ;
		/// 連続起動失敗数。三回で停止する。
		unsigned											failures{0U} ;
		/// 部分入力・拒否・protocol異常後は自動再試行しない。
		bool												blocked{false} ;
		/// 死活断後の固定更新要求を同じ試行中に重複予約しない。
		bool												recovery_armed{true} ;
		/// 固定失敗表示をowner STAへ一回だけ配送した状態。
		bool												failure_reported{false} ;
		/// 初回設定queueの競合でも世代と失敗counterを保持する。
		bool												configuration_pending{true} ;
	} ;

	/**
	 * @brief 終了済みsession STAをjoinしてvectorから除きport slotを解放する。
	 */
	void					   CleanupDestroyedWindows() noexcept ;

	/**
	 * @brief session STAがqueueした接続履歴をmanager STAへ反映する。
	 */
	void					   DrainSuccessfulConnections() noexcept ;

	/**
	 * @brief 生存session数と次のportをmanagerへ反映する。
	 */
	void					   UpdateManagerStatus() noexcept ;
	/** @brief 更新workerの所有結果をmanager UIへ反映し、同意を取得する。 */
	void					   DrainClientUpdates() noexcept ;
	/** @brief Agent設定workerの最大一件の結果を生存画面へ渡す。 */
	void					   DrainAgentSettings() noexcept ;
	/** @brief 設定workerを最初の編集または起動時だけ生成する。 */
	void					   EnsureAgentSettingsService() ;
	/** @brief 所有sessionのキー要求結果と対象世代を照合する。 */
	void					   DrainAgentDeployment() noexcept ;
	/** @brief 接続別設定、認証世代、有限復旧をmanagerだけで進める。 */
	void					   AdvanceAgentLifecycle() noexcept ;
	/** @brief session入口の要求を同じ生存対象だけに反映する。 */
	void					   DrainSessionAgentSettings() noexcept ;
	/** @brief 全session終了後だけ同意済み更新の適用を予約する。 */
	void					   AdvanceClientUpdate() noexcept ;
	/**
	 * @brief error、URL、内容を分離し、固定更新errorだけを利用者へ示す。
	 * @param[in] error 機密情報を含まない更新分類。
	 */
	void					   ShowClientUpdateError( update::UpdateError error) const noexcept ;
	/**
	 * @brief 更新UIだけに使用する固定resourceを読み込む。
	 * @param[in] identifier 日本語resource ID。
	 * @return 有限の所有文字列。欠落時は空。
	 */
	[[nodiscard]] std::wstring LoadUpdateString( UINT identifier) const ;

	/**
	 * @brief manager Windowでdialog互換処理を試す。
	 * @param[in] message manager message loopが取得した一件。
	 * @return manager Window階層で処理した場合はtrue。
	 */
	[[nodiscard]] bool		   ProcessDialogMessage( MSG &message) const noexcept ;

	// -------------------------------------------------------------------------
	// manager STAが所有するprocess状態
	// -------------------------------------------------------------------------

	/// Client resourceを所有するmodule instanceへの非所有参照。
	HINSTANCE				   instance_ ;

	/// session threadからの完了通知を受けるmanager thread ID。
	DWORD					   manager_thread_id_{0U} ;
	/// UI生成完了をMainから通知し、workerで回収する再起動確認状態。
	std::shared_ptr<update::ClientUpdateStartupState>	   update_startup_ ;
	/// 独立更新worker。既存RDP／Bridgeのworkerと資源を共有しない。
	std::unique_ptr<update::ClientUpdateService>		   update_service_ ;
	/// manager threadの24時間周期timer。0は未生成。
	UINT_PTR											   update_timer_{0U} ;
	/// 自動Agent操作をmanagerで観測する一秒周期timer。
	UINT_PTR											   agent_timer_{0U} ;
	/// Load／Save成功時だけ採用する完全保存一覧。
	agent_settings::AgentSettingsProfiles				   agent_profiles_ ;
	/// 復号失敗を空設定として扱わない読込み成功flag。
	bool												   agent_profiles_loaded_{false} ;
	/// helper受付後、sessionを強制終了せず通常message loopを終える状態。
	bool												   exit_for_update_{false} ;

	/// 全sessionへ複製する検証済みloopback Proxy base設定。
	proxy::ClientProxyConfiguration						   proxy_configuration_{} ;

	/// 設定異常をmanager UIへ表示するfail-closed読込み結果。
	proxy::ClientProxyConfigurationResult				   proxy_configuration_result_{} ;

	/// manager STAだけが操作する有限Proxy port allocator。
	std::unique_ptr<SessionProxyPortAllocator>			   proxy_port_allocator_ ;

	/// processに一つだけ存在する接続入力Window。
	std::unique_ptr<ConnectionManagerWindow>			   manager_ ;
	/// 初回の設定操作時だけ生成する独立した一件queue worker。
	std::unique_ptr<agent_settings::AgentSettingsService>  agent_settings_service_ ;
	/// manager終了で未保存draftを失わない独立top-level設定画面。
	std::unique_ptr<AgentSettingsWindow>				   agent_settings_window_ ;
	/// 0は配置対象なし。生存recordの単調増加ID。
	std::uint64_t										   deployment_target_{0U} ;
	/// 配置準備開始時の認証済みRDP世代。
	std::uint64_t										   deployment_epoch_{0U} ;
	/// 設定worker受付ID。結果とowner STA応答の照合に共用する。
	std::uint64_t										   deployment_request_{0U} ;
	/// port slotと異なりsession終了後も再利用しないID系列。
	std::uint64_t										   session_sequence_{0U} ;
	/// session起動の保存設定Loadを待つ一件のworker受付ID。
	std::uint64_t										   launch_load_request_{0U} ;
	/// sessionからの起動入口queueだけを保護するmutex。
	std::mutex											   session_agent_mutex_ ;
	/// 最大一件のsession入口通知。IDと世代だけを保持する。
	std::optional<std::pair<std::uint64_t, std::uint64_t>> session_agent_request_ ;

	/// 生存中または終了通知待ちのsession STA群。
	std::vector<SessionRecord>							   sessions_ ;

	/// session STAから渡される履歴queueだけを保護するmutex。
	std::mutex											   successful_connections_mutex_ ;

	/// manager STAが次回messageでdrainする認証済みprofile群。
	std::vector<PendingSuccessfulConnection>			   pending_successful_connections_ ;
} ;

} // namespace zrdp::client::app
