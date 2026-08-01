/**
 * @file allowlist.cpp
 * @brief Agent allowlistのstrict INI解析と完全一致検索を実装する。
 *
 * @details 曖昧なIPv4表現や未知設定を受理せず、設定全体を検証してから
 *          public `Allowlist`へmoveする。
 */

#include "agent/network/allowlist.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>

namespace zrdp::agent::network {
namespace {

/// 初期設定で認識する唯一のsection名。
constexpr std::string_view kAllowlistSection = "[allowlist]";

/// 匿名数値IDを持つentry key prefix。
constexpr std::string_view kEndpointPrefix = "endpoint.";

/**
 * @brief ASCII spaceとtabだけを設定上の周辺空白として除く。
 * @param[in] value 一行内の非owning文字列。
 * @return 元buffer内のtrim済みview。
 */
[[nodiscard]] std::string_view TrimAscii(
    std::string_view value) noexcept {
    // locale依存分類を使わず、設定契約で許可した二文字だけを除去する。
    while (!value.empty() &&
        (value.front() == ' ' ||
         value.front() == '\t')) {
        value.remove_prefix(1U);
    }

    // 行末側も同じ規則で有限に縮める。
    while (!value.empty() &&
        (value.back() == ' ' ||
         value.back() == '\t')) {
        value.remove_suffix(1U);
    }

    return value;
}

/**
 * @brief 非0のdecimal uint32を曖昧表現なしで解析する。
 * @param[in] text digitだけを含む候補。
 * @param[out] value 成功時の非0値。
 * @return 全文字を消費して範囲内ならtrue。
 */
[[nodiscard]] bool ParseEndpointId(
    const std::string_view text,
    std::uint32_t& value) noexcept {
    value = 0U;
    // 空、符号、先頭0を拒否し、同じIDの別表記を作らない。
    if (text.empty() ||
        text.front() == '0') {
        return false;
    }

    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result =
        std::from_chars(begin, end, value, 10);
    return result.ec == std::errc{} &&
        result.ptr == end &&
        value != 0U;
}

/**
 * @brief decimal octetをstrict dotted-decimal規則で解析する。
 * @param[in] text 一つのIPv4 octet候補。
 * @param[out] value 成功時の0..255。
 * @return 先頭0による別表記を含まず有効ならtrue。
 */
[[nodiscard]] bool ParseIpv4Octet(
    const std::string_view text,
    std::uint8_t& value) noexcept {
    value = 0U;
    // 空、3桁超過、複数桁の先頭0を曖昧なaddress表現として拒否する。
    if (text.empty() ||
        text.size() > 3U ||
        (text.size() > 1U && text.front() == '0')) {
        return false;
    }

    unsigned int parsed = 0U;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result =
        std::from_chars(begin, end, parsed, 10);
    // 全文字がdigitで、IPv4 octet上限内の場合だけ縮小castする。
    if (result.ec != std::errc{} ||
        result.ptr != end ||
        parsed > 255U) {
        return false;
    }

    value = static_cast<std::uint8_t>(parsed);
    return true;
}

/**
 * @brief strict dotted-decimal IPv4 literalを4 octetへ分解する。
 * @param[in] text address部分だけの候補。
 * @param[out] ipv4 成功時にnetwork表記順の4 octet。
 * @return 4要素を過不足なく解析し、接続先として使用可能ならtrue。
 */
[[nodiscard]] bool ParseIpv4(
    const std::string_view text,
    std::array<std::uint8_t, 4>& ipv4) noexcept {
    ipv4 = {};
    std::size_t begin = 0U;
    // 必ず4 octetだけを左から順に解析する。
    for (std::size_t index = 0U;
         index < ipv4.size();
         ++index) {
        const std::size_t separator =
            text.find('.', begin);
        const bool is_last =
            index + 1U == ipv4.size();
        // 末尾要素だけdotなし、それ以前はdotありを必須とする。
        if ((is_last &&
             separator != std::string_view::npos) ||
            (!is_last &&
             separator == std::string_view::npos)) {
            return false;
        }

        const std::size_t end =
            is_last ? text.size() : separator;
        if (!ParseIpv4Octet(
                text.substr(begin, end - begin),
                ipv4[index])) {
            return false;
        }

        begin = end + 1U;
    }

    // unspecified、multicast、broadcastをoutbound unicast候補として使用しない。
    if ((ipv4[0U] == 0U &&
         ipv4[1U] == 0U &&
         ipv4[2U] == 0U &&
         ipv4[3U] == 0U) ||
        ipv4[0U] >= 224U) {
        return false;
    }

    return true;
}

/**
 * @brief decimal TCP portを非0 uint16へ解析する。
 * @param[in] text colon後のport候補。
 * @param[out] port 成功時の1..65535。
 * @return 全文字を消費し、先頭0の別表記がなければtrue。
 */
[[nodiscard]] bool ParsePort(
    const std::string_view text,
    std::uint16_t& port) noexcept {
    port = 0U;
    // portの別表記や符号を拒否し、完全一致設定を一意にする。
    if (text.empty() ||
        text.front() == '0') {
        return false;
    }

    unsigned int parsed = 0U;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result result =
        std::from_chars(begin, end, parsed, 10);
    // TCP portの非0上限内だけをuint16へ変換する。
    if (result.ec != std::errc{} ||
        result.ptr != end ||
        parsed == 0U ||
        parsed >
            static_cast<unsigned int>(
                std::numeric_limits<std::uint16_t>::max())) {
        return false;
    }

    port = static_cast<std::uint16_t>(parsed);
    return true;
}

/**
 * @brief `IPv4:port`をhostname解決なしのOpenTargetへ変換する。
 * @param[in] text entry value全体。
 * @param[out] target 成功時の完全一致target。
 * @return IPv4不正時は`InvalidIpv4`、port不正時は`InvalidPort`。
 */
[[nodiscard]] AllowlistError ParseTarget(
    const std::string_view text,
    protocol::OpenTarget& target) noexcept {
    target = {};
    const std::size_t separator = text.find(':');
    // colonはIPv4とportを分ける一文字だけに限定する。
    if (separator == std::string_view::npos ||
        text.find(':', separator + 1U) !=
            std::string_view::npos) {
        return AllowlistError::InvalidIpv4;
    }

