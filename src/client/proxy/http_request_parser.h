//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file http_request_parser.h
 * @brief localhost Forward Proxy向けHTTP request変換境界を定義する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details request headerだけを有限bufferで解析し、bodyは入力chunk単位で
 *          所有出力へ移す。URL、header、bodyを診断文字列へ変換しない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Connection拒否条件とchunkedの維持契約を明記。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "protocol/multiplexer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace zrdp::client::proxy {

// 一requestのheaderとして保持できる最大byte数
inline constexpr std::size_t   kMaximumHttpRequestHeaderBytes = 64U * 1024U ;

// caller一回の入力を所有出力へcopyできる最大byte数
inline constexpr std::size_t   kMaximumHttpParserInputBytes = 64U * 1024U ;

// Phase 5で一requestとして中継するdecoded bodyの有限上限
inline constexpr std::uint64_t kMaximumHttpRequestBodyBytes = 64ULL * 1024ULL * 1024ULL * 1024ULL ;

// 一回のPushで処理するpipelined request header数上限
inline constexpr std::size_t   kMaximumHttpRequestsPerPush = 128U ;

// chunk size lineとして保持できる最大byte数
inline constexpr std::size_t   kMaximumHttpChunkLineBytes = 8U * 1024U ;

// chunked trailer blockとして保持できる最大byte数
inline constexpr std::size_t   kMaximumHttpTrailerBytes = 64U * 1024U ;

/**
 * @brief request内容を含めず拒否理由を表す固定分類。
 */
enum class HttpRequestError : std::uint32_t {
	/// 異常は検出されておらず、要求の処理を継続できる。
	None						 = 0U,
	/// HTTP headerが有限上限を超え、要求を拒否する。
	HeaderTooLarge				 = 1U,
	/// method、request-target、HTTP versionの構文が不正である。
	MalformedRequestLine		 = 2U,
	/// HTTP header行のnameまたは区切りが不正である。
	MalformedHeader				 = 3U,
	/// HTTP/1.1要求に必要なHost headerがない。
	MissingHost					 = 4U,
	/// Host headerが複数あり、宛先を一意に決定できない。
	DuplicateHost				 = 5U,
	/// absolute-formのauthorityとHost headerが一致しない。
	HostMismatch				 = 6U,
	/// 接続先が許可されたIPv4 literalの形式を満たさない。
	InvalidIpv4					 = 7U,
	/// TCP portが指定可能な範囲または形式を満たさない。
	InvalidPort					 = 8U,
	/// http以外のschemeの中継を要求している。
	UnsupportedScheme			 = 9U,
	/// CONNECT tunnel要求をProxyの許可範囲外として拒否する。
	ConnectNotAllowed			 = 10U,
	/// protocolのUpgradeを許可せずHTTP要求を拒否する。
	UpgradeNotAllowed			 = 11U,
	/// 対応していないtransfer codingを指定している。
	TransferEncodingNotSupported = 12U,
	/// Content-Lengthの重複でmessage長が曖昧になっている。
	DuplicateContentLength		 = 13U,
	/// Content-Lengthの形式または範囲が不正である。
	InvalidContentLength		 = 14U,
	/// Keep-Alive中の宛先変更を検出し、同じ接続で中継しない。
	DestinationChanged			 = 15U,
	/// memory等の有限資源を確保できず、部分状態を公開しない。
	ResourceExhausted			 = 16U,
	/// Transfer-Encodingの構文または並びが不正である。
	InvalidTransferEncoding		 = 17U,
	/// Content-Lengthとtransfer codingが競合し、長さを決定できない。
	ConflictingMessageFraming	 = 18U,
	/// chunk長の十六進表記または拡張構文が不正である。
	InvalidChunkSize			 = 19U,
	/// chunk末尾のCRLFが欠落または不正である。
	InvalidChunkTerminator		 = 20U,
	/// chunk長を指定する行が有限上限を超えている。
	ChunkLineTooLarge			 = 21U,
	/// 終端chunkのtrailerが有限上限を超えている。
	TrailerTooLarge				 = 22U,
	/// trailer headerの構文が不正である。
	MalformedTrailer			 = 23U,
} ;

/**
 * @brief incremental request framingの非機密状態。
 */
enum class HttpRequestParseState {
	/// 次request headerを収集中。
	Header,
	/// Content-Length bodyを中継中。
	FixedBody,
	/// chunk size lineを収集中。
	ChunkSize,
	/// 現chunk dataを中継中。
	ChunkData,
	/// chunk data後のCRLFを検証中。
	ChunkTerminator,
	/// zero chunk後のtrailerを収集中。
	ChunkTrailers,
} ;

/**
 * @brief parserの非機密状態をtest／ownerへ返す。
 */
struct HttpRequestParserSnapshot {
	/// 最初の正常headerから宛先を固定済みの場合はtrue。
	bool				  target_locked{false} ;

	/// 次request headerの先頭で待機中の場合はtrue。
	bool				  at_request_boundary{true} ;

