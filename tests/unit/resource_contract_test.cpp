/**
 * @file resource_contract_test.cpp
 * @brief 配布EXEがbranding iconと主要UI文字列を持つresource契約を検証する。
 *
 * EXEを実行せずresource dataとして開き、Client／Agent共通icon、domain欄、
 * 表示mode、全画面切替、複数session表示、接続時音声設定の欠落を検出する。
 */

#include "client/app/resource.h"

#include <Windows.h>

#include <array>
#include <string_view>

namespace {

/**
 * @brief resource dataとして開いたmoduleを一意所有するRAII型。
 */
class ScopedResourceModule final {
public:
    /**
     * @brief LoadLibraryExWが返した所有moduleを受け取る。
     * @param module 解放対象のmodule。
     */
    explicit ScopedResourceModule(const HMODULE module) noexcept
        : module_(module) {}

    /// 所有moduleをFreeLibraryで解放する。
    ~ScopedResourceModule() {
        // 有効なmoduleを所有している場合だけ参照数を解放する。
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    ScopedResourceModule(const ScopedResourceModule&) = delete;
    ScopedResourceModule& operator=(const ScopedResourceModule&) = delete;

private:
    /// LoadLibraryExWから受け取った所有module。
    HMODULE module_{nullptr};
};

/**
 * @brief moduleに共通ZRdp icon groupが埋め込まれているか検証する。
 * @param[in] module resource dataとして開いたClientまたはAgent。
 * @return icon groupと実dataが存在する場合はtrue。
 */
[[nodiscard]] bool HasApplicationIcon(
    const HMODULE module) noexcept {
    const HRSRC resource = FindResourceW(
        module,
        MAKEINTRESOURCEW(IDI_ZRDP_APP),
        RT_GROUP_ICON);
    // group entryだけでなく有限のresource dataを持つことを必須とする。
    if (resource == nullptr) {
        return false;
    }

    return SizeofResource(module, resource) > 0U;
}

/**
 * @brief 一つのstring resource IDと期待する利用者向け表示を対応付ける。
 */
struct ExpectedResourceString {
    UINT identifier;             ///< 検証対象のstring resource ID。
    std::wstring_view expected;  ///< 完全一致させる固定表示。
};

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Client UI文字列とClient／Agent共通iconを検証する。
 * @param[in] argument_count EXE pathを含む引数個数。
 * @param[in] arguments 第1引数Client、第2引数AgentのEXE path。
 * @return 契約を満たす場合は0。それ以外は失敗段階を表す固定code。
 */
int wmain(
    const int argument_count,
    wchar_t* arguments[]) {
    // CTestからClient／Agentの二つが指定された場合に限定する。
    if (argument_count != 3 ||
        arguments[1] == nullptr ||
        arguments[2] == nullptr) {
        return 1;
    }

    const HMODULE client_module = LoadLibraryExW(
        arguments[1],
        nullptr,
        LOAD_LIBRARY_AS_DATAFILE |
            LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    // Clientを実行せずresourceとして開けない場合は検証対象として不成立とする。
    if (client_module == nullptr) {
        return 2;
    }

    const ScopedResourceModule client_module_owner(
        client_module);
    const HMODULE agent_module = LoadLibraryExW(
        arguments[2],
        nullptr,
        LOAD_LIBRARY_AS_DATAFILE |
            LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    // Agent側のicon resourceも独立して検査する。
    if (agent_module == nullptr) {
        return 3;
    }

    const ScopedResourceModule agent_module_owner(
        agent_module);
    // Explorer表示とwindow表示の共通iconが両配布物へ存在することを必須とする。
    if (!HasApplicationIcon(client_module) ||
        !HasApplicationIcon(agent_module)) {
        return 4;
    }

    constexpr std::array<ExpectedResourceString, 20> expectations{{
        {IDS_CONNECTION_NAME_LABEL, L"接続名:"},
        {IDS_DOMAIN_LABEL, L"ドメイン:"},
        {IDS_DISPLAY_MODE_LABEL, L"表示モード:"},
        {IDS_DISPLAY_MODE_NATIVE, L"等倍表示（スクロール）"},
        {
            IDS_DISPLAY_MODE_SMART_SIZING,
            L"Smart Sizing（縮小表示）",
        },
        {
            IDS_DISPLAY_MODE_DYNAMIC_RESOLUTION,
            L"動的解像度（Window追従）",
        },
        {
            IDS_FULL_SCREEN,
            L"全画面",
        },
        {
            IDS_WINDOW_MODE,
            L"ウィンドウ",
        },
        {
            IDS_PROXY_STOPPED,
            L"Proxy: 停止",
        },
        {
            IDS_CONNECTION_MANAGER_TITLE,
            L"ZRdp 接続マネージャー (0.7.20)",
        },
        {
            IDS_CONNECT_NEW_SESSION,
            L"新しい接続",
        },
        {
            IDS_MANAGER_STATUS_FORMAT,
            L"接続中セッション: %u / 次のProxy: 127.0.0.1:%u",
        },
        {
            IDS_SESSION_TITLE_FORMAT,
            L"ZRdp SVN Bridge 0.7.20 - %s | RDP:%s | Agent:%s | Proxy:%s 127.0.0.1:%u | stream=%u | C→A=%llu | A→C=%llu",
        },
        {
            IDS_LOCAL_RESOURCES_LABEL,
            L"ローカル リソース:",
        },
        {
            IDS_REDIRECT_MICROPHONE,
            L"マイクを接続先で使用する",
        },
        {
            IDS_REMOTE_AUDIO_LABEL,
            L"リモート音声:",
        },
        {
            IDS_AUDIO_PLAY_ON_CLIENT,
            L"このPCで再生する",
        },
        {
            IDS_AUDIO_PLAY_ON_REMOTE,
            L"接続先PCで再生する",
        },
        {
            IDS_AUDIO_DO_NOT_PLAY,
            L"再生しない",
        },
        {
            IDS_STATUS_PRE_LOGIN_RETRY,
            L"RDP: 一時的な認証接続失敗を検出したため、しばらく待って再試行します | SVN Bridge: 停止",
        },
    }};

    // 接続管理とsession生成を含む主要表示を同じ完全一致契約で検証する。
    for (const ExpectedResourceString& expectation : expectations) {
        std::array<wchar_t, 256> text{};
        const int length = LoadStringW(
            client_module,
            expectation.identifier,
            text.data(),
            static_cast<int>(text.size()));
        // 設定文字列が欠落またはbuffer上限を超える状態を許可しない。
        if (length <= 0 ||
            static_cast<std::size_t>(length) >= text.size()) {
            return 5;
        }

        const std::wstring_view actual(
            text.data(),
            static_cast<std::size_t>(length));
        // 表示modeの意味が曖昧な旧文字列へ戻らないよう完全一致させる。
        if (actual != expectation.expected) {
            return 6;
        }
    }

    return 0;
}