    if (!ParseIpv4(
            text.substr(0U, separator),
            target.ipv4)) {
        return AllowlistError::InvalidIpv4;
    }

    if (!ParsePort(
            text.substr(separator + 1U),
            target.port)) {
        return AllowlistError::InvalidPort;
    }

    return AllowlistError::None;
}

/**
 * @brief 一行のentryを検証済み値へ変換する。
 * @param[in] line trim済み設定行。
 * @param[out] entry 成功時の匿名IDとtarget。
 * @return keyまたはendpointの固定error分類。
 */
[[nodiscard]] AllowlistError ParseEntry(
    const std::string_view line,
    AllowlistEntry& entry) noexcept {
    entry = {};
    const std::size_t equals = line.find('=');
    // keyとvalueの境界は一つだけとし、未知の追記を受理しない。
    if (equals == std::string_view::npos ||
        line.find('=', equals + 1U) !=
            std::string_view::npos) {
        return AllowlistError::InvalidKey;
    }

    const std::string_view key =
        TrimAscii(line.substr(0U, equals));
    const std::string_view value =
        TrimAscii(line.substr(equals + 1U));
    // `endpoint.`以外の設定を将来機能として暗黙に無視しない。
    if (!key.starts_with(kEndpointPrefix)) {
        return AllowlistError::InvalidKey;
    }

    if (!ParseEndpointId(
            key.substr(kEndpointPrefix.size()),
            entry.endpoint_id)) {
        return AllowlistError::InvalidEndpointId;
    }

    return ParseTarget(value, entry.target);
}

}  // namespace

bool Allowlist::Find(
    const protocol::OpenTarget& target,
    std::uint32_t& endpoint_id) const noexcept {
    endpoint_id = 0U;
    // 完全一致以外のprefix、range、port fallbackを行わない。
    for (const AllowlistEntry& entry : entries_) {
        if (entry.target == target) {
            endpoint_id = entry.endpoint_id;
            return true;
        }
    }

    return false;
}

std::size_t Allowlist::size() const noexcept {
    return entries_.size();
}

AllowlistResult ParseAllowlist(
    std::string_view text,
    Allowlist& allowlist) noexcept {
    try {
        // public text parserにもfile loaderと同じ総byte上限を適用する。
        if (text.size() >
            kMaximumAllowlistFileBytes) {
            return AllowlistResult{
                AllowlistError::FileTooLarge,
                0U,
            };
        }

        // UTF-8 BOMだけはfile editor互換として先頭で除去する。
        if (text.size() >= 3U &&
            static_cast<unsigned char>(text[0U]) == 0xEFU &&
            static_cast<unsigned char>(text[1U]) == 0xBBU &&
            static_cast<unsigned char>(text[2U]) == 0xBFU) {
            text.remove_prefix(3U);
        }

        // 現行grammarはASCII subsetだけであり、不可視Unicodeの同形文字を拒否する。
        for (const char character : text) {
            const auto byte =
                static_cast<unsigned char>(character);
            if (byte >= 0x80U ||
                (byte < 0x20U &&
                 character != '\r' &&
                 character != '\n' &&
                 character != '\t')) {
                return AllowlistResult{
                    AllowlistError::NonAsciiInput,
                    0U,
                };
            }
        }

        std::vector<AllowlistEntry> parsed_entries;
        parsed_entries.reserve(
            std::min(
                kMaximumAllowlistEntries,
                static_cast<std::size_t>(16U)));
        bool section_seen = false;
        bool in_allowlist_section = false;
        std::size_t offset = 0U;
        std::uint32_t line_number = 1U;

        // CRLFとLFを同じ規則で一行ずつ解析し、最後の空行も有限に処理する。
        while (offset <= text.size()) {
            const std::size_t newline =
                text.find('\n', offset);
            const std::size_t line_end =
                newline == std::string_view::npos
                    ? text.size()
                    : newline;
            std::string_view line =
                text.substr(offset, line_end - offset);
            if (!line.empty() &&
                line.back() == '\r') {
                line.remove_suffix(1U);
            }

            // 一行の極端な入力でparserの作業量と診断位置を曖昧にしない。
            if (line.size() >
                kMaximumAllowlistLineBytes) {
                return AllowlistResult{
                    AllowlistError::LineTooLong,
                    line_number,
                };
            }

            line = TrimAscii(line);
            if (!line.empty() &&
                line.front() != '#' &&
                line.front() != ';') {
                if (line.front() == '[') {
                    // 初期版は未知sectionを無視せず設定誤りとして停止する。
                    if (line != kAllowlistSection) {
                        return AllowlistResult{
                            AllowlistError::UnknownSection,
                            line_number,
                        };
                    }

                    // 同じsectionを分割定義して重複判定を迂回させない。
                    if (section_seen) {
                        return AllowlistResult{
                            AllowlistError::DuplicateSection,
                            line_number,
                        };
                    }

                    section_seen = true;
                    in_allowlist_section = true;
                } else {
                    // entryは明示section外へ置かれた場合に採用しない。
                    if (!in_allowlist_section) {
                        return AllowlistResult{
                            AllowlistError::EntryOutsideSection,
                            line_number,
                        };
                    }

                    AllowlistEntry entry{};
                    const AllowlistError entry_error =
                        ParseEntry(line, entry);
                    if (entry_error !=
                        AllowlistError::None) {
                        return AllowlistResult{
                            entry_error,
                            line_number,
                        };
                    }

                    // resource上限を越える設定は部分採用せず全体を拒否する。
                    if (parsed_entries.size() >=
                        kMaximumAllowlistEntries) {
                        return AllowlistResult{
                            AllowlistError::TooManyEntries,
                            line_number,
                        };
                    }

                    // 匿名IDと実接続先のどちらも一意であることを確認する。
                    for (const AllowlistEntry& existing :
                         parsed_entries) {
                        if (existing.endpoint_id ==
                            entry.endpoint_id) {
                            return AllowlistResult{
                                AllowlistError::
                                    DuplicateEndpointId,
                                line_number,
                            };
                        }

                        if (existing.target ==
                            entry.target) {
                            return AllowlistResult{
                                AllowlistError::
                                    DuplicateEndpoint,
                                line_number,
                            };
                        }
                    }

                    parsed_entries.push_back(entry);
                }
            }

            // newlineなしの末尾を処理した時点でloopを終了する。
            if (newline == std::string_view::npos) {
                break;
            }

            offset = newline + 1U;
            ++line_number;
        }

        // section自体の欠落と、entryを持たない危険な全拒否設定を区別する。
        if (!section_seen) {
            return AllowlistResult{
                AllowlistError::MissingSection,
                0U,
            };
        }

        if (parsed_entries.empty()) {
            return AllowlistResult{
                AllowlistError::EmptyAllowlist,
                0U,
            };
        }

        allowlist.entries_ = std::move(parsed_entries);
        return AllowlistResult{};
    } catch (const std::bad_alloc&) {
        return AllowlistResult{
            AllowlistError::ResourceExhausted,
            0U,
        };
    } catch (...) {
        // filesystem以外の予期しない標準library例外も部分設定を公開せず固定分類へ変換する。
        return AllowlistResult{
            AllowlistError::ResourceExhausted,
            0U,
        };
    }
}

AllowlistResult LoadAllowlistFile(
    const std::filesystem::path& path,
    Allowlist& allowlist) noexcept {
    try {
        std::ifstream input(
            path,
            std::ios::binary | std::ios::ate);
        // file pathやOS messageを診断値へ含めず、利用不能だけを分類する。
        if (!input.is_open()) {
            return AllowlistResult{
                AllowlistError::FileUnavailable,
                0U,
            };
        }

        const std::streampos end_position = input.tellg();
        // tellg失敗と負値をsize_tへ変換しない。
        if (end_position < std::streampos{0}) {
            return AllowlistResult{
                AllowlistError::FileReadFailed,
                0U,
            };
        }

        const auto file_size =
            static_cast<std::uintmax_t>(end_position);
        // file全体を読む前にmemory上限を適用する。
        if (file_size >
            static_cast<std::uintmax_t>(
                kMaximumAllowlistFileBytes)) {
            return AllowlistResult{
                AllowlistError::FileTooLarge,
                0U,
            };
        }

        std::string text(
            static_cast<std::size_t>(file_size),
            '\0');
        input.seekg(0, std::ios::beg);
        if (!text.empty()) {
            input.read(
                text.data(),
                static_cast<std::streamsize>(
                    text.size()));
        }

        // partial readを欠落設定としてparserへ渡さない。
        if (!input.good() &&
            !input.eof()) {
            return AllowlistResult{
                AllowlistError::FileReadFailed,
                0U,
            };
        }

        if (input.gcount() !=
            static_cast<std::streamsize>(
                text.size())) {
            return AllowlistResult{
                AllowlistError::FileReadFailed,
                0U,
            };
        }

        return ParseAllowlist(text, allowlist);
    } catch (const std::bad_alloc&) {
        return AllowlistResult{
            AllowlistError::ResourceExhausted,
            0U,
        };
    } catch (...) {
        // filesystem／iostream由来例外を機密文字列なしの固定結果へ閉じ込める。
        return AllowlistResult{
            AllowlistError::FileReadFailed,
            0U,
        };
    }
}

}  // namespace zrdp::agent::network
