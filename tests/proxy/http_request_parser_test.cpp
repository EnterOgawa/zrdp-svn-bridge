/**
 * @file http_request_parser_test.cpp
 * @brief Phase 5 HTTP request変換、chunked framing、認証header透過を検証する。
 *
 * @details 文書用IPv4 literalだけを使用し、Socket、DNS、実SVNへ接続しない。
 */

#include "client/proxy/http_request_parser.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zrdp::client::proxy::HttpRequestError;
using zrdp::client::proxy::HttpRequestTransformer;

/**
 * @brief ASCII test dataをbinary spanとして返す。
 * @param[in] text test request。
 * @return 同一memoryを参照するbyte span。
 */
[[nodiscard]] std::span<const std::byte> Bytes(
    const std::string_view text) noexcept {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()),
        text.size());
}

/**
 * @brief parser出力chunkを一つのtest文字列へ連結する。
 * @param[in] chunks parserが生成したwire chunk列。
 * @return byte順を維持した文字列。
 */
[[nodiscard]] std::string Join(
    const std::vector<std::vector<std::byte>>& chunks) {
    std::string result;
    // headerとbodyの出力境界に依存せずwire順で連結する。
    for (const std::vector<std::byte>& chunk : chunks) {
        result.append(
            reinterpret_cast<const char*>(chunk.data()),
            chunk.size());
    }

    return result;
}

/**
 * @brief 一requestが期待した固定errorで拒否されることを確認する。
 * @param[in] request test request。
 * @param[in] expected_error 期待分類。
 * @return Push失敗とerror一致の両方を満たす場合はtrue。
 */