	/// 現request bodyの未受信byte数。
	std::uint64_t		  body_bytes_remaining{0U} ;

	/// 現在のrequest framing状態。
	HttpRequestParseState parse_state{HttpRequestParseState::Header} ;

	/// 現requestがchunked codingの場合はtrue。
	bool				  chunked{false} ;

	/// 受理済みrequest header数。
	std::uint64_t		  request_count{0U} ;

	/// 最後の固定失敗分類。
	HttpRequestError	  error{HttpRequestError::None} ;
} ;

/**
 * @brief HTTP requestを検証し、origin server向けbyte列へincremental変換する。
 *
 * absolute-formはorigin-formへ変換し、最初のIPv4 literalとportへconnectionを
 * 固定する。`CONNECT`、Upgrade、hostname、IPv6、未知Transfer-Encodingは
 * Phase 5境界でfail-closedにする。ConnectionがHostまたはContent-Lengthを
 * 指名する要求は出力前に拒否し、検証済みchunkedのframingは次hopでも維持する。
 */
class HttpRequestTransformer final {
public:
	/**
	 * @brief parser、宛先、body進捗を空のconnection状態へ戻す。
	 */
	void									Reset() noexcept ;

	/**
	 * @brief local clientから受信した任意fragmentを検証して変換する。
	 * @param[in] data HTTP request streamの次fragment。
	 * @param[in,out] output 成功時にupstreamへ送る所有chunkを末尾追加する。
	 * @return fragmentを受理した場合はtrue、固定errorで停止した場合はfalse。
	 *
	 * header未完了では出力を追加しない。bodyは全量保持せず、入力fragment以下の
	 * 単位へcopyする。
	 * @note 失敗時はその呼出しで追加されたoutputを送信せず破棄する。同一fragmentの
	 * 先行する正常requestの出力が既に含まれる場合も、失敗した追加分は使用しない。
	 */
	[[nodiscard]] bool						Push( std::span<const std::byte>			  data,
												 std::vector<std::vector<std::byte>> &output) noexcept ;

	/**
	 * @brief 最初の正常requestから固定した接続先を返す。
	 * @param[out] target IPv4 literalとport。
	 * @return target固定済みの場合はtrue。
	 */
	[[nodiscard]] bool						target( protocol::OpenTarget &target) const noexcept ;

	/**
	 * @brief payloadを含まないparser状態を返す。
	 * @return 現在の有限counterとerror分類。
	 */
	[[nodiscard]] HttpRequestParserSnapshot snapshot() const noexcept ;

private:
	/**
	 * @brief CRLFCRLFまで収集済みの一header blockを検証・変換する。
	 * @param[out] transformed origin serverへ送るrequest lineとheader。
	 * @return 正常変換できた場合はtrue。
	 */
	[[nodiscard]] bool	   TransformHeader( std::vector<std::byte> &transformed) noexcept ;

	/**
	 * @brief 完成したchunk size lineを検証して次状態へ進める。
	 * @param[out] output 検証済みwire line。
	 * @return size、extension、上限が有効な場合はtrue。
	 */
	[[nodiscard]] bool	   CompleteChunkSizeLine( std::vector<std::byte> &output) noexcept ;

	/**
	 * @brief 完成したtrailer blockを構文検証して次requestへ進める。
	 * @param[out] output 検証済みwire trailer。
	 * @return 全fieldが安全な場合はtrue。
	 */
	[[nodiscard]] bool	   CompleteChunkTrailers( std::vector<std::byte> &output) noexcept ;

	/**
	 * @brief parserを指定固定errorでterminal状態へ移す。
	 * @param[in] error request内容を含まない分類。
	 */
	void				   Fail( HttpRequestError error) noexcept ;

	/// CRLFCRLF未到達のheaderだけを保持する有限buffer。
	std::vector<std::byte> header_buffer_ ;

	/// 最初の正常headerが確定したconnection固定宛先。
	protocol::OpenTarget   target_{} ;

	/// target_が有効な場合はtrue。
	bool				   target_locked_{false} ;

	/// 現requestのContent-Length bodyで未受信のbyte数。
	std::uint64_t		   body_bytes_remaining_{0U} ;

	/// 現requestのincremental framing状態。
	HttpRequestParseState  parse_state_{HttpRequestParseState::Header} ;

	/// chunk size lineまたはchunk後CRLFを有限保持するbuffer。
	std::vector<std::byte> chunk_line_buffer_ ;

	/// 現chunkで未受信のdata byte数。
	std::uint64_t		   chunk_bytes_remaining_{0U} ;

	/// 一chunked requestで検証済みのdecoded body byte合計。
	std::uint64_t		   chunked_body_bytes_{0U} ;

	/// zero chunk後のtrailer blockを有限保持するbuffer。
	std::vector<std::byte> trailer_buffer_ ;

	/// 受理済みrequest header数。
	std::uint64_t		   request_count_{0U} ;

	/// `None`以外では後続dataを受理しない。
	HttpRequestError	   error_{HttpRequestError::None} ;
} ;

} // namespace zrdp::client::proxy
