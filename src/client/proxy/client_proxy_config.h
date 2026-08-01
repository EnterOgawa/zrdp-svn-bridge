/**
 * @file client_proxy_config.h
 * @brief loopback Proxy portだけを許可するClient local設定境界を定義する。
 *
 * @details bind address、hostname、upstream endpoint、credentialを設定形式へ
 *          持ち込まず、EXE隣接の固定`client.ini`だけを任意で読み込む。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace zrdp::client::proxy {

/// Client Proxyの既定loopback port。
inline constexpr std::uint16_t kDefaultClientProxyPort = 18080U;

/// local設定file全体の有限上限。
inline constexpr std::size_t kMaximumClientProxyConfigBytes =
    64U * 1024U;

/// local設定一行の有限上限。
inline constexpr std::size_t kMaximumClientProxyConfigLineBytes =
    1024U;

/**
 * @brief payloadを含めずUI／testへ公開できるlocal設定結果。
 */
enum class ClientProxyConfigurationError : std::uint32_t {
    None = 0U,
    ExecutablePathUnavailable = 1U,
    FileOpenFailed = 2U,
    FileTooLarge = 3U,
    FileReadFailed = 4U,
    UnsupportedEncoding = 5U,
    LineTooLong = 6U,
    MalformedLine = 7U,
    UnknownSection = 8U,
    UnknownKey = 9U,
    DuplicatePort = 10U,
    InvalidPort = 11U,
    MissingPort = 12U,
    ResourceExhausted = 13U,
};

/**
 * @brief 検証済みProxy local設定。
 */
struct ClientProxyConfiguration {
    /// 常に1..65535。bind addressは別fieldとして公開しない。
    std::uint16_t port{kDefaultClientProxyPort};

    /// `client.ini`が存在してportを明示した場合だけtrue。
    bool loaded_from_file{false};
};

/**
 * @brief local設定読込み結果。
 */
struct ClientProxyConfigurationResult {
    /// local設定を利用可能か決める固定失敗分類。
    ClientProxyConfigurationError error{
        ClientProxyConfigurationError::None};

    /// file API失敗時だけ設定するWin32 error code。
    std::uint32_t win32_error{0U};

    /**
     * @brief 設定を安全に利用できるか返す。
     * @return file欠落による既定値を含め、成功時はtrue。
     */
    [[nodiscard]] bool succeeded() const noexcept {
        return error ==
            ClientProxyConfigurationError::None;
    }
};

/**
 * @brief `client.ini`の所有byte列をProxy port設定として検証する。
 * @param[in] text file全体。UTF-8 BOMまたはASCIIを受け付ける。
 * @param[out] configuration 成功時の明示port。
 * @return 内容を含まない固定結果。
 *
 * `[proxy]`の`port=<1..65535>`だけを受理し、未知section／key、重複、
 * 巨大入力をfail-closedにする。
 */
[[nodiscard]] ClientProxyConfigurationResult
ParseClientProxyConfiguration(
    std::string_view text,
    ClientProxyConfiguration& configuration) noexcept;

/**
 * @brief 実行中EXEと同じdirectoryの固定`client.ini`を任意読込みする。
 * @param[out] configuration file欠落時は既定18080、存在時は検証済みport。
 * @return 内容を含まない固定結果。
 *
 * file名やdirectoryは利用者入力から受け取らず、file欠落だけを正常な既定値とする。
 */
[[nodiscard]] ClientProxyConfigurationResult
LoadClientProxyConfigurationFromExecutableDirectory(
    ClientProxyConfiguration& configuration) noexcept;

}  // namespace zrdp::client::proxy
