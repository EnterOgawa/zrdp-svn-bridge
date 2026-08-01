/**
 * @file client_proxy_config.cpp
 * @brief EXE隣接`client.ini`の有限読込みと厳格なport解析を実装する。
 */

#include <Windows.h>

#include "client/proxy/client_proxy_config.h"

#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace zrdp::client::proxy {
namespace {

constexpr wchar_t kClientProxyConfigFileName[] =
    L"client.ini";

/**
 * @brief Windows HANDLEを一所有者で閉じる。
 */
class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;

    explicit UniqueHandle(
        const HANDLE handle) noexcept
        : handle_(handle) {
    }

    ~UniqueHandle() {
        Reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&&) = delete;
    UniqueHandle& operator=(UniqueHandle&&) = delete;

    /**
     * @brief 現在の非owning HANDLE値を返す。
     * @return 未所有時は`INVALID_HANDLE_VALUE`。
     */
    [[nodiscard]] HANDLE Get() const noexcept {
        return handle_;
    }

    /**
     * @brief 所有HANDLEを一度だけ閉じる。
     */
    void Reset() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(handle_));
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

/**
 * @brief ASCII空白だけを両端から除外する。
 * @param[in] value 対象範囲。
 * @return 同じ所有bufferを参照するtrim済み範囲。
 */
[[nodiscard]] std::string_view TrimAscii(
    std::string_view value) noexcept {
    std::size_t first = 0U;
    // INI tokenの前置きspaceとtabだけを構文外として読み飛ばす。
    while (first < value.size() &&
           (value[first] == ' ' ||
            value[first] == '\t')) {
        ++first;
    }

    std::size_t last = value.size();
    // INI tokenの後置きspaceとtabだけを構文外として読み飛ばす。
    while (last > first &&
           (value[last - 1U] == ' ' ||
            value[last - 1U] == '\t')) {
        --last;
    }

    return value.substr(first, last - first);
}

/**
 * @brief 一つのdecimal portを範囲検証する。
 * @param[in] value trim済みtoken。
 * @param[out] port 成功時の1..65535。
 * @return 全文字をdecimalとして消費できた場合はtrue。
 */
[[nodiscard]] bool ParsePort(
    const std::string_view value,
    std::uint16_t& port) noexcept {
    if (value.empty()) {
        return false;
    }

    std::uint32_t parsed = 0U;
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed,
        10);
    // 部分parse、overflow、0、TCP port上限超過を設定として受け付けない。
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        parsed == 0U ||
        parsed >
            std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }

    port = static_cast<std::uint16_t>(parsed);
    return true;
}

/**
 * @brief 実行中moduleの絶対pathを取得する。
 * @param[out] path 成功時のNULを含まないpath。
 * @return 完全pathを取得できた場合はtrue。
 */
[[nodiscard]] bool ResolveExecutablePath(
    std::wstring& path) noexcept {
    try {
        std::array<wchar_t, 32768U> buffer{};
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        // 0またはbuffer全長は失敗／切詰めであり固定設定pathを確定できない。
        if (length == 0U ||
            static_cast<std::size_t>(length) >=
                buffer.size()) {
            return false;
        }

        path.assign(buffer.data(), length);
    } catch (...) {
        path.clear();
        return false;
    }

    return true;
}

}  // namespace

