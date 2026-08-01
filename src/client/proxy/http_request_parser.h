/**
 * @file http_request_parser.h
 * @brief localhost Forward Proxy向けHTTP request変換境界を定義する。
 *
 * @details request headerだけを有限bufferで解析し、bodyは入力chunk単位で
 *          所有出力へ移す。URL、header、bodyを診断文字列へ変換しない。
 */
#pragma once

#include "protocol/multiplexer.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace zrdp::client::proxy {

/// 一requestのheaderとして保持できる最大byte数。
inline constexpr std::size_t kMaximumHttpRequestHeaderBytes =
    64U * 1024U;

/// caller一回の入力を所有出力へcopyできる最大byte数。
inline constexpr std::size_t kMaximumHttpParserInputBytes =
    64U * 1024U;

/// Phase 5で一requestとして中継するdecoded bodyの有限上限。
inline constexpr std::uint64_t kMaximumHttpRequestBodyBytes =
    64ULL * 1024ULL * 1024ULL * 1024ULL;

/// 一回のPushで処理するpipelined request header数上限。
inline constexpr std::size_t kMaximumHttpRequestsPerPush = 128U;

/// chunk size lineとして保持できる最大byte数。
inline constexpr std::size_t kMaximumHttpChunkLineBytes = 8U * 1024U;

/// chunked trailer blockとして保持できる最大byte数。
inline constexpr std::size_t kMaximumHttpTrailerBytes = 64U * 1024U;

/**
 * @brief request内容を含めず拒否理由を表す固定分類。
 */
enum class HttpRequestError : std::uint32_t {
    None = 0U,
    HeaderTooLarge = 1U,
    MalformedRequestLine = 2U,
    MalformedHeader = 3U,
    MissingHost = 4U,
    DuplicateHost = 5U,
    HostMismatch = 6U,
    InvalidIpv4 = 7U,
    InvalidPort = 8U,
    UnsupportedScheme = 9U,
    ConnectNotAllowed = 10U,
    UpgradeNotAllowed = 11U,
    TransferEncodingNotSupported = 12U,
    DuplicateContentLength = 13U,
    InvalidContentLength = 14U,
    DestinationChanged = 15U,
    ResourceExhausted = 16U,
    InvalidTransferEncoding = 17U,
    ConflictingMessageFraming = 18U,
    InvalidChunkSize = 19U,
    InvalidChunkTerminator = 20U,
    ChunkLineTooLarge = 21U,
    TrailerTooLarge = 22U,
    MalformedTrailer = 23U,
};

/**
 * @brief incremental request framingの非機密状態。
 */
enum class HttpRequestParseState {
    Header,           ///< 次request headerを収集中。
    FixedBody,        ///< Content-Length bodyを中継中。
    ChunkSize,        ///< chunk size lineを収集中。
    ChunkData,        ///< 現chunk dataを中継中。
    ChunkTerminator,  ///< chunk data後のCRLFを検証中。
    ChunkTrailers,    ///< zero chunk後のtrailerを収集中。
};

/**
 * @brief parserの非機密状態をtest／ownerへ返す。
 */
struct HttpRequestParserSnapshot {
    /// 最初の正常headerから宛先を固定済みの場合はtrue。
    bool target_locked{false};

    /// 次request headerの先頭で待機中の場合はtrue。
    bool at_request_boundary{true};

    /// 現request bodyの未受信byte数。
    std::uint64_t body_bytes_remaining{0U};

    /// 現在のrequest framing状態。
    HttpRequestParseState parse_state{
        HttpRequestParseState::Header};

    /// 現requestがchunked codingの場合はtrue。
    bool chunked{false};

    /// 受理済みrequest header数。
    std::uint64_t request_count{0U};

    /// 最後の固定失敗分類。
    HttpRequestError error{HttpRequestError::None};
};

