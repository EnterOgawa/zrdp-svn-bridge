//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_update_policy_test.cpp
 * @brief 更新version、manifest、公開source、redirectの有限検証を確認する。
 * @version 01.00.01
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  更新policyの正常／拒否境界testを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  セットmanifestの欠落・固定Agent・旧format拒否を検証。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/update/update_manifest.h"

#include <array>
#include <string>
#include <string_view>

namespace {

// 更新policyを短く参照するtest用namespace
namespace update = zrdp::client::update ;

// 架空の公開Release manifest。実EXEや署名者の識別情報は使用しない
constexpr std::string_view kManifest =
	"[update]\nformat=2\nversion=0.8.0.0\ntag=v0.8.0\nasset=ZRdpSvnBridge.exe\n"
	"sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
	"size=4096\nbridge_protocol=1\n"
	"agent_asset=ZRdpSvnAgent.exe\nagent_size=2048\n"
	"agent_sha256=abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\n" ;

/**
 * @brief manifestの一tokenを差替え、拒否結果を確認する。
 * @param[in] from 架空fixture内の一意なtoken。
 * @param[in] to 拒否対象値。
 * @param[in] expected 固定拒否分類。
 * @return 指定分類で拒否し、部分値が残らない場合はtrue。
 */
[[nodiscard]] bool RejectsReplacement( const std::string_view from, const std::string_view to,
									  const update::UpdateError expected)
{
	std::string text( kManifest) ;
	text.replace( text.find( from), from.size(), to) ;
	update::ReleaseManifest release ;
	return update::ParseReleaseManifest( text, release) == expected && release.tag.empty() &&
		   release.size_bytes == 0U ;
}

} // namespace

/**
 * @brief version、manifest、設定、URLの正常・overflow・拒否経路を確認する。
 * @return 成功は0、失敗は境界を示す固定非0 code。
 */
