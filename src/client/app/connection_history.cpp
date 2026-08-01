/**
 * @file connection_history.cpp
 * @brief RDP接続履歴の検証、serialize、DPAPI保護、atomic保存を実装する。
 *
 * @details 平文履歴はprocess memory内だけで扱い、fileへ書く前にcurrent-user
 *          DPAPIで暗号化する。file I/OとDPAPIは専用workerが直列実行する。
 */
#include "client/app/connection_history.h"

#include <ShlObj.h>
#include <Wincrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace zrdp::client::app {
namespace {

// -----------------------------------------------------------------------------
// 接続履歴の永続化形式とresource上限
// -----------------------------------------------------------------------------

/// format識別と将来version誤読防止に使用する8-byte magic。
constexpr std::array<std::uint8_t, 8> kHistoryMagic{
    'Z', 'R', 'D', 'P', 'H', 'S', 'T', '1',
};
/// 名前を持たない既存履歴を読込むために受理する旧format version。
constexpr std::uint32_t kLegacyHistoryVersion = 1U;
/// 任意の接続名を追加した既存format version。
constexpr std::uint32_t kNamedProfileHistoryVersion = 2U;
/// 接続時音声設定を追加した現在の平文format version。
constexpr std::uint32_t kCurrentHistoryVersion = 3U;
constexpr std::size_t kMaximumEncryptedFileBytes = 64U * 1024U;
constexpr std::size_t kMaximumPlaintextBytes = 32U * 1024U;
constexpr std::size_t kMaximumServerCharacters = 255U;
constexpr std::size_t kMaximumStoredUserNameCharacters =
    kMaximumQualifiedRdpUserNameCharacters;
constexpr std::size_t kMaximumServerUtf8Bytes =
    kMaximumServerCharacters * 4U;
constexpr std::size_t kMaximumConnectionNameUtf8Bytes =
    kMaximumConnectionNameCharacters * 4U;
constexpr std::size_t kMaximumStoredUserNameUtf8Bytes =
    kMaximumStoredUserNameCharacters * 4U;
constexpr char kDpapiEntropy[] =
    "ZRdpSvnBridge.ConnectionHistory.v1";
constexpr wchar_t kHistoryDirectoryName[] = L"ZRdpSvnBridge";
constexpr wchar_t kHistoryFileName[] = L"connection-history.bin";

// -----------------------------------------------------------------------------
// Win32 resourceのRAII wrapper
// -----------------------------------------------------------------------------

/**
 * @brief DPAPIがLocalAllocしたDATA_BLOBを一意所有する。
 *
 * @note 復号済み平文を所有する場合は、LocalFree前にSecureZeroMemoryを行う。
 */
class LocalDataBlob final {
public:
    /**
     * @brief 空の受信blobを生成する。
     * @param[in] clear_on_release 解放前に内容をzero clearする場合はtrue。
     */
    explicit LocalDataBlob(const bool clear_on_release) noexcept
        : clear_on_release_(clear_on_release) {}

    /**
     * @brief 必要に応じて内容を消去し、LocalFreeでDPAPI bufferを解放する。
     */
    ~LocalDataBlob() {
        if (blob_.pbData != nullptr) {
            // 復号済み履歴の平文をallocatorへ返す前にmemoryから除去する。
            if (clear_on_release_ && blob_.cbData > 0U) {
                SecureZeroMemory(blob_.pbData, blob_.cbData);
            }

            LocalFree(blob_.pbData);
        }
    }

    LocalDataBlob(const LocalDataBlob&) = delete;
    LocalDataBlob& operator=(const LocalDataBlob&) = delete;

    /**
     * @brief DPAPI出力先として空blobのaddressを渡す。
     * @return CryptProtectData／CryptUnprotectData用DATA_BLOB pointer。
     */
    [[nodiscard]] DATA_BLOB* receive() noexcept {
        return &blob_;
    }

    /**
     * @brief DPAPIが設定したblobをread-only参照する。
     * @return LocalDataBlobが所有するDATA_BLOB。
     */
    [[nodiscard]] const DATA_BLOB& value() const noexcept {
        return blob_;
    }

private:
    /// LocalFreeで解放するDPAPI割当てbuffer。
    DATA_BLOB blob_{};

    /// blobが平文を含み、解放前消去を必要とする場合はtrue。
    bool clear_on_release_{false};
};

/**
 * @brief history worker threadのCOM MTA初期化をscopeで管理する。
 */
class ScopedComApartment final {
public:
    /**
     * @brief current worker threadをCOM MTAとして初期化する。
     */
    ScopedComApartment() noexcept
        : result_(CoInitializeEx(
              nullptr,
              COINIT_MULTITHREADED)) {}

    /**
     * @brief 初期化成功時だけ対応するCoUninitializeを実行する。
     */
    ~ScopedComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    ScopedComApartment(const ScopedComApartment&) = delete;
    ScopedComApartment& operator=(const ScopedComApartment&) = delete;

