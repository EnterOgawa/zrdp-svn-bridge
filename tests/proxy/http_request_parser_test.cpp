//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file http_request_parser_test.cpp
 * @brief Phase 5 HTTP request変換、chunked framing、認証header透過を検証する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details 文書用IPv4 literalだけを使用し、Socket、DNS、実SVNへ接続しない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Connection指名の拒否と変換後の要求境界を検証。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/proxy/http_request_parser.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zrdp::client::proxy::HttpRequestError ;
using zrdp::client::proxy::HttpRequestTransformer ;

/**
 * @brief ASCII test dataをbinary spanとして返す。
 * @param[in] text test request。
 * @return 同一memoryを参照するbyte span。
 */
[[nodiscard]] std::span<const std::byte> Bytes( const std::string_view text) noexcept
{
	return std::span<const std::byte>( reinterpret_cast<const std::byte *>( text.data()),
									  text.size()) ;
}

/**
 * @brief parser出力chunkを一つのtest文字列へ連結する。
 * @param[in] chunks parserが生成したwire chunk列。
 * @return byte順を維持した文字列。
 */
[[nodiscard]] std::string Join( const std::vector<std::vector<std::byte>> &chunks)
{
	std::string result ;
	// headerとbodyの出力境界に依存せずwire順で連結する
	for( const std::vector<std::byte> &chunk : chunks) {
		result.append( reinterpret_cast<const char *>( chunk.data()), chunk.size()) ;
	}

	return result ;
}

/**
 * @brief 一requestが期待した固定errorで拒否されることを確認する。
 * @param[in] request test request。
 * @param[in] expected_error 期待分類。
 * @return Push失敗とerror一致の両方を満たす場合はtrue。
 */
[[nodiscard]] bool ExpectError( const std::string_view request,
							   const HttpRequestError expected_error)
{
	HttpRequestTransformer				parser ;
	std::vector<std::vector<std::byte>> output ;
	return !parser.Push( Bytes( request), output) && parser.snapshot().error == expected_error ;
}

/**
 * @brief 不正Connection要求を全二分割位置で拒否し、部分出力も残さないことを検証する。
 * @param[in] request 文書用宛先だけを使用する不正要求。
 * @return 各分割で固定error、出力なし、宛先未確定、後続要求の拒否を満たす場合はtrue。
 */
[[nodiscard]] bool ExpectConnectionRejected( const std::string_view request)
{
	// header、終端CRLF、bodyを跨ぐすべての二分割位置で同じ拒否結果を得る
	for( std::size_t split = 0U; split <= request.size(); ++split) {
		HttpRequestTransformer				parser ;
		std::vector<std::vector<std::byte>> output ;
		bool								accepted = true ;
		// 空のprefixではparserを呼ばず、先頭からの一括入力も含める
		if( split > 0U) {
			accepted = parser.Push( Bytes( request.substr( 0U, split)), output) ;
		}

		// prefixで拒否済みなら後続bodyを与えず、未完成なら残りを渡す
		if( accepted && split < request.size()) {
			accepted = parser.Push( Bytes( request.substr( split)), output) ;
		}

		zrdp::protocol::OpenTarget target{} ;
		const auto				   snapshot = parser.snapshot() ;
		// 宛先確定と要求出力より前でheaderを拒否し、要求数も増やさない
		if( accepted || snapshot.error != HttpRequestError::MalformedHeader ||
		   snapshot.target_locked || snapshot.request_count != 0U ||
		   snapshot.body_bytes_remaining != 0U || !output.empty() || parser.target( target)) {
			return false ;
		}

		// 拒否後は正常要求でも同じparserを再利用して転送を再開しない
		if( parser.Push( Bytes( "GET /repo HTTP/1.1\r\nHost: 192.0.2.10\r\n\r\n"), output) ||
		   parser.snapshot().error != HttpRequestError::MalformedHeader || !output.empty()) {
			return false ;
		}
	}

	HttpRequestTransformer				bytewise ;
	std::vector<std::vector<std::byte>> output ;
	bool								rejected = false ;
	// 多数のcallbackへ分割されたheaderでも、拒否まで一byteも出力しない
	for( const char &value : request) {
		const bool accepted = bytewise.Push( Bytes( std::string_view( &value, 1U)), output) ;
		// header途中で部分的な出力を生成していたら検証を失敗にする
		if( !output.empty()) {
			return false ;
		}

		// 完全headerで拒否されたらbodyを与えずterminal状態を確認する
		if( !accepted) {
			rejected = true ;
			break ;
		}
	}

	return rejected && bytewise.snapshot().error == HttpRequestError::MalformedHeader &&
		   !bytewise.snapshot().target_locked && bytewise.snapshot().request_count == 0U ;
}

