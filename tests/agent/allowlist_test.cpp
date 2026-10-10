//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file allowlist_test.cpp
 * @brief Agent allowlistのstrict解析、完全一致、atomic更新を検証する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details 文書用IPv4 addressだけを使用し、実endpoint、DNS、Socket APIへ
 *          アクセスしない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "agent/network/allowlist.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

using zrdp::agent::network::Allowlist ;
using zrdp::agent::network::AllowlistError ;

/**
 * @brief 指定textが期待した固定errorで拒否されることを確認する。
 * @param[in] text 検証対象設定。
 * @param[in] expected_error 期待する分類。
 * @return 完全一致した場合はtrue。
 */
[[nodiscard]] bool ExpectError( const std::string_view text, const AllowlistError expected_error)
{
	Allowlist  allowlist ;
	const auto result = zrdp::agent::network::ParseAllowlist( text, allowlist) ;
	return result.error == expected_error && allowlist.size() == 0U ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief allowlistのstrict解析、完全一致、file読込み時のatomic更新を検証する。
 * @param[in] argument_count 実行fileを含む引数個数。
 * @param[in] arguments 任意の一時directory rootを第1引数で受け取る。
 * @return 全検証成功時は0、引数不正または失敗時は識別用の非0 code。
 */
int wmain( const int argument_count, wchar_t *arguments[])
{
	using zrdp::protocol::OpenTarget ;

	constexpr std::string_view valid = "\xEF\xBB\xBF[allowlist]\r\n"
									   "# document-only endpoints\r\n"
									   "endpoint.1001 = 192.0.2.10:80\r\n"
									   "endpoint.2002=198.51.100.20:8080\r\n" ;
	Allowlist				   allowlist ;
	const auto				   result = zrdp::agent::network::ParseAllowlist( valid, allowlist) ;
	// 正常allowlistを全件受理し、診断行0と登録件数が一致することを確認する
	if( !result.succeeded() || result.line != 0U || allowlist.size() != 2U) {
		return 1 ;
	}

	std::uint32_t endpoint_id = 0U ;
	// IPv4とportが両方一致する二件だけを許可する
	if( !allowlist.Find( OpenTarget{{192U, 0U, 2U, 10U}, 80U}, endpoint_id) || endpoint_id != 1001U ||
	   !allowlist.Find( OpenTarget{{198U, 51U, 100U, 20U}, 8080U}, endpoint_id) ||
	   endpoint_id != 2002U) {
		return 2 ;
	}

	// addressまたはport一方だけの一致へfallbackしない
	if( allowlist.Find( OpenTarget{{192U, 0U, 2U, 11U}, 80U}, endpoint_id) || endpoint_id != 0U ||
	   allowlist.Find( OpenTarget{{192U, 0U, 2U, 10U}, 81U}, endpoint_id) || endpoint_id != 0U) {
		return 3 ;
	}

	// section欠落・空設定・未知section・重複sectionの固定拒否分類を確認する
	if( !ExpectError( "", AllowlistError::MissingSection) ||
	   !ExpectError( "[allowlist]\n", AllowlistError::EmptyAllowlist) ||
	   !ExpectError( "endpoint.1=192.0.2.1:80\n", AllowlistError::EntryOutsideSection) ||
	   !ExpectError( "[other]\nendpoint.1=192.0.2.1:80\n", AllowlistError::UnknownSection) ||
	   !ExpectError( "[allowlist]\n[allowlist]\n"
					"endpoint.1=192.0.2.1:80\n",
					AllowlistError::DuplicateSection)) {
		return 4 ;
	}

	// keyと匿名endpoint IDの構文・値域違反が拒否されることを確認する
	if( !ExpectError( "[allowlist]\nserver.1=192.0.2.1:80\n", AllowlistError::InvalidKey) ||
	   !ExpectError( "[allowlist]\nendpoint.0=192.0.2.1:80\n", AllowlistError::InvalidEndpointId) ||
	   !ExpectError( "[allowlist]\nendpoint.01=192.0.2.1:80\n", AllowlistError::InvalidEndpointId) ||
	   !ExpectError( "[allowlist]\n"
					"endpoint.4294967296=192.0.2.1:80\n",
					AllowlistError::InvalidEndpointId)) {
		return 5 ;
	}

	constexpr std::string_view invalid_ipv4[] = {
		"svn.example.test:80", "192.0.2.0/24:80", "*:80",		"192.0.2:80",	"192.0.2.1.2:80",
		"192.00.2.1:80",	   "256.0.2.1:80",	  "0.0.0.0:80", "224.0.0.1:80", "[::1]:80",
	} ;
	// hostname、CIDR、wildcard、IPv6、曖昧／非unicast IPv4をすべて拒否する
	for( const std::string_view endpoint : invalid_ipv4) {
		const std::string text = "[allowlist]\nendpoint.1=" + std::string( endpoint) + "\n" ;
		// 不正IPv4候補が名前解決やwildcardへ読み替えられず拒否されることを確認する
		if( !ExpectError( text, AllowlistError::InvalidIpv4)) {
			return 6 ;
		}
	}

	constexpr std::string_view invalid_ports[] = {
		"192.0.2.1",	   "192.0.2.1:",	  "192.0.2.1:0",   "192.0.2.1:080",
		"192.0.2.1:65536", "192.0.2.1:80-81", "192.0.2.1:+80",
	} ;
	// port欠落、range、符号、範囲外、別表記を受理しない
	for( const std::string_view endpoint : invalid_ports) {
		const std::string	 text = "[allowlist]\nendpoint.1=" + std::string( endpoint) + "\n" ;
		const AllowlistError expected =
			endpoint == "192.0.2.1" ? AllowlistError::InvalidIpv4 : AllowlistError::InvalidPort ;
		// 不正port候補が期待した固定errorで拒否されることを確認する
		if( !ExpectError( text, expected)) {
			return 7 ;
		}
	}

	// 匿名IDとIPv4・portの重複がそれぞれ拒否されることを確認する
	if( !ExpectError( "[allowlist]\n"
					"endpoint.1=192.0.2.1:80\n"
					"endpoint.1=198.51.100.1:80\n",
					AllowlistError::DuplicateEndpointId) ||
	   !ExpectError( "[allowlist]\n"
					"endpoint.1=192.0.2.1:80\n"
					"endpoint.2=192.0.2.1:80\n",
					AllowlistError::DuplicateEndpoint)) {
		return 8 ;
	}

	const std::string non_ascii = "[allowlist]\nendpoint.1=192.0.2.1:80\n"
								  "\xE3\x81\x82" ;
	const std::string long_line( zrdp::agent::network::kMaximumAllowlistLineBytes + 1U, 'x') ;
	const std::string oversized_file( zrdp::agent::network::kMaximumAllowlistFileBytes + 1U, '\n') ;
	// 非ASCII・長行・巨大fileの入力上限が有限値で拒否されることを確認する
	if( !ExpectError( non_ascii, AllowlistError::NonAsciiInput) ||
	   !ExpectError( "[allowlist]\n" + long_line + "\n", AllowlistError::LineTooLong) ||
	   !ExpectError( oversized_file, AllowlistError::FileTooLarge)) {
		return 9 ;
	}

	std::string too_many = "[allowlist]\n" ;
	// endpoint上限を一件だけ越え、全体が部分採用されないことを確認する
	for( std::size_t index = 0U; index <= zrdp::agent::network::kMaximumAllowlistEntries; ++index) {
		too_many += "endpoint." + std::to_string( index + 1U) + "=192.0.2." +
					std::to_string( index + 1U) + ":80\n" ;
	}

	// entry件数上限を越える設定が拒否されることを確認する
	if( !ExpectError( too_many, AllowlistError::TooManyEntries)) {
		return 10 ;
	}

	// 失敗したreloadは以前の検証済み設定を置換しない
	const auto failed_reload =
		zrdp::agent::network::ParseAllowlist( "[allowlist]\nendpoint.9=*:80\n", allowlist) ;
	endpoint_id = 0U ;
	// reload失敗時に以前の検証済みallowlistが保持されることを確認する
	if( failed_reload.error != AllowlistError::InvalidIpv4 || allowlist.size() != 2U ||
	   !allowlist.Find( OpenTarget{{192U, 0U, 2U, 10U}, 80U}, endpoint_id) || endpoint_id != 1001U) {
		return 11 ;
	}

	// repository内template自体をproduction file loaderで検証する
	if( argument_count != 2 || arguments == nullptr || arguments[1U] == nullptr) {
		return 12 ;
	}

	Allowlist  template_allowlist ;
	const auto template_result =
		zrdp::agent::network::LoadAllowlistFile( arguments[1U], template_allowlist) ;
	// 配布templateの例示allowlistが正常に解析できることを確認する
	if( !template_result.succeeded() || template_allowlist.size() != 3U) {
		return 13 ;
	}

	return 0 ;
}