    /**
     * @brief COM初期化結果を取得する。
     * @return CoInitializeExが返したHRESULT。
     */
    [[nodiscard]] HRESULT result() const noexcept {
        return result_;
    }

private:
    /// CoUninitializeの要否とworker操作結果の基準となる初期化結果。
    HRESULT result_{E_UNEXPECTED};
};

// -----------------------------------------------------------------------------
// error・文字列検証helper
// -----------------------------------------------------------------------------

/**
 * @brief current threadのLastErrorを失敗HRESULTへ変換する。
 * @return LastErrorが未設定ならE_FAIL、それ以外は対応するHRESULT。
 */
[[nodiscard]] HRESULT LastErrorResult() noexcept {
    const DWORD error = GetLastError();
    return error == ERROR_SUCCESS
        ? E_FAIL
        : HRESULT_FROM_WIN32(error);
}

/**
 * @brief 外部fileまたは復号dataの不正を表す共通HRESULTを生成する。
 * @return ERROR_INVALID_DATA由来のHRESULT。
 */
[[nodiscard]] HRESULT InvalidDataResult() noexcept {
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
}

/**
 * @brief Win32 APIへ渡す文字列に埋込みNULがあるか検査する。
 * @param[in] value 終端を含まない文字列view。
 * @return 埋込みNULを含む場合はtrue。
 */
[[nodiscard]] bool ContainsEmbeddedNull(
    const std::wstring_view value) noexcept {
    return value.find(L'\0') != std::wstring_view::npos;
}

/**
 * @brief Windows ordinal規則で二つの文字列を大小文字無視比較する。
 * @param[in] left 比較対象。
 * @param[in] right 比較対象。
 * @return 同一の場合はtrue。
 */
[[nodiscard]] bool EqualCaseInsensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    // 文字数が異なる値はWindows APIを呼ばずに不一致とする。
    if (left.size() != right.size()) {
        return false;
    }

    if (left.empty()) {
        return true;
    }

    // CompareStringOrdinalのint長へ安全に変換できない入力は拒否する。
    if (left.size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max()) ||
        right.size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max())) {
        return false;
    }

    return CompareStringOrdinal(
               left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

/**
 * @brief 接続先、ドメイン、ユーザー名の組を大小文字無視で比較する。
 * @param[in] left 比較する履歴entry。
 * @param[in] right 比較する履歴entry。
 * @return 3 fieldすべてが同一の場合はtrue。
 */
[[nodiscard]] bool EqualEntry(
    const ConnectionHistoryEntry& left,
    const ConnectionHistoryEntry& right) noexcept {
    return EqualCaseInsensitive(left.server, right.server) &&
        EqualCaseInsensitive(left.domain, right.domain) &&
        EqualCaseInsensitive(left.user_name, right.user_name);
}

/**
 * @brief 二つの非空接続名を大小文字無視で比較する。
 * @param[in] left 比較する履歴entry。
 * @param[in] right 比較する履歴entry。
 * @return 両方が名前付きで同名の場合はtrue。
 */
[[nodiscard]] bool EqualNamedProfile(
    const ConnectionHistoryEntry& left,
    const ConnectionHistoryEntry& right) noexcept {
    return !left.connection_name.empty() &&
        !right.connection_name.empty() &&
        EqualCaseInsensitive(
            left.connection_name,
            right.connection_name);
}

/**
 * @brief UTF-16文字列をstrict UTF-8へ変換する。
 * @param[in] value 変換元の所有しないview。
 * @param[out] encoded 変換成功時のUTF-8 bytes。
 * @return 変換結果を表すHRESULT。
 */
[[nodiscard]] HRESULT EncodeUtf8(
    const std::wstring_view value,
    std::string& encoded) {
    encoded.clear();
    if (value.empty()) {
        return S_OK;
    }

    // WideCharToMultiByteのint長へ変換不能な入力は処理しない。
    if (value.size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max())) {
        return E_INVALIDARG;
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return LastErrorResult();
    }

    encoded.resize(static_cast<std::size_t>(required));
    const int written = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        encoded.data(),
        required,
        nullptr,
        nullptr);
    if (written != required) {
        encoded.clear();
        return LastErrorResult();
    }

    return S_OK;
}

/**
 * @brief strict UTF-8 bytesをUTF-16文字列へ変換する。
 * @param[in] encoded 終端を持たないUTF-8 bytes。
 * @param[out] value 変換成功時のUTF-16文字列。
 * @return 不正UTF-8をInvalidDataとして扱うHRESULT。
 */
[[nodiscard]] HRESULT DecodeUtf8(
    const std::span<const std::uint8_t> encoded,
    std::wstring& value) {
    value.clear();
    if (encoded.empty()) {
        return S_OK;
    }

    // MultiByteToWideCharのint長へ変換不能なfile dataは拒否する。
    if (encoded.size() > static_cast<std::size_t>(
            std::numeric_limits<int>::max())) {
        return InvalidDataResult();
    }

    const auto* text =
        reinterpret_cast<const char*>(encoded.data());
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text,
        static_cast<int>(encoded.size()),
        nullptr,
        0);
    if (required <= 0) {
        return InvalidDataResult();
    }

    value.resize(static_cast<std::size_t>(required));
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text,
        static_cast<int>(encoded.size()),
        value.data(),
        required);
    if (written != required) {
        value.clear();
        return InvalidDataResult();
    }

    return S_OK;
}

