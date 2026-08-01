/**
 * @file client_proxy_config_test.cpp
 * @brief Client Proxy port設定の許可構文とfail-closed拒否を検証する。
 */

#include "client/proxy/client_proxy_config.h"

#include <string>
#include <string_view>

namespace {

using zrdp::client::proxy::ClientProxyConfiguration;
using zrdp::client::proxy::ClientProxyConfigurationError;
using zrdp::client::proxy::ParseClientProxyConfiguration;

/**
 * @brief 入力が指定errorで拒否されることを確認する。
 * @param[in] text 設定全体。
 * @param[in] expected 期待する固定分類。
 * @return 分類一致かつ既定値へ戻された場合はtrue。
 */
[[nodiscard]] bool IsRejected(
    const std::string_view text,
    const ClientProxyConfigurationError expected) {
    ClientProxyConfiguration configuration{};
    configuration.port = 12345U;
    configuration.loaded_from_file = true;
    const auto result =
        ParseClientProxyConfiguration(
            text,
            configuration);
    return result.error == expected &&
        configuration.port ==
            zrdp::client::proxy::
                kDefaultClientProxyPort &&
        !configuration.loaded_from_file;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief client.iniのProxy port検証とfail-closed読込みを検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int main() {
    ClientProxyConfiguration configuration{};
    constexpr std::string_view valid =
        "\xEF\xBB\xBF"
        "; documentation-only setting\r\n"
        "[proxy]\r\n"
        " port = 28080 \r\n";
    const auto valid_result =
        ParseClientProxyConfiguration(
            valid,
            configuration);
    if (!valid_result.succeeded() ||
        configuration.port != 28080U ||
        !configuration.loaded_from_file) {
        return 1;
    }

    if (!IsRejected(
            "",
            ClientProxyConfigurationError::
                MissingPort) ||
        !IsRejected(
            "[other]\nport=18080\n",
            ClientProxyConfigurationError::
                UnknownSection) ||
        !IsRejected(
            "[proxy]\nbind=127.0.0.1\n",
            ClientProxyConfigurationError::
                UnknownKey) ||
        !IsRejected(
            "[proxy]\nport=18080\nport=18081\n",
            ClientProxyConfigurationError::
                DuplicatePort) ||
        !IsRejected(
            "[proxy]\nport=0\n",
            ClientProxyConfigurationError::
                InvalidPort) ||
        !IsRejected(
            "[proxy]\nport=65536\n",
            ClientProxyConfigurationError::
                InvalidPort) ||
        !IsRejected(
            "[proxy]\nport=-1\n",
            ClientProxyConfigurationError::
                InvalidPort) ||
        !IsRejected(
            "[proxy]\nport=18x80\n",
            ClientProxyConfigurationError::
                InvalidPort) ||
        !IsRejected(
            "port=18080\n",
            ClientProxyConfigurationError::
                MalformedLine) ||
        !IsRejected(
            "[proxy]\nport=18080=1\n",
            ClientProxyConfigurationError::
                MalformedLine) ||
        !IsRejected(
            "[proxy]\nport=18080\n[proxy]\n",
            ClientProxyConfigurationError::
                MalformedLine)) {
        return 2;
    }

    std::string long_line(
        zrdp::client::proxy::
                kMaximumClientProxyConfigLineBytes +
            1U,
        'x');
    if (!IsRejected(
            long_line,
            ClientProxyConfigurationError::
                LineTooLong)) {
        return 3;
    }

    std::string oversized(
        zrdp::client::proxy::
                kMaximumClientProxyConfigBytes +
            1U,
        'x');
    if (!IsRejected(
            oversized,
            ClientProxyConfigurationError::
                FileTooLarge)) {
        return 4;
    }

    const std::string unsupported =
        "[proxy]\nport=18080\n\xC2\xA0";
    if (!IsRejected(
            unsupported,
            ClientProxyConfigurationError::
                UnsupportedEncoding)) {
        return 5;
    }

    // TCP最大portは上限値を含めて有効とする。
    const auto maximum_result =
        ParseClientProxyConfiguration(
            "[proxy]\nport=65535",
            configuration);
    if (!maximum_result.succeeded() ||
        configuration.port != 65535U) {
        return 6;
    }

    return 0;
}
