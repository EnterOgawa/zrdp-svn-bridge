//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file allowlist.h
 * @brief Agentの接続先をIPv4 literalとTCP portの完全一致へ限定する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details 設定値は診断文字列として保持せず、検証済み`OpenTarget`と匿名数値IDへ
 *          変換する。解析失敗時は途中までのentryを公開しない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  Client設定画面へ読取り専用entry viewを公開。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "protocol/multiplexer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace zrdp::agent::network {

// Agentが読み込むallowlist fileの最大byte数
inline constexpr std::size_t kMaximumAllowlistFileBytes = 64U * 1024U ;

// 一行だけでmemoryを占有しないための最大byte数
inline constexpr std::size_t kMaximumAllowlistLineBytes = 256U ;

// 初期製品で一つのAgent設定へ登録できるendpoint上限
inline constexpr std::size_t kMaximumAllowlistEntries = 128U ;

/**
 * @brief allowlistを採用しなかった固定理由。
 *
 * 入力文字列やfile pathをlogへ渡さず、この値と行番号だけで診断する。
 */
enum class AllowlistError : std::uint32_t {
	/// 異常は検出されておらず、要求の処理を継続できる。
	None				= 0U,
	/// 設定fileを取得できず、設定を有効化しない。
	FileUnavailable		= 1U,
	/// 設定fileが読込み上限を超え、memoryへの取込みを拒否する。
	FileTooLarge		= 2U,
	/// file全体を正常に読めず、部分設定を採用しない。
	FileReadFailed		= 3U,
	/// 許可していない文字を設定内で検出した。
	NonAsciiInput		= 4U,
	/// 設定の一行が長さ上限を超え、解析を中止する。
	LineTooLong			= 5U,
	/// 必須の設定sectionがなく、設定を有効化できない。
	MissingSection		= 6U,
	/// 同じ設定sectionが複数あり、曖昧な定義を拒否する。
	DuplicateSection	= 7U,
	/// 仕様で許可していない設定sectionを検出した。
	UnknownSection		= 8U,
	/// 許可されたsection外のentryを検出した。
	EntryOutsideSection = 9U,
	/// 設定keyの形式が許可された構文を満たさない。
	InvalidKey			= 10U,
	/// 匿名endpoint IDの形式または範囲が不正である。
	InvalidEndpointId	= 11U,
	/// 接続先が許可されたIPv4 literalの形式を満たさない。
	InvalidIpv4			= 12U,
	/// TCP portが指定可能な範囲または形式を満たさない。
	InvalidPort			= 13U,
	/// 同じ匿名endpoint IDが複数のentryへ割り当てられている。
	DuplicateEndpointId = 14U,
	/// 同じIPv4とportの組を重複して登録している。
	DuplicateEndpoint	= 15U,
	/// allowlist件数が有限上限を超えている。
	TooManyEntries		= 16U,
	/// 許可された接続先が一件もなく、中継を有効化できない。
	EmptyAllowlist		= 17U,
	/// memory等の有限資源を確保できず、部分状態を公開しない。
	ResourceExhausted	= 18U,
} ;

/**
 * @brief allowlist解析結果を機密値なしで表す。
 */
struct AllowlistResult {
	/// 成功時は`None`。
	AllowlistError	   error{AllowlistError::None} ;

	/// 入力行に起因する場合の1-origin行番号。file全体の失敗では0。
	std::uint32_t	   line{0U} ;

	/**
	 * @brief allowlistがatomicに採用されたかを返す。
	 * @return `error == None`ならtrue。
	 */
	[[nodiscard]] bool succeeded() const noexcept
	{
		return error == AllowlistError::None ;
	}
} ;

/**
 * @brief allowlist一件の匿名診断IDと完全一致target。
 */
struct AllowlistEntry {
	/// logへ記録できる設定内の非0匿名ID。
	std::uint32_t		 endpoint_id{0U} ;

	/// DNSやwildcardを表現しないIPv4 literalとport。
	protocol::OpenTarget target{} ;
} ;

/**
 * @brief 検証済みallowlistをimmutableな検索境界として所有する。
 */
class Allowlist final {
public:
	/**
	 * @brief IPv4とportが両方一致するentryだけを検索する。
	 * @param[in] target Clientから受信した不可信な接続候補。
	 * @param[out] endpoint_id 成功時の匿名数値ID。失敗時は0。
	 * @return 完全一致entryが一件存在する場合はtrue。
	 */
	[[nodiscard]] bool							  Find( const protocol::OpenTarget &target,
													   std::uint32_t			  &endpoint_id) const noexcept ;

	/**
	 * @brief 検証済みentry数を返す。
	 * @return `1..kMaximumAllowlistEntries`の件数。
	 */
	[[nodiscard]] std::size_t					  size() const noexcept ;

	/**
	 * @brief 検証済みentryを変更不能なviewとして返す。
	 * @return このallowlistの変更／破棄まで有効な非所有view。
	 * @note 設定編集での再表示専用であり、接続の許可判定はFindで行う。
	 */
	[[nodiscard]] std::span<const AllowlistEntry> Entries() const noexcept ;

private:
	friend AllowlistResult ParseAllowlist( std::string_view text, Allowlist &allowlist) noexcept ;

	/// parser成功時に一括moveされる検証済みentry列。
	std::vector<AllowlistEntry> entries_ ;
} ;

/**
 * @brief UTF-8 ASCII subsetのallowlist textをatomicに解析する。
 * @param[in] text `[allowlist]` sectionを含む有限設定。
 * @param[out] allowlist 成功時だけ全entryを置換する。
 * @return 固定error分類と、該当する場合の行番号。
 *
 * @note hostname、CIDR、wildcard、port range、IPv6は表現として拒否する。
 */
[[nodiscard]] AllowlistResult ParseAllowlist( std::string_view text, Allowlist &allowlist) noexcept ;

/**
 * @brief 指定fileを有限長で読み込み、検証済みallowlistへ変換する。
 * @param[in] path Agent実行fileと同じdirectoryの`agent.ini`。
 * @param[out] allowlist 成功時だけ全entryを置換する。
 * @return file I/Oまたはparserの固定結果。
 *
 * @warning pathや入力値は戻り値へ含めず、通常logへも出力しない。
 */
[[nodiscard]] AllowlistResult LoadAllowlistFile( const std::filesystem::path &path,
												Allowlist					&allowlist) noexcept ;

} // namespace zrdp::agent::network