// -----------------------------------------------------------------------------
// 明示的little-endian serialize／deserialize
// -----------------------------------------------------------------------------

/**
 * @brief 32-bit整数を明示的little-endianで末尾へ追加する。
 * @param[in,out] output 追記先buffer。
 * @param[in] value encodeする値。
 */
void AppendUint32(
    std::vector<std::uint8_t>& output,
    const std::uint32_t value) {
    // compilerの構造体layoutやhost endianに依存しない形式で4 byteを出力する。
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(
            static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

/**
 * @brief offset位置から明示的little-endian 32-bit整数を読む。
 * @param[in] input 検証対象buffer。
 * @param[in,out] offset 読込み開始位置。成功時は4 byte進める。
 * @param[out] value decodeした値。
 * @return 完全な4 byteを読めた場合はtrue。
 */
[[nodiscard]] bool ReadUint32(
    const std::span<const std::uint8_t> input,
    std::size_t& offset,
    std::uint32_t& value) noexcept {
    constexpr std::size_t encoded_size = sizeof(std::uint32_t);
    // subtraction前にoffsetを検証し、wraparoundと範囲外readを防止する。
    if (offset > input.size() ||
        encoded_size > input.size() - offset) {
        return false;
    }

    value = 0U;
    // host endianに依存せず1 byteずつ値を再構成する。
    for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
        value |=
            static_cast<std::uint32_t>(input[offset++]) << shift;
    }

    return true;
}

/**
 * @brief 検証済みprofile entry群を現在の平文形式へserializeする。
 * @param[in] entries 新しい順のbounded履歴。
 * @param[out] plaintext DPAPIへ渡す平文buffer。
 * @return encode、length、全体size検証の結果を表すHRESULT。
 */
[[nodiscard]] HRESULT Serialize(
    const std::vector<ConnectionHistoryEntry>& entries,
    std::vector<std::uint8_t>& plaintext) {
    plaintext.clear();
    plaintext.reserve(kMaximumPlaintextBytes);
    plaintext.insert(
        plaintext.end(),
        kHistoryMagic.begin(),
        kHistoryMagic.end());
    AppendUint32(plaintext, kCurrentHistoryVersion);
    AppendUint32(
        plaintext,
        static_cast<std::uint32_t>(entries.size()));

    // 新しい順を維持し、domainとuserは従来どおり一つのfieldへ結合する。
    for (const ConnectionHistoryEntry& entry : entries) {
        // serializer単独でも接続名の文字数とNUL境界を越えたstateを保存しない。
        if (entry.connection_name.size() >
                kMaximumConnectionNameCharacters ||
            ContainsEmbeddedNull(entry.connection_name)) {
            return E_INVALIDARG;
        }

        // ActiveXへ渡せない未知の音声値を永続化して次回接続へ持ち越さない。
        if (!rdp::IsValidAudioPlaybackMode(
                entry.audio.playback_mode)) {
            return E_INVALIDARG;
        }

        std::string connection_name;
        HRESULT result = EncodeUtf8(
            entry.connection_name,
            connection_name);
        if (FAILED(result)) {
            return result;
        }

        std::string server;
        result = EncodeUtf8(entry.server, server);
        if (FAILED(result)) {
            return result;
        }

        std::wstring qualified_user_name;
        result = FormatQualifiedRdpUserName(
            RdpUserIdentity{
                entry.domain,
                entry.user_name,
            },
            qualified_user_name);
        if (FAILED(result)) {
            return result;
        }

        std::string user_name;
        result = EncodeUtf8(qualified_user_name, user_name);
        if (FAILED(result)) {
            return result;
        }

        // 平文formatへ入る前にもfield単位のbyte上限を強制する。
        if (connection_name.size() >
                kMaximumConnectionNameUtf8Bytes ||
            server.empty() ||
            server.size() > kMaximumServerUtf8Bytes ||
            user_name.size() > kMaximumStoredUserNameUtf8Bytes) {
            return E_INVALIDARG;
        }

        AppendUint32(
            plaintext,
            static_cast<std::uint32_t>(
                connection_name.size()));
        AppendUint32(
            plaintext,
            static_cast<std::uint32_t>(server.size()));
        AppendUint32(
            plaintext,
            static_cast<std::uint32_t>(user_name.size()));
        AppendUint32(
            plaintext,
            entry.audio.redirect_microphone ? 1U : 0U);
        AppendUint32(
            plaintext,
            static_cast<std::uint32_t>(
                entry.audio.playback_mode));
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
            user_name.begin(),
            user_name.end());

        // entry数だけでなく実際の合計byte数も有限に保つ。
        if (plaintext.size() > kMaximumPlaintextBytes) {
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        }
    }

    return S_OK;
}

/**
 * @brief 復号済みVersion 1～3形式を検証し、profileをdeserializeする。
 * @param[in] plaintext DPAPIから得た平文bytes。
 * @param[out] entries 成功時だけ置換する新しい順のentry群。
 * @return format、version、length、UTF-8検証の結果を表すHRESULT。
 */
