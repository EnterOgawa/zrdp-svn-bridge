/**
 * @file connection_history_test.cpp
 * @brief 接続profileと音声設定の正規化、重複排除、DPAPI保護を検証する。
 *
 * 実環境情報を使用せず、文書用IPv4アドレスとprocess固有の一時ファイルだけを
 * 対象にする。各失敗箇所は一意な終了codeで識別する。
 */

#include "client/app/connection_history.h"

#include <Windows.h>
#include <Wincrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::array<std::uint8_t, 8> kLegacyHistoryMagic{
    'Z', 'R', 'D', 'P', 'H', 'S', 'T', '1',
};
constexpr char kHistoryEntropy[] =
    "ZRdpSvnBridge.ConnectionHistory.v1";

/**
 * @brief test用のDPAPI出力bufferをLocalFreeまで一意所有する。
 */
class TestLocalDataBlob final {
public:
    /// DPAPI出力bufferを所有していれば解放する。
    ~TestLocalDataBlob() {
        if (blob_.pbData != nullptr) {
            LocalFree(blob_.pbData);
        }
    }

    TestLocalDataBlob() = default;
    TestLocalDataBlob(const TestLocalDataBlob&) = delete;
    TestLocalDataBlob& operator=(const TestLocalDataBlob&) = delete;

    /**
     * @brief CryptProtectDataへ空の出力先を渡す。
     * @return DPAPIが設定するDATA_BLOB。
     */
    [[nodiscard]] DATA_BLOB* receive() noexcept {
        return &blob_;
    }

    /**
     * @brief DPAPI出力をread-only参照する。
     * @return このobjectが所有するDATA_BLOB。
     */
    [[nodiscard]] const DATA_BLOB& value() const noexcept {
        return blob_;
    }

private:
    /// LocalFreeで解放するDPAPI割当てbuffer。
    DATA_BLOB blob_{};
};

/**
 * @brief 32-bit整数を旧履歴と同じlittle-endianで追記する。
 * @param[in,out] output 追記先buffer。
 * @param[in] value encodeする値。
 */
