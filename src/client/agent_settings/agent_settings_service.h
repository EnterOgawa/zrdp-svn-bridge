//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_settings_service.h
 * @brief Agent設定のfile I/OとDPAPIをUIから分離する有限workerを定義する。
 * @version 01.00.04
 * @date 2026/10/08
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  一件queueとplatform保存境界を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "client/agent_settings/agent_settings_model.h"
#include "client/agent_deployment/agent_deployment.h"
#include "common/agent_setup.h"

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace zrdp::client::agent_settings {

/** @brief workerが直列実行する設定操作。 */
enum class AgentSettingsOperation {
	/// 暗号化profile一覧を読む。
	Load,
	/// 暗号化profile一覧全体を保存する。
	Save,
	/// 利用者が選択したINIを未承認draftとして読む。
	Import,
	/// 明示承認済み一件をAgent互換INIへ出力する。
	Export,
	/// 承認済みsnapshotと同梱Agentを配置用memoryへ準備する。
	PrepareDeployment,
} ;

/** @brief UIからworkerへ所有移転する一件の有限要求。 */
struct AgentSettingsRequest {
	/// 固定操作種別。
	AgentSettingsOperation								  operation{AgentSettingsOperation::Load} ;
	/// Saveだけで使用する一括保存一覧。
	AgentSettingsProfiles								  profiles ;
	/// Importの設定名、Export／配置の完全profile。
	AgentSettingsProfile								  profile ;
	/// 利用者が標準dialogで指定したlocal INI path。
	std::filesystem::path								  path ;
	/// Exportで標準dialogの上書き確認を経た場合だけtrue。
	bool												  overwrite_confirmed{false} ;
	/// 自動配置の場合だけ所有する、接続前に登録済みの空配布drive。
	std::shared_ptr<common::agent_setup::DeploymentDrive> deployment_drive ;
} ;

/** @brief workerからUIへ渡す最大一件の所有結果。 */
struct AgentSettingsNotice {
	/// UI世代の照合用。0は未受付。
	std::uint64_t										  request_id{0U} ;
	/// 完了した操作種別。
	AgentSettingsOperation								  operation{AgentSettingsOperation::Load} ;
	/// 内容を含まない固定結果。
	AgentSettingsResult									  result ;
	/// Load／Save成功時に公開する完全一覧。
	AgentSettingsProfiles								  profiles ;
	/// Import成功時の未承認draft。
	AgentSettingsProfile								  profile ;
	/// PrepareDeployment成功時だけ存在する変更不能二ファイル。
	std::shared_ptr<const agent_deployment::Package>	  package ;
	/// 同じ要求の公開済みdrive。別sessionへ付け替えない。
	std::shared_ptr<common::agent_setup::DeploymentDrive> deployment_drive ;
} ;

/** @brief workerのみが使う保存境界。testはfilesystemを使わないfakeに差替えられる。 */
class IAgentSettingsBackend {
public:
	/** @brief platform所有者をvirtual境界で解放する。 */
	virtual ~IAgentSettingsBackend()												= default ;
	/**
	 * @brief current-user暗号化一覧を一括読込みする。
	 * @param[out] profiles 成功時の一覧。不在時は空、失敗時は空。
	 * @param[in] stop worker終了要求。
	 * @return 全体を採用できる場合だけ成功。
	 */
	[[nodiscard]] virtual AgentSettingsResult Load( AgentSettingsProfiles &profiles,
												   std::stop_token		  stop) noexcept	= 0 ;
	/**
	 * @brief 検証済み一覧をcurrent-user暗号化で保存する。
	 * @param[in] profiles 完全な保存一覧。
	 * @param[in] stop worker終了要求。
	 * @return 原子的に保存できた場合だけ成功。
	 */
	[[nodiscard]] virtual AgentSettingsResult Save( const AgentSettingsProfiles &profiles,
												   std::stop_token				stop) noexcept	= 0 ;
	/**
	 * @brief 明示指定されたINIだけを読む。
	 * @param[in] path 標準dialog由来のlocal file。
	 * @param[in] name 有効な設定名。
	 * @param[out] profile 完全読込み後の未承認draft。
	 * @param[in] stop worker終了要求。
	 * @return 全体を採用できた場合だけ成功。
	 */
	[[nodiscard]] virtual AgentSettingsResult Import( const std::filesystem::path &path,
													 std::wstring_view			  name,
													 AgentSettingsProfile		 &profile,
													 std::stop_token stop) noexcept = 0 ;
	/**
	 * @brief 明示承認済みINIを固定名のlocal fileへ出力する。
	 * @param[in] path 利用者が選択したagent.ini。
	 * @param[in] profile 全entry承認済みの完全profile。
	 * @param[in] overwrite_confirmed 標準dialogで既存fileの上書きを承認した場合だけtrue。
	 * @param[in] stop worker終了要求。
	 * @return 原子的に全体を書けた場合だけ成功。
	 */
	[[nodiscard]] virtual AgentSettingsResult Export( const std::filesystem::path &path,
													 const AgentSettingsProfile	 &profile,
													 bool			 overwrite_confirmed,
													 std::stop_token stop) noexcept = 0 ;
	/**
	 * @brief 配置snapshotを準備する。既定fakeは実filesystemへ触れず拒否する。
	 * @param[in] profile 全entry承認済みのsnapshot。
	 * @param[out] package 成功時の二ファイル。
	 * @param[in] stop worker終了要求。
	 * @return platform実装不在ではInvalidData。
	 */
	[[nodiscard]] virtual AgentSettingsResult
	PrepareDeployment( const AgentSettingsProfile					   &profile,
					  std::shared_ptr<const agent_deployment::Package> &package,
					  std::stop_token									stop) noexcept
	{
		static_cast<void>( profile) ;
		static_cast<void>( stop) ;
		package.reset() ;
		return {AgentSettingsError::InvalidData} ;
	}
} ;

/** @brief 一worker、要求一件、結果一件だけを所有する設定service。 */
class AgentSettingsService final {
public:
	/**
	 * @brief platform境界と非blockingなUI通知を受けてworkerを開始する。
	 * @param[in] backend 単独所有する保存実装。
	 * @param[in] notify 所有threadを起床するだけのcallback。
	 */
	AgentSettingsService( std::unique_ptr<IAgentSettingsBackend> backend,
						 std::function<void()>					notify) ;
	/** @brief 全UI終了後に終了要求を送りworkerをjoinする。 */
	~AgentSettingsService() ;
	/** @brief threadと要求の複製を禁止する。 */
	AgentSettingsService( const AgentSettingsService &)					= delete ;
	/**
	 * @brief threadと要求の複製を禁止する。
	 * @return 呼出し禁止のため値を生成しない。
	 */
	AgentSettingsService	   &operator=( const AgentSettingsService &) = delete ;
	/**
	 * @brief 有限要求を一件だけ受付ける。既存要求・結果を上書きしない。
	 * @param[in] request 所有要求。Move後はcallerで使用しない。
	 * @return 受付ID。終了中／busy／不正状態では0。
	 */
	[[nodiscard]] std::uint64_t Request( AgentSettingsRequest request) noexcept ;
	/**
	 * @brief UIが所有結果を最大一件だけ取得する。
	 * @param[out] notice 完了した結果。
	 * @return 結果を移転できた場合はtrue。
	 */
	[[nodiscard]] bool			TakeNotice( AgentSettingsNotice &notice) noexcept ;
	/**
	 * @brief 実行中要求または未取得結果があるか判定する。
	 * @return 操作を競合開始できない場合はtrue。
	 */
	[[nodiscard]] bool			Busy() const noexcept ;
	/** @brief 全Window終了後に未実行要求を取消し、workerを終了する。 */
	void						Stop() noexcept ;

private:
	/**
	 * @brief mutexをfile I/O中に保持せず、一件ずつ処理する。
	 * @param[in] stop jthread終了要求。
	 */
	void								   Run( std::stop_token stop) noexcept ;
	/// queue、受付ID、busy、結果の所有だけを保護する。
	mutable std::mutex					   mutex_ ;
	/// workerだけが待つ条件変数。
	std::condition_variable_any			   condition_ ;
	/// workerだけが呼ぶ単独所有platform境界。
	std::unique_ptr<IAgentSettingsBackend> backend_ ;
	/// I/OとUI再入を行わない通知callback。
	std::function<void()>				   notify_ ;
	/// 最大一件の未実行要求。
	std::optional<AgentSettingsRequest>	   request_ ;
	/// 最大一件の未取得結果。
	std::optional<AgentSettingsNotice>	   notice_ ;
	/// monotonically increasing受付ID。overflow時は新規受付を拒否する。
	std::uint64_t						   sequence_{0U} ;
	/// platform実行中も競合要求を受付けない状態。
	bool								   busy_{false} ;
	/// Stop後の新規受付を禁止する状態。
	bool								   stopping_{false} ;
	/// 他member構築後に開始し、backend解放前に終了する所有worker。
	std::jthread						   worker_ ;
} ;

/**
 * @brief 実ユーザーの固定保存先をworker上で導出するWindows実装を作る。
 * @return I/Oをまだ行っていない単独所有backend。
 */
[[nodiscard]] std::unique_ptr<IAgentSettingsBackend> MakeWindowsAgentSettingsBackend() ;
/**
 * @brief 隔離testで指定した保存fileだけを使うWindows実装を作る。
 * @param[in] storage_path testが作成したdirectory内のagent-settings.bin。
 * @return productionと同じDPAPI／file処理を持つbackend。
 * @note runtime UIやcommand lineから保存先のoverrideを受け付けない。
 */
[[nodiscard]] std::unique_ptr<IAgentSettingsBackend>
MakeFileAgentSettingsBackend( std::filesystem::path storage_path) ;

} // namespace zrdp::client::agent_settings
