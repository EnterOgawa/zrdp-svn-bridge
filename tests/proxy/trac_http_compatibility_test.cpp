/**
 * @file trac_http_compatibility_test.cpp
 * @brief Mylyn Trac Web／XML-RPC requestのHTTP Proxy互換性を検証する。
 *
 * @details 文書用IPv4 literalと架空のTrac pathだけを使用し、Socket、DNS、
 *          実Trac、credentialへアクセスしない。
 */

#include "client/proxy/http_request_parser.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using zrdp::client::proxy::HttpRequestError;
using zrdp::client::proxy::HttpRequestTransformer;

/**
 * @brief ASCII test dataをbinary spanとして返す。
 * @param[in] text test request fragment。
 * @return 同一memoryを参照するbyte span。
 */
[[nodiscard]] std::span<const std::byte> Bytes(
    const std::string_view text) noexcept {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()),
        text.size());
}

/**
 * @brief parser出力chunkをwire順に一つの文字列へ連結する。
 * @param[in] chunks parserが生成した所有chunk列。
 * @return byte順を維持した文字列。
 */
[[nodiscard]] std::string Join(
    const std::vector<std::vector<std::byte>>& chunks) {
    std::string result;
    // headerとbodyの出力境界に依存せず全chunkをwire順に連結する。
    for (const std::vector<std::byte>& chunk : chunks) {
        result.append(
            reinterpret_cast<const char*>(chunk.data()),
            chunk.size());
    }

    return result;
}

/**
 * @brief 一requestが期待したsecurity errorで拒否されることを確認する。
 * @param[in] request test request。
 * @param[in] expected_error 期待する固定分類。
 * @return fail-closedと分類一致の両方を満たす場合はtrue。
 */
[[nodiscard]] bool ExpectError(
    const std::string_view request,
    const HttpRequestError expected_error) {
    HttpRequestTransformer transformer;
    std::vector<std::vector<std::byte>> output;
    return !transformer.Push(Bytes(request), output) &&
        transformer.snapshot().error == expected_error;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Mylyn Trac相当のGET、XML-RPC、Cookie、gzip透過性を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int main() {
    constexpr std::string_view xml_body =
        "<?xml version=\"1.0\"?>"
        "<methodCall>"
        "<methodName>system.getAPIVersion</methodName>"
        "<params/>"
        "</methodCall>";

    std::string local;
    local.append(
        "GET http://192.0.2.40:8080/trac/ProjectA/"
        "login?referer=%2Fquery HTTP/1.1\r\n"
        "Host: 192.0.2.40:8080\r\n"
        "User-Agent: Mylyn-Trac-Test\r\n"
        "Accept-Encoding: gzip\r\n"
        "Proxy-Connection: keep-alive\r\n"
        "Content-Length: 0\r\n\r\n");
    local.append(
        "POST http://192.0.2.40:8080/trac/ProjectA/"
        "xmlrpc HTTP/1.1\r\n"
        "Host: 192.0.2.40:8080\r\n"
        "User-Agent: Mylyn-Trac-Test\r\n"
        "Content-Type: text/xml\r\n"
        "Accept-Encoding: gzip\r\n"
        "Authorization: Digest opaque-repository-response\r\n"
        "Cookie: trac_auth=opaque-session\r\n"
        "Proxy-Connection: keep-alive\r\n"
        "Connection: keep-alive, X-Proxy-Hop\r\n"
        "X-Proxy-Hop: remove-me\r\n"
        "Content-Length: ");
    local.append(std::to_string(xml_body.size()));
    local.append("\r\n\r\n");
    local.append(xml_body);

    std::string expected;
    expected.append(
        "GET /trac/ProjectA/login?referer=%2Fquery HTTP/1.1\r\n"
        "Host: 192.0.2.40:8080\r\n"
        "User-Agent: Mylyn-Trac-Test\r\n"
        "Accept-Encoding: gzip\r\n"
        "Content-Length: 0\r\n\r\n");
    expected.append(
        "POST /trac/ProjectA/xmlrpc HTTP/1.1\r\n"
        "Host: 192.0.2.40:8080\r\n"
        "User-Agent: Mylyn-Trac-Test\r\n"
        "Content-Type: text/xml\r\n"
        "Accept-Encoding: gzip\r\n"
        "Authorization: Digest opaque-repository-response\r\n"
        "Cookie: trac_auth=opaque-session\r\n"
        "Content-Length: ");
    expected.append(std::to_string(xml_body.size()));
    expected.append("\r\n\r\n");
    expected.append(xml_body);

    HttpRequestTransformer transformer;
    std::vector<std::vector<std::byte>> output;
    const std::size_t first_boundary = 37U;
    const std::size_t second_boundary =
        local.size() - 19U;
    // request line、header、XML bodyを跨ぐfragmentでも同じwire出力へ再構成する。
    if (!transformer.Push(
            Bytes(std::string_view(local).substr(
                0U,
                first_boundary)),
            output) ||
        !transformer.Push(
            Bytes(std::string_view(local).substr(
                first_boundary,
                second_boundary - first_boundary)),
            output) ||
        !transformer.Push(
            Bytes(std::string_view(local).substr(
                second_boundary)),
            output) ||
        Join(output) != expected) {
        return 1;
    }

    zrdp::protocol::OpenTarget target{};
    const auto snapshot = transformer.snapshot();
    if (!transformer.target(target) ||
        target != zrdp::protocol::OpenTarget{
            {192U, 0U, 2U, 40U},
            8080U} ||
        !snapshot.target_locked ||
        !snapshot.at_request_boundary ||
        snapshot.request_count != 2U ||
        snapshot.error != HttpRequestError::None) {
        return 2;
    }

    output.clear();
    constexpr std::string_view changed_endpoint =
        "GET http://198.51.100.50/trac/ProjectB HTTP/1.1\r\n"
        "Host: 198.51.100.50\r\n\r\n";
    // 同じlocal connectionを別endpoint向けの汎用Proxyとして再利用させない。
    if (transformer.Push(Bytes(changed_endpoint), output) ||
        transformer.snapshot().error !=
            HttpRequestError::DestinationChanged) {
        return 3;
    }

    // Trac追加後もHTTPS、CONNECT、hostnameをfail-closedで拒否する。
    if (!ExpectError(
            "GET https://192.0.2.40/trac/ProjectA HTTP/1.1\r\n"
            "Host: 192.0.2.40\r\n\r\n",
            HttpRequestError::UnsupportedScheme) ||
        !ExpectError(
            "CONNECT 192.0.2.40:8080 HTTP/1.1\r\n"
            "Host: 192.0.2.40:8080\r\n\r\n",
            HttpRequestError::ConnectNotAllowed) ||
        !ExpectError(
            "GET http://trac.example.invalid/ProjectA HTTP/1.1\r\n"
            "Host: trac.example.invalid\r\n\r\n",
            HttpRequestError::InvalidIpv4)) {
        return 4;
    }

    return 0;
}
