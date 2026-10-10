//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_proxy_config_test.cpp
 * @brief Client Proxy port設定の許可構文とfail-closed拒否を検証する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Clientの同意付き更新と設定の分離を追加。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/proxy/client_proxy_config.h"

#include <string>
#include <string_view>

namespace {

using zrdp::client::proxy::ClientProxyConfiguration ;
using zrdp::client::proxy::ClientProxyConfigurationError ;
using zrdp::client::proxy::ParseClientProxyConfiguration ;

/**
 * @brief 入力が指定errorで拒否されることを確認する。
 * @param[in] text 設定全体。
 * @param[in] expected 期待する固定分類。
 * @return 分類一致かつ既定値へ戻された場合はtrue。
 */
[[nodiscard]] bool IsRejected( const std::string_view			  text,
							  const ClientProxyConfigurationError expected)
{
	ClientProxyConfiguration configuration{} ;
	configuration.port			   = 12345U ;
	configuration.loaded_from_file = true ;
	const auto result			   = ParseClientProxyConfiguration( text, configuration) ;
	return result.error == expected &&
		   configuration.port == zrdp::client::proxy::kDefaultClientProxyPort &&
		   !configuration.loaded_from_file ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief client.iniのProxy port検証とfail-closed読込みを検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int main()
{
	ClientProxyConfiguration   configuration{} ;
	constexpr std::string_view valid		= "\xEF\xBB\xBF"
											  "; documentation-only setting\r\n"
											  "[proxy]\r\n"
											  " port = 28080 \r\n" ;
	const auto				   valid_result = ParseClientProxyConfiguration( valid, configuration) ;
	// 正常port設定とfile読込みflagが保持されることを確認する
	if( !valid_result.succeeded() || configuration.port != 28080U ||
	   !configuration.loaded_from_file) {
		return 1 ;
	}

	// 空設定・未知section・未知key・重複port等が固定分類で拒否されることを確認する
	if( !IsRejected( "", ClientProxyConfigurationError::MissingPort) ||
	   !IsRejected( "[other]\nport=18080\n", ClientProxyConfigurationError::UnknownSection) ||
	   !IsRejected( "[proxy]\nbind=127.0.0.1\n", ClientProxyConfigurationError::UnknownKey) ||
	   !IsRejected( "[proxy]\nport=18080\nport=18081\n",
				   ClientProxyConfigurationError::DuplicatePort) ||
	   !IsRejected( "[proxy]\nport=0\n", ClientProxyConfigurationError::InvalidPort) ||
	   !IsRejected( "[proxy]\nport=65536\n", ClientProxyConfigurationError::InvalidPort) ||
	   !IsRejected( "[proxy]\nport=-1\n", ClientProxyConfigurationError::InvalidPort) ||
	   !IsRejected( "[proxy]\nport=18x80\n", ClientProxyConfigurationError::InvalidPort) ||
	   !IsRejected( "port=18080\n", ClientProxyConfigurationError::MalformedLine) ||
	   !IsRejected( "[proxy]\nport=18080=1\n", ClientProxyConfigurationError::MalformedLine) ||
	   !IsRejected( "[proxy]\nport=18080\n[proxy]\n",
				   ClientProxyConfigurationError::MalformedLine)) {
		return 2 ;
	}

	std::string long_line( zrdp::client::proxy::kMaximumClientProxyConfigLineBytes + 1U, 'x') ;
	// 長い設定行が有限line上限で拒否されることを確認する
	if( !IsRejected( long_line, ClientProxyConfigurationError::LineTooLong)) {
		return 3 ;
	}

	std::string oversized( zrdp::client::proxy::kMaximumClientProxyConfigBytes + 1U, 'x') ;
	// 巨大設定fileが有限byte上限で拒否されることを確認する
	if( !IsRejected( oversized, ClientProxyConfigurationError::FileTooLarge)) {
		return 4 ;
	}

	const std::string unsupported = "[proxy]\nport=18080\n\xC2\xA0" ;
	// 非ASCII設定が代替文字に変換されず拒否されることを確認する
	if( !IsRejected( unsupported, ClientProxyConfigurationError::UnsupportedEncoding)) {
		return 5 ;
	}

	// TCP最大portは上限値を含めて有効とする
	const auto maximum_result = ParseClientProxyConfiguration( "[proxy]\nport=65535", configuration) ;
	// 最大TCP port 65535は上限を含めて正常受理されることを確認する
	if( !maximum_result.succeeded() || configuration.port != 65535U) {
		return 6 ;
	}

	const auto with_update = ParseClientProxyConfiguration(
		"[proxy]\nport=28080\n[update]\nrepository=example/project\nenabled=true\n", configuration) ;
	// 更新source設定が既存のProxy portやfile読込みflagを変化させないことを確認する
	if( !with_update.succeeded() || configuration.port != 28080U ||
	   !configuration.loaded_from_file) {
		return 7 ;
	}

	const auto invalid_update = ParseClientProxyConfiguration(
		"[update]\nrepository=不正な取得先\nno-equals\n[update]\nenabled=unknown\n[proxy]\nport=28080\n",
		configuration) ;
	// 更新専用parserが拒否する値でもProxyの正常portは継続する
	if( !invalid_update.succeeded() || configuration.port != 28080U ||
	   !configuration.loaded_from_file) {
		return 8 ;
	}

	const auto update_only =
		ParseClientProxyConfiguration( "[update]\nenabled=false\n", configuration) ;
	// 更新設定だけのfileではProxy既定portを変更せず、空fileの既存拒否は維持する
	if( !update_only.succeeded() || configuration.port != 18080U || configuration.loaded_from_file ||
	   !IsRejected( "[proxy]\n[update]\nenabled=false\n",
				   ClientProxyConfigurationError::MissingPort)) {
		return 9 ;
	}

	return 0 ;
}