/**
 * @brief HTTP requestを検証し、origin server向けbyte列へincremental変換する。
 *
 * absolute-formはorigin-formへ変換し、最初のIPv4 literalとportへconnectionを
 * 固定する。`CONNECT`、Upgrade、hostname、IPv6、Transfer-Encodingは
 * Phase 5境界でfail-closedにする。
 */
class HttpRequestTransformer final {
public:
    /**
     * @brief parser、宛先、body進捗を空のconnection状態へ戻す。
     */
    void Reset() noexcept;

    /**
     * @brief local clientから受信した任意fragmentを検証して変換する。
     * @param[in] data HTTP request streamの次fragment。
     * @param[in,out] output 成功時にupstreamへ送る所有chunkを末尾追加する。
     * @return fragmentを受理した場合はtrue、固定errorで停止した場合はfalse。
     *
     * header未完了では出力を追加しない。bodyは全量保持せず、入力fragment以下の
     * 単位へcopyする。
     */
    [[nodiscard]] bool Push(
        std::span<const std::byte> data,
        std::vector<std::vector<std::byte>>& output) noexcept;

    /**
     * @brief 最初の正常requestから固定した接続先を返す。
     * @param[out] target IPv4 literalとport。
     * @return target固定済みの場合はtrue。
     */
    [[nodiscard]] bool target(
        protocol::OpenTarget& target) const noexcept;

    /**
     * @brief payloadを含まないparser状態を返す。
     * @return 現在の有限counterとerror分類。
     */
    [[nodiscard]] HttpRequestParserSnapshot snapshot() const noexcept;

private:
    /**
     * @brief CRLFCRLFまで収集済みの一header blockを検証・変換する。
     * @param[out] transformed origin serverへ送るrequest lineとheader。
     * @return 正常変換できた場合はtrue。
     */
    [[nodiscard]] bool TransformHeader(
        std::vector<std::byte>& transformed) noexcept;

    /**
     * @brief 完成したchunk size lineを検証して次状態へ進める。
     * @param[out] output 検証済みwire line。
     * @return size、extension、上限が有効な場合はtrue。
     */
    [[nodiscard]] bool CompleteChunkSizeLine(
        std::vector<std::byte>& output) noexcept;

    /**
     * @brief 完成したtrailer blockを構文検証して次requestへ進める。
     * @param[out] output 検証済みwire trailer。
     * @return 全fieldが安全な場合はtrue。
     */
    [[nodiscard]] bool CompleteChunkTrailers(
        std::vector<std::byte>& output) noexcept;

    /**
     * @brief parserを指定固定errorでterminal状態へ移す。
     * @param[in] error request内容を含まない分類。
     */
    void Fail(HttpRequestError error) noexcept;

    /// CRLFCRLF未到達のheaderだけを保持する有限buffer。
    std::vector<std::byte> header_buffer_;

    /// 最初の正常headerが確定したconnection固定宛先。
    protocol::OpenTarget target_{};

    /// target_が有効な場合はtrue。
    bool target_locked_{false};

    /// 現requestのContent-Length bodyで未受信のbyte数。
    std::uint64_t body_bytes_remaining_{0U};

    /// 現requestのincremental framing状態。
    HttpRequestParseState parse_state_{
        HttpRequestParseState::Header};

    /// chunk size lineまたはchunk後CRLFを有限保持するbuffer。
    std::vector<std::byte> chunk_line_buffer_;

    /// 現chunkで未受信のdata byte数。
    std::uint64_t chunk_bytes_remaining_{0U};

    /// 一chunked requestで検証済みのdecoded body byte合計。
    std::uint64_t chunked_body_bytes_{0U};

    /// zero chunk後のtrailer blockを有限保持するbuffer。
    std::vector<std::byte> trailer_buffer_;

    /// 受理済みrequest header数。
    std::uint64_t request_count_{0U};

    /// `None`以外では後続dataを受理しない。
    HttpRequestError error_{HttpRequestError::None};
};

}  // namespace zrdp::client::proxy
