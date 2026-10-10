//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file http_request_parser.cpp
 * @brief HTTP request line、authority、hop-by-hop headerの有限変換を実装する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Connectionによる宛先・固定長framingの除去を拒否。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/proxy/http_request_parser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace zrdp::client::proxy {
namespace {

// HTTP header終端
constexpr std::array<std::byte, 4> kHeaderTerminator{
	std::byte{'\r'},
	std::byte{'\n'},
	std::byte{'\r'},
	std::byte{'\n'},
} ;

/**
 * @brief 解析中だけ保持する一header line。
 */
struct HeaderLine {
	/// 比較用にASCII小文字化したheader名。内容をlogへ渡さない。
	std::string name_lower ;
	/// opaqueに中継する元header行の借用view。解析入力の寿命内だけ有効。
	std::string raw ;
	/// nameと区切りを除いたheader値の借用view。認証内容を保存しない。
	std::string value ;
} ;

/**
 * @brief ASCII英大文字を小文字へ変換する。
 * @param[in] value ASCII byte。
 * @return ASCII小文字または元のbyte。
 */
[[nodiscard]] constexpr char AsciiLower( const char value) noexcept
{
	// HTTP名比較に必要なASCII大文字だけを小文字へ正規化する
	if( value >= 'A' && value <= 'Z') {
		return static_cast<char>( value - 'A' + 'a') ;
	}

	return value ;
}

/**
 * @brief ASCII大小文字を区別せず二つの値を比較する。
 * @param[in] left 左辺。
 * @param[in] right 右辺。
 * @return byte数とASCII case-fold後の全byteが一致する場合はtrue。
 */
[[nodiscard]] bool EqualAsciiCaseInsensitive( const std::string_view left,
											 const std::string_view right) noexcept
{
	// 長さが異なるHTTP tokenは文字比較前に不一致として返す
	if( left.size() != right.size()) {
		return false ;
	}

	// localeへ依存せずHTTP tokenのASCIIだけを比較する
	for( std::size_t index = 0U; index < left.size(); ++index) {
		// 一文字でも大小文字無視で異なれば同じheader名と扱わない
		if( AsciiLower( left[index]) != AsciiLower( right[index])) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief ASCII文字列を小文字の所有値へ変換する。
 * @param[in] value 変換元。
 * @return case-fold済み文字列。
 */
[[nodiscard]] std::string ToAsciiLower( const std::string_view value)
{
	std::string result ;
	result.reserve( value.size()) ;
	// header名とConnection tokenだけを正規化する
	for( const char character : value) {
		result.push_back( AsciiLower( character)) ;
	}

	return result ;
}

/**
 * @brief HTTP OWSを両端から除く。
 * @param[in] value header field value。
 * @return 元bufferを参照するtrim済み範囲。
 */
[[nodiscard]] std::string_view TrimOptionalWhitespace( std::string_view value) noexcept
{
	// field value先頭のspace／tabだけを除く
	while( !value.empty() && (value.front() == ' ' || value.front() == '\t')) {
		value.remove_prefix( 1U) ;
	}

	// field value末尾のspace／tabだけを除く
	while( !value.empty() && (value.back() == ' ' || value.back() == '\t')) {
		value.remove_suffix( 1U) ;
	}

	return value ;
}

/**
 * @brief HTTP tokenに利用可能な一byteか検証する。
 * @param[in] value 検証byte。
 * @return RFC tokenのASCII集合に含まれる場合はtrue。
 */
[[nodiscard]] constexpr bool IsTokenCharacter( const char value) noexcept
{
	// HTTP tokenで許可されたASCII英数字は区切り文字ではない
	if( (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
	   (value >= 'a' && value <= 'z')) {
		return true ;
	}

	// 英数字以外のHTTP tcharだけを許可集合として判定する
	switch( value) {
	// 感嘆符はHTTP tokenの構成文字として許可する
	case '!':
	// hash記号はHTTP tokenの構成文字として許可する
	case '#':
	// dollar記号はHTTP tokenの構成文字として許可する
	case '$':
	// percent記号はHTTP tokenの構成文字として許可する
	case '%':
	// ampersandはHTTP tokenの構成文字として許可する
	case '&':
	// apostropheはHTTP tokenの構成文字として許可する
	case '\'':
	// asteriskはHTTP tokenの構成文字として許可する
	case '*':
	// plus記号はHTTP tokenの構成文字として許可する
	case '+':
	// hyphenはHTTP tokenの構成文字として許可する
	case '-':
	// periodはHTTP tokenの構成文字として許可する
	case '.':
	// caretはHTTP tokenの構成文字として許可する
	case '^':
	// underscoreはHTTP tokenの構成文字として許可する
	case '_':
	// backtickはHTTP tokenの構成文字として許可する
	case '`':
	// vertical barはHTTP tokenの構成文字として許可する
	case '|':
	// tildeはHTTP tokenの構成文字として許可する
	case '~':
		return true ;
	// その他のdelimiter・空白・制御文字はHTTP tokenとして拒否する
	default:
		return false ;
	}
}

/**
 * @brief 非空文字列の全byteがHTTP tokenか確認する。
 * @param[in] value 検証対象。
 * @return method／header名／Connection tokenとして安全ならtrue。
 */
[[nodiscard]] bool IsToken( const std::string_view value) noexcept
{
	// 空のmethodやheader名を有効tokenとして受理しない
	if( value.empty()) {
		return false ;
	}

	// delimiterや制御文字を一byteでも含むtokenを拒否する
	for( const char character : value) {
		// 許可外文字を一byteでも含むtokenは全体を拒否する
		if( !IsTokenCharacter( character)) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief header field valueに禁止制御文字がないか確認する。
 * @param[in] value CRLFを除いたfield value。
 * @return horizontal tabと表示byteだけならtrue。
 */
[[nodiscard]] bool IsValidHeaderValue( const std::string_view value) noexcept
{
	// NUL、改行、DEL等をrequest smugglingへ利用させない
	for( const unsigned char character : value) {
		// HTAB以外の制御文字とDELをheader値として受理しない
		if( (character < 0x20U && character != 0x09U) || character == 0x7FU) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief chunk extension部分が改行や制御文字を含まないか確認する。
 * @param[in] value `;`以降のopaque extension。
 * @return 表示ASCIIとspaceだけならtrue。
 */
[[nodiscard]] bool IsValidChunkExtension( const std::string_view value) noexcept
{
	// extensionを解釈せず、request smugglingに使えるcontrolだけを拒否する
	for( const unsigned char character : value) {
		// 制御文字とDELをrequest-targetの内容として転送しない
		if( character < 0x20U || character == 0x7FU) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief 10進数を指定上限内の64-bit値へ厳密変換する。
 * @param[in] text 符号や空白を含まない値。
 * @param[in] maximum 受理上限。
 * @param[out] value 成功時の値。
 * @return 全byteを10進数として変換できた場合はtrue。
 */
[[nodiscard]] bool ParseUnsignedDecimal( const std::string_view text, const std::uint64_t maximum,
										std::uint64_t &value) noexcept
{
	value = 0U ;
	// 空の数値fieldを0と近似せず不正入力として拒否する
	if( text.empty()) {
		return false ;
	}

	const char *const begin	 = text.data() ;
	const char *const end	 = begin + text.size() ;
	const auto		  result = std::from_chars( begin, end, value, 10) ;
	return result.ec == std::errc{} && result.ptr == end && value <= maximum ;
}

/**
 * @brief 一byteのASCII 16進数値を返す。
 * @param[in] character 検証対象。
 * @param[out] value 成功時の0..15。
 * @return 16進digitの場合はtrue。
 */
[[nodiscard]] constexpr bool ParseHexDigit( const char character, std::uint8_t &value) noexcept
{
	// 十六進数の数字部分は0から9の値へ変換する
	if( character >= '0' && character <= '9') {
		value = static_cast<std::uint8_t>( character - '0') ;
		return true ;
	}

	// 大文字の十六進英字を10から15へ変換する
	if( character >= 'A' && character <= 'F') {
		value = static_cast<std::uint8_t>( character - 'A' + 10) ;
		return true ;
	}

	// 小文字の十六進英字も同じ値域へ変換する
	if( character >= 'a' && character <= 'f') {
		value = static_cast<std::uint8_t>( character - 'a' + 10) ;
		return true ;
	}

	return false ;
}

/**
 * @brief chunk sizeの16進数部分を有限64-bit値へ変換する。
 * @param[in] text extensionを除いた非空size。
 * @param[out] value 成功時のdecoded chunk byte数。
 * @return 全digitが有効でrequest body上限内ならtrue。
 */
[[nodiscard]] bool ParseChunkSize( const std::string_view text, std::uint64_t &value) noexcept
{
	value = 0U ;
	// 空のchunk長を終端chunkの0として扱わない
	if( text.empty()) {
		return false ;
	}

	// shift前に上限を検査し64-bit overflowと巨大bodyを同時に拒否する
	for( const char character : text) {
		std::uint8_t digit = 0U ;
		// 不正桁またはbody上限を越える十六進数はshift前に拒否する
		if( !ParseHexDigit( character, digit) ||
		   value > (kMaximumHttpRequestBodyBytes - digit) / 16U) {
			return false ;
		}

		value = value * 16U + digit ;
	}

	return true ;
}

/**
 * @brief dotted decimal IPv4 literalを4 octetへ変換する。
 * @param[in] text host部分。
 * @param[out] ipv4 成功時のoctet。
 * @return DNSやIPv6表現を含まない厳密IPv4ならtrue。
 */
[[nodiscard]] bool ParseIpv4Literal( const std::string_view		 text,
									std::array<std::uint8_t, 4> &ipv4) noexcept
{
	std::size_t offset = 0U ;
	// 4個のdecimal componentを順に検証する
	for( std::size_t component = 0U; component < ipv4.size(); ++component) {
		const std::size_t separator		  = text.find( '.', offset) ;
		const bool		  final_component = component + 1U == ipv4.size() ;
		// 最後以外はdot必須、最後には追加dotを許可しない
		if( (!final_component && separator == std::string_view::npos) ||
		   (final_component && separator != std::string_view::npos)) {
			return false ;
		}

		const std::size_t	   end	 = final_component ? text.size() : separator ;
		const std::string_view part	 = text.substr( offset, end - offset) ;
		std::uint64_t		   value = 0U ;
		// IPv4の各octetは空欄・過長・255超過を拒否する
		if( part.empty() || part.size() > 3U || !ParseUnsignedDecimal( part, 255U, value)) {
			return false ;
		}

		ipv4[component] = static_cast<std::uint8_t>( value) ;
		offset			= end + 1U ;
	}

	return offset == text.size() + 1U ;
}

/**
 * @brief IPv4 literalとoptional portをOpenTargetへ変換する。
 * @param[in] authority userinfoやbracketを含まないauthority候補。
 * @param[out] target 成功時のtarget。port省略時は80。
 * @param[out] error 失敗時の固定分類。
 * @return 初期対象形式として有効ならtrue。
 */
[[nodiscard]] bool ParseAuthority( const std::string_view authority, protocol::OpenTarget &target,
								  HttpRequestError &error) noexcept
{
	error = HttpRequestError::None ;
	// userinfo、IPv6 bracket、空authorityを接続候補へ変換しない
	if( authority.empty() || authority.find( '@') != std::string_view::npos ||
	   authority.find( '[') != std::string_view::npos ||
	   authority.find( ']') != std::string_view::npos) {
		error = HttpRequestError::InvalidIpv4 ;
		return false ;
	}

	const std::size_t colon = authority.find( ':') ;
	// 複数colonはIPv6または不正portとして拒否する
	if( colon != std::string_view::npos &&
	   authority.find( ':', colon + 1U) != std::string_view::npos) {
		error = HttpRequestError::InvalidIpv4 ;
		return false ;
	}

	const std::string_view host =
		colon == std::string_view::npos ? authority : authority.substr( 0U, colon) ;
	// IPv4 literalでないauthorityは名前解決せず拒否する
	if( !ParseIpv4Literal( host, target.ipv4)) {
		error = HttpRequestError::InvalidIpv4 ;
		return false ;
	}

	target.port = 80U ;
	// 明示portがある場合だけport文字列を値域検証して採用する
	if( colon != std::string_view::npos) {
		const std::string_view port_text = authority.substr( colon + 1U) ;
		std::uint64_t		   port		 = 0U ;
		// 非数値・値域外・0のportは既定portへfallbackせず拒否する
		if( !ParseUnsignedDecimal( port_text, std::numeric_limits<std::uint16_t>::max(), port) ||
		   port == 0U) {
			error = HttpRequestError::InvalidPort ;
			return false ;
		}

		target.port = static_cast<std::uint16_t>( port) ;
	}

	return true ;
}

/**
 * @brief 一header blockをCRLF lineへ分割する。
 * @param[in] header CRLFCRLFを含む所有文字列。
 * @param[out] lines 終端CRLFを除いたline列。
 * @return 空行終端まで構文どおり分割できた場合はtrue。
 */
[[nodiscard]] bool SplitHeaderLines( const std::string_view		   header,
									std::vector<std::string_view> &lines)
{
	lines.clear() ;
	std::size_t offset = 0U ;
	// request lineからheader終端までCRLFを順に探索する
	while( offset < header.size()) {
		const std::size_t end = header.find( "\r\n", offset) ;
		// CRLFがない行は完全headerの一部として解析しない
		if( end == std::string_view::npos) {
			return false ;
		}

		const std::string_view line = header.substr( offset, end - offset) ;
		offset						= end + 2U ;
		// 空行はheader終端として扱い、終端後の余分なdataも検証する
		if( line.empty()) {
			return offset == header.size() ;
		}

		lines.push_back( line) ;
	}

	return false ;
}

/**
 * @brief comma区切りConnection tokenを小文字集合へ追加する。
 * @param[in] value Connection field value。
 * @param[in,out] tokens 既存token列。
 * @return 全tokenが非空のHTTP tokenならtrue。
 */
[[nodiscard]] bool ParseConnectionTokens( const std::string_view	   value,
										 std::vector<std::string> &tokens)
{
	std::size_t offset = 0U ;
	// commaごとにOWSを除き、空tokenを拒否する
	while( offset <= value.size()) {
		const std::size_t	   comma = value.find( ',', offset) ;
		const std::size_t	   end	 = comma == std::string_view::npos ? value.size() : comma ;
		const std::string_view token = TrimOptionalWhitespace( value.substr( offset, end - offset)) ;
		// Connectionで指名されたheader名が不正tokenなら要求を拒否する
		if( !IsToken( token)) {
			return false ;
		}

		tokens.push_back( ToAsciiLower( token)) ;
		// 最後のConnection tokenを処理したら一覧の解析を終える
		if( comma == std::string_view::npos) {
			break ;
		}

		offset = comma + 1U ;
	}

	return true ;
}

/**
 * @brief 固定またはConnection列挙済みhop-by-hop headerか判定する。
 * @param[in] name_lower 小文字header名。
 * @param[in] connection_tokens Connection fieldのtoken。
 * @param[in] preserve_chunk_framing 検証済みchunked framing用のheaderを透過する場合はtrue。
 * @return upstreamへ転送しない場合はtrue。
 */
[[nodiscard]] bool IsHopByHopHeader( const std::string_view			name_lower,
									const std::vector<std::string> &connection_tokens,
									const bool						preserve_chunk_framing) noexcept
{
	// Connectionで指名されても、検証済みchunked codingを次hopのframingとして再送する
	if( preserve_chunk_framing && (name_lower == "transfer-encoding" || name_lower == "trailer")) {
		return false ;
	}

	// このhopだけに属するheaderはupstreamへそのまま転送しない
	if( name_lower == "connection" || name_lower == "keep-alive" ||
	   name_lower == "proxy-authenticate" || name_lower == "proxy-authorization" ||
	   name_lower == "proxy-connection" || name_lower == "te" || name_lower == "trailer" ||
	   name_lower == "transfer-encoding" || name_lower == "upgrade") {
		return true ;
	}

	// Connectionが指名したheaderもhop-by-hopとして除外する
	for( const std::string &token : connection_tokens) {
		// Connectionが指名した追加headerもhop-by-hopとして除外する
		if( name_lower == token) {
			return true ;
		}
	}

	return false ;
}

/**
 * @brief method、request-target、HTTP versionを厳密に分割する。
 * @param[in] line request line。
 * @param[out] method method token。
 * @param[out] request_target request-target。
 * @param[out] version HTTP/1.0またはHTTP/1.1。
 * @return 余分なspaceや制御文字がなく有効ならtrue。
 */
[[nodiscard]] bool ParseRequestLine( const std::string_view line, std::string_view &method,
									std::string_view &request_target,
									std::string_view &version) noexcept
{
	const std::size_t first_space = line.find( ' ') ;
	// methodとtargetを区切れないrequest lineは構文不正として拒否する
	if( first_space == std::string_view::npos) {
		return false ;
	}

	const std::size_t second_space = line.find( ' ', first_space + 1U) ;
	// version区切りの欠落または余分な空白項目を持つlineを拒否する
	if( second_space == std::string_view::npos ||
	   line.find( ' ', second_space + 1U) != std::string_view::npos) {
		return false ;
	}

	method		   = line.substr( 0U, first_space) ;
	request_target = line.substr( first_space + 1U, second_space - first_space - 1U) ;
	version		   = line.substr( second_space + 1U) ;
	// 不正method・空target・対象外HTTP versionでは要求を中継しない
	if( !IsToken( method) || request_target.empty() ||
	   (version != "HTTP/1.1" && version != "HTTP/1.0")) {
		return false ;
	}

	// request-targetにspace、control、DELを許可しない
	for( const unsigned char character : request_target) {
		// request-targetの空白・制御文字は要求分割の曖昧さを防ぐため拒否する
		if( character <= 0x20U || character == 0x7FU) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief request-targetをorigin-formと宛先へ変換する。
 * @param[in] request_target request lineから分離した値。
 * @param[in] host_target 検証済みHost header。
 * @param[out] origin_target upstream request-target。
 * @param[out] target URLまたはHostから得た宛先。
 * @param[out] error 失敗時の固定分類。
 * @return Phase 5対象形式ならtrue。
 */
[[nodiscard]] bool ResolveRequestTarget( const std::string_view		request_target,
										const protocol::OpenTarget &host_target,
										std::string &origin_target, protocol::OpenTarget &target,
										HttpRequestError &error)
{
	error  = HttpRequestError::None ;
	target = host_target ;
	// origin-formとasterisk-formはpathを不要に変更せず維持する
	if( request_target.front() == '/' || request_target == "*") {
		origin_target.assign( request_target) ;
		return true ;
	}

	constexpr std::string_view kHttpPrefix = "http://" ;
	// absolute-formはHTTPだけを受け付け、HTTPS等への転送を開始しない
	if( request_target.size() < kHttpPrefix.size() ||
	   !EqualAsciiCaseInsensitive( request_target.substr( 0U, kHttpPrefix.size()), kHttpPrefix)) {
		error = HttpRequestError::UnsupportedScheme ;
		return false ;
	}

	const std::string_view remainder = request_target.substr( kHttpPrefix.size()) ;
	const std::size_t	   delimiter = remainder.find_first_of( "/?#") ;
	const std::string_view authority =
		delimiter == std::string_view::npos ? remainder : remainder.substr( 0U, delimiter) ;
	// absolute authorityが不正ならorigin-formへの変換を中止する
	if( !ParseAuthority( authority, target, error)) {
		return false ;
	}

	const std::string_view suffix =
		delimiter == std::string_view::npos ? std::string_view{} : remainder.substr( delimiter) ;
	// fragmentはorigin serverへ送信せず、曖昧な切捨ても行わない
	if( suffix.find( '#') != std::string_view::npos) {
		error = HttpRequestError::MalformedRequestLine ;
		return false ;
	}

	// path欠落のHTTP URLはorigin-formのroot pathへ変換する
	if( suffix.empty()) {
		origin_target = "/" ;
	}
	// queryだけのURLはroot pathを補い、query本文を維持する
	else if( suffix.front() == '?') {
		origin_target = "/" ;
		origin_target.append( suffix) ;
	} else {
		origin_target.assign( suffix) ;
	}

	// absolute URIとHostが異なるrequest smugglingを拒否する
	if( target != host_target) {
		error = HttpRequestError::HostMismatch ;
		return false ;
	}

	return true ;
}

/**
 * @brief string byte列をbinary所有vectorへcopyする。
 * @param[in] text 変換後HTTP data。
 * @param[out] output 所有byte列。
 */
void CopyStringToBytes( const std::string_view text, std::vector<std::byte> &output)
{
	output.resize( text.size()) ;
	// HTTP wire byteを文字コード変換せず一対一でcopyする
	for( std::size_t index = 0U; index < text.size(); ++index) {
		output[index] = static_cast<std::byte>( static_cast<unsigned char>( text[index])) ;
	}
}

} // namespace

void HttpRequestTransformer::Reset() noexcept
{
	header_buffer_.clear() ;
	target_				  = protocol::OpenTarget{} ;
	target_locked_		  = false ;
	body_bytes_remaining_ = 0U ;
	parse_state_		  = HttpRequestParseState::Header ;
	chunk_line_buffer_.clear() ;
	chunk_bytes_remaining_ = 0U ;
	chunked_body_bytes_	   = 0U ;
	trailer_buffer_.clear() ;
	request_count_ = 0U ;
	error_		   = HttpRequestError::None ;
}

bool HttpRequestTransformer::Push( const std::span<const std::byte>	   data,
								  std::vector<std::vector<std::byte>> &output) noexcept
{
	// terminal error後のbyteを新request境界として再利用しない
	if( error_ != HttpRequestError::None) {
		return false ;
	}

	// callerがworkerの16 KiB receive境界を迂回しても一回のcopy量を有限にする
	if( data.size() > kMaximumHttpParserInputBytes) {
		Fail( HttpRequestError::ResourceExhausted) ;
		return false ;
	}

	try {
		std::size_t offset			  = 0U ;
		std::size_t headers_processed = 0U ;
		// 一fragment内のheader／body／次headerをwire順に処理する
		while( offset < data.size()) {
			// 固定長body中はheader解析を行わず残量内のdataをstreaming転送する
			if( parse_state_ == HttpRequestParseState::FixedBody) {
				const std::size_t body_size = static_cast<std::size_t>(
					std::min<std::uint64_t>( body_bytes_remaining_, data.size() - offset)) ;
				std::vector<std::byte> body( data.begin() + static_cast<std::ptrdiff_t>( offset),
											data.begin() +
												static_cast<std::ptrdiff_t>( offset + body_size)) ;
				output.push_back( std::move( body)) ;
				body_bytes_remaining_ -= static_cast<std::uint64_t>( body_size) ;
				offset += body_size ;
				// Content-Lengthを完全消費したbyteの次を新request headerとする
				if( body_bytes_remaining_ == 0U) {
					parse_state_ = HttpRequestParseState::Header ;
				}

				continue ;
			}

			// chunk data中は指定chunk残量内のbinary dataだけを転送する
			if( parse_state_ == HttpRequestParseState::ChunkData) {
				const std::size_t chunk_size = static_cast<std::size_t>(
					std::min<std::uint64_t>( chunk_bytes_remaining_, data.size() - offset)) ;
				std::vector<std::byte> chunk( data.begin() + static_cast<std::ptrdiff_t>( offset),
											 data.begin() +
												 static_cast<std::ptrdiff_t>( offset + chunk_size)) ;
				output.push_back( std::move( chunk)) ;
				chunk_bytes_remaining_ -= static_cast<std::uint64_t>( chunk_size) ;
				offset += chunk_size ;
				// chunk data直後のCRLFをdataの一部として明示検証する
				if( chunk_bytes_remaining_ == 0U) {
					chunk_line_buffer_.clear() ;
					parse_state_ = HttpRequestParseState::ChunkTerminator ;
				}

				continue ;
			}

			// chunk data後はfragmentを跨いでもCRLF二byteを検証する
			if( parse_state_ == HttpRequestParseState::ChunkTerminator) {
				// fragmentationに備えCRLFの二byteだけを有限保持する
				while( offset < data.size() && chunk_line_buffer_.size() < 2U) {
					chunk_line_buffer_.push_back( data[offset]) ;
					++offset ;
				}

				// chunk末尾が未完成なら次のfragmentまで解析を保留する
				if( chunk_line_buffer_.size() < 2U) {
					break ;
				}

				// chunk末尾がCRLFでなければ境界を推測せず要求を拒否する
				if( chunk_line_buffer_[0] != std::byte{'\r'} ||
				   chunk_line_buffer_[1] != std::byte{'\n'}) {
					Fail( HttpRequestError::InvalidChunkTerminator) ;
					return false ;
				}

				output.push_back( chunk_line_buffer_) ;
				chunk_line_buffer_.clear() ;
				parse_state_ = HttpRequestParseState::ChunkSize ;
				continue ;
			}

			// chunk長行の状態では完全なCRLF行まで有限bufferへ収集する
			if( parse_state_ == HttpRequestParseState::ChunkSize) {
				bool line_complete = false ;
				// size lineのCRLFまでを保持し、検証前にupstreamへ送らない
				while( offset < data.size()) {
					// chunk長行の上限超過ではbufferを増やさず要求を拒否する
					if( chunk_line_buffer_.size() >= kMaximumHttpChunkLineBytes) {
						Fail( HttpRequestError::ChunkLineTooLarge) ;
						return false ;
					}

					chunk_line_buffer_.push_back( data[offset]) ;
					++offset ;
					line_complete =
						chunk_line_buffer_.size() >= 2U &&
						chunk_line_buffer_[chunk_line_buffer_.size() - 2U] == std::byte{'\r'} &&
						chunk_line_buffer_.back() == std::byte{'\n'} ;
					// CRLFを取得したらchunk長行の収集を終えて検証へ進む
					if( line_complete) {
						break ;
					}
				}

				// chunk長行が未完成なら後続fragmentを待つ
				if( !line_complete) {
					break ;
				}

				std::vector<std::byte> line ;
				// chunk長と拡張の検証が失敗した場合は未検証行をupstreamへ送らない
				if( !CompleteChunkSizeLine( line)) {
					return false ;
				}

				output.push_back( std::move( line)) ;
				continue ;
			}

			// zero chunk後はtrailer全体の終端まで有限bufferへ収集する
			if( parse_state_ == HttpRequestParseState::ChunkTrailers) {
				bool trailers_complete = false ;
				// zero chunk後のtrailer終端までだけを有限bufferへ保持する
				while( offset < data.size()) {
					// trailer上限超過はbody全体を保持せず要求を拒否する
					if( trailer_buffer_.size() >= kMaximumHttpTrailerBytes) {
						Fail( HttpRequestError::TrailerTooLarge) ;
						return false ;
					}

					trailer_buffer_.push_back( data[offset]) ;
					++offset ;
					trailers_complete =
						(trailer_buffer_.size() == 2U && trailer_buffer_[0] == std::byte{'\r'} &&
						 trailer_buffer_[1] == std::byte{'\n'}) ||
						(trailer_buffer_.size() >= 4U &&
						 std::equal( kHeaderTerminator.begin(), kHeaderTerminator.end(),
									trailer_buffer_.end() -
										static_cast<std::ptrdiff_t>( kHeaderTerminator.size()))) ;
					// trailer終端を取得したら収集を止めてfield検証へ進む
					if( trailers_complete) {
						break ;
					}
				}

				// trailer未完成の間は次のHTTP要求として解釈しない
				if( !trailers_complete) {
					break ;
				}

				std::vector<std::byte> trailers ;
				// trailer検証に失敗した場合は不正fieldをupstreamへ送らない
				if( !CompleteChunkTrailers( trailers)) {
					return false ;
				}

				output.push_back( std::move( trailers)) ;
				continue ;
			}

			// Header以外の未知stateを別requestとして近似処理しない
			if( parse_state_ != HttpRequestParseState::Header) {
				Fail( HttpRequestError::ResourceExhausted) ;
				return false ;
			}

			// header terminatorを検出するまで一byteずつ有限bufferへ追加する
			while( offset < data.size()) {
				// 上限を越えるheaderを追加前に拒否する
				if( header_buffer_.size() >= kMaximumHttpRequestHeaderBytes) {
					Fail( HttpRequestError::HeaderTooLarge) ;
					return false ;
				}

				header_buffer_.push_back( data[offset]) ;
				++offset ;
				const bool has_terminator =
					header_buffer_.size() >= kHeaderTerminator.size() &&
					std::equal( kHeaderTerminator.begin(), kHeaderTerminator.end(),
							   header_buffer_.end() -
								   static_cast<std::ptrdiff_t>( kHeaderTerminator.size())) ;
				// header終端へ達したら追加bodyをheader bufferへ取り込まない
				if( has_terminator) {
					break ;
				}
			}

			const bool header_complete =
				header_buffer_.size() >= kHeaderTerminator.size() &&
				std::equal( kHeaderTerminator.begin(), kHeaderTerminator.end(),
						   header_buffer_.end() -
							   static_cast<std::ptrdiff_t>( kHeaderTerminator.size())) ;
			// header未完成の間は解析結果を公開せず次のfragmentを待つ
			if( !header_complete) {
				break ;
			}

			// 一fragmentで無制限の空body requestを処理し続けない
			if( headers_processed >= kMaximumHttpRequestsPerPush) {
				Fail( HttpRequestError::ResourceExhausted) ;
				return false ;
			}

			std::vector<std::byte> transformed ;
			// 完全headerの検証・宛先変換に失敗した場合は要求を転送しない
			if( !TransformHeader( transformed)) {
				return false ;
			}

			output.push_back( std::move( transformed)) ;
			header_buffer_.clear() ;
			++headers_processed ;
			++request_count_ ;
		}
	} catch( const std::bad_alloc &) {
		Fail( HttpRequestError::ResourceExhausted) ;
		return false ;
	} catch( ...) {
		Fail( HttpRequestError::ResourceExhausted) ;
		return false ;
	}

	return true ;
}

bool HttpRequestTransformer::target( protocol::OpenTarget &target) const noexcept
{
	// 初回の有効header前は未確定targetを呼出し側へ返さない
	if( !target_locked_) {
		return false ;
	}

	target = target_ ;
	return true ;
}

HttpRequestParserSnapshot HttpRequestTransformer::snapshot() const noexcept
{
	return HttpRequestParserSnapshot{
		target_locked_,
		parse_state_ == HttpRequestParseState::Header && header_buffer_.empty() &&
			body_bytes_remaining_ == 0U && chunk_line_buffer_.empty() &&
			chunk_bytes_remaining_ == 0U && trailer_buffer_.empty(),
		parse_state_ == HttpRequestParseState::FixedBody ? body_bytes_remaining_
														 : chunk_bytes_remaining_,
		parse_state_,
		parse_state_ == HttpRequestParseState::ChunkSize ||
			parse_state_ == HttpRequestParseState::ChunkData ||
			parse_state_ == HttpRequestParseState::ChunkTerminator ||
			parse_state_ == HttpRequestParseState::ChunkTrailers,
		request_count_,
		error_,
	} ;
}

bool HttpRequestTransformer::TransformHeader( std::vector<std::byte> &transformed) noexcept
{
	try {
		std::string header ;
		header.resize( header_buffer_.size()) ;
		// binary inputをHTTP ASCII／opaque field byteとして一対一copyする
		for( std::size_t index = 0U; index < header_buffer_.size(); ++index) {
			header[index] =
				static_cast<char>( std::to_integer<unsigned char>( header_buffer_[index])) ;
		}

		std::vector<std::string_view> lines ;
		// header行分割に失敗または先頭lineがない場合は構文不正で停止する
		if( !SplitHeaderLines( header, lines) || lines.empty()) {
			Fail( HttpRequestError::MalformedRequestLine) ;
			return false ;
		}

		std::string_view method ;
		std::string_view request_target ;
		std::string_view version ;
		// method・target・versionを検証できなければ要求の解析を中止する
		if( !ParseRequestLine( lines.front(), method, request_target, version)) {
			Fail( HttpRequestError::MalformedRequestLine) ;
			return false ;
		}

		// CONNECTはauthority-formを任意TCP tunnelへ変換するため常に拒否する
		if( EqualAsciiCaseInsensitive( method, "CONNECT")) {
			Fail( HttpRequestError::ConnectNotAllowed) ;
			return false ;
		}

		std::vector<HeaderLine> headers ;
		headers.reserve( lines.size() - 1U) ;
		std::vector<std::string> connection_tokens ;
		bool					 host_seen				= false ;
		bool					 content_length_seen	= false ;
		bool					 transfer_encoding_seen = false ;
		bool					 chunked				= false ;
		protocol::OpenTarget	 host_target{} ;
		std::uint64_t			 content_length = 0U ;

		// request line以降の全headerを先に検証し、除外対象を後から決定する
		for( std::size_t index = 1U; index < lines.size(); ++index) {
			const std::string_view line = lines[index] ;
			// obs-foldは複数解釈を生むためPhase 5でも受け付けない
			if( line.empty() || line.front() == ' ' || line.front() == '\t') {
				Fail( HttpRequestError::MalformedHeader) ;
				return false ;
			}

			const std::size_t colon = line.find( ':') ;
			// colon欠落または不正header名は要求解釈の差を防ぐため拒否する
			if( colon == std::string_view::npos || !IsToken( line.substr( 0U, colon))) {
				Fail( HttpRequestError::MalformedHeader) ;
				return false ;
			}

			const std::string_view raw_value = line.substr( colon + 1U) ;
			// 制御文字を含むheader値は透過対象として受理しない
			if( !IsValidHeaderValue( raw_value)) {
				Fail( HttpRequestError::MalformedHeader) ;
				return false ;
			}

			HeaderLine parsed{} ;
			parsed.name_lower = ToAsciiLower( line.substr( 0U, colon)) ;
			parsed.raw.assign( line) ;
			parsed.value.assign( TrimOptionalWhitespace( raw_value)) ;

			// Host fieldでは宛先の重複・形式を検証する
			if( parsed.name_lower == "host") {
				// duplicate Hostを一つに近似せずrequest smugglingを拒否する
				if( host_seen) {
					Fail( HttpRequestError::DuplicateHost) ;
					return false ;
				}

				HttpRequestError authority_error = HttpRequestError::None ;
				// Host authorityが不正ならそのerror分類で要求を拒否する
				if( !ParseAuthority( parsed.value, host_target, authority_error)) {
					Fail( authority_error) ;
					return false ;
				}

				host_seen = true ;
			}
			// Content-Length fieldでは一意なbody長を確定する
			else if( parsed.name_lower == "content-length") {
				// duplicate Content-Lengthは同値でも複数parser解釈を避けるため拒否する
				if( content_length_seen) {
					Fail( HttpRequestError::DuplicateContentLength) ;
					return false ;
				}

				// 不正またはbody上限を越えるContent-Lengthは要求を拒否する
				if( !ParseUnsignedDecimal( parsed.value, kMaximumHttpRequestBodyBytes,
										 content_length)) {
					Fail( HttpRequestError::InvalidContentLength) ;
					return false ;
				}

				content_length_seen = true ;
			}
			// Transfer-Encoding fieldではcodingの一意性と対象範囲を検証する
			else if( parsed.name_lower == "transfer-encoding") {
				// 複数fieldや複数codingを曖昧なparser差へ持ち込まない
				if( transfer_encoding_seen) {
					Fail( HttpRequestError::InvalidTransferEncoding) ;
					return false ;
				}

				transfer_encoding_seen = true ;
				chunked				   = EqualAsciiCaseInsensitive( parsed.value, "chunked") ;
				// Phase 5は検証可能な単一chunked codingだけを再送する
				if( !chunked) {
					Fail( HttpRequestError::TransferEncodingNotSupported) ;
					return false ;
				}
			}
			// Upgrade fieldによるprotocol切替は初期製品の対象外として拒否する
			else if( parsed.name_lower == "upgrade") {
				Fail( HttpRequestError::UpgradeNotAllowed) ;
				return false ;
			}
			// Connection fieldではこのhop固有headerの除外対象を収集する
			else if( parsed.name_lower == "connection") {
				// Connectionのtoken構文が不正なら曖昧なhop-by-hop解釈を許さない
				if( !ParseConnectionTokens( parsed.value, connection_tokens)) {
					Fail( HttpRequestError::MalformedHeader) ;
					return false ;
				}
			}

			headers.push_back( std::move( parsed)) ;
		}

		// 二つのmessage framingを同時指定したrequest smugglingを拒否する
		if( content_length_seen && transfer_encoding_seen) {
			Fail( HttpRequestError::ConflictingMessageFraming) ;
			return false ;
		}

		// HTTP/1.0でも接続先決定にHostを必須とし、任意fallbackを行わない
		if( !host_seen) {
			Fail( HttpRequestError::MissingHost) ;
			return false ;
		}

		// Connectionが宛先・固定長framingを除去する指定やprotocol切替を要求したら拒否する
		for( const std::string &token : connection_tokens) {
			// HostやContent-Lengthを落としたまま元の宛先・body処理を継続しない
			if( token == "host" || token == "content-length") {
				Fail( HttpRequestError::MalformedHeader) ;
				return false ;
			}

			// ConnectionでUpgradeが指名された場合もprotocol切替を拒否する
			if( token == "upgrade") {
				Fail( HttpRequestError::UpgradeNotAllowed) ;
				return false ;
			}
		}

		std::string			 origin_target ;
		protocol::OpenTarget resolved_target{} ;
		HttpRequestError	 target_error = HttpRequestError::None ;
		// request-targetとHostを解決・照合できなければupstream宛先を採用しない
		if( !ResolveRequestTarget( request_target, host_target, origin_target, resolved_target,
								 target_error)) {
			Fail( target_error) ;
			return false ;
		}

		// Keep-Alive中は最初のupstream Socket以外へrequestをrouteしない
		if( target_locked_ && resolved_target != target_) {
			Fail( HttpRequestError::DestinationChanged) ;
			return false ;
		}

		std::string output ;
		output.reserve( header.size()) ;
		output.append( method) ;
		output.push_back( ' ') ;
		output.append( origin_target) ;
		output.push_back( ' ') ;
		output.append( version) ;
		output.append( "\r\n") ;
		// end-to-end headerの元byte列を維持し、hop-by-hopだけを除く
		for( const HeaderLine &parsed : headers) {
			// end-to-end headerだけを原文のままorigin requestへ残す
			if( !IsHopByHopHeader( parsed.name_lower, connection_tokens, chunked)) {
				output.append( parsed.raw) ;
				output.append( "\r\n") ;
			}
		}

		output.append( "\r\n") ;
		CopyStringToBytes( output, transformed) ;
		// 最初の有効requestでだけconnectionの宛先IPv4・portを固定する
		if( !target_locked_) {
			target_		   = resolved_target ;
			target_locked_ = true ;
		}

		body_bytes_remaining_ = content_length ;
		chunk_line_buffer_.clear() ;
		chunk_bytes_remaining_ = 0U ;
		chunked_body_bytes_	   = 0U ;
		trailer_buffer_.clear() ;
		// chunked要求では固定長bodyではなくchunk長解析へ遷移する
		if( chunked) {
			parse_state_ = HttpRequestParseState::ChunkSize ;
		}
		// 非空Content-Length bodyは残量を計測するstreaming状態へ遷移する
		else if( content_length > 0U) {
			parse_state_ = HttpRequestParseState::FixedBody ;
		} else {
			parse_state_ = HttpRequestParseState::Header ;
		}

		return true ;
	} catch( const std::bad_alloc &) {
		Fail( HttpRequestError::ResourceExhausted) ;
		return false ;
	} catch( ...) {
		Fail( HttpRequestError::ResourceExhausted) ;
		return false ;
	}
}

bool HttpRequestTransformer::CompleteChunkSizeLine( std::vector<std::byte> &output) noexcept
{
	try {
		// 最小の`0\r\n`より短いlineをsizeとして解釈しない
		if( chunk_line_buffer_.size() < 3U ||
		   chunk_line_buffer_[chunk_line_buffer_.size() - 2U] != std::byte{'\r'} ||
		   chunk_line_buffer_.back() != std::byte{'\n'}) {
			Fail( HttpRequestError::InvalidChunkSize) ;
			return false ;
		}

		std::string line ;
		line.resize( chunk_line_buffer_.size() - 2U) ;
		// chunk metadataは文字コード変換せずASCII構文だけを検証する
		for( std::size_t index = 0U; index < line.size(); ++index) {
			line[index] =
				static_cast<char>( std::to_integer<unsigned char>( chunk_line_buffer_[index])) ;
		}

		const std::size_t	   semicolon = line.find( ';') ;
		const std::string_view size_text = semicolon == std::string::npos
											   ? std::string_view( line)
											   : std::string_view( line).substr( 0U, semicolon) ;
		// extensionはopaqueのまま再送するが、空extensionとcontrolを拒否する
		if( semicolon != std::string::npos &&
		   (semicolon + 1U == line.size() ||
			!IsValidChunkExtension( std::string_view( line).substr( semicolon + 1U)))) {
			Fail( HttpRequestError::InvalidChunkSize) ;
			return false ;
		}

		std::uint64_t chunk_size = 0U ;
		// chunk長が不正または累積body上限を越える場合は要求を拒否する
		if( !ParseChunkSize( size_text, chunk_size) ||
		   chunk_size > kMaximumHttpRequestBodyBytes - chunked_body_bytes_) {
			Fail( HttpRequestError::InvalidChunkSize) ;
			return false ;
		}

		output = std::move( chunk_line_buffer_) ;
		chunk_line_buffer_.clear() ;
		chunk_bytes_remaining_ = chunk_size ;
		// zero chunkはbody終了としてtrailer検証へ遷移する
		if( chunk_size == 0U) {
			trailer_buffer_.clear() ;
			parse_state_ = HttpRequestParseState::ChunkTrailers ;
		} else {
			chunked_body_bytes_ += chunk_size ;
			parse_state_ = HttpRequestParseState::ChunkData ;
		}

		return true ;
	} catch( const std::bad_alloc &) {
		Fail( HttpRequestError::ResourceExhausted) ;
		return false ;
	} catch( ...) {
		Fail( HttpRequestError::ResourceExhausted) ;
		return false ;
	}
}

bool HttpRequestTransformer::CompleteChunkTrailers( std::vector<std::byte> &output) noexcept
{
	try {
		std::string trailers ;
		trailers.resize( trailer_buffer_.size()) ;
		// trailerは保存やlog出力をせず、構文検証中の一時所有値だけを作る
		for( std::size_t index = 0U; index < trailer_buffer_.size(); ++index) {
			trailers[index] =
				static_cast<char>( std::to_integer<unsigned char>( trailer_buffer_[index])) ;
		}

		// 空trailer以外はfield構文と転送禁止headerを検証する
		if( trailers != "\r\n") {
			std::vector<std::string_view> lines ;
			// trailer行分割失敗または空の解析結果は構文不正として拒否する
			if( !SplitHeaderLines( trailers, lines) || lines.empty()) {
				Fail( HttpRequestError::MalformedTrailer) ;
				return false ;
			}

			const std::vector<std::string> no_connection_tokens ;
			// framingやroutingを変更しないend-to-end trailerだけを許可する
			for( const std::string_view line : lines) {
				// 空fieldや折返し形式のtrailerを受理せず解釈の曖昧さを防ぐ
				if( line.empty() || line.front() == ' ' || line.front() == '\t') {
					Fail( HttpRequestError::MalformedTrailer) ;
					return false ;
				}

				const std::size_t colon = line.find( ':') ;
				// trailer名・colon・値が不正なら原文の転送を行わない
				if( colon == std::string_view::npos || !IsToken( line.substr( 0U, colon)) ||
				   !IsValidHeaderValue( line.substr( colon + 1U))) {
					Fail( HttpRequestError::MalformedTrailer) ;
					return false ;
				}

				const std::string name_lower = ToAsciiLower( line.substr( 0U, colon)) ;
				// trailerからmessage framing、routing、hop-by-hop stateを変更させない
				if( name_lower == "host" || name_lower == "content-length" ||
				   IsHopByHopHeader( name_lower, no_connection_tokens, false)) {
					Fail( HttpRequestError::MalformedTrailer) ;
					return false ;
				}
			}
		}

		output = std::move( trailer_buffer_) ;
		trailer_buffer_.clear() ;
		chunk_line_buffer_.clear() ;
		chunk_bytes_remaining_ = 0U ;
		chunked_body_bytes_	   = 0U ;
		body_bytes_remaining_  = 0U ;
		parse_state_		   = HttpRequestParseState::Header ;
		return true ;
	} catch( const std::bad_alloc &) {
		Fail( HttpRequestError::ResourceExhausted) ;
		return false ;
	} catch( ...) {
		Fail( HttpRequestError::ResourceExhausted) ;
		return false ;
	}
}

void HttpRequestTransformer::Fail( const HttpRequestError error) noexcept
{
	error_ = error ;
	header_buffer_.clear() ;
	body_bytes_remaining_ = 0U ;
	parse_state_		  = HttpRequestParseState::Header ;
	chunk_line_buffer_.clear() ;
	chunk_bytes_remaining_ = 0U ;
	chunked_body_bytes_	   = 0U ;
	trailer_buffer_.clear() ;
}

} // namespace zrdp::client::proxy