[[nodiscard]] HRESULT Deserialize(
    const std::span<const std::uint8_t> plaintext,
    std::vector<ConnectionHistoryEntry>& entries) {
    entries.clear();
    // header未満または上限超過の平文を解析対象にしない。
    if (plaintext.size() < kHistoryMagic.size() + 8U ||
        plaintext.size() > kMaximumPlaintextBytes) {
        return InvalidDataResult();
    }

    // 固定magicの全byteを比較し、別formatの誤読を防止する。
    for (std::size_t index = 0U;
         index < kHistoryMagic.size();
         ++index) {
        // 一byteでも異なるfileはZRdp履歴として扱わない。
        if (plaintext[index] != kHistoryMagic[index]) {
            return InvalidDataResult();
        }
    }

    std::size_t offset = kHistoryMagic.size();
    std::uint32_t version = 0U;
    std::uint32_t count = 0U;
    // 切れたheaderと過剰件数はentry用memoryを確保する前に拒否する。
    if (!ReadUint32(plaintext, offset, version) ||
        !ReadUint32(plaintext, offset, count) ||
        count > kMaximumConnectionHistoryEntries) {
        return InvalidDataResult();
    }

    // Version 1／2だけを後方互換で受理し、未知versionは誤読しない。
    if (version != kLegacyHistoryVersion &&
        version != kNamedProfileHistoryVersion &&
        version != kCurrentHistoryVersion) {
        return InvalidDataResult();
    }

    std::vector<ConnectionHistoryEntry> decoded;
    decoded.reserve(count);

    // 宣言件数を走査し、各組のbyte数・文字数・重複を検証する。
    for (std::uint32_t index = 0U; index < count; ++index) {
        std::uint32_t connection_name_bytes = 0U;
        std::uint32_t server_bytes = 0U;
        std::uint32_t user_name_bytes = 0U;
        std::uint32_t redirect_microphone = 0U;
        std::uint32_t audio_playback_mode =
            static_cast<std::uint32_t>(
                rdp::RdpAudioPlaybackMode::PlayOnClient);

        // Version 2以降だけが先頭の任意接続名長を持つ。
        if (version != kLegacyHistoryVersion &&
            !ReadUint32(
                plaintext,
                offset,
                connection_name_bytes)) {
            return InvalidDataResult();
        }

        if (!ReadUint32(plaintext, offset, server_bytes) ||
            !ReadUint32(plaintext, offset, user_name_bytes)) {
            return InvalidDataResult();
        }

        // Version 3だけが接続時音声設定を持ち、旧版は安全な既定値へ移行する。
        if (version == kCurrentHistoryVersion &&
            (!ReadUint32(
                 plaintext,
                 offset,
                 redirect_microphone) ||
             !ReadUint32(
                 plaintext,
                 offset,
                 audio_playback_mode))) {
            return InvalidDataResult();
        }

        const auto playback_mode =
            static_cast<rdp::RdpAudioPlaybackMode>(
                audio_playback_mode);
        // bool表現とenumを完全一致で検証し、不正profileをActiveXへ渡さない。
        if (redirect_microphone > 1U ||
            !rdp::IsValidAudioPlaybackMode(playback_mode)) {
            return InvalidDataResult();
        }

        // 全fieldを有限上限へ制限し、接続先の空値を許可しない。
        if (connection_name_bytes >
                kMaximumConnectionNameUtf8Bytes ||
            server_bytes == 0U ||
            server_bytes > kMaximumServerUtf8Bytes ||
            user_name_bytes > kMaximumStoredUserNameUtf8Bytes) {
            return InvalidDataResult();
        }

        const std::size_t combined_length =
            static_cast<std::size_t>(
                connection_name_bytes) +
            static_cast<std::size_t>(server_bytes) +
            static_cast<std::size_t>(user_name_bytes);
        // 三つのbounded field長を使う前に残りbuffer内へ収まることを確認する。
        if (offset > plaintext.size() ||
            combined_length > plaintext.size() - offset) {
            return InvalidDataResult();
        }

        ConnectionHistoryEntry entry;
        entry.audio.redirect_microphone =
            redirect_microphone != 0U;
        entry.audio.playback_mode = playback_mode;
        HRESULT result = DecodeUtf8(
            plaintext.subspan(
                offset,
                connection_name_bytes),
            entry.connection_name);
        if (FAILED(result)) {
            return result;
        }
        offset += connection_name_bytes;

        result = DecodeUtf8(
            plaintext.subspan(offset, server_bytes),
            entry.server);
        if (FAILED(result)) {
            return result;
        }
        offset += server_bytes;

        std::wstring qualified_user_name;
        result = DecodeUtf8(
            plaintext.subspan(offset, user_name_bytes),
            qualified_user_name);
        if (FAILED(result)) {
            return result;
        }
        offset += user_name_bytes;

        RdpUserIdentity identity;
        result = NormalizeRdpUserIdentity(
            {},
            std::move(qualified_user_name),
            identity);
        // DPAPI平文内の曖昧なlogon識別子を部分的に公開しない。
        if (result == E_INVALIDARG) {
            return InvalidDataResult();
        }
        if (FAILED(result)) {
            return result;
        }

        entry.domain = std::move(identity.domain);
        entry.user_name = std::move(identity.user_name);

        // UTF-16変換後も文字数と埋込みNULのsecurity boundaryを再検証する。
        if (entry.connection_name.size() >
                kMaximumConnectionNameCharacters ||
            entry.server.empty() ||
            entry.server.size() > kMaximumServerCharacters ||
            ContainsEmbeddedNull(entry.connection_name) ||
            ContainsEmbeddedNull(entry.server) ||
            ContainsEmbeddedNull(entry.domain) ||
            ContainsEmbeddedNull(entry.user_name)) {
            return InvalidDataResult();
        }

        const bool duplicate = std::ranges::any_of(
            decoded,
            [&entry](const ConnectionHistoryEntry& existing) {
                return EqualEntry(existing, entry) ||
                    EqualNamedProfile(existing, entry);
            });
        // 重複接続組または同名profileは最初の新しいentryだけを保持する。
        if (!duplicate) {
            decoded.push_back(std::move(entry));
        }
    }

    // 宣言entry後の余剰dataを許可せずformat差異を明示的に拒否する。
    if (offset != plaintext.size()) {
        return InvalidDataResult();
    }

    entries = std::move(decoded);
    return S_OK;
}