ClientProxyConfigurationResult
ParseClientProxyConfiguration(
    std::string_view text,
    ClientProxyConfiguration& configuration) noexcept {
    configuration = ClientProxyConfiguration{};
    ClientProxyConfiguration parsed_configuration{};
    // file reader以外から呼ぶunit testでも同じ全体上限を適用する。
    if (text.size() > kMaximumClientProxyConfigBytes) {
        return {
            ClientProxyConfigurationError::FileTooLarge,
            0U,
        };
    }

    // UTF-8 BOMだけを任意に除き、data自体はASCII subsetへ制限する。
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3U);
    }

    // 設定名の多義的なUnicode表現と埋込みNULを受け入れない。
    for (const unsigned char character : text) {
        if (character == 0U ||
            character > 0x7FU) {
            return {
                ClientProxyConfigurationError::
                    UnsupportedEncoding,
                0U,
            };
        }
    }

    bool proxy_section_seen = false;
    bool in_proxy_section = false;
    bool port_seen = false;
    std::size_t offset = 0U;
    // 改行単位で有限長を検証し、最後の改行がない一行も同じ規則で解析する。
    while (offset <= text.size()) {
        const std::size_t line_end =
            text.find('\n', offset);
        const std::size_t end =
            line_end == std::string_view::npos
                ? text.size()
                : line_end;
        if (end - offset >
            kMaximumClientProxyConfigLineBytes) {
            return {
                ClientProxyConfigurationError::
                    LineTooLong,
                0U,
            };
        }

        std::string_view line =
            text.substr(offset, end - offset);
        if (!line.empty() &&
            line.back() == '\r') {
            line.remove_suffix(1U);
        }

        line = TrimAscii(line);
        if (!line.empty() &&
            line.front() != ';' &&
            line.front() != '#') {
            if (line.front() == '[') {
                // 閉じ括弧以外のsuffixや未知sectionを黙って無視しない。
                if (line != "[proxy]") {
                    return {
                        ClientProxyConfigurationError::
                            UnknownSection,
                        0U,
                    };
                }

                if (proxy_section_seen) {
                    return {
                        ClientProxyConfigurationError::
                            MalformedLine,
                        0U,
                    };
                }

                proxy_section_seen = true;
                in_proxy_section = true;
            } else {
                const std::size_t separator =
                    line.find('=');
                // section外、区切りなし、複数区切りを曖昧なkeyとして扱わない。
                if (!in_proxy_section ||
                    separator == std::string_view::npos ||
                    line.find('=', separator + 1U) !=
                        std::string_view::npos) {
                    return {
                        ClientProxyConfigurationError::
                            MalformedLine,
                        0U,
                    };
                }

                const std::string_view key =
                    TrimAscii(line.substr(0U, separator));
                const std::string_view value =
                    TrimAscii(line.substr(separator + 1U));
                if (key != "port") {
                    return {
                        ClientProxyConfigurationError::
                            UnknownKey,
                        0U,
                    };
                }

                if (port_seen) {
                    return {
                        ClientProxyConfigurationError::
                            DuplicatePort,
                        0U,
                    };
                }

                std::uint16_t port = 0U;
                if (!ParsePort(value, port)) {
                    return {
                        ClientProxyConfigurationError::
                            InvalidPort,
                        0U,
                    };
                }

                parsed_configuration.port = port;
                parsed_configuration.loaded_from_file = true;
                port_seen = true;
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }

        offset = line_end + 1U;
    }

    // 存在するfileは明示portを一件だけ持つ完全設定に限定する。
    if (!proxy_section_seen ||
        !port_seen) {
        return {
            ClientProxyConfigurationError::MissingPort,
            0U,
        };
    }

    configuration = parsed_configuration;
    return {};
}

ClientProxyConfigurationResult
LoadClientProxyConfigurationFromExecutableDirectory(
    ClientProxyConfiguration& configuration) noexcept {
    configuration = ClientProxyConfiguration{};
    std::wstring executable_path;
    if (!ResolveExecutablePath(executable_path)) {
        return {
            ClientProxyConfigurationError::
                ExecutablePathUnavailable,
            static_cast<std::uint32_t>(GetLastError()),
        };
    }

    const std::size_t separator =
        executable_path.find_last_of(L"\\/");
    // directoryなしのmodule pathから相対設定fileを推測しない。
    if (separator == std::wstring::npos) {
        return {
            ClientProxyConfigurationError::
                ExecutablePathUnavailable,
            0U,
        };
    }

    try {
        executable_path.resize(separator + 1U);
        executable_path.append(
            kClientProxyConfigFileName);
    } catch (...) {
        return {
            ClientProxyConfigurationError::
                ResourceExhausted,
            0U,
        };
    }

    UniqueHandle file(CreateFileW(
        executable_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (file.Get() == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        // optional file欠落だけは既定portを使う正常経路とする。
        if (error == ERROR_FILE_NOT_FOUND ||
            error == ERROR_PATH_NOT_FOUND) {
            return {};
        }

        return {
            ClientProxyConfigurationError::
                FileOpenFailed,
            static_cast<std::uint32_t>(error),
        };
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file.Get(), &file_size)) {
        return {
            ClientProxyConfigurationError::
                FileReadFailed,
            static_cast<std::uint32_t>(GetLastError()),
        };
    }

    // 負値、64 KiB超過、DWORDへ安全に変換できないfileを読まない。
    if (file_size.QuadPart < 0LL ||
        static_cast<unsigned long long>(
            file_size.QuadPart) >
            kMaximumClientProxyConfigBytes) {
        return {
            ClientProxyConfigurationError::FileTooLarge,
            0U,
        };
    }

    std::vector<char> bytes;
    try {
        bytes.resize(
            static_cast<std::size_t>(
                file_size.QuadPart));
    } catch (...) {
        return {
            ClientProxyConfigurationError::
                ResourceExhausted,
            0U,
        };
    }

    DWORD bytes_read = 0U;
    if (!bytes.empty() &&
        (!ReadFile(
             file.Get(),
             bytes.data(),
             static_cast<DWORD>(bytes.size()),
             &bytes_read,
             nullptr) ||
         static_cast<std::size_t>(bytes_read) !=
             bytes.size())) {
        return {
            ClientProxyConfigurationError::
                FileReadFailed,
            static_cast<std::uint32_t>(GetLastError()),
        };
    }

    return ParseClientProxyConfiguration(
        std::string_view(bytes.data(), bytes.size()),
        configuration);
}

}  // namespace zrdp::client::proxy
