//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file agent_setup.h
 * @brief 専用配布driveと固定二ファイルの初期配置境界を定義する。
 * @version 01.00.05
 * @date 2026/10/09
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/09  初回配置と起動の限定境界を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  トレイ終了後の同じRDP内Agent再起動を修正。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  協調停止とINI-only原子更新を追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once
#include <Windows.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <stop_token>

namespace zrdp::common::agent_setup {

/** @brief sessionより長く公開しない一時driveの所有者。 */
class DeploymentDrive final {
public:
	/** @brief 不完全な所有状態を外部へ公開しない。 */
	DeploymentDrive() ;
	/** @brief 自分のmappingと固定二fileだけを回収する。 */
	~DeploymentDrive() ;
	/** @brief drive所有権の複製を禁止する。 */
	DeploymentDrive( const DeploymentDrive &)					  = delete ;
	/** @brief drive所有権の複製を禁止する。 @return 呼出し禁止。 */
	DeploymentDrive			  &operator=( const DeploymentDrive &) = delete ;
	/** @brief 個別drive選択用の固定letterを返す。 @return 未生成なら0。 */
	[[nodiscard]] wchar_t	   letter() const noexcept ;
	/** @brief 固定source commandを構築する。 @return 固定EXEと固定argumentだけの文字列。 */
	[[nodiscard]] std::wstring launch_command() const ;
	/** @brief worker上で二fileをcreate-newし、readonlyへ封じる。
	 * @param[in] agent 検証済みEXEの有限byte列。
	 * @param[in] ini 承認済みINI。
	 * @param[in] stop 終了要求。
	 * @return 完全に封じた場合だけtrue。失敗では公開fileを回収する。
	 */
	[[nodiscard]] bool		   Publish( std::span<const std::byte> agent, std::string_view ini,
									   std::stop_token stop = {}) noexcept ;
	/** @brief workerで初回公開または同じ固定二fileの再起動準備を行う。
	 * @param[in] agent 今回も検証したEXEの有限byte列。
	 * @param[in] ini 今回の承認済みINI。
	 * @param[in] stop 終了要求。
	 * @return 初回公開成功または既存二fileが完全一致した場合だけtrue。
	 * @note 公開済みfileを差替えず、異なるpackageは新しいRDP接続を必要とする。
	 */
	[[nodiscard]] bool		   PrepareLaunch( std::span<const std::byte> agent, std::string_view ini,
											 std::stop_token stop = {}) noexcept ;
	/** @brief 固定二fileの公開履歴を返す。 @return 取消後も公開済みならtrue。 */
	[[nodiscard]] bool		   published() const noexcept ;
	/** @brief 今回の起動入力への許可状態を返す。 @return 検証済み二fileが未取消ならtrue。 */
	[[nodiscard]] bool		   sealed() const noexcept ;
	/** @brief 未送信の起動操作を失効させる。file I/Oは行わない。 */
	void					   Cancel() noexcept ;
	/** @brief testで公開範囲を確認する。 @return 生成した専用directory。 */
	[[nodiscard]] const std::filesystem::path &directory() const noexcept ;
	/** @brief Windowsユーザーの専用baseを返す。 @return 導出失敗時は空。 */
	[[nodiscard]] static std::filesystem::path DefaultBase() noexcept ;
	/** @brief 未公開directoryと未使用driveだけを生成する。
	 * @param[in] base 信頼したlocal directory。testはworkspace以下を指定する。
	 * @return 全成功時だけ所有者。既存driveへfallbackしない。
	 */
	[[nodiscard]] static std::shared_ptr<DeploymentDrive>
	Create( const std::filesystem::path &base) noexcept ;

private:
	/** @brief Windows所有状態をheaderから隔離する。 */
	struct Impl ;
	/// mapping、directory lock、readonly fileの単独所有。
	std::unique_ptr<Impl> impl_ ;
} ;

/** @brief 初回配置の固定結果。 */
enum class InstallResult {
	/// 固定二fileの配置を確認した。
	Installed,
	/// 既存の完全に同一な二fileを再利用した。
	AlreadyInstalled,
	/// 入力・link・製品metadata・allowlistが不正だった。
	InvalidPackage,
	/// link、過大、読込不能等の不安全な既存fileを保全して停止した。
	ExistingFiles,
	/// file I/Oが失敗した。
	FileFailure,
	/// 旧二fileをbackupへ保全して更新した。
	Updated,
	/// 部分更新の自動復旧に失敗した。backupは保全する。
	RollbackFailure,
} ;
/** @brief testで製品metadata検証を隔離する境界。productionのCLIへは公開しない。 */
using PackageVerifier = std::function<bool( const std::filesystem::path &)> ;
/** @brief 固定二fileを初回配置、同一再利用またはbackup付き更新する。
 * @param[in] source 検証前の二fileがあるsource directory。
 * @param[in] destination 固定配置directory。runtimeではLocalAppData以下だけ。
 * @param[in] verifier 単体testだけの製品検証境界。空なら正式metadataとsetup対応を検証する。
 * @return 二file検証後だけInstalled／AlreadyInstalled／Updated。
 * @note processは起動しない。更新前backupを保全し、部分commitは旧二fileへ復旧する。
 */
[[nodiscard]] InstallResult Install( const std::filesystem::path &source,
									const std::filesystem::path &destination,
									PackageVerifier				 verifier = {}) noexcept ;
/** @brief 実Agentの限定CLIを処理する。
 * @return 0は配置と固定Agent起動受付、他は固定error。
 * @note active RDP、tsclient単一drive、固定EXEを必須にし、署名ではなく製品metadataとallowlistを検証する。
 */
[[nodiscard]] int			RunRdpSetup() noexcept ;
/** @brief 同じRDPユーザーsessionの協調停止eventを新規作成する。
 * @param[in] session_id 現processのsession ID。
 * @return callerがCloseHandleするevent。既存objectまたは失敗はnullptr。
 */
[[nodiscard]] HANDLE		CreateAgentStopEvent( DWORD session_id) noexcept ;
/** @brief 検証済みINIだけをbackup付きatomic renameで更新する。
 * @param[in] destination runtimeでは固定LocalAppData/ZRdp。
 * @param[in] ini 完全一致allowlistの有限INI。
 * @return 同一ならAlreadyInstalled、変更成功ならUpdated。
 */
[[nodiscard]] InstallResult InstallConfiguration( const std::filesystem::path &destination,
												 std::string_view			  ini) noexcept ;
/** @brief 固定配置先の自身だけをconsoleなしで再起動する。
 * @return 固定EXEのprocess生成受付ならtrue。
 * @note callerはchannel、tray、singleton、stop eventを解放済みとする。
 */
[[nodiscard]] bool			RestartInstalledAgent() noexcept ;
} // namespace zrdp::common::agent_setup