// -----------------------------------------------------------------------------
// current-user DPAPI境界
// -----------------------------------------------------------------------------

/**
 * @brief ZRdp履歴用途へDPAPI ciphertextを束縛するoptional entropyを生成する。
 * @return static文字列を参照する非所有DATA_BLOB。
 */
[[nodiscard]] DATA_BLOB EntropyBlob() noexcept {
    DATA_BLOB entropy{};
    entropy.cbData =
        static_cast<DWORD>(sizeof(kDpapiEntropy) - 1U);
    entropy.pbData = reinterpret_cast<BYTE*>(
        const_cast<char*>(kDpapiEntropy));
    return entropy;
}

/**
 * @brief serialize済み平文をcurrent-user DPAPIで暗号化する。
 * @param[in] plaintext 暗号化するbounded平文。
 * @param[out] encrypted fileへ保存可能なciphertext。
 * @return DPAPIと出力size検証の結果を表すHRESULT。
 */
[[nodiscard]] HRESULT Protect(
    const std::span<const std::uint8_t> plaintext,
    std::vector<std::uint8_t>& encrypted) {
    // DPAPIのDWORD長へ変換可能なbounded平文だけを受け付ける。
    if (plaintext.empty() ||
        plaintext.size() > static_cast<std::size_t>(
            std::numeric_limits<DWORD>::max())) {
        return E_INVALIDARG;
    }

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(plaintext.size());
    input.pbData = const_cast<BYTE*>(
        reinterpret_cast<const BYTE*>(plaintext.data()));
    DATA_BLOB entropy = EntropyBlob();
    LocalDataBlob protected_blob(false);
    // background処理中にUI promptを発生させずcurrent-user scopeで保護する。
    if (!CryptProtectData(
            &input,
            L"ZRdp SVN Bridge connection history",
            &entropy,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            protected_blob.receive())) {
        return LastErrorResult();
    }

    const DATA_BLOB& value = protected_blob.value();
    // DPAPI成功後も空または異常に大きいciphertextをfileへ保存しない。
    if (value.pbData == nullptr ||
        value.cbData == 0U ||
        value.cbData > kMaximumEncryptedFileBytes) {
        return InvalidDataResult();
    }

    encrypted.assign(value.pbData, value.pbData + value.cbData);
    return S_OK;
}

/**
 * @brief current-user DPAPI ciphertextを復号して履歴formatを検証する。
 * @param[in] encrypted fileから読んだciphertext。
 * @param[out] entries 復号・検証成功時の履歴。
 * @return DPAPIまたはformat検証の結果を表すHRESULT。
 *
 * 復号平文はLocalDataBlobがscope終了時にzero clearする。
 */
[[nodiscard]] HRESULT Unprotect(
    const std::span<const std::uint8_t> encrypted,
    std::vector<ConnectionHistoryEntry>& entries) {
    // file sizeとDPAPI DWORD長の両上限を満たさない入力は復号しない。
    if (encrypted.empty() ||
        encrypted.size() > kMaximumEncryptedFileBytes ||
        encrypted.size() > static_cast<std::size_t>(
            std::numeric_limits<DWORD>::max())) {
        return InvalidDataResult();
    }

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(encrypted.size());
    input.pbData = const_cast<BYTE*>(
        reinterpret_cast<const BYTE*>(encrypted.data()));
    DATA_BLOB entropy = EntropyBlob();
    LocalDataBlob plaintext_blob(true);
    // user操作を要求せず、同一Windowsユーザーだけが復号できる形で処理する。
    if (!CryptUnprotectData(
            &input,
            nullptr,
            &entropy,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            plaintext_blob.receive())) {
        return LastErrorResult();
    }

    const DATA_BLOB& value = plaintext_blob.value();
    // 復号結果の上限をdeserialize前にも検証し、memory消費を有限に保つ。
    if (value.pbData == nullptr ||
        value.cbData == 0U ||
        value.cbData > kMaximumPlaintextBytes) {
        return InvalidDataResult();
    }

    return Deserialize(
        std::span<const std::uint8_t>(
            value.pbData,
            value.cbData),
        entries);
}

