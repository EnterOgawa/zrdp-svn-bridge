//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_deployment.h
 * @brief 承認済み設定と固定Agentの二ファイル配置境界を定義する。
 * @version 01.00.04
 * @date 2026/10/08
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  有限の配置packageとOLE境界を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  接続別設定と自動Agent管理・診断削除を反映。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once
#include "client/agent_settings/agent_settings_model.h"
#include <Windows.h>
#include <objidl.h>
#include <filesystem>
#include <memory>
#include <stop_token>

namespace zrdp::client::agent_deployment {

// Agent全体の保持を有限にするbyte上限
inline constexpr std::size_t		  kMaximumAgentBytes = 16U * 1024U * 1024U ;

// Agent起動応答と安定稼働の有限確認期間。
inline constexpr std::uint64_t		  kAgentLaunchResponseMilliseconds = 45000U ;
// 一瞬の疎通だけで連続失敗数を解除しない期間。
inline constexpr std::uint64_t		  kAgentStableMilliseconds		   = 30000U ;
// 認証世代ごとの連続復旧失敗上限。
inline constexpr unsigned			  kMaximumAgentRecoveryFailures	   = 3U ;
/** @brief 連続失敗数からoverflowしない有限backoffを返す。
 * @param[in] failures 現認証世代の失敗数。
 * @return 5秒から60秒の待機millisecond。
 */
[[nodiscard]] constexpr std::uint64_t AgentRecoveryDelay( unsigned failures) noexcept
{
	return failures >= 4U ? 60000U : (5000U << failures) > 60000U ? 60000U : (5000U << failures) ;
}

/** @brief 未開始とclock逆転を区別し、起動待機期限を判定する。
 * @param[in] started 起動要求tick。0は未開始。
 * @param[in] now 現在tick。
 * @return 開始済みかつ期限到達またはclock逆転ならtrue。
 */
[[nodiscard]] constexpr bool AgentLaunchExpired( std::uint64_t started, std::uint64_t now) noexcept
{
	return started != 0U && (now < started || now - started >= kAgentLaunchResponseMilliseconds) ;
}

/** @brief clipboard公開前のRDP対象条件を固定する。
 * @param[in] sessions Client内の生存session数。
 * @param[in] epoch 認証完了世代。0は未認証。
 * @return 一件の認証完了sessionだけtrue。
 */
[[nodiscard]] constexpr bool CanPrepareDeployment( std::size_t	sessions,
												  std::uint64_t epoch) noexcept
{
	return sessions == 1U && epoch != 0U ;
}

/** @brief 送信直前の世代を照合し、古い対象・別clipboardを拒否する。
 * @param[in] expected_epoch 準備開始時の認証世代。
 * @param[in] actual_epoch 現在の認証世代。0は切断・認証中。
 * @param[in] expected_clipboard キー要求時のclipboard世代。
 * @param[in] actual_clipboard 現在のclipboard世代。
 * @return 全ての非0世代が一致する場合だけtrue。
 */
[[nodiscard]] constexpr bool CanPasteDeployment( std::uint64_t expected_epoch,
												std::uint64_t actual_epoch,
												DWORD		  expected_clipboard,
												DWORD		  actual_clipboard) noexcept
{
	return expected_epoch != 0U && expected_epoch == actual_epoch && expected_clipboard != 0U &&
		   expected_clipboard == actual_clipboard ;
}

/** @brief 変更不能の二ファイル。最後のOLE参照解放まで生存する。 */
struct Package final {
	/// 検証済みAgentの全byte。任意pathをclipboardへ公開しない。
	std::vector<std::byte> agent ;
	/// 共有parserで検証済みの承認済みINI。
	std::string			   ini ;
	/// 正式metadataで初期配置modeを確認した場合だけtrue。
	bool				   agent_setup{false} ;
	/** @brief 業務設定の保持が終わった時点でINIをゼロ化する。 */
	~Package() ;
} ;

/**
 * @brief 固定ClientとAgentの正式metadata・同一版・setup対応を確認する。
 * @param[in] client 検証するClientのlocal file。
 * @param[in] agent 検証するAgentのlocal file。
 * @return 両方の製品情報と版が一致し、Agentがsetup対応ならtrue。
 * @note 署名・発行者・改ざんの検証ではない。workerはfileのwrite/delete共有を拒否して呼ぶ。
 */
[[nodiscard]] bool ValidateAgentImagePair( const std::filesystem::path &client,
										  const std::filesystem::path &agent) noexcept ;

/**
 * @brief worker上で実行中Client隣接の固定名Agentだけを検証・読込みする。
 * @param[in] profile 全宛先承認済みの完全snapshot。
 * @param[out] package 全成功時だけ公開する二ファイル。
 * @param[in] stop worker終了要求。
 * @return 読込み、正式metadata、同一版を満たす場合だけ成功。
 * @note 署名は検証しない。link・network pathは拒否し、信頼できる配布元の二EXEを使用する。
 */
[[nodiscard]] agent_settings::AgentSettingsResult
					  PreparePackage( const agent_settings::AgentSettingsProfile &profile,
									 std::shared_ptr<const Package> &package, std::stop_token stop) noexcept ;

/**
 * @brief 固定二ファイルをvirtual Shell fileとして提供するOLE objectを作る。
 * @param[in] package 検証済みの所有snapshot。16 MiB / 64 KiB上限。
 * @param[out] object callerがReleaseする一参照。失敗時はnullptr。
 * @return 完全なobjectを作れた場合はS_OK。
 * @note 実clipboardは変更せず、file I/Oも行わない。testから直接検証できる。
 */
[[nodiscard]] HRESULT MakeDataObject( std::shared_ptr<const Package> package,
									 IDataObject				  **object) noexcept ;

} // namespace zrdp::client::agent_deployment
