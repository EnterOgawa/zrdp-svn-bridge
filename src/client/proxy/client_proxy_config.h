//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_proxy_config.h
 * @brief loopback Proxy portだけを許可するClient local設定境界を定義する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details bind address、hostname、upstream endpoint、credentialを設定形式へ
 *          持ち込まず、EXE隣接の固定`client.ini`だけを任意で読み込む。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Clientの同意付き更新と設定の分離を追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zrdp::client::proxy {

// Client Proxyの既定loopback port
inline constexpr std::uint16_t kDefaultClientProxyPort = 18080U ;

// local設定file全体の有限上限
inline constexpr std::size_t   kMaximumClientProxyConfigBytes = 64U * 1024U ;

// local設定一行の有限上限
inline constexpr std::size_t   kMaximumClientProxyConfigLineBytes = 1024U ;

/**
 * @brief payloadを含めずUI／testへ公開できるlocal設定結果。
 */
enum class ClientProxyConfigurationError : std::uint32_t {
	/// 異常は検出されておらず、要求の処理を継続できる。
	None					  = 0U,
	/// EXE隣接の設定位置を解決できなかった。
	ExecutablePathUnavailable = 1U,
	/// 設定fileを開けず、指定値を使用できない。
	FileOpenFailed			  = 2U,
	/// 設定fileが読込み上限を超え、memoryへの取込みを拒否する。
	FileTooLarge			  = 3U,
	/// file全体を正常に読めず、部分設定を採用しない。
	FileReadFailed			  = 4U,
	/// 設定fileの文字コードが許可された形式ではない。
	UnsupportedEncoding		  = 5U,
	/// 設定の一行が長さ上限を超え、解析を中止する。
	LineTooLong				  = 6U,
	/// 設定行の区切りまたは構文が不正である。
	MalformedLine			  = 7U,
	/// 仕様で許可していない設定sectionを検出した。
	UnknownSection			  = 8U,
	/// 対象sectionで許可していないkeyを検出した。
	UnknownKey				  = 9U,
	/// Proxy portを複数回指定しており、設定が曖昧である。
	DuplicatePort			  = 10U,
	/// TCP portが指定可能な範囲または形式を満たさない。
	InvalidPort				  = 11U,
	/// 必要なProxy portの定義がない。
	MissingPort				  = 12U,
	/// memory等の有限資源を確保できず、部分状態を公開しない。
	ResourceExhausted		  = 13U,
} ;

/**
 * @brief 検証済みProxy local設定。
 */
struct ClientProxyConfiguration {
	/// 常に1..65535。bind addressは別fieldとして公開しない。
	std::uint16_t port{kDefaultClientProxyPort} ;

	/// `client.ini`が存在してportを明示した場合だけtrue。
	bool		  loaded_from_file{false} ;
} ;

/**
 * @brief local設定読込み結果。
 */
struct ClientProxyConfigurationResult {
	/// local設定を利用可能か決める固定失敗分類。
	ClientProxyConfigurationError error{ClientProxyConfigurationError::None} ;

	/// file API失敗時だけ設定するWin32 error code。
	std::uint32_t				  win32_error{0U} ;

	/**
	 * @brief 設定を安全に利用できるか返す。
	 * @return file欠落による既定値を含め、成功時はtrue。
	 */
	[[nodiscard]] bool			  succeeded() const noexcept
	{
		return error == ClientProxyConfigurationError::None ;
	}
} ;

/**
 * @brief `client.ini`の所有byte列をProxy port設定として検証する。
 * @param[in] text file全体。UTF-8 BOMまたはASCIIを受け付ける。
 * @param[out] configuration 成功時の明示port。[update]だけの場合は既定18080。
 * @return 内容を含まない固定結果。
 *
 * `[proxy]`の`port=<1..65535>`を受理し、未知section／Proxy key、重複、
 * 巨大入力をfail-closedにする。[update]の値は更新parserへ委譲し、更新設定の
 * 失敗だけで正常なProxy設定を無効化しない。明示[proxy]ではportを必須とする。
 */
[[nodiscard]] ClientProxyConfigurationResult
											 ParseClientProxyConfiguration( std::string_view			 text,
																		   ClientProxyConfiguration &configuration) noexcept ;

/**
 * @brief 実行中EXEと同じdirectoryの固定`client.ini`を任意読込みする。
 * @param[out] configuration file欠落時は既定18080、存在時は検証済みport。
 * @return 内容を含まない固定結果。
 *
 * file名やdirectoryは利用者入力から受け取らず、file欠落だけを正常な既定値とする。
 */
[[nodiscard]] ClientProxyConfigurationResult LoadClientProxyConfigurationFromExecutableDirectory(
	ClientProxyConfiguration &configuration) noexcept ;

} // namespace zrdp::client::proxy