// -----------------------------------------------------------------------------
// bounded file I/Oとatomic置換
// -----------------------------------------------------------------------------

/**
 * @brief 暗号化履歴fileをsize検証して全量読込む。
 * @param[in] path 読込み対象のlocal file。
 * @param[out] encrypted 成功時のciphertext。
 * @retval S_FALSE fileが存在しない。
 * @return 読込みまたはsize検証結果を表すHRESULT。
 */
[[nodiscard]] HRESULT ReadEncryptedFile(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& encrypted) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return HRESULT_FROM_WIN32(error.value());
    }

    // 初回起動時のfile不在はerrorではなく空履歴として扱う。
    if (!exists) {
        return S_FALSE;
    }

    const std::uintmax_t file_size =
        std::filesystem::file_size(path, error);
    if (error) {
        return HRESULT_FROM_WIN32(error.value());
    }

    // 空fileと上限超過fileはDPAPIへ渡さず破損dataとして拒否する。
    if (file_size == 0U ||
        file_size > kMaximumEncryptedFileBytes) {
        return InvalidDataResult();
    }

    encrypted.resize(static_cast<std::size_t>(file_size));
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
    }

    file.read(
        reinterpret_cast<char*>(encrypted.data()),
        static_cast<std::streamsize>(encrypted.size()));
    // short readを成功扱いせず、未初期化部分の復号を防止する。
    if (!file ||
        file.gcount() != static_cast<std::streamsize>(encrypted.size())) {
        encrypted.clear();
        return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
    }

    return S_OK;
}

/**
 * @brief ciphertextを同一directoryの一時file経由でatomic置換する。
 * @param[in] path 最終保存先。
 * @param[in] encrypted 保存するDPAPI ciphertext。
 * @return directory作成、write、置換の結果を表すHRESULT。
 */
[[nodiscard]] HRESULT WriteEncryptedFileAtomically(
    const std::filesystem::path& path,
    const std::span<const std::uint8_t> encrypted) {
    std::error_code error;
    const std::filesystem::path directory = path.parent_path();
    std::filesystem::create_directories(directory, error);
    if (error) {
        return HRESULT_FROM_WIN32(error.value());
    }

    const std::filesystem::path temporary_path =
        path.wstring() +
        L"." +
        std::to_wstring(GetCurrentProcessId()) +
        L".tmp";
    std::ofstream temporary_file(
        temporary_path,
        std::ios::binary |
        std::ios::out |
        std::ios::trunc);
    if (!temporary_file.is_open()) {
        return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
    }

    temporary_file.write(
        reinterpret_cast<const char*>(encrypted.data()),
        static_cast<std::streamsize>(encrypted.size()));
    temporary_file.flush();
    const bool write_succeeded =
        static_cast<bool>(temporary_file);
    temporary_file.close();
    // 不完全な一時fileは削除し、既存の有効fileを置換しない。
    if (!write_succeeded) {
        std::filesystem::remove(temporary_path, error);
        return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }

    // write-through付きatomic置換で途中状態を読込み側へ公開しない。
    if (!MoveFileExW(
            temporary_path.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const HRESULT result = LastErrorResult();
        std::filesystem::remove(temporary_path, error);
        return result;
    }

    return S_OK;
}

}  // namespace

// -----------------------------------------------------------------------------
// ConnectionHistoryStoreの同期永続化
// -----------------------------------------------------------------------------

ConnectionHistoryStore::ConnectionHistoryStore(
    std::filesystem::path storage_path)
    : storage_path_(std::move(storage_path)) {}

HRESULT ConnectionHistoryStore::Load() noexcept {
    try {
        // 失敗時に古い復号済みentryを残さないよう、処理開始時に公開stateを空にする。
        entries_.clear();

        // path解決、bounded read、DPAPI復号の順に段階的に検証する。
        std::filesystem::path resolved_path;
        HRESULT result = ResolveStoragePath(resolved_path);
        if (FAILED(result)) {
            return result;
        }

        std::vector<std::uint8_t> encrypted;
        result = ReadEncryptedFile(resolved_path, encrypted);
        // file不在は空履歴としてそのままcallerへ伝える。
        if (result == S_FALSE || FAILED(result)) {
            return result;
        }

        std::vector<ConnectionHistoryEntry> loaded;
        result = Unprotect(encrypted, loaded);
        if (FAILED(result)) {
            return result;
        }

        entries_ = std::move(loaded);
        return S_OK;
    } catch (...) {
        // allocation／filesystem例外をHRESULTへ変換し、平文stateを公開しない。
        entries_.clear();
        return E_UNEXPECTED;
    }
}