int main()
{
	update::UpdateVersion version ;
	update::UpdateVersion maximum ;
	// 四要素を文字列順ではなく数値順で比較し、WORD最大値まで扱えるか確認する
	if( !update::ParseUpdateVersion( "0.10.0.0", version) ||
	   !update::ParseUpdateVersion( "65535.65535.65535.65535", maximum) || maximum <= version ||
	   update::FormatUpdateVersion( version) != "0.10.0.0") {
		return 1 ;
	}

	const std::array<std::string_view, 10U> invalid_versions{
		"",			"1.2.3",	   "1.2.3.4.5", "1..3.4",	"-1.2.3.4",
		"+1.2.3.4", "65536.0.0.0", "0.0.0.1x",	" 1.2.3.4", "999999999999999999999999999999.0.0.0"} ;
	// 部分解析、符号、巨大数、空要素をゼロ補完で許可しない
	for( const auto text : invalid_versions) {
		// 拒否後の部分versionを次の更新判断へ残さない
		if( update::ParseUpdateVersion( text, version) || version != update::UpdateVersion{}) {
			return 2 ;
		}
	}

	update::ReleaseManifest release ;
	// 十の必須値とSHA-256 binary表現を正しく解析する
	if( update::ParseReleaseManifest( kManifest, release) != update::UpdateError::None ||
	   release.tag != "v0.8.0" || release.size_bytes != 4096U ||
	   release.agent_size_bytes != 2048U || release.bridge_protocol != 1U ||
	   update::FormatSha256( release.sha256) !=
		   "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") {
		return 3 ;
	}

	// 未知形式、Agent asset、path逃逸、巨大size、overflowを拒否する
	if( !RejectsReplacement( "format=2", "format=1", update::UpdateError::UnsupportedFormat) ||
	   !RejectsReplacement( "version=0.8.0.0", "version=65536.0.0.0",
						   update::UpdateError::InvalidVersion) ||
	   !RejectsReplacement( "asset=ZRdpSvnBridge.exe", "asset=ZRdpSvnAgent.exe",
						   update::UpdateError::InvalidManifest) ||
	   !RejectsReplacement( "tag=v0.8.0", "tag=../other", update::UpdateError::InvalidManifest) ||
	   !RejectsReplacement( "size=4096", "size=67108865", update::UpdateError::DataTooLarge) ||
	   !RejectsReplacement( "size=4096", "size=0", update::UpdateError::DataTooLarge) ||
	   !RejectsReplacement( "agent_size=2048", "agent_size=0", update::UpdateError::DataTooLarge) ||
	   !RejectsReplacement( "agent_size=2048", "agent_size=16777217",
						   update::UpdateError::DataTooLarge) ||
	   !RejectsReplacement( "agent_asset=ZRdpSvnAgent.exe", "agent_asset=../other.exe",
						   update::UpdateError::InvalidManifest) ||
	   !RejectsReplacement( "agent_asset=ZRdpSvnAgent.exe", "missing_asset=ZRdpSvnAgent.exe",
						   update::UpdateError::InvalidManifest) ||
	   !RejectsReplacement( "agent_sha256=abcdef", "agent_sha256=xyz",
						   update::UpdateError::InvalidManifest) ||
	   !RejectsReplacement( "bridge_protocol=1", "bridge_protocol=65536",
						   update::UpdateError::InvalidManifest) ||
	   update::ParseReleaseManifest( std::string( kManifest) + "tag=other\n", release) !=
		   update::UpdateError::InvalidManifest ||
	   update::ParseReleaseManifest( std::string( kManifest) + "unknown=value\n", release) !=
		   update::UpdateError::InvalidManifest ||
	   update::ParseReleaseManifest( "[update]\nformat=1\n", release) !=
		   update::UpdateError::InvalidManifest ||
	   update::ParseReleaseManifest( std::string( update::kMaximumUpdateManifestBytes + 1U, 'x'),
									release) != update::UpdateError::InvalidManifest) {
		return 4 ;
	}

	update::UpdateConfiguration configuration ;
	// ユーザー指定の既定sourceと明示的無効化を正しく分離する
	if( update::ParseUpdateConfiguration( "", configuration) != update::UpdateError::None ||
	   !configuration.enabled || configuration.repository != update::kDefaultUpdateRepository ||
	   update::ParseUpdateConfiguration(
		   "; 更新設定のコメント\n[proxy]\nport=18080\n[update]\nenabled=false\n", configuration) !=
		   update::UpdateError::None ||
	   configuration.enabled ||
	   update::ParseUpdateConfiguration( "[update]\nrepository=example/project\nenabled=true\n",
										configuration) != update::UpdateError::None ||
	   !configuration.enabled || configuration.repository != "example/project") {
		return 5 ;
	}

	const std::array<std::string_view, 8U> invalid_repositories{"",
																"owner",
																"owner/repo/other",
																"owner/../repo",
																"https://github.com/owner/repo",
																"owner/repo?token=value",
																"user@host/repo",
																"owner/repo%2Fother"} ;
	// arbitrary URL、escape、userinfo、複数pathをrepository名として採用しない
	for( const auto name : invalid_repositories) {
		// 不正な取得先では通信可能設定を残さない
		if( update::IsValidUpdateRepository( name) ||
		   update::ParseUpdateConfiguration( "[update]\nrepository=" + std::string( name) + "\n",
											configuration) == update::UpdateError::None ||
		   configuration.enabled || !configuration.repository.empty()) {
			return 6 ;
		}
	}

	// 重複key／sectionと未知booleanはfail-closedにする
	if( update::ParseUpdateConfiguration( "[update]\nenabled=1\n", configuration) !=
		   update::UpdateError::InvalidConfiguration ||
	   update::ParseUpdateConfiguration( "[update]\nenabled=true\nenabled=false\n", configuration) !=
		   update::UpdateError::InvalidConfiguration ||
	   update::ParseUpdateConfiguration( "[update]\n[update]\n", configuration) !=
		   update::UpdateError::InvalidConfiguration ||
	   update::ParseUpdateConfiguration( "[update]\nunknown=true\n", configuration) !=
		   update::UpdateError::InvalidConfiguration) {
		return 7 ;
	}

	const std::array<std::string_view, 3U> allowed{
		"https://github.com/example/project/releases/latest/download/zrdp-client-update.ini",
		"https://objects.githubusercontent.com:443/asset?signature=opaque",
		"https://release-assets.githubusercontent.com/asset?query=opaque"} ;
	// GitHubと二つの固定asset hostのHTTPSだけを許可する
	for( const auto url : allowed) {
		// 既定HTTPS portを含む正規取得先が誤って拒否されないことを確認する
		if( !update::IsAllowedUpdateUrl( url)) {
			return 8 ;
		}
	}

	const std::array<std::string_view, 11U> denied{"http://github.com/asset",
												   "https://github.com:80/asset",
												   "https://github.com.evil.invalid/asset",
												   "https://github.com@evil.invalid/asset",
												   "https://user@github.com/asset",
												   "https://127.0.0.1/asset",
												   "https://github.com\\evil/asset",
												   "https://github.com/asset#fragment",
												   "/relative/asset",
												   "https://github.com/asset\r\nheader",
												   "https://%67ithub.com/asset"} ;
	// host偽装、plaintext、別port、control、relative redirectを拒否する
	for( const auto url : denied) {
		// 取得先制限を外す別解釈が通らないことを確認する
		if( update::IsAllowedUpdateUrl( url)) {
			return 9 ;
		}
	}

	// 最新manifestと同意したtagのEXEを別URLとして固定構築する
	if( update::ParseReleaseManifest( kManifest, release) != update::UpdateError::None ||
	   update::BuildManifestUrl( "example/project") !=
		   "https://github.com/example/project/releases/latest/download/zrdp-client-update.ini" ||
	   update::BuildExecutableUrl( "example/project", release) !=
		   "https://github.com/example/project/releases/download/v0.8.0/ZRdpSvnBridge.exe" ||
	   update::BuildAgentExecutableUrl( "example/project", release) !=
		   "https://github.com/example/project/releases/download/v0.8.0/ZRdpSvnAgent.exe" ||
	   !update::BuildAgentExecutableUrl( "../invalid", release).empty()) {
		return 10 ;
	}

	return 0 ;
}