void AppendUint32(
    std::vector<std::uint8_t>& output,
    const std::uint32_t value) {
    // host endianに依存せず旧formatを再現する。
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(
            static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/**
 * @brief testが構築した旧平文履歴をproductionと同じDPAPI条件で保存する。
 * @param[in] history_path test専用出力file。
 * @param[in,out] plaintext 暗号化後にzero clearする旧format平文。
 * @return ciphertextを完全に保存できた場合はtrue。
 */
[[nodiscard]] bool WriteProtectedHistoryFile(
    const std::filesystem::path& history_path,
    std::vector<std::uint8_t>& plaintext) {
    // test helper自体がDPAPIのDWORD長を切り詰めない。
    if (plaintext.empty() ||
        plaintext.size() >
            static_cast<std::size_t>(
                std::numeric_limits<DWORD>::max())) {
        return false;
    }

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(plaintext.size());
    input.pbData = plaintext.data();
    DATA_BLOB entropy{};
    entropy.cbData =
        static_cast<DWORD>(sizeof(kHistoryEntropy) - 1U);
    entropy.pbData = reinterpret_cast<BYTE*>(
        const_cast<char*>(kHistoryEntropy));
    TestLocalDataBlob encrypted;
    // productionと同じentropy・UI禁止条件で旧ciphertextを再現する。
    if (!CryptProtectData(
            &input,
            L"ZRdp SVN Bridge connection history",
            &entropy,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            encrypted.receive())) {
        SecureZeroMemory(plaintext.data(), plaintext.size());
        return false;
    }

    const DATA_BLOB& encrypted_value = encrypted.value();
    std::ofstream file(
        history_path,
        std::ios::binary |
        std::ios::out |
        std::ios::trunc);
    // 空のDPAPI出力を旧履歴fileとして保存しない。
    if (!file.is_open() ||
        encrypted_value.pbData == nullptr ||
        encrypted_value.cbData == 0U) {
        SecureZeroMemory(plaintext.data(), plaintext.size());
        return false;
    }

    file.write(
        reinterpret_cast<const char*>(
            encrypted_value.pbData),
        static_cast<std::streamsize>(
            encrypted_value.cbData));
    file.close();
    SecureZeroMemory(plaintext.data(), plaintext.size());
    return static_cast<bool>(file);
}

/**
 * @brief 名前fieldを持たないVersion 1履歴をcurrent-user DPAPIで作成する。
 * @param[in] history_path test専用出力file。
 * @param[in] server ASCIIだけで構成する文書用接続先。
 * @param[in] qualified_user_name ASCIIだけで構成する結合済みlogon識別子。
 * @return Version 1 ciphertextを完全に保存できた場合はtrue。
 */
[[nodiscard]] bool WriteLegacyHistoryFile(
    const std::filesystem::path& history_path,
    const std::string& server,
    const std::string& qualified_user_name) {
    // test helper自体がDWORDへ切り詰める入力を作らない。
    if (server.empty() ||
        server.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max()) ||
        qualified_user_name.size() >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }

    std::vector<std::uint8_t> plaintext;
    plaintext.insert(
        plaintext.end(),
        kLegacyHistoryMagic.begin(),
        kLegacyHistoryMagic.end());
    AppendUint32(plaintext, 1U);
    AppendUint32(plaintext, 1U);
    AppendUint32(
        plaintext,
        static_cast<std::uint32_t>(server.size()));
    AppendUint32(
        plaintext,
        static_cast<std::uint32_t>(
            qualified_user_name.size()));
    plaintext.insert(
        plaintext.end(),
        server.begin(),
        server.end());
    plaintext.insert(
        plaintext.end(),
        qualified_user_name.begin(),
        qualified_user_name.end());

    return WriteProtectedHistoryFile(
        history_path,
        plaintext);
}

/**
 * @brief 音声fieldを持たないVersion 2名前付き履歴を作成する。
 * @param[in] history_path test専用出力file。
 * @return Version 2 ciphertextを完全に保存できた場合はtrue。
 */
[[nodiscard]] bool WriteVersion2HistoryFile(
    const std::filesystem::path& history_path) {
    constexpr std::string_view connection_name =
        "Version 2 profile";
    constexpr std::string_view server = "198.51.100.52";
    constexpr std::string_view qualified_user_name =
        "EXAMPLE\\version2-user";

    std::vector<std::uint8_t> plaintext;
    plaintext.insert(
        plaintext.end(),
        kLegacyHistoryMagic.begin(),
        kLegacyHistoryMagic.end());
    AppendUint32(plaintext, 2U);
    AppendUint32(plaintext, 1U);
    AppendUint32(
        plaintext,
        static_cast<std::uint32_t>(
            connection_name.size()));
    AppendUint32(
        plaintext,
        static_cast<std::uint32_t>(server.size()));
    AppendUint32(
        plaintext,
        static_cast<std::uint32_t>(
            qualified_user_name.size()));
    plaintext.insert(
        plaintext.end(),
        connection_name.begin(),
        connection_name.end());
    plaintext.insert(
        plaintext.end(),
        server.begin(),
        server.end());
    plaintext.insert(
        plaintext.end(),
        qualified_user_name.begin(),
        qualified_user_name.end());
    return WriteProtectedHistoryFile(
        history_path,
        plaintext);
}

/**
 * @brief 不正microphone flagを持つVersion 3履歴を作成する。
 * @param[in] history_path test専用出力file。
 * @return malformed ciphertextを完全に保存できた場合はtrue。
 */
[[nodiscard]] bool WriteInvalidVersion3HistoryFile(
    const std::filesystem::path& history_path) {
    constexpr std::string_view connection_name =
        "Invalid Version 3";
    constexpr std::string_view server = "203.0.113.53";
    constexpr std::string_view qualified_user_name =
        "EXAMPLE\\invalid-version3-user";

    std::vector<std::uint8_t> plaintext;
    plaintext.insert(
        plaintext.end(),
        kLegacyHistoryMagic.begin(),
        kLegacyHistoryMagic.end());
    AppendUint32(plaintext, 3U);
    AppendUint32(plaintext, 1U);
    AppendUint32(
        plaintext,
        static_cast<std::uint32_t>(
            connection_name.size()));
    AppendUint32(
        plaintext,
        static_cast<std::uint32_t>(server.size()));
    AppendUint32(
        plaintext,
        static_cast<std::uint32_t>(
            qualified_user_name.size()));
    // boolとして許可する0／1を外し、deserializeのfail-closed境界を刺激する。
    AppendUint32(plaintext, 2U);
    AppendUint32(
        plaintext,
        static_cast<std::uint32_t>(
            zrdp::client::rdp::
                RdpAudioPlaybackMode::PlayOnClient));
    plaintext.insert(
        plaintext.end(),
        connection_name.begin(),
        connection_name.end());
    plaintext.insert(
        plaintext.end(),
        server.begin(),
        server.end());
    plaintext.insert(
        plaintext.end(),
        qualified_user_name.begin(),
        qualified_user_name.end());
    return WriteProtectedHistoryFile(
        history_path,
        plaintext);
}

/**
 * @brief test実行ファイルが置かれたディレクトリを取得する。
 * @return 完全なmodule pathを取得できた場合の親ディレクトリ。
 */
[[nodiscard]] std::filesystem::path TestExecutableDirectory() {
    std::array<wchar_t, MAX_PATH> module_path{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        module_path.data(),
        static_cast<DWORD>(module_path.size()));
    // 固定バッファで切り詰められたパスをtest削除対象として使用しない。
    if (length == 0U || length >= module_path.size()) {
        return {};
    }

    return std::filesystem::path(
        std::wstring(module_path.data(), length)).parent_path();
}

/**
 * @brief このtestが作成する履歴本体とprocess固有一時ファイルだけを除去する。
 * @param history_path test専用履歴ファイルの完全パス。
 */
void RemoveTestFiles(
    const std::filesystem::path& history_path) noexcept {
    std::error_code error;
    std::filesystem::remove(history_path, error);
    error.clear();
    std::filesystem::remove(
        history_path.wstring() +
            L"." +
            std::to_wstring(GetCurrentProcessId()) +
            L".tmp",
        error);
}

/**
 * @brief byte列に指定patternが含まれるか検査する。
 * @param contents 検査対象の暗号化ファイル内容。
 * @param pattern 平文漏えいを調べるbyte pattern。
 * @return patternが存在する場合はtrue。
 */
[[nodiscard]] bool ContainsBytes(
    const std::vector<std::uint8_t>& contents,
    const std::vector<std::uint8_t>& pattern) {
    // 空patternは標準searchと同じく常に一致とする。
    if (pattern.empty()) {
        return true;
    }

    return std::search(
               contents.begin(),
               contents.end(),
               pattern.begin(),
               pattern.end()) != contents.end();
}

/**
 * @brief UTF-16文字列のraw byte表現を平文漏えい検査用に作る。
 * @param value 変換するtest文字列。
 * @return wchar_t配列表現の所有byte列。
 */
[[nodiscard]] std::vector<std::uint8_t> Utf16Bytes(
    const std::wstring& value) {
    const auto* begin =
        reinterpret_cast<const std::uint8_t*>(value.data());
    return std::vector<std::uint8_t>(
        begin,
        begin + value.size() * sizeof(wchar_t));
}

/**
 * @brief worker通知先として使う非表示system-class windowを一つ所有する。
 */
class TestNotificationWindow final {
public:
    /// 生成済みtest windowを確実に破棄する。
    ~TestNotificationWindow() {
        // 有効な所有HWNDがある場合だけDestroyWindowを呼ぶ。
        if (window_ != nullptr) {
            DestroyWindow(window_);
        }
    }

    TestNotificationWindow(const TestNotificationWindow&) = delete;
    TestNotificationWindow& operator=(
        const TestNotificationWindow&) = delete;

    TestNotificationWindow() = default;

    /**
     * @brief メッセージ通知を受け取る非表示STATIC windowを生成する。
     * @return HWND生成に成功した場合はtrue。
     */
    [[nodiscard]] bool Create() noexcept {
        window_ = CreateWindowExW(
            0U,
            L"STATIC",
            L"",
            WS_OVERLAPPED,
            0,
            0,
            0,
            0,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        return window_ != nullptr;
    }

    /// workerへ渡す非所有HWNDを返す。
    [[nodiscard]] HWND get() const noexcept {
        return window_;
    }

private:
    /// このRAII型が所有するtest専用HWND。
    HWND window_{nullptr};
};

/**
 * @brief 指定種別のworker結果を有限時間だけpollして取得する。
 * @param worker 検証対象の直列履歴worker。
 * @param operation 待機する操作種別。
 * @param[out] completed 一致した所有結果。
 * @return 期限内に期待結果を取得できた場合はtrue。
 */
[[nodiscard]] bool WaitForHistoryResult(
    zrdp::client::app::ConnectionHistoryWorker& worker,
    const zrdp::client::app::ConnectionHistoryOperation operation,
    zrdp::client::app::ConnectionHistoryResult& completed) noexcept {
    // 通知messageをdispatchせず、短い有限時間だけ結果queueをpollする。
    for (unsigned int attempt = 0U; attempt < 200U; ++attempt) {
        std::vector<zrdp::client::app::ConnectionHistoryResult> results =
            worker.TakeResults();

        // 直列worker testが待っている操作だけを完了結果として選ぶ。
        for (auto& result : results) {
            // 別操作の完了結果を期待結果と取り違えない。
            if (result.operation == operation) {
                completed = std::move(result);
                return true;
            }
        }

        Sleep(5U);
    }

    return false;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief 接続履歴の重複排除、DPAPI永続化、旧version互換性を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::client::app::ConnectionHistoryOperation;
    using zrdp::client::app::ConnectionHistoryResult;
    using zrdp::client::app::ConnectionHistoryStore;
    using zrdp::client::app::ConnectionHistoryWorker;
    using zrdp::client::app::FormatQualifiedRdpUserName;
    using zrdp::client::app::NormalizeRdpUserIdentity;
    using zrdp::client::app::RdpUserIdentity;
    using zrdp::client::app::kMaximumConnectionHistoryEntries;
    using zrdp::client::app::kMaximumConnectionNameCharacters;

    // test専用ファイルを実行ファイルディレクトリ内へ限定する。
    const std::filesystem::path executable_directory =
        TestExecutableDirectory();
    // 対象ディレクトリを確定できなければファイル操作を開始しない。
    if (executable_directory.empty()) {
        return 1;
    }

    const std::filesystem::path history_path =
        executable_directory /
        (L"connection-history-test-" +
         std::to_wstring(GetCurrentProcessId()) +
         L".bin");
    RemoveTestFiles(history_path);

    // 存在しない履歴は正常な空状態としてS_FALSEを返す契約を確認する。
    ConnectionHistoryStore store(history_path);
    if (store.Load() != S_FALSE || !store.entries().empty()) {
        RemoveTestFiles(history_path);
        return 2;
    }

    // 最初の接続組を保存できることを確認する。
    if (FAILED(store.Remember(
            L"Primary profile",
            L"192.0.2.10",
            L"EXAMPLE",
            L"History-Test-User"))) {
        RemoveTestFiles(history_path);
        return 3;
    }

    // 大小文字だけが異なる組は同一とし、古い表記を新しい表記で置換する。
    if (FAILED(store.Remember(
            L"primary PROFILE",
            L"192.0.2.10",
            L"example",
            L"history-test-user")) ||
        store.entries().size() != 1U ||
        store.entries().front().connection_name !=
            L"primary PROFILE" ||
        store.entries().front().domain != L"example" ||
        store.entries().front().user_name !=
            L"history-test-user") {
        RemoveTestFiles(history_path);
        return 4;
    }

    // 同じ接続先でも別ユーザー名は独立した履歴組として保持する。
    if (FAILED(store.Remember(
            L"Reviewer profile",
            L"192.0.2.10",
            L"EXAMPLE",
            L"history-test-reviewer")) ||
        store.entries().size() != 2U) {
        RemoveTestFiles(history_path);
        return 5;
    }

    // 同じ接続先とユーザー名でもdomainが異なる組は独立して保持する。
    if (FAILED(store.Remember(
            L"Other domain profile",
            L"192.0.2.10",
            L"OTHER",
            L"history-test-user")) ||
        store.entries().size() != 3U) {
        RemoveTestFiles(history_path);
        return 18;
    }

    // 既存組の再利用は件数を増やさず、その組を最新位置へ移動する。
    if (FAILED(store.Remember(
            L"Primary profile",
            L"192.0.2.10",
            L"EXAMPLE",
            L"HISTORY-TEST-USER")) ||
        store.entries().size() != 3U ||
        store.entries().front().domain != L"EXAMPLE" ||
        store.entries().front().user_name !=
            L"HISTORY-TEST-USER") {
        RemoveTestFiles(history_path);
        return 6;
    }

    // 同じ非空接続名は別の接続組へ再割当てしても一件だけを保持する。
    if (FAILED(store.Remember(
            L"reviewer PROFILE",
            L"198.51.100.30",
            L"EXAMPLE",
            L"replacement-user")) ||
        store.entries().size() != 3U ||
        store.entries().front().connection_name !=
            L"reviewer PROFILE" ||
        store.entries().front().server != L"198.51.100.30") {
        RemoveTestFiles(history_path);
        return 29;
    }

    // 有限storeの上限を超える数の一意な組を追加し、古い項目の破棄を確認する。
    for (std::size_t index = 0U;
         index < kMaximumConnectionHistoryEntries + 2U;
         ++index) {
        const std::wstring server =
            L"203.0.113." + std::to_wstring(index + 1U);
        const std::wstring user =
            L"history-test-user-" + std::to_wstring(index + 1U);
        const std::wstring connection_name =
            L"Profile " + std::to_wstring(index + 1U);
        const zrdp::client::rdp::RdpAudioRedirectionSettings audio{
            index ==
                kMaximumConnectionHistoryEntries + 1U,
            index ==
                    kMaximumConnectionHistoryEntries + 1U
                ? zrdp::client::rdp::
                      RdpAudioPlaybackMode::DoNotPlay
                : zrdp::client::rdp::
                      RdpAudioPlaybackMode::PlayOnClient,
        };
        // 一件でも保存に失敗した場合は上限検証を継続しない。
        if (FAILED(store.Remember(
                connection_name,
                server,
                L"EXAMPLE",
                user,
                audio))) {
            RemoveTestFiles(history_path);
            return 7;
        }
    }

    // 件数上限と最新項目の順序を同時に確認する。
    if (store.entries().size() !=
            kMaximumConnectionHistoryEntries ||
        store.entries().front().server != L"203.0.113.12" ||
        store.entries().front().connection_name !=
            L"Profile 12" ||
        store.entries().front().domain != L"EXAMPLE" ||
        store.entries().front().user_name !=
            L"history-test-user-12" ||
        !store.entries().front().audio.redirect_microphone ||
        store.entries().front().audio.playback_mode !=
            zrdp::client::rdp::
                RdpAudioPlaybackMode::DoNotPlay) {
        RemoveTestFiles(history_path);
        return 8;
    }

    // 永続化した暗号化履歴を再読込して同じ最新順序を復元できることを確認する。
    ConnectionHistoryStore reloaded(history_path);
    if (FAILED(reloaded.Load()) ||
        reloaded.entries().size() !=
            kMaximumConnectionHistoryEntries ||
        reloaded.entries().front().server != L"203.0.113.12" ||
        reloaded.entries().front().connection_name !=
            L"Profile 12" ||
        reloaded.entries().front().domain != L"EXAMPLE" ||
        reloaded.entries().front().user_name !=
            L"history-test-user-12" ||
        !reloaded.entries().front().audio.redirect_microphone ||
        reloaded.entries().front().audio.playback_mode !=
            zrdp::client::rdp::
                RdpAudioPlaybackMode::DoNotPlay) {
        RemoveTestFiles(history_path);
        return 9;
    }

    // 保存ファイルにUTF-8またはUTF-16のユーザー名平文が残らないことを検査する。
    std::ifstream encrypted_file(history_path, std::ios::binary);
    const std::vector<std::uint8_t> encrypted{
        std::istreambuf_iterator<char>(encrypted_file),
        std::istreambuf_iterator<char>(),
    };
    encrypted_file.close();
    const std::string plaintext_utf8 = "history-test-user-12";
    const std::vector<std::uint8_t> plaintext_utf8_bytes(
        plaintext_utf8.begin(),
        plaintext_utf8.end());
    const std::string domain_plaintext_utf8 = "EXAMPLE";
    const std::vector<std::uint8_t> domain_plaintext_utf8_bytes(
        domain_plaintext_utf8.begin(),
        domain_plaintext_utf8.end());
    const std::string profile_plaintext_utf8 = "Profile 12";
    const std::vector<std::uint8_t> profile_plaintext_utf8_bytes(
        profile_plaintext_utf8.begin(),
        profile_plaintext_utf8.end());
    // 空file、接続名、ユーザー名、domainのUTF-8／UTF-16平文を許可しない。
    if (encrypted.empty() ||
        ContainsBytes(encrypted, profile_plaintext_utf8_bytes) ||
        ContainsBytes(encrypted, plaintext_utf8_bytes) ||
        ContainsBytes(encrypted, domain_plaintext_utf8_bytes) ||
        ContainsBytes(
            encrypted,
            Utf16Bytes(L"history-test-user-12")) ||
        ContainsBytes(encrypted, Utf16Bytes(L"Profile 12")) ||
        ContainsBytes(encrypted, Utf16Bytes(L"EXAMPLE"))) {
        RemoveTestFiles(history_path);
        return 10;
    }

    // 破損ciphertextへ置換し、復号失敗時に古いentriesを公開しないことを確認する。
    std::ofstream corrupt_file(
        history_path,
        std::ios::binary |
        std::ios::out |
        std::ios::trunc);
    const std::array<std::uint8_t, 4> corrupt_data{
        0x01U,
        0x02U,
        0x03U,
        0x04U,
    };
    corrupt_file.write(
        reinterpret_cast<const char*>(corrupt_data.data()),
        static_cast<std::streamsize>(corrupt_data.size()));
    corrupt_file.close();

    ConnectionHistoryStore corrupt(history_path);
    // malformed履歴はfail-closedとし、部分entryを返してはならない。
    if (SUCCEEDED(corrupt.Load()) || !corrupt.entries().empty()) {
        RemoveTestFiles(history_path);
        return 11;
    }

    RemoveTestFiles(history_path);
    int worker_failure = 0;
    {
        // UI threadをblockせずLoad/Saveするworker契約を非表示windowで検証する。
        TestNotificationWindow notification_window;
        // 通知windowを生成できなければworker検証へ進まない。
        if (!notification_window.Create()) {
            worker_failure = 12;
        } else {
            ConnectionHistoryWorker worker(history_path);
            // worker開始失敗は後続queue検証と区別する。
            if (FAILED(worker.Start(
                    notification_window.get(),
                    WM_APP + 42U))) {
                worker_failure = 13;
            }

            ConnectionHistoryResult load_result;
            // 初回Loadが空状態として完了し、有限時間内に取得できることを確認する。
            if (worker_failure == 0 &&
                (!WaitForHistoryResult(
                     worker,
                     ConnectionHistoryOperation::Load,
                     load_result) ||
                 load_result.result != S_FALSE ||
                 !load_result.entries.empty())) {
                worker_failure = 14;
            }

            // workerが一つの履歴保存要求を受理できることを確認する。
            if (worker_failure == 0 &&
                FAILED(worker.QueueRemember(
                    L"Worker profile",
                    L"198.51.100.25",
                    L"EXAMPLE",
                    L"history-worker-user",
                    {
                        true,
                        zrdp::client::rdp::
                            RdpAudioPlaybackMode::
                                PlayOnRemoteComputer,
                    }))) {
                worker_failure = 15;
            }

            ConnectionHistoryResult save_result;
            // Save完了結果が成功と最新snapshot一件を返すことを確認する。
            if (worker_failure == 0 &&
                (!WaitForHistoryResult(
                     worker,
                     ConnectionHistoryOperation::Save,
                     save_result) ||
                 FAILED(save_result.result) ||
                 save_result.entries.size() != 1U)) {
                worker_failure = 16;
            }
        }
    }

    // RAII破棄後に検出したworker失敗を、cleanupしてからそのまま返す。
    if (worker_failure != 0) {
        RemoveTestFiles(history_path);
        return worker_failure;
    }

    // worker保存結果がファイルから復元可能であることを最終確認する。
    ConnectionHistoryStore worker_reloaded(history_path);
    if (FAILED(worker_reloaded.Load()) ||
        worker_reloaded.entries().size() != 1U ||
        worker_reloaded.entries().front().server !=
            L"198.51.100.25" ||
        worker_reloaded.entries().front().connection_name !=
            L"Worker profile" ||
        worker_reloaded.entries().front().domain != L"EXAMPLE" ||
        worker_reloaded.entries().front().user_name !=
            L"history-worker-user" ||
        !worker_reloaded.entries().front().audio.redirect_microphone ||
        worker_reloaded.entries().front().audio.playback_mode !=
            zrdp::client::rdp::
                RdpAudioPlaybackMode::
                    PlayOnRemoteComputer) {
        RemoveTestFiles(history_path);
        return 17;
    }

    // 分離入力はfieldを変えず、そのまま正規形として受け付ける。
    RdpUserIdentity identity;
    if (FAILED(NormalizeRdpUserIdentity(
            L"EXAMPLE",
            L"identity-user",
            identity)) ||
        identity.domain != L"EXAMPLE" ||
        identity.user_name != L"identity-user") {
        RemoveTestFiles(history_path);
        return 19;
    }

    // 旧DOMAIN\user入力はdomain欄が空の場合だけ二fieldへ分離する。
    if (FAILED(NormalizeRdpUserIdentity(
            {},
            L"EXAMPLE\\legacy-user",
            identity)) ||
        identity.domain != L"EXAMPLE" ||
        identity.user_name != L"legacy-user") {
        RemoveTestFiles(history_path);
        return 20;
    }

    // UPN形式はdomainを推測せず、ユーザー名としてそのまま維持する。
    if (FAILED(NormalizeRdpUserIdentity(
            {},
            L"identity-user@example.test",
            identity)) ||
        !identity.domain.empty() ||
        identity.user_name != L"identity-user@example.test") {
        RemoveTestFiles(history_path);
        return 21;
    }

    // 分離domainと結合済みuserを同時指定する曖昧な入力を拒否する。
    if (NormalizeRdpUserIdentity(
            L"EXAMPLE",
            L"OTHER\\identity-user",
            identity) != E_INVALIDARG) {
        RemoveTestFiles(history_path);
        return 22;
    }

    // domainだけでは標準credential UIへ完全なlogon識別子を渡せない。
    if (NormalizeRdpUserIdentity(
            L"EXAMPLE",
            {},
            identity) != E_INVALIDARG) {
        RemoveTestFiles(history_path);
        return 23;
    }

    // 複数区切りを持つ旧形式を推測で分割しない。
    if (NormalizeRdpUserIdentity(
            {},
            L"EXAMPLE\\group\\identity-user",
            identity) != E_INVALIDARG) {
        RemoveTestFiles(history_path);
        return 24;
    }

    std::wstring qualified_user_name;
    if (FAILED(FormatQualifiedRdpUserName(
            RdpUserIdentity{
                L"EXAMPLE",
                L"identity-user",
            },
            qualified_user_name)) ||
        qualified_user_name != L"EXAMPLE\\identity-user") {
        RemoveTestFiles(history_path);
        return 25;
    }

    // 旧履歴上限ちょうどのdomain付き識別子を受け付ける。
    if (FAILED(NormalizeRdpUserIdentity(
            std::wstring(255U, L'd'),
            std::wstring(256U, L'u'),
            identity))) {
        RemoveTestFiles(history_path);
        return 26;
    }

    // 旧履歴上限を一文字超える識別子はmemory増加前に拒否する。
    if (NormalizeRdpUserIdentity(
            std::wstring(255U, L'd'),
            std::wstring(257U, L'u'),
            identity) != E_INVALIDARG) {
        RemoveTestFiles(history_path);
        return 27;
    }

    std::wstring embedded_null_user = L"identity";
    embedded_null_user.push_back(L'\0');
    embedded_null_user.append(L"hidden");
    // COM表示と異なるNUL後の識別子を履歴へ隠せないよう拒否する。
    if (NormalizeRdpUserIdentity(
            {},
            std::move(embedded_null_user),
            identity) != E_INVALIDARG) {
        RemoveTestFiles(history_path);
        return 28;
    }

    // 接続名上限ちょうどは保存でき、上限超過は保存前に拒否する。
    if (FAILED(worker_reloaded.Remember(
            std::wstring(
                kMaximumConnectionNameCharacters,
                L'n'),
            L"203.0.113.200",
            L"EXAMPLE",
            L"name-limit-user")) ||
        worker_reloaded.Remember(
            std::wstring(
                kMaximumConnectionNameCharacters + 1U,
                L'n'),
            L"203.0.113.201",
            L"EXAMPLE",
            L"name-overflow-user") != E_INVALIDARG) {
        RemoveTestFiles(history_path);
        return 30;
    }

    std::wstring embedded_null_name = L"visible-name";
    embedded_null_name.push_back(L'\0');
    embedded_null_name.append(L"hidden-name");
    // title表示と異なるNUL後のprofile名を暗号化履歴へ隠せないよう拒否する。
    if (worker_reloaded.Remember(
            std::move(embedded_null_name),
            L"203.0.113.202",
            L"EXAMPLE",
            L"name-null-user") != E_INVALIDARG) {
        RemoveTestFiles(history_path);
        return 31;
    }

    // 未知の音声再生先をVersion 3履歴またはActiveX設定へ持ち越さない。
    if (worker_reloaded.Remember(
            L"Invalid audio",
            L"203.0.113.203",
            L"EXAMPLE",
            L"invalid-audio-user",
            {
                true,
                static_cast<
                    zrdp::client::rdp::
                        RdpAudioPlaybackMode>(99U),
            }) != E_INVALIDARG) {
        RemoveTestFiles(history_path);
        return 36;
    }

    RemoveTestFiles(history_path);
    // Version 2の名前付きprofileは新規音声fieldを安全な既定値で補完する。
    if (!WriteVersion2HistoryFile(history_path)) {
        RemoveTestFiles(history_path);
        return 37;
    }

    ConnectionHistoryStore version2_store(history_path);
    if (FAILED(version2_store.Load()) ||
        version2_store.entries().size() != 1U ||
        version2_store.entries().front().connection_name !=
            L"Version 2 profile" ||
        version2_store.entries().front().audio.redirect_microphone ||
        version2_store.entries().front().audio.playback_mode !=
            zrdp::client::rdp::
                RdpAudioPlaybackMode::PlayOnClient) {
        RemoveTestFiles(history_path);
        return 38;
    }

    RemoveTestFiles(history_path);
    // 暗号化済みでも不正なVersion 3音声値を部分profileとして公開しない。
    if (!WriteInvalidVersion3HistoryFile(history_path)) {
        RemoveTestFiles(history_path);
        return 39;
    }

    ConnectionHistoryStore invalid_version3_store(
        history_path);
    if (SUCCEEDED(invalid_version3_store.Load()) ||
        !invalid_version3_store.entries().empty()) {
        RemoveTestFiles(history_path);
        return 40;
    }

    RemoveTestFiles(history_path);
    // Version 1の名前なし履歴を後方互換経路で復元する。
    if (!WriteLegacyHistoryFile(
            history_path,
            "192.0.2.44",
            "EXAMPLE\\legacy-profile-user")) {
        RemoveTestFiles(history_path);
        return 32;
    }

    ConnectionHistoryStore legacy_store(history_path);
    if (FAILED(legacy_store.Load()) ||
        legacy_store.entries().size() != 1U ||
        !legacy_store.entries().front().connection_name.empty() ||
        legacy_store.entries().front().server != L"192.0.2.44" ||
        legacy_store.entries().front().domain != L"EXAMPLE" ||
        legacy_store.entries().front().user_name !=
            L"legacy-profile-user" ||
        legacy_store.entries().front().audio.redirect_microphone ||
        legacy_store.entries().front().audio.playback_mode !=
            zrdp::client::rdp::
                RdpAudioPlaybackMode::PlayOnClient) {
        RemoveTestFiles(history_path);
        return 33;
    }

    // 旧entryを保持したまま名前付きentryを追加し、Version 3へ更新できる。
    if (FAILED(legacy_store.Remember(
            L"Named profile",
            L"198.51.100.44",
            L"EXAMPLE",
            L"named-profile-user"))) {
        RemoveTestFiles(history_path);
        return 34;
    }

    ConnectionHistoryStore migrated_store(history_path);
    if (FAILED(migrated_store.Load()) ||
        migrated_store.entries().size() != 2U ||
        migrated_store.entries().front().connection_name !=
            L"Named profile" ||
        !migrated_store.entries().back().connection_name.empty()) {
        RemoveTestFiles(history_path);
        return 35;
    }

    RemoveTestFiles(history_path);
    return 0;
}