HRESULT ConnectionHistoryStore::Remember(
    std::wstring connection_name,
    std::wstring server,
    std::wstring domain,
    std::wstring user_name,
    const rdp::RdpAudioRedirectionSettings audio) noexcept {
    try {
        // 接続名は任意だが、DPAPI平文とtitle表示を有限かつNULなしに保つ。
        if (connection_name.size() >
                kMaximumConnectionNameCharacters ||
            ContainsEmbeddedNull(connection_name)) {
            return E_INVALIDARG;
        }

        // 永続化前に接続先の文字数、必須値、埋込みNULを検証する。
        if (server.empty() ||
            server.size() > kMaximumServerCharacters ||
            ContainsEmbeddedNull(server)) {
            return E_INVALIDARG;
        }

        // 永続化した未知値で将来のRDP接続設定を曖昧にしない。
        if (!rdp::IsValidAudioPlaybackMode(
                audio.playback_mode)) {
            return E_INVALIDARG;
        }

        RdpUserIdentity identity;
        const HRESULT identity_result = NormalizeRdpUserIdentity(
            std::move(domain),
            std::move(user_name),
            identity);
        // 曖昧なlogon識別子をDPAPI平文へ取り込まない。
        if (FAILED(identity_result)) {
            return identity_result;
        }

        ConnectionHistoryEntry incoming{
            std::move(connection_name),
            std::move(server),
            std::move(identity.domain),
            std::move(identity.user_name),
            audio,
        };
        // 同じ接続組または同じ非空名を除去し、新しい定義を最新位置へ置く。
        std::erase_if(
            entries_,
            [&incoming](const ConnectionHistoryEntry& existing) {
                return EqualEntry(existing, incoming) ||
                    EqualNamedProfile(existing, incoming);
            });
        entries_.insert(entries_.begin(), std::move(incoming));
        // 永続化件数とprocess内平文memoryを同じ有限上限へ揃える。
        if (entries_.size() > kMaximumConnectionHistoryEntries) {
            entries_.resize(kMaximumConnectionHistoryEntries);
        }

        return Save();
    } catch (...) {
        // public noexcept境界からallocation／filesystem例外を漏らさない。
        return E_UNEXPECTED;
    }
}

const std::vector<ConnectionHistoryEntry>&
ConnectionHistoryStore::entries() const noexcept {
    return entries_;
}

HRESULT ConnectionHistoryStore::Save() {
    // path解決後に全entryを一つのversion付きbufferへserializeする。
    std::filesystem::path resolved_path;
    HRESULT result = ResolveStoragePath(resolved_path);
    if (FAILED(result)) {
        return result;
    }

    std::vector<std::uint8_t> plaintext;
    result = Serialize(entries_, plaintext);
    if (FAILED(result)) {
        return result;
    }

    std::vector<std::uint8_t> encrypted;
    result = Protect(plaintext, encrypted);
    // DPAPI成功／失敗にかかわらず、不要になった平文bufferを直ちに消去する。
    if (!plaintext.empty()) {
        SecureZeroMemory(plaintext.data(), plaintext.size());
    }
    if (FAILED(result)) {
        return result;
    }

    return WriteEncryptedFileAtomically(resolved_path, encrypted);
}

HRESULT ConnectionHistoryStore::ResolveStoragePath(
    std::filesystem::path& resolved_path) const {
    // testが明示したpathはproduction profile解決を行わずそのまま使用する。
    if (!storage_path_.empty()) {
        resolved_path = storage_path_;
        return S_OK;
    }

    PWSTR local_app_data = nullptr;
    // OSが管理するcurrent-user LocalAppDataを使用し、環境変数文字列へ依存しない。
    const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData,
        KF_FLAG_CREATE,
        nullptr,
        &local_app_data);
    if (FAILED(result)) {
        return result;
    }

    // path構築中に例外が発生してもCOM allocatorのbufferを必ず解放する。
    try {
        resolved_path =
            std::filesystem::path(local_app_data) /
            kHistoryDirectoryName /
            kHistoryFileName;
    } catch (...) {
        CoTaskMemFree(local_app_data);
        throw;
    }

    CoTaskMemFree(local_app_data);
    return S_OK;
}

// -----------------------------------------------------------------------------
// ConnectionHistoryWorkerの非同期実行
// -----------------------------------------------------------------------------

ConnectionHistoryWorker::ConnectionHistoryWorker(
    std::filesystem::path storage_path)
    : store_(std::move(storage_path)) {}

ConnectionHistoryWorker::~ConnectionHistoryWorker() {
    Stop();
}

