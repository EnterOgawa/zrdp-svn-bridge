//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file update_image_verifier.h
 * @brief WindowsのClient EXE署名、hash、PE、versionの検証境界を定義する。
 * @version 01.00.05
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  更新EXEの完全検証境界を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  取得Agentの署名・同一版・protocolとセット更新対応を検証。
 * </pre>
 *
 * @note file I/Oと署名検証は更新workerまたはmaintenance helperでのみ行う。
 */
//------------------------------------------------------------------------------
#pragma once

#include "client/update/update_manifest.h"

#include <filesystem>

namespace zrdp::client::update {

/** @brief 署名の有無とは独立したClientのWindows metadata。 */
struct ClientImageMetadata {
	/// FileVersionとProductVersionが一致する四要素。
	UpdateVersion version ;
	/// 署名対象resourceに埋め込まれたBridge protocol。
	std::uint16_t bridge_protocol{0U} ;
	/// 再起動確認の内部形式1を実装したEXEだけtrue。
	bool		  update_bootstrap{false} ;
	/// format 2の二EXEセット更新を実装したClientだけtrue。
	bool		  paired_update{false} ;
	/// 固定sourceからの初期配置modeを実装したAgentだけtrue。
	bool		  agent_setup{false} ;
} ;

/**
 * @brief regular fileを開き、64 KiB単位でSHA-256を求める。
 * @param[in] path 読取り対象。reparse pointと複数hard linkは拒否する。
 * @param[out] hash 完全なdigest。失敗時は空。
 * @param[out] size_bytes 完全検証したfile byte数。
 * @return 有限sizeのfile全体を読めた場合だけNone。
 */
[[nodiscard]] UpdateError ComputeUpdateFileHash( const std::filesystem::path &path, Sha256 &hash,
												std::uint64_t &size_bytes) noexcept ;

/**
 * @brief x64 GUI PEと正式Client metadataをWindows resourceから読む。
 * @param[in] path 読取り対象Client。
 * @param[out] metadata 完全なversionとbootstrap形式。
 * @return 基本Client metadataが有効ならNone。旧版はbootstrap=falseで読める。
 */
[[nodiscard]] UpdateError ReadClientImageMetadata( const std::filesystem::path &path,
												  ClientImageMetadata		  &metadata) noexcept ;

/**
 * @brief 正式Agentのx64 console PEと製品versionを確認する。
 * @param[in] path 固定名Agentの読取り対象。
 * @param[out] metadata 完全な製品version。Client bootstrapは要求しない。
 * @return 正式Agent metadataが有効な場合だけNone。
 */
[[nodiscard]] UpdateError ReadAgentImageMetadata( const std::filesystem::path &path,
												 ClientImageMetadata		 &metadata) noexcept ;

/**
 * @brief Windows trust、SHA-256署名、timestamp、同一公開鍵を確認する。
 * @param[in] installed 信頼anchorとなる実行中または保全済みClient。
 * @param[in] candidate 取得した候補。
 * @return 完全な信頼と署名者一致を確認できた場合だけNone。
 * @note 署名者識別情報を返さず、設定やlogへ保存しない。cache-only revocationを使う。
 */
[[nodiscard]] UpdateError
						  VerifyClientSignaturePair( const std::filesystem::path &installed,
													const std::filesystem::path &candidate) noexcept ;

/**
 * @brief 開発テスト用に署名の不在だけを確認する。
 * @param[in] path metadataを別途検証する固定製品EXE。
 * @return certificate tableなし、Windows署名なし判定の場合だけtrue。
 * @note 信頼検証の成功ではない。署名破損・未知の失敗を未署名と扱わない。
 */
[[nodiscard]] bool		  IsUnsignedDevelopmentImage( const std::filesystem::path &path) noexcept ;

/**
 * @brief 旧版の不変と候補の全要件を再検証する。
 * @param[in] installed 旧版Clientまたは保全済みcopy。
 * @param[in] candidate 候補Client。
 * @param[in] release 同意済みmanifest。
 * @param[in] installed_hash 旧版を初回検証したSHA-256。
 * @return 署名、hash、size、metadata、新版、protocolが全て有効ならNone。
 */
[[nodiscard]] UpdateError ValidateDownloadedClient( const std::filesystem::path &installed,
												   const std::filesystem::path &candidate,
												   const ReleaseManifest	   &release,
												   const Sha256 &installed_hash) noexcept ;

/**
 * @brief Agent候補の全byteと署名・製品・同一版・setup・protocolを確認する。
 * @param[in] installed 信頼anchorの旧Clientまたは保全済みcopy。
 * @param[in] candidate 固定Agent候補または置換済みAgent。
 * @param[in] release 同意した二EXEセット。
 * @return 完全検証できた場合だけNone。
 * @note Agentを実行しない。署名は通常起動ではなく自動更新にだけ要求する。
 */
[[nodiscard]] UpdateError ValidateDownloadedAgent( const std::filesystem::path &installed,
												  const std::filesystem::path &candidate,
												  const ReleaseManifest		  &release) noexcept ;

/**
 * @brief Agent候補の版・setup・wire metadataを同意セットへ照合する。
 * @param[in] agent 実PEから読み取ったAgent情報。
 * @param[in] release 同意済みセットmanifest。
 * @return 同版・固定setup・互換wireの場合だけNone。
 * @note 署名・hashの代替ではない。取得Agent検証の一段階だけを扱う。
 */
[[nodiscard]] UpdateError ValidateAgentUpdateMetadata( const ClientImageMetadata &agent,
													  const ReleaseManifest		&release) noexcept ;

} // namespace zrdp::client::update
