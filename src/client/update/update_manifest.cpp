//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file update_manifest.cpp
 * @brief 更新manifestとGitHub取得先の有限ASCII検証を実装する。
 * @version 01.00.01
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  同意付きClient更新用の解析境界を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  Agent必須のformat 2と固定二asset URLを検証。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/update/update_manifest.h"

#include <charconv>
#include <limits>
#include <utility>
#include <vector>

namespace zrdp::client::update {
namespace {

/** @brief INI解析中だけ所有入力を参照する一key/value。 */
struct IniValue {
	/// keyが所属するsection。
	std::string_view section ;
	/// trim済みのkey。
	std::string_view key ;
	/// trim済みのvalue。
	std::string_view value ;
} ;

/**
 * @brief ASCII spaceとtabだけを両端から除く。
 * @param[in] value 元の範囲。
 * @return 同じ入力の部分範囲。
 */
[[nodiscard]] std::string_view Trim( std::string_view value) noexcept
{
	// INIの前置き空白だけを読み飛ばす
	while( !value.empty() && (value.front() == ' ' || value.front() == '\t')) {
		value.remove_prefix( 1U) ;
	}
	// INIの後置き空白だけを読み飛ばす
	while( !value.empty() && (value.back() == ' ' || value.back() == '\t')) {
		value.remove_suffix( 1U) ;
	}

	return value ;
}

/**
 * @brief URI componentの英数字か判定する。
 * @param[in] value ASCII文字。
 * @return 英数字ならtrue。
 */
[[nodiscard]] bool IsAlnum( const char value) noexcept
{
	return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
		   (value >= '0' && value <= '9') ;
}

/**
 * @brief tagを一つの安全なGitHub URI componentとして検証する。
 * @param[in] tag Release tag。
 * @return ASCII上限と許可文字を満たす場合はtrue。
 */
[[nodiscard]] bool IsValidTag( const std::string_view tag) noexcept
{
	// 空、巨大、相対path表現のtagをダウンロードpathへ使用しない
	if( tag.empty() || tag.size() > 128U || !IsAlnum( tag.front()) ||
	   tag.find( "..") != std::string_view::npos) {
		return false ;
	}

	// slash、percent escape、query区切り等を一path componentへ混入させない
	for( const char value : tag) {
		// ASCII英数字と限定した記号以外は許可しない
		if( !IsAlnum( value) && value != '-' && value != '_' && value != '.') {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief decimalを完全消費して有限上限で解析する。
 * @param[in] value decimal token。
 * @param[in] maximum 許可上限。
 * @param[out] result 成功時の値。
 * @return 空、部分解析、overflow、上限超過がない場合はtrue。
 */
[[nodiscard]] bool ParseNumber( const std::string_view value, const std::uint64_t maximum,
							   std::uint64_t &result) noexcept
{
	// 空の数値を0へ近似しない
	if( value.empty()) {
		return false ;
	}

	std::uint64_t parsed	= 0U ;
	const auto	  converted = std::from_chars( value.data(), value.data() + value.size(), parsed) ;
	// 全文字の解析と型・policy両方の上限を満たす値だけを返す
	if( converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
	   parsed > maximum) {
		return false ;
	}

	result = parsed ;
	return true ;
}

/**
 * @brief 有限ASCII INIを重複sectionなしで分割する。
 * @param[in] text 所有fileの範囲。
 * @param[in] maximum 全体byte上限。
 * @param[out] values section付きkey/value。
 * @return 許可構文ならtrue。viewは入力の寿命内だけ有効。
 */
[[nodiscard]] bool ReadIni( std::string_view text, const std::size_t maximum,
						   std::vector<IniValue> &values)
{
	// 外部fileの全量を無制限に解析しない
	if( text.size() > maximum) {
		return false ;
	}

	// UTF-8 BOMだけを任意に除く
	if( text.starts_with( "\xEF\xBB\xBF")) {
		text.remove_prefix( 3U) ;
	}

	// NUL、DEL、改行以外の制御文字で別解釈を生む入力を拒否する
	for( const unsigned char value : text) {
		// コメントのUTF-8は解釈せず、構文を壊す制御文字だけを全体で拒否する
		if( value == 0x7FU || (value < 0x20U && value != '\t' && value != '\r' && value != '\n')) {
			return false ;
		}
	}

	std::string_view			  section ;
	std::vector<std::string_view> sections ;
	std::size_t					  offset = 0U ;
	// 上限済みfileを一行ずつ解析し、viewだけを保持する
	while( offset <= text.size()) {
		const auto newline = text.find( '\n', offset) ;
		const auto end	   = newline == std::string_view::npos ? text.size() : newline ;
		// 一行の巨大化とkey数の増加を個別に制限する
		if( end - offset > 1024U || values.size() > 64U) {
			return false ;
		}

		std::string_view line = text.substr( offset, end - offset) ;
		// CRLFの終端CRだけを除く
		if( !line.empty() && line.back() == '\r') {
			line.remove_suffix( 1U) ;
		}

		line = Trim( line) ;
		// 空行とコメントを除いた行だけを構文解析する
		if( !line.empty() && line.front() != ';' && line.front() != '#') {
			// 使用する設定値はASCIIだけに固定し、Unicodeの別解釈を排除する
			for( const unsigned char value : line) {
				// UTF-8はコメントだけに許可し、repositoryやmanifestへ混入させない
				if( value >= 0x7FU) {
					return false ;
				}
			}

			// sectionを既知の更新／Proxy用途へ制限する
			if( line.front() == '[') {
				// suffix付きや未知のsectionを黙って採用しない
				if( line != "[update]" && line != "[proxy]") {
					return false ;
				}

				// 同一sectionの重複を優先順位で解釈しない
				for( const auto previous : sections) {
					// 再宣言は曖昧な更新sourceとして拒否する
					if( previous == line) {
						return false ;
					}
				}

				sections.push_back( line) ;
				section = line ;
			} else {
				const auto equals = line.find( '=') ;
				// section外や複数の区切りを不正として拒否する
				if( section.empty() || equals == std::string_view::npos ||
				   line.find( '=', equals + 1U) != std::string_view::npos) {
					return false ;
				}

				const IniValue value{section, Trim( line.substr( 0U, equals)),
									 Trim( line.substr( equals + 1U))} ;
				// key欠落は不正な設定行とする
				if( value.key.empty() || values.size() >= 64U) {
					return false ;
				}

				// 同一sectionのkey重複を後勝ちで採用しない
				for( const auto &previous : values) {
					// 署名済みsizeやsourceの多義的な定義を拒否する
					if( previous.section == value.section && previous.key == value.key) {
						return false ;
					}
				}

				values.push_back( value) ;
			}
		}

		// 最終行を処理したら走査を終える
		if( newline == std::string_view::npos) {
			break ;
		}

		offset = newline + 1U ;
	}

	return true ;
}

/**
 * @brief ASCII hex一桁を数値へ変換する。
 * @param[in] value hex文字。
 * @return 0..15、非hexは-1。
 */
[[nodiscard]] int Hex( const char value) noexcept
{
	// 数字をhex下位範囲へ変換する
	if( value >= '0' && value <= '9') {
		return value - '0' ;
	}

	// 小文字hexを上位範囲へ変換する
	if( value >= 'a' && value <= 'f') {
		return value - 'a' + 10 ;
	}

	// 大文字hexも同じdigestへ正規化する
	if( value >= 'A' && value <= 'F') {
		return value - 'A' + 10 ;
	}

	return -1 ;
}
} // namespace

bool ParseUpdateVersion( const std::string_view text, UpdateVersion &version) noexcept
{
	version = {} ;
	// WORD四要素の正規表記上限を越えるtokenは解析しない
	if( text.empty() || text.size() > 23U) {
		return false ;
	}

	UpdateVersion parsed ;
	std::size_t	  offset = 0U ;
	// Windows WORD四要素だけを数値として解析する
	for( std::size_t index = 0U; index < parsed.components.size(); ++index) {
		const auto	  dot	 = text.find( '.', offset) ;
		const auto	  end	 = dot == std::string_view::npos ? text.size() : dot ;
		std::uint64_t number = 0U ;
		// 四要素の区切り数と各要素の16 bit上限を同時に検証する
		if( (index < 3U && dot == std::string_view::npos) ||
		   (index == 3U && dot != std::string_view::npos) ||
		   !ParseNumber( text.substr( offset, end - offset),
						std::numeric_limits<std::uint16_t>::max(), number)) {
			return false ;
		}

		parsed.components[index] = static_cast<std::uint16_t>( number) ;
		offset					 = end + 1U ;
	}

	version = parsed ;
	return true ;
}

std::string FormatUpdateVersion( const UpdateVersion &version)
{
	std::string result ;
	// 四要素を数値表記に正規化する
	for( const auto value : version.components) {
		// 二要素目以降だけ区切りを挿入する
		if( !result.empty()) {
			result.push_back( '.') ;
		}

		result.append( std::to_string( value)) ;
	}

	return result ;
}

bool ParseSha256( const std::string_view text, Sha256 &hash) noexcept
{
	hash = {} ;
	// 固定32 byte以外のdigestを受け付けない
	if( text.size() != 64U) {
		return false ;
	}

	Sha256 parsed{} ;
	// 各二桁を一byteへ変換する
	for( std::size_t index = 0U; index < parsed.size(); ++index) {
		const int high = Hex( text[index * 2U]) ;
		const int low  = Hex( text[index * 2U + 1U]) ;
		// 非hexは途中までのdigestを公開せず拒否する
		if( high < 0 || low < 0) {
			return false ;
		}

		parsed[index] = static_cast<std::byte>( high * 16 + low) ;
	}

	hash = parsed ;
	return true ;
}

std::string FormatSha256( const Sha256 &hash)
{
	// 小文字hex用の固定alphabet
	constexpr std::string_view alphabet = "0123456789abcdef" ;
	std::string				   result( 64U, '0') ;
	// 32 byteをそれぞれ二桁へ変換する
	for( std::size_t index = 0U; index < hash.size(); ++index) {
		const auto value		= std::to_integer<unsigned char>( hash[index]) ;
		result[index * 2U]		= alphabet[value >> 4U] ;
		result[index * 2U + 1U] = alphabet[value & 15U] ;
	}

	return result ;
}

bool IsValidUpdateRepository( const std::string_view repository) noexcept
{
	const auto slash = repository.find( '/') ;
	// 二path要素以外、空要素、GitHub名上限を受け付けない
	if( slash == std::string_view::npos || slash == 0U || slash > 39U ||
	   repository.size() - slash - 1U == 0U || repository.size() - slash - 1U > 100U ||
	   repository.find( '/', slash + 1U) != std::string_view::npos || !IsAlnum( repository.front()) ||
	   !IsAlnum( repository[slash - 1U]) || !IsAlnum( repository[slash + 1U]) ||
	   repository.find( "..") != std::string_view::npos) {
		return false ;
	}

	// percent escape、userinfo、queryをrepository名に混入させない
	for( std::size_t index = 0U; index < repository.size(); ++index) {
		const char value = repository[index] ;
		// ownerのhyphenとrepositoryの限定記号だけを許可する
		if( index != slash && !IsAlnum( value) && value != '-' &&
		   !(index > slash && (value == '_' || value == '.'))) {
			return false ;
		}
	}

	return true ;
}

UpdateError ParseUpdateConfiguration( const std::string_view text,
									 UpdateConfiguration   &configuration) noexcept
{
	configuration = {} ;
	try {
		std::vector<IniValue> values ;
		// 不正なfile全体から有効に見える一部分だけを採用しない
		if( !ReadIni( text, kMaximumUpdateConfigurationBytes, values)) {
			return UpdateError::InvalidConfiguration ;
		}

		UpdateConfiguration candidate ;
		candidate.repository.assign( kDefaultUpdateRepository) ;
		bool explicit_enabled = true ;
		// 更新sectionだけを扱い、Proxyのport解釈は既存parserへ残す
		for( const auto &value : values) {
			// Proxy設定は更新sourceとして解釈しない
			if( value.section == "[proxy]") {
				continue ;
			}

			// repositoryだけを公開GitHub配下のsource名として採用する
			if( value.key == "repository") {
				// 空値、URL、未知hostへの指定は更新を無効化する
				if( !IsValidUpdateRepository( value.value)) {
					return UpdateError::InvalidRepository ;
				}

				candidate.repository.assign( value.value) ;
			}
			// 外部通信の明示的な有効／無効設定を解析する
			else if( value.key == "enabled") {
				// 任意のtruthy値ではなく明示した二値だけを採用する
				if( value.value != "true" && value.value != "false") {
					return UpdateError::InvalidConfiguration ;
				}

				explicit_enabled = value.value == "true" ;
			} else {
				return UpdateError::InvalidConfiguration ;
			}
		}

		candidate.enabled = explicit_enabled && !candidate.repository.empty() ;
		configuration	  = std::move( candidate) ;
		return UpdateError::None ;
	} catch( ...) {
		return UpdateError::ResourceExhausted ;
	}
}

UpdateError ParseReleaseManifest( const std::string_view text, ReleaseManifest &manifest) noexcept
{
	manifest = {} ;
	try {
		std::vector<IniValue> values ;
		// 外部manifestは未知keyやsectionを含む部分設定として採用しない
		if( !ReadIni( text, kMaximumUpdateManifestBytes, values) || values.size() != 10U) {
			return UpdateError::InvalidManifest ;
		}

		ReleaseManifest candidate ;
		std::uint32_t	seen = 0U ;
		// 十の必須keyを完全に検証してからcallerへ公開する
		for( const auto &value : values) {
			// manifestは更新sectionの値だけで構成する
			if( value.section != "[update]") {
				return UpdateError::InvalidManifest ;
			}

			std::uint64_t number = 0U ;
			// format 2以外では自動適用を行わない
			if( value.key == "format") {
				// 未知formatを現形式へ近似しない
				if( value.value != "2") {
					return UpdateError::UnsupportedFormat ;
				}

				seen |= 1U ;
			}
			// 署名済みEXEと比較する四要素versionを解析する
			else if( value.key == "version") {
				// Windows versionと比較できない値を拒否する
				if( !ParseUpdateVersion( value.value, candidate.version)) {
					return UpdateError::InvalidVersion ;
				}

				seen |= 2U ;
			}
			// 同意したReleaseだけを指す固定path tokenを確認する
			else if( value.key == "tag") {
				// tagから任意URLや複数path要素を組み立てない
				if( !IsValidTag( value.value)) {
					return UpdateError::InvalidManifest ;
				}

				candidate.tag.assign( value.value) ;
				seen |= 4U ;
			}
			// Clientの配布assetへの任意変更を拒否する
			else if( value.key == "asset") {
				// Client側assetを固定製品EXEへ限定する
				if( value.value != kUpdateExecutableAssetName) {
					return UpdateError::InvalidManifest ;
				}

				seen |= 8U ;
			}
			// 完成EXEの全byteを照合するdigestを解析する
			else if( value.key == "sha256") {
				// 完全なSHA-256だけを検証値として使用する
				if( !ParseSha256( value.value, candidate.sha256)) {
					return UpdateError::InvalidManifest ;
				}

				seen |= 16U ;
			}
			// streaming取得のexact sizeと上限を確認する
			else if( value.key == "size") {
				// 空EXEと有限download上限を超える値は拒否する
				if( !ParseNumber( value.value, kMaximumUpdateExecutableBytes, number) ||
				   number == 0U) {
					return UpdateError::DataTooLarge ;
				}

				candidate.size_bytes = number ;
				seen |= 32U ;
			}
			// 現行Agentとの互換性を判定するwire versionを解析する
			else if( value.key == "bridge_protocol") {
				// wire versionはuint16の非0値へ限定する
				if( !ParseNumber( value.value, 65535U, number) || number == 0U) {
					return UpdateError::InvalidManifest ;
				}

				candidate.bridge_protocol = static_cast<std::uint16_t>( number) ;
				seen |= 64U ;
			}
			// Agentも任意assetやpathではなく固定配布名だけを許可する
			else if( value.key == "agent_asset") {
				// 別EXEをセットの一部として採用しない
				if( value.value != kUpdateAgentAssetName) {
					return UpdateError::InvalidManifest ;
				}

				seen |= 128U ;
			}
			// Agentの全byteをClientと独立したdigestへ照合する
			else if( value.key == "agent_sha256") {
				// 不完全なdigestでは取得・適用へ進まない
				if( !ParseSha256( value.value, candidate.agent_sha256)) {
					return UpdateError::InvalidManifest ;
				}

				seen |= 256U ;
			}
			// Agentにも一EXEの有限取得上限を適用する
			else if( value.key == "agent_size") {
				// 空Agentや上限超過をClientだけの更新へ近似しない
				if( !ParseNumber( value.value, kMaximumUpdateAgentExecutableBytes, number) ||
				   number == 0U) {
					return UpdateError::DataTooLarge ;
				}

				candidate.agent_size_bytes = number ;
				seen |= 512U ;
			} else {
				return UpdateError::InvalidManifest ;
			}
		}

		// 十の必須値が一件ずつ存在する場合だけ完全manifestを返す
		if( seen != 1023U) {
			return UpdateError::InvalidManifest ;
		}

		manifest = std::move( candidate) ;
		return UpdateError::None ;
	} catch( ...) {
		return UpdateError::ResourceExhausted ;
	}
}

std::string BuildManifestUrl( const std::string_view repository)
{
	// 検証済みrepositoryだけを固定GitHub hostへ組み込む
	if( !IsValidUpdateRepository( repository)) {
		return {} ;
	}

	return "https://github.com/" + std::string( repository) + "/releases/latest/download/" +
		   std::string( kUpdateManifestAssetName) ;
}

std::string BuildExecutableUrl( const std::string_view repository, const ReleaseManifest &manifest)
{
	// 外部入力から任意の取得先を生成しない
	if( !IsValidUpdateRepository( repository) || !IsValidTag( manifest.tag)) {
		return {} ;
	}

	return "https://github.com/" + std::string( repository) + "/releases/download/" + manifest.tag +
		   "/" + std::string( kUpdateExecutableAssetName) ;
}

bool IsAllowedUpdateUrl( const std::string_view url) noexcept
{
	// plaintext、巨大URL、userinfo、fragmentを含む取得先を拒否する
	if( !url.starts_with( "https://") || url.size() > 8192U ||
	   url.find( '#') != std::string_view::npos) {
		return false ;
	}

	// OS parserへの受渡し前にcontrolとUnicodeの別解釈を排除する
	for( const unsigned char value : url) {
		// ASCII可視文字だけをURLに許可する
		if( value <= 0x20U || value >= 0x7FU || value == '\\') {
			return false ;
		}
	}

	const auto		 end = url.find_first_of( "/?", 8U) ;
	std::string_view authority =
		url.substr( 8U, end == std::string_view::npos ? std::string_view::npos : end - 8U) ;
	// 明示した既定HTTPS portだけは同じhostとして受理する
	if( authority.ends_with( ":443")) {
		authority.remove_suffix( 4U) ;
	}

	return authority == "github.com" || authority == "objects.githubusercontent.com" ||
		   authority == "release-assets.githubusercontent.com" ;
}

std::string BuildAgentExecutableUrl( const std::string_view repository,
									const ReleaseManifest &manifest)
{
	const auto client = BuildExecutableUrl( repository, manifest) ;
	// 同じrepositoryとtagを検証できない場合はAgentだけのURLも返さない
	if( client.empty()) {
		return {} ;
	}

	return client.substr( 0U, client.size() - kUpdateExecutableAssetName.size()) +
		   std::string( kUpdateAgentAssetName) ;
}

} // namespace zrdp::client::update