/**
 * @brief 正常要求を全二分割位置で変換し、wire byteと前後の要求数・body境界を検証する。
 * @param[in] request 変換前の正常要求列。
 * @param[in] expected 変換後に期待するheaderとbodyの全byte列。
 * @param[in] expected_requests 完了した要求数。
 * @return byte列と再解析後の要求境界が一致する場合はtrue。
 */
[[nodiscard]] bool ExpectPreservedBoundaries( const std::string_view request,
											 const std::string_view expected,
											 const std::uint64_t	expected_requests)
{
	// 正常header、body、chunk framing、次要求を跨ぐ各分割でも境界を維持する
	for( std::size_t split = 0U; split <= request.size(); ++split) {
		HttpRequestTransformer				before ;
		std::vector<std::vector<std::byte>> output ;
		// 非空prefixは完全headerやbody途中のどこで終わっても受理する
		if( split > 0U && !before.Push( Bytes( request.substr( 0U, split)), output)) {
			return false ;
		}

		// 残りを入力し、bodyを含む期待wireとの完全一致を確認する
		if( (split < request.size() && !before.Push( Bytes( request.substr( split)), output)) ||
		   Join( output) != expected || before.snapshot().request_count != expected_requests ||
		   !before.snapshot().at_request_boundary || before.snapshot().body_bytes_remaining != 0U) {
			return false ;
		}
	}

	HttpRequestTransformer				after ;
	std::vector<std::vector<std::byte>> reparsed ;
	return after.Push( Bytes( expected), reparsed) && Join( reparsed) == expected &&
		   after.snapshot().request_count == expected_requests &&
		   after.snapshot().at_request_boundary && after.snapshot().body_bytes_remaining == 0U ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief HTTP request parserの変換、境界、malformed入力拒否を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int main()
{
	using zrdp::protocol::OpenTarget ;

	HttpRequestTransformer				parser ;
	std::vector<std::vector<std::byte>> output ;
	constexpr std::string_view			first_part =
		"PROPFIND http://192.0.2.10:8080/svn/ProjectA?x=1 HTTP/1.1\r\n"
		"Ho" ;
	constexpr std::string_view second_part =
		"st: 192.0.2.10:8080\r\n"
		"Depth: 1\r\n"
		"Proxy-Connection: keep-alive\r\n"
		"Proxy-Authorization: Basic secret-must-not-forward\r\n"
		"Connection: keep-alive, X-Hop\r\n"
		"X-Hop: remove-me\r\n"
		"Authorization: Basic opaque-end-to-end\r\n"
		"Content-Length: 5\r\n\r\nab" ;
	constexpr std::string_view third_part = "cde" ;
	// header fragmentation中は部分requestをupstreamへ公開しない
	if( !parser.Push( Bytes( first_part), output) || !output.empty() ||
	   !parser.Push( Bytes( second_part), output) || !parser.Push( Bytes( third_part), output)) {
		return 1 ;
	}

	const std::string		   transformed = Join( output) ;
	constexpr std::string_view expected	   = "PROPFIND /svn/ProjectA?x=1 HTTP/1.1\r\n"
											 "Host: 192.0.2.10:8080\r\n"
											 "Depth: 1\r\n"
											 "Authorization: Basic opaque-end-to-end\r\n"
											 "Content-Length: 5\r\n\r\nabcde" ;
	// absolute-formからorigin-formへの変換結果が期待byte列と一致することを確認する
	if( transformed != expected) {
		return 2 ;
	}

	OpenTarget target{} ;
	const auto snapshot = parser.snapshot() ;
	// 初回target固定・request境界・body残量・件数の整合性を確認する
	if( !parser.target( target) || target != OpenTarget{{192U, 0U, 2U, 10U}, 8080U} ||
	   !snapshot.target_locked || !snapshot.at_request_boundary ||
	   snapshot.body_bytes_remaining != 0U || snapshot.request_count != 1U) {
		return 3 ;
	}

	output.clear() ;
	constexpr std::string_view keep_alive = "GET /svn/ProjectA HTTP/1.1\r\n"
											"Host: 192.0.2.10:8080\r\n\r\n" ;
	// 同じauthorityのKeep-Alive要求は不要に変更せず再利用できることを確認する
	if( !parser.Push( Bytes( keep_alive), output) || Join( output) != keep_alive ||
	   parser.snapshot().request_count != 2U) {
		return 4 ;
	}

	// 同一local connection内の別IPv4またはportへの切替を拒否する
	output.clear() ;
	constexpr std::string_view changed_destination = "GET http://192.0.2.11/svn/Other HTTP/1.1\r\n"
													 "Host: 192.0.2.11\r\n\r\n" ;
	// Keep-Alive中の宛先変更が固定DestinationChangedで拒否されることを確認する
	if( parser.Push( Bytes( changed_destination), output) ||
	   parser.snapshot().error != HttpRequestError::DestinationChanged) {
		return 5 ;
	}

	HttpRequestTransformer pipeline ;
	output.clear() ;
	constexpr std::string_view pipelined		 = "GET http://198.51.100.20/a HTTP/1.1\r\n"
												   "Host: 198.51.100.20\r\n\r\n"
												   "HEAD http://198.51.100.20/b HTTP/1.1\r\n"
												   "Host: 198.51.100.20:80\r\n\r\n" ;
	constexpr std::string_view pipeline_expected = "GET /a HTTP/1.1\r\n"
												   "Host: 198.51.100.20\r\n\r\n"
												   "HEAD /b HTTP/1.1\r\n"
												   "Host: 198.51.100.20:80\r\n\r\n" ;
	// pipelineの二要求が同じ宛先で順序どおり変換されることを確認する
	if( !pipeline.Push( Bytes( pipelined), output) || Join( output) != pipeline_expected ||
	   pipeline.snapshot().request_count != 2U) {
		return 6 ;
	}

	// queryだけのabsolute targetはorigin-formの`/?...`へ変換する
	HttpRequestTransformer query_parser ;
	output.clear() ;
	constexpr std::string_view query_only	  = "OPTIONS http://203.0.113.30?view=1 HTTP/1.1\r\n"
												"Host: 203.0.113.30\r\n\r\n" ;
	constexpr std::string_view query_expected = "OPTIONS /?view=1 HTTP/1.1\r\n"
												"Host: 203.0.113.30\r\n\r\n" ;
	// queryのみのabsolute URLでroot pathが正しく補われることを確認する
	if( !query_parser.Push( Bytes( query_only), output) || Join( output) != query_expected) {
		return 7 ;
	}

	HttpRequestTransformer chunked_parser ;
	output.clear() ;
	constexpr std::string_view chunked_first	= "REPORT http://203.0.113.40/svn/P HTTP/1.1\r\n"
												  "Host: 203.0.113.40\r\n"
												  "Transfer-Encoding: chunked\r\n"
												  "Trailer: X-Checksum\r\n"
												  "Expect: 100-continue\r\n"
												  "Authorization: NTLM opaque-token\r\n"
												  "Cookie: opaque-cookie\r\n"
												  "Connection: keep-alive\r\n\r\n"
												  "4;ext=1\r" ;
	constexpr std::string_view chunked_second	= "\nWi" ;
	constexpr std::string_view chunked_third	= "ki\r\n5\r\npedia\r\n"
												  "0\r\nX-Checksum: opaque\r\n\r\n" ;
	constexpr std::string_view chunked_expected = "REPORT /svn/P HTTP/1.1\r\n"
												  "Host: 203.0.113.40\r\n"
												  "Transfer-Encoding: chunked\r\n"
												  "Trailer: X-Checksum\r\n"
												  "Expect: 100-continue\r\n"
												  "Authorization: NTLM opaque-token\r\n"
												  "Cookie: opaque-cookie\r\n\r\n"
												  "4;ext=1\r\nWiki\r\n5\r\npedia\r\n"
												  "0\r\nX-Checksum: opaque\r\n\r\n" ;
	// size、data、CRLF、trailerを跨ぐfragmentでもwire byteを維持する
	if( !chunked_parser.Push( Bytes( chunked_first), output) ||
	   !chunked_parser.Push( Bytes( chunked_second), output) ||
	   !chunked_parser.Push( Bytes( chunked_third), output) || Join( output) != chunked_expected ||
	   !chunked_parser.snapshot().at_request_boundary || chunked_parser.snapshot().chunked ||
	   chunked_parser.snapshot().request_count != 1U) {
		return 8 ;
	}

	// chunked完了後も同じupstream targetのKeep-Alive requestを受理する
	output.clear() ;
	constexpr std::string_view after_chunked = "PROPFIND /svn/P HTTP/1.1\r\n"
											   "Host: 203.0.113.40\r\n"
											   "Depth: 1\r\n\r\n" ;
	// chunked完了後に次のKeep-Alive要求を解析できることを確認する
	if( !chunked_parser.Push( Bytes( after_chunked), output) || Join( output) != after_chunked ||
	   chunked_parser.snapshot().request_count != 2U) {
		return 9 ;
	}

	// authority／Host、request line、禁止protocolを固定分類で拒否する
	if( !ExpectError( "GET / HTTP/1.1\r\n\r\n", HttpRequestError::MissingHost) ||
	   !ExpectError( "GET http://192.0.2.1/a HTTP/1.1\r\n"
					"Host: 192.0.2.2\r\n\r\n",
					HttpRequestError::HostMismatch) ||
	   !ExpectError( "GET / HTTP/1.1\r\n"
					"Host: svn.example.invalid\r\n\r\n",
					HttpRequestError::InvalidIpv4) ||
	   !ExpectError( "GET / HTTP/1.1\r\n"
					"Host: [::1]\r\n\r\n",
					HttpRequestError::InvalidIpv4) ||
	   !ExpectError( "GET / HTTP/1.1\r\n"
					"Host: 192.0.2.1:0\r\n\r\n",
					HttpRequestError::InvalidPort) ||
	   !ExpectError( "CONNECT 192.0.2.1:80 HTTP/1.1\r\n"
					"Host: 192.0.2.1\r\n\r\n",
					HttpRequestError::ConnectNotAllowed) ||
	   !ExpectError( "GET https://192.0.2.1/a HTTP/1.1\r\n"
					"Host: 192.0.2.1\r\n\r\n",
					HttpRequestError::UnsupportedScheme) ||
	   !ExpectError( "GET / HTTP/1.1\r\n"
					"Host: 192.0.2.1\r\n"
					"Connection: Upgrade\r\n\r\n",
					HttpRequestError::UpgradeNotAllowed) ||
	   !ExpectError( "POST / HTTP/1.1\r\n"
					"Host: 192.0.2.1\r\n"
					"Transfer-Encoding: gzip\r\n\r\n",
					HttpRequestError::TransferEncodingNotSupported) ||
	   !ExpectError( "POST / HTTP/1.1\r\n"
					"Host: 192.0.2.1\r\n"
					"Content-Length: 1\r\n"
					"Transfer-Encoding: chunked\r\n\r\n",
					HttpRequestError::ConflictingMessageFraming) ||
	   !ExpectError( "POST / HTTP/1.1\r\n"
					"Host: 192.0.2.1\r\n"
					"Transfer-Encoding: chunked\r\n"
					"Transfer-Encoding: chunked\r\n\r\n",
					HttpRequestError::InvalidTransferEncoding) ||
	   !ExpectError( "POST / HTTP/1.1\r\n"
					"Host: 192.0.2.1\r\n"
					"Transfer-Encoding: chunked\r\n\r\n"
					"Z\r\n",
					HttpRequestError::InvalidChunkSize) ||
	   !ExpectError( "POST / HTTP/1.1\r\n"
					"Host: 192.0.2.1\r\n"
					"Transfer-Encoding: chunked\r\n\r\n"
					"1\r\nxX\n",
					HttpRequestError::InvalidChunkTerminator) ||
	   !ExpectError( "POST / HTTP/1.1\r\n"
					"Host: 192.0.2.1\r\n"
					"Transfer-Encoding: chunked\r\n\r\n"
					"0\r\nHost: 198.51.100.1\r\n\r\n",
					HttpRequestError::MalformedTrailer) ||
	   !ExpectError( "POST / HTTP/1.1\r\n"
					"Host: 192.0.2.1\r\n"
					"Content-Length: 1\r\n"
					"Content-Length: 1\r\n\r\nx",
					HttpRequestError::DuplicateContentLength)) {
		return 10 ;
	}

	// header上限は終端の有無に関係なく追加前に適用する
	HttpRequestTransformer oversized ;
	std::string			   huge( zrdp::client::proxy::kMaximumHttpRequestHeaderBytes, 'A') ;
	output.clear() ;
	// header上限を越える追加byteが固定HeaderTooLargeで拒否されることを確認する
	if( !oversized.Push( Bytes( huge), output) || oversized.Push( Bytes( "A"), output) ||
	   oversized.snapshot().error != HttpRequestError::HeaderTooLarge) {
		return 11 ;
	}

	HttpRequestTransformer input_limited ;
	std::string oversized_input( zrdp::client::proxy::kMaximumHttpParserInputBytes + 1U, 'B') ;
	output.clear() ;
	// 一回の入力上限を越えるfragmentが資源保護で拒否されることを確認する
	if( input_limited.Push( Bytes( oversized_input), output) ||
	   input_limited.snapshot().error != HttpRequestError::ResourceExhausted) {
		return 12 ;
	}

	constexpr std::array<std::string_view, 8U> forbidden_connections{
		"Connection: Host\r\n",
		"Connection: Content-Length\r\n",
		"cOnNeCtIoN: hOsT\r\n",
		"Connection: cOnTeNt-LeNgTh\r\n",
		"Connection: keep-alive, Host, X-Hop\r\n",
		"Connection: keep-alive, X-Hop, Content-Length\r\n",
		"Connection: keep-alive\r\nConnection: Host\r\n",
		"Connection: X-Hop\r\nConnection: keep-alive, Content-Length\r\n",
	} ;
	// 大小文字、comma、複数行を含む重要headerの指名は全入力分割で拒否する
	for( const std::string_view connection : forbidden_connections) {
		const std::string request = "POST http://192.0.2.10/repo HTTP/1.1\r\n"
									"Host: 192.0.2.10\r\nContent-Length: 4\r\n" +
									std::string( connection) + "X-Hop: dummy\r\n\r\nDATA" ;
		// headerとbodyの転送前に拒否されることを確認する
		if( !ExpectConnectionRejected( request)) {
			return 13 ;
		}
	}

	// bodyなしやContent-Length欠落でも、重要headerのConnection指名自体を拒否する
	if( !ExpectConnectionRejected( "GET /repo HTTP/1.1\r\nHost: 192.0.2.10\r\n"
								 "Connection: Content-Length\r\n\r\n") ||
	   !ExpectConnectionRejected( "POST /repo HTTP/1.1\r\nConnection: Content-Length\r\n"
								 "Host: 192.0.2.10\r\nContent-Length: 0\r\n\r\n") ||
	   !ExpectConnectionRejected( "POST /repo HTTP/1.1\r\nHost: 192.0.2.10\r\n"
								 "Content-Length: 0\r\nConnection: Host\r\n\r\n")) {
		return 14 ;
	}

	const std::string nested_request = "GET /repo/second HTTP/1.1\r\nHost: 192.0.2.10\r\n\r\n" ;
	const std::string framing_prefix = "POST /repo HTTP/1.1\r\nHost: 192.0.2.10\r\n"
									   "Content-Length: " +
									   std::to_string( nested_request.size()) + "\r\n" ;
	// body内の要求文字列を次要求へ誤解釈させるConnection指定を転送前に拒否する
	if( !ExpectConnectionRejected( framing_prefix + "Connection: Content-Length\r\n\r\n" +
								 nested_request)) {
		return 15 ;
	}

	constexpr std::string_view following = "GET /repo/next HTTP/1.1\r\nHost: 192.0.2.10\r\n\r\n" ;
	const std::string		   fixed_expected =
		framing_prefix + "\r\n" + nested_request + std::string( following) ;
	const std::string fixed_input = framing_prefix +
									"Connection: keep-alive, X-Hop\r\n"
									"X-Hop: remove\r\n\r\n" +
									nested_request + std::string( following) ;
	// 正常固定長bodyは要求文字列を含んでも一bodyのまま維持し、次要求と区別する
	if( !ExpectPreservedBoundaries( fixed_input, fixed_expected, 2U)) {
		return 16 ;
	}

	const std::string chunk_header = "POST /repo HTTP/1.1\r\nHost: 192.0.2.10\r\n"
									 "Transfer-Encoding: chunked\r\nTrailer: X-Checksum\r\n" ;
	const std::string chunk_body =
		"4\r\nDATA\r\n0\r\nX-Checksum: dummy\r\n\r\n" + std::string( following) ;
	const std::string chunk_expected = chunk_header + "\r\n" + chunk_body ;
	// chunkedとTrailerのConnection指名は検証済みframingとして再送し、次要求も維持する
	if( !ExpectPreservedBoundaries( chunk_header +
									  "Connection: keep-alive, Transfer-Encoding, "
									  "Trailer, X-Hop\r\nX-Hop: remove\r\n\r\n" +
									  chunk_body,
								  chunk_expected, 2U)) {
		return 17 ;
	}

	HttpRequestTransformer reused ;
	output.clear() ;
	// 正常要求で宛先を固定した後のKeep-Alive上でも、不正な次要求を転送しない
	if( !reused.Push( Bytes( following), output)) {
		return 18 ;
	}

	output.clear() ;
	// 旧要求の件数と宛先を維持したまま次の不正headerを拒否する
	if( reused.Push( Bytes( framing_prefix + "Connection: Content-Length\r\n\r\n" + nested_request),
				   output) ||
	   !output.empty() || reused.snapshot().error != HttpRequestError::MalformedHeader ||
	   reused.snapshot().request_count != 1U || !reused.snapshot().target_locked) {
		return 19 ;
	}

	return 0 ;
}