[[nodiscard]] bool ExpectError(
    const std::string_view request,
    const HttpRequestError expected_error) {
    HttpRequestTransformer parser;
    std::vector<std::vector<std::byte>> output;
    return !parser.Push(Bytes(request), output) &&
        parser.snapshot().error == expected_error;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief HTTP request parserの変換、境界、malformed入力拒否を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int main() {
    using zrdp::protocol::OpenTarget;

    HttpRequestTransformer parser;
    std::vector<std::vector<std::byte>> output;
    constexpr std::string_view first_part =
        "PROPFIND http://192.0.2.10:8080/svn/ProjectA?x=1 HTTP/1.1\r\n"
        "Ho";
    constexpr std::string_view second_part =
        "st: 192.0.2.10:8080\r\n"
        "Depth: 1\r\n"
        "Proxy-Connection: keep-alive\r\n"
        "Proxy-Authorization: Basic secret-must-not-forward\r\n"
        "Connection: keep-alive, X-Hop\r\n"
        "X-Hop: remove-me\r\n"
        "Authorization: Basic opaque-end-to-end\r\n"
        "Content-Length: 5\r\n\r\nab";
    constexpr std::string_view third_part = "cde";
    // header fragmentation中は部分requestをupstreamへ公開しない。
    if (!parser.Push(Bytes(first_part), output) ||
        !output.empty() ||
        !parser.Push(Bytes(second_part), output) ||
        !parser.Push(Bytes(third_part), output)) {
        return 1;
    }

    const std::string transformed = Join(output);
    constexpr std::string_view expected =
        "PROPFIND /svn/ProjectA?x=1 HTTP/1.1\r\n"
        "Host: 192.0.2.10:8080\r\n"
        "Depth: 1\r\n"
        "Authorization: Basic opaque-end-to-end\r\n"
        "Content-Length: 5\r\n\r\nabcde";
    if (transformed != expected) {
        return 2;
    }

    OpenTarget target{};
    const auto snapshot = parser.snapshot();
    if (!parser.target(target) ||
        target != OpenTarget{
            {192U, 0U, 2U, 10U},
            8080U} ||
        !snapshot.target_locked ||
        !snapshot.at_request_boundary ||
        snapshot.body_bytes_remaining != 0U ||
        snapshot.request_count != 1U) {
        return 3;
    }

    output.clear();
    constexpr std::string_view keep_alive =
        "GET /svn/ProjectA HTTP/1.1\r\n"
        "Host: 192.0.2.10:8080\r\n\r\n";
    if (!parser.Push(Bytes(keep_alive), output) ||
        Join(output) != keep_alive ||
        parser.snapshot().request_count != 2U) {
        return 4;
    }

    // 同一local connection内の別IPv4またはportへの切替を拒否する。
    output.clear();
    constexpr std::string_view changed_destination =
        "GET http://192.0.2.11/svn/Other HTTP/1.1\r\n"
        "Host: 192.0.2.11\r\n\r\n";
    if (parser.Push(Bytes(changed_destination), output) ||
        parser.snapshot().error !=
            HttpRequestError::DestinationChanged) {
        return 5;
    }

    HttpRequestTransformer pipeline;
    output.clear();
    constexpr std::string_view pipelined =
        "GET http://198.51.100.20/a HTTP/1.1\r\n"
        "Host: 198.51.100.20\r\n\r\n"
        "HEAD http://198.51.100.20/b HTTP/1.1\r\n"
        "Host: 198.51.100.20:80\r\n\r\n";
    constexpr std::string_view pipeline_expected =
        "GET /a HTTP/1.1\r\n"
        "Host: 198.51.100.20\r\n\r\n"
        "HEAD /b HTTP/1.1\r\n"
        "Host: 198.51.100.20:80\r\n\r\n";
    if (!pipeline.Push(Bytes(pipelined), output) ||
        Join(output) != pipeline_expected ||
        pipeline.snapshot().request_count != 2U) {
        return 6;
    }

    // queryだけのabsolute targetはorigin-formの`/?...`へ変換する。
    HttpRequestTransformer query_parser;
    output.clear();
    constexpr std::string_view query_only =
        "OPTIONS http://203.0.113.30?view=1 HTTP/1.1\r\n"
        "Host: 203.0.113.30\r\n\r\n";
    constexpr std::string_view query_expected =
        "OPTIONS /?view=1 HTTP/1.1\r\n"
        "Host: 203.0.113.30\r\n\r\n";
    if (!query_parser.Push(Bytes(query_only), output) ||
        Join(output) != query_expected) {
        return 7;
    }

    HttpRequestTransformer chunked_parser;
    output.clear();
    constexpr std::string_view chunked_first =
        "REPORT http://203.0.113.40/svn/P HTTP/1.1\r\n"
        "Host: 203.0.113.40\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Trailer: X-Checksum\r\n"
        "Expect: 100-continue\r\n"
        "Authorization: NTLM opaque-token\r\n"
        "Cookie: opaque-cookie\r\n"
        "Connection: keep-alive\r\n\r\n"
        "4;ext=1\r";
    constexpr std::string_view chunked_second = "\nWi";
    constexpr std::string_view chunked_third =
        "ki\r\n5\r\npedia\r\n"
        "0\r\nX-Checksum: opaque\r\n\r\n";
    constexpr std::string_view chunked_expected =
        "REPORT /svn/P HTTP/1.1\r\n"
        "Host: 203.0.113.40\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Trailer: X-Checksum\r\n"
        "Expect: 100-continue\r\n"
        "Authorization: NTLM opaque-token\r\n"
        "Cookie: opaque-cookie\r\n\r\n"
        "4;ext=1\r\nWiki\r\n5\r\npedia\r\n"
        "0\r\nX-Checksum: opaque\r\n\r\n";
    // size、data、CRLF、trailerを跨ぐfragmentでもwire byteを維持する。
    if (!chunked_parser.Push(
            Bytes(chunked_first),
            output) ||
        !chunked_parser.Push(
            Bytes(chunked_second),
            output) ||
        !chunked_parser.Push(
            Bytes(chunked_third),
            output) ||
        Join(output) != chunked_expected ||
        !chunked_parser.snapshot().at_request_boundary ||
        chunked_parser.snapshot().chunked ||
        chunked_parser.snapshot().request_count != 1U) {
        return 8;
    }

    // chunked完了後も同じupstream targetのKeep-Alive requestを受理する。
    output.clear();
    constexpr std::string_view after_chunked =
        "PROPFIND /svn/P HTTP/1.1\r\n"
        "Host: 203.0.113.40\r\n"
        "Depth: 1\r\n\r\n";
    if (!chunked_parser.Push(
            Bytes(after_chunked),
            output) ||
        Join(output) != after_chunked ||
        chunked_parser.snapshot().request_count != 2U) {
        return 9;
    }

    // authority／Host、request line、禁止protocolを固定分類で拒否する。
    if (!ExpectError(
            "GET / HTTP/1.1\r\n\r\n",
            HttpRequestError::MissingHost) ||
        !ExpectError(
            "GET http://192.0.2.1/a HTTP/1.1\r\n"
            "Host: 192.0.2.2\r\n\r\n",
            HttpRequestError::HostMismatch) ||
        !ExpectError(
            "GET / HTTP/1.1\r\n"
            "Host: svn.example.invalid\r\n\r\n",
            HttpRequestError::InvalidIpv4) ||
        !ExpectError(
            "GET / HTTP/1.1\r\n"
            "Host: [::1]\r\n\r\n",
            HttpRequestError::InvalidIpv4) ||
        !ExpectError(
            "GET / HTTP/1.1\r\n"
            "Host: 192.0.2.1:0\r\n\r\n",
            HttpRequestError::InvalidPort) ||
        !ExpectError(
            "CONNECT 192.0.2.1:80 HTTP/1.1\r\n"
            "Host: 192.0.2.1\r\n\r\n",
            HttpRequestError::ConnectNotAllowed) ||
        !ExpectError(
            "GET https://192.0.2.1/a HTTP/1.1\r\n"
            "Host: 192.0.2.1\r\n\r\n",
            HttpRequestError::UnsupportedScheme) ||
        !ExpectError(
            "GET / HTTP/1.1\r\n"
            "Host: 192.0.2.1\r\n"
            "Connection: Upgrade\r\n\r\n",
            HttpRequestError::UpgradeNotAllowed) ||
        !ExpectError(
            "POST / HTTP/1.1\r\n"
            "Host: 192.0.2.1\r\n"
            "Transfer-Encoding: gzip\r\n\r\n",
            HttpRequestError::
                TransferEncodingNotSupported) ||
        !ExpectError(
            "POST / HTTP/1.1\r\n"
            "Host: 192.0.2.1\r\n"
            "Content-Length: 1\r\n"
            "Transfer-Encoding: chunked\r\n\r\n",
            HttpRequestError::
                ConflictingMessageFraming) ||
        !ExpectError(
            "POST / HTTP/1.1\r\n"
            "Host: 192.0.2.1\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Transfer-Encoding: chunked\r\n\r\n",
            HttpRequestError::
                InvalidTransferEncoding) ||
        !ExpectError(
            "POST / HTTP/1.1\r\n"
            "Host: 192.0.2.1\r\n"
            "Transfer-Encoding: chunked\r\n\r\n"
            "Z\r\n",
            HttpRequestError::InvalidChunkSize) ||
        !ExpectError(
            "POST / HTTP/1.1\r\n"
            "Host: 192.0.2.1\r\n"
            "Transfer-Encoding: chunked\r\n\r\n"
            "1\r\nxX\n",
            HttpRequestError::
                InvalidChunkTerminator) ||
        !ExpectError(
            "POST / HTTP/1.1\r\n"
            "Host: 192.0.2.1\r\n"
            "Transfer-Encoding: chunked\r\n\r\n"
            "0\r\nHost: 198.51.100.1\r\n\r\n",
            HttpRequestError::MalformedTrailer) ||
        !ExpectError(
            "POST / HTTP/1.1\r\n"
            "Host: 192.0.2.1\r\n"
            "Content-Length: 1\r\n"
            "Content-Length: 1\r\n\r\nx",
            HttpRequestError::
                DuplicateContentLength)) {
        return 10;
    }

    // header上限は終端の有無に関係なく追加前に適用する。
    HttpRequestTransformer oversized;
    std::string huge(
        zrdp::client::proxy::
            kMaximumHttpRequestHeaderBytes,
        'A');
    output.clear();
    if (!oversized.Push(Bytes(huge), output) ||
        oversized.Push(Bytes("A"), output) ||
        oversized.snapshot().error !=
            HttpRequestError::HeaderTooLarge) {
        return 11;
    }

    HttpRequestTransformer input_limited;
    std::string oversized_input(
        zrdp::client::proxy::
            kMaximumHttpParserInputBytes + 1U,
        'B');
    output.clear();
    if (input_limited.Push(
            Bytes(oversized_input),
            output) ||
        input_limited.snapshot().error !=
            HttpRequestError::ResourceExhausted) {
        return 12;
    }

    return 0;
}
