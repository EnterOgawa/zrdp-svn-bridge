/**
 * @file allowlist.h
 * @brief Agentの接続先をIPv4 literalとTCP portの完全一致へ限定する。
 *
 * @details 設定値は診断文字列として保持せず、検証済み`OpenTarget`と匿名数値IDへ
 *          変換する。解析失敗時は途中までのentryを公開しない。
 */
#pragma once

#include "protocol/multiplexer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace zrdp::agent::network {

/// Agentが読み込むallowlist fileの最大byte数。
inline constexpr std::size_t kMaximumAllowlistFileBytes = 64U * 1024U;

/// 一行だけでmemoryを占有しないための最大byte数。
inline constexpr std::size_t kMaximumAllowlistLineBytes = 256U;

/// 初期製品で一つのAgent設定へ登録できるendpoint上限。
inline constexpr std::size_t kMaximumAllowlistEntries = 128U;

/**
 * @brief allowlistを採用しなかった固定理由。
 *
 * 入力文字列やfile pathをlogへ渡さず、この値と行番号だけで診断する。
 */
enum class AllowlistError : std::uint32_t {
    None = 0U,
    FileUnavailable = 1U,
    FileTooLarge = 2U,
    FileReadFailed = 3U,
    NonAsciiInput = 4U,
    LineTooLong = 5U,
    MissingSection = 6U,
    DuplicateSection = 7U,
    UnknownSection = 8U,
    EntryOutsideSection = 9U,
    InvalidKey = 10U,
    InvalidEndpointId = 11U,
    InvalidIpv4 = 12U,
    InvalidPort = 13U,
    DuplicateEndpointId = 14U,
    DuplicateEndpoint = 15U,
    TooManyEntries = 16U,
    EmptyAllowlist = 17U,
    ResourceExhausted = 18U,
};

/**
 * @brief allowlist解析結果を機密値なしで表す。
 */
struct AllowlistResult {
    /// 成功時は`None`。
    AllowlistError error{AllowlistError::None};

    /// 入力行に起因する場合の1-origin行番号。file全体の失敗では0。
    std::uint32_t line{0U};

    /**
     * @brief allowlistがatomicに採用されたかを返す。
     * @return `error == None`ならtrue。
     */
    [[nodiscard]] bool succeeded() const noexcept {
        return error == AllowlistError::None;
    }
};

/**
 * @brief allowlist一件の匿名診断IDと完全一致target。
 */
struct AllowlistEntry {
    /// logへ記録できる設定内の非0匿名ID。
    std::uint32_t endpoint_id{0U};

    /// DNSやwildcardを表現しないIPv4 literalとport。
    protocol::OpenTarget target{};
};

/**
 * @brief 検証済みallowlistをimmutableな検索境界として所有する。
 */
class Allowlist final {
public:
    /**
     * @brief IPv4とportが両方一致するentryだけを検索する。
     * @param[in] target Clientから受信した不可信な接続候補。
     * @param[out] endpoint_id 成功時の匿名数値ID。失敗時は0。
     * @return 完全一致entryが一件存在する場合はtrue。
     */
    [[nodiscard]] bool Find(
        const protocol::OpenTarget& target,
        std::uint32_t& endpoint_id) const noexcept;

    /**
     * @brief 検証済みentry数を返す。
     * @return `1..kMaximumAllowlistEntries`の件数。
     */
    [[nodiscard]] std::size_t size() const noexcept;

private:
    friend AllowlistResult ParseAllowlist(
        std::string_view text,
        Allowlist& allowlist) noexcept;

    /// parser成功時に一括moveされる検証済みentry列。
    std::vector<AllowlistEntry> entries_;
};

/**
 * @brief UTF-8 ASCII subsetのallowlist textをatomicに解析する。
 * @param[in] text `[allowlist]` sectionを含む有限設定。
 * @param[out] allowlist 成功時だけ全entryを置換する。
 * @return 固定error分類と、該当する場合の行番号。
 *
 * @note hostname、CIDR、wildcard、port range、IPv6は表現として拒否する。
 */
[[nodiscard]] AllowlistResult ParseAllowlist(
    std::string_view text,
    Allowlist& allowlist) noexcept;

/**
 * @brief 指定fileを有限長で読み込み、検証済みallowlistへ変換する。
 * @param[in] path Agent実行fileと同じdirectoryの`agent.ini`。
 * @param[out] allowlist 成功時だけ全entryを置換する。
 * @return file I/Oまたはparserの固定結果。
 *
 * @warning pathや入力値は戻り値へ含めず、通常logへも出力しない。
 */
[[nodiscard]] AllowlistResult LoadAllowlistFile(
    const std::filesystem::path& path,
    Allowlist& allowlist) noexcept;

}  // namespace zrdp::agent::network
