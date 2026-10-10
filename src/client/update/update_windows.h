//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file update_windows.h
 * @brief 同じClient EXEの内部helperとWindows更新backendを定義する。
 * @version 01.00.02
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  同意後だけ使うClient差替えbackendを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  固定二EXEのセット置換境界とAgentの部分復元を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "client/update/update_service.h"

#include <filesystem>
#include <memory>

namespace zrdp::client::update {

/** @brief 再起動時だけ継承HANDLEと検証済みstageを所有する内部状態。 */
class ClientUpdateStartupState ;

/** @brief EXE内部maintenance要求または通常UI起動の固定結果。 */
struct ClientUpdateEntry {
	/// helper要求／不正な内部要求を処理した場合だけtrue。
	bool									  handled{false} ;
	/// helperの固定更新errorを表す終了値。
	int										  exit_code{0} ;
	/// 親終了後のhelper失敗を固定dialogで通知する場合だけtrue。
	bool									  show_failure{false} ;
	/// rollback後に旧版を再起動した場合だけtrue。初回の自動確認を抑止する。
	bool									  recovered{false} ;
	/// 新版の通常UI起動へ移転する内部確認状態。通常起動ではnull。
	std::shared_ptr<ClientUpdateStartupState> startup ;
} ;

/**
 * @brief 内部command lineを限定構文で処理する。
 * @return maintenance処理済み結果または通常UIへ渡す再起動確認状態。
 * @note 任意path、URL、実行command、RDP入力を受け付けない。
 */
[[nodiscard]] ClientUpdateEntry HandleClientUpdateEntry() noexcept ;

/**
 * @brief 接続管理Windowを生成できた新版から初期化完了を知らせる。
 * @param[in] startup HandleClientUpdateEntryからの共有状態。通常起動ではnull。
 * @note UI threadで呼べる非blocking event通知だけを行う。
 */
void							SignalClientUpdateRestartReady(
							   const std::shared_ptr<ClientUpdateStartupState> &startup) noexcept ;

/**
 * @brief 同一ReleaseのClient/AgentセットのHTTPS取得・検証・差替えを担うWindows backendを作る。
 * @param[in] startup 再起動直後の検証済みstage回収情報。
 * @return workerが単独所有するbackend。確保失敗は例外でcallerへ返す。
 */
[[nodiscard]] std::unique_ptr<IClientUpdateBackend>
MakeWindowsUpdateBackend( std::shared_ptr<ClientUpdateStartupState> startup = {}) ;

/**
 * @brief 同じvolume内の固定ClientまたはAgentを対応stage候補からbackup付きで置換する。
 * @param[in] target 固定名ZRdpSvnBridge.exeまたはZRdpSvnAgent.exe。
 * @param[in] candidate 隣接の専用stageにあるcandidate.exeまたはagent-candidate.exe。
 * @param[in] backup 対応するprevious.exeまたはagent-previous.exe。事前に存在しないこと。
 * @return 成功はNone。失敗時の部分置換はtransaction側で検証・復元する。
 * @pre callerが旧版・候補の署名・hashを検証済みで、親Clientは終了している。
 */
[[nodiscard]] UpdateError ReplaceClientUpdateFile( const std::filesystem::path &target,
												  const std::filesystem::path &candidate,
												  const std::filesystem::path &backup) noexcept ;

/**
 * @brief 検証済みの対応旧版backupだけを固定ClientまたはAgent配置へ戻す。
 * @param[in] target 固定名ZRdpSvnBridge.exeまたはZRdpSvnAgent.exeの配置。
 * @param[in] backup 対応する隣接stageのprevious.exeまたはagent-previous.exe。
 * @return 置換できた場合だけNone。
 * @pre callerがbackupの署名・旧版hashを検証し、新版が生存していないこと。
 */
[[nodiscard]] UpdateError RestoreClientUpdateFile( const std::filesystem::path &target,
												  const std::filesystem::path &backup) noexcept ;

/**
 * @brief 固定同一配置の二EXEをAgent、Clientの順でbackup付き置換する。
 * @param[in] target セットの固定Client絶対path。
 * @param[in] stage 二候補と旧digestを検証済みの専用隣接stage。
 * @return 両置換成功だけNone。部分失敗はcallerの元hash検証と復元で扱う。
 * @pre callerが両候補・元二EXEを完全検証し、親Clientが終了している。
 * @note 二fileのOS-level atomic性は保証しない。強制終了や任意pathは扱わない。
 */
[[nodiscard]] UpdateError ReplaceClientUpdatePair( const std::filesystem::path &target,
												  const std::filesystem::path &stage) noexcept ;

} // namespace zrdp::client::update