HRESULT ConnectionHistoryWorker::Start(
    const HWND notification_window,
    const UINT notification_message) noexcept {
    // 生存中のcaller所有windowとprivate application messageだけを通知先にする。
    if (!IsWindow(notification_window) ||
        notification_message < WM_APP ||
        notification_message > 0xBFFFU) {
        return E_INVALIDARG;
    }

    try {
        std::scoped_lock lock(mutex_);
        // lifecycleを一意に保ち、二つのworker thread生成を防止する。
        if (started_) {
            return S_FALSE;
        }

        requests_.push_back(Request{
            ConnectionHistoryOperation::Load,
            {},
            {},
            {},
            {},
            {},
        });
        notification_window_ = notification_window;
        notification_message_ = notification_message;
        stopping_ = false;
        started_ = true;
        // thread生成失敗時はStart前のstateへrollbackする。
        try {
            worker_ = std::thread(
                &ConnectionHistoryWorker::WorkerLoop,
                this);
        } catch (...) {
            started_ = false;
            notification_window_ = nullptr;
            notification_message_ = 0U;
            requests_.clear();
            throw;
        }

        return S_OK;
    } catch (...) {
        // public noexcept境界からthread／allocation例外を漏らさない。
        return E_UNEXPECTED;
    }
}

HRESULT ConnectionHistoryWorker::QueueRemember(
    std::wstring connection_name,
    std::wstring server,
    std::wstring domain,
    std::wstring user_name,
    const rdp::RdpAudioRedirectionSettings audio) noexcept {
    try {
        {
            std::scoped_lock lock(mutex_);
            // 停止済みworkerへ永続化要求を受理すると所有dataが未処理になる。
            if (!started_ || stopping_) {
                return HRESULT_FROM_WIN32(ERROR_NOT_READY);
            }

            // callback連打でも平文request memoryが無制限に増えないよう上限を強制する。
            if (requests_.size() >= kMaximumPendingRequests) {
                return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
            }

            requests_.push_back(Request{
                ConnectionHistoryOperation::Save,
                std::move(connection_name),
                std::move(server),
                std::move(domain),
                std::move(user_name),
                audio,
            });
        }

        request_ready_.notify_one();
        return S_OK;
    } catch (...) {
        // queue登録中のallocation例外を固定HRESULTへ変換する。
        return E_UNEXPECTED;
    }
}

std::vector<ConnectionHistoryResult>
ConnectionHistoryWorker::TakeResults() noexcept {
    try {
        std::vector<ConnectionHistoryResult> completed;
        std::scoped_lock lock(mutex_);
        completed.reserve(results_.size());

        // UIがworker所有stateを参照しないよう、全snapshotの所有権を移動する。
        while (!results_.empty()) {
            completed.push_back(std::move(results_.front()));
            results_.pop_front();
        }

        return completed;
    } catch (...) {
        // UI側へ例外を漏らさず、結果なしとして次の通知を待てるようにする。
        return {};
    }
}

void ConnectionHistoryWorker::Stop() noexcept {
    try {
        {
            std::scoped_lock lock(mutex_);
            // 未開始または停止済みobjectへの重複Stopは何も行わない。
            if (!started_) {
                return;
            }

            stopping_ = true;
            notification_window_ = nullptr;
            notification_message_ = 0U;
        }

        request_ready_.notify_one();
        // 受理済みrequestのdrainが完了するまでworker終了を待つ。
        if (worker_.joinable()) {
            worker_.join();
        }

        std::scoped_lock lock(mutex_);
        started_ = false;
        stopping_ = false;
        requests_.clear();
        results_.clear();
    } catch (...) {
        // destructorからWin32 application境界へ例外を漏らさない。
    }
}

void ConnectionHistoryWorker::WorkerLoop() noexcept {
    try {
        const ScopedComApartment com_apartment;

        // LoadとSaveを一つのworkerで直列化し、storeのmutable stateを共有しない。
        while (true) {
            Request request;
            {
                std::unique_lock lock(mutex_);
                request_ready_.wait(
                    lock,
                    [this]() {
                        return stopping_ || !requests_.empty();
                    });

                // Stop前に受理した全requestをdrainしてからthreadを終了する。
                if (stopping_ && requests_.empty()) {
                    break;
                }

                request = std::move(requests_.front());
                requests_.pop_front();
            }

            ConnectionHistoryResult completed;
            completed.operation = request.operation;
            // COM初期化失敗時はDPAPI／store処理へ進まず同じerrorを公開する。
            if (FAILED(com_apartment.result())) {
                completed.result = com_apartment.result();
            } else if (request.operation ==
                ConnectionHistoryOperation::Load) {
                completed.result = store_.Load();
            } else {
                completed.result = store_.Remember(
                    std::move(request.connection_name),
                    std::move(request.server),
                    std::move(request.domain),
                    std::move(request.user_name),
                    request.audio);
            }
            completed.entries = store_.entries();

            HWND notification_window = nullptr;
            UINT notification_message = 0U;
            {
                std::scoped_lock lock(mutex_);
                // UI dispatch遅延時も新しいsnapshotだけを有限件数保持する。
                if (results_.size() >= kMaximumPendingResults) {
                    results_.pop_front();
                }

                results_.push_back(std::move(completed));
                notification_window = notification_window_;
                notification_message = notification_message_;
            }

            // Stopが通知先を無効化した後は、破棄中windowへmessageを送らない。
            if (notification_window != nullptr) {
                static_cast<void>(PostMessageW(
                    notification_window,
                    notification_message,
                    0U,
                    0L));
            }
        }
    } catch (...) {
        // store method外で発生するallocation例外の最終thread境界。
    }
}

}  // namespace zrdp::client::app
