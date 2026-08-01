/**
 * @file rdp_host_smoke_test.cpp
 * @brief 非表示Win32環境でRDP ActiveX生成と接続前設定の契約を検証する。
 *
 * 実際のConnectは行わず、文書用IPv4アドレスだけを設定する。実環境へのRDP
 * 通信やcredential promptを発生させないsmoke testとする。
 */

#include "client/rdp/rdp_control_host.h"

#include <Windows.h>

#include <atlbase.h>
#include <atlhost.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <thread>
#include <utility>

namespace {

/**
 * @brief ATL ActiveX host機能のprocess単位初期化を所有するtest module。
 */
class ZRdpTestAtlModule final
    : public ATL::CAtlExeModuleT<ZRdpTestAtlModule> {};

/// test process全体でATL module lifetimeを保持する。
ZRdpTestAtlModule g_atl_module;

/**
 * @brief 非表示smoke-test hostの存続中にRDP callbackを安全に破棄する。
 */
class DiscardingObserver final
    : public zrdp::client::rdp::IRdpEventObserver {
public:
    /**
     * @brief 所有copy化済みcallbackを受け取り、内容を参照せず破棄する。
     * @param event RdpControlHostから移動されたtest event。
     */
    void OnRdpEvent(zrdp::client::rdp::RdpEvent event) override {
        static_cast<void>(std::move(event));
    }
};

/// test専用STAへActiveX破棄を要求するthread message。
constexpr UINT kMessageDestroyIsolatedHost = WM_APP + 1U;

/// 別STAが応答可能か確認するtest thread message。
constexpr UINT kMessagePingIsolatedHost = WM_APP + 2U;

/**
 * @brief 一つの非接続RDP ActiveXを専用STAで生成し、test commandを処理する。
 */
struct IsolatedHostThreadState {
    std::atomic<DWORD> thread_id{0U};
    std::promise<HRESULT> ready;
    std::promise<void> destroyed;
    std::promise<void> pinged;
};

/**
 * @brief ActiveXを一つの専用STAに閉じ込め、破棄と応答確認をmessage化する。
 * @param[in,out] state main test threadとの有限同期状態。
 */
void RunIsolatedHostThread(
    IsolatedHostThreadState& state) noexcept {
    state.thread_id.store(
        GetCurrentThreadId(),
        std::memory_order_release);

    // main threadがPostThreadMessageを行う前に、このSTAのmessage queueを生成する。
    MSG queue_probe{};
    static_cast<void>(
        PeekMessageW(
            &queue_probe,
            nullptr,
            WM_USER,
            WM_USER,
            PM_NOREMOVE));

    const HRESULT com_result =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED);
    // COM初期化失敗時はActiveX生成へ進まず、待機中testを解放する。
    if (FAILED(com_result)) {
        state.ready.set_value(com_result);
        return;
    }

    DiscardingObserver observer;
    zrdp::client::rdp::RdpControlHost host(observer);
    const HWND parent = CreateWindowExW(
        0U,
        L"STATIC",
        L"ZRdp isolated STA test parent",
        WS_OVERLAPPED,
        0,
        0,
        800,
        600,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr);
    HRESULT create_result =
        parent == nullptr
            ? HRESULT_FROM_WIN32(GetLastError())
            : host.Create(parent, RECT{0, 0, 800, 600});
    state.ready.set_value(create_result);

    // 生成成功時だけ専用message loopを維持し、他STAの処理と直列化しない。
    while (SUCCEEDED(create_result)) {
        MSG message{};
        const BOOL result =
            GetMessageW(
                &message,
                nullptr,
                0U,
                0U);
        if (result <= 0) {
            break;
        }

        if (message.message ==
            kMessageDestroyIsolatedHost) {
            host.Destroy();
            state.destroyed.set_value();
            continue;
        }

        if (message.message ==
            kMessagePingIsolatedHost) {
            state.pinged.set_value();
            continue;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    host.Destroy();
    // ActiveX child解放後だけtest親Windowを同じ所有STAで破棄する。
    if (parent != nullptr) {
        DestroyWindow(parent);
    }

    CoUninitialize();
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief RDP ActiveXの生成、破棄、複数STA session分離をsmoke検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::client::rdp::CanUpdateSessionDisplaySettings;
    using zrdp::client::rdp::IsCredentialPromptCancellation;
    using zrdp::client::rdp::RdpConnectionState;

    constexpr long kCredentialPromptCancellationReason = 0x1F07L;

    // transport接続だけではremote desktopが未初期化のため動的更新を許可しない。
    if (CanUpdateSessionDisplaySettings(
            RdpConnectionState::Connected)) {
        return 36;
    }

    // OnLoginComplete後だけを動的解像度APIの呼出し可能状態とする。
    if (!CanUpdateSessionDisplaySettings(
            RdpConnectionState::LoggedIn)) {
        return 37;
    }

    // 再接続中は旧session寸法を変更せず、完了eventによる状態復元を待つ。
    if (CanUpdateSessionDisplaySettings(
            RdpConnectionState::Reconnecting)) {
        return 38;
    }

    // 接続処理中の完全一致reasonをcredential UIの利用者取消として扱う。
    if (!IsCredentialPromptCancellation(
            RdpConnectionState::Connecting,
            kCredentialPromptCancellationReason,
            0L,
            false)) {
        return 21;
    }

    // transport接続後でもLoginComplete前なら同じcredential取消経路を許可する。
    if (!IsCredentialPromptCancellation(
            RdpConnectionState::Connected,
            kCredentialPromptCancellationReason,
            0L,
            false)) {
        return 22;
    }

    // LoginComplete後の同じ数値を通常の切断errorから除外しない。
    if (IsCredentialPromptCancellation(
            RdpConnectionState::LoggedIn,
            kCredentialPromptCancellationReason,
            0L,
            false)) {
        return 23;
    }

    // 未知または別原因のdisconnect reasonを利用者取消へ近似しない。
    if (IsCredentialPromptCancellation(
            RdpConnectionState::Connecting,
            kCredentialPromptCancellationReason - 1L,
            0L,
            false)) {
        return 24;
    }

    // 拡張切断理由がある接続失敗は通常の診断対象として維持する。
    if (IsCredentialPromptCancellation(
            RdpConnectionState::Connecting,
            kCredentialPromptCancellationReason,
            1L,
            false)) {
        return 25;
    }

    // ZRdp自身が要求した切断をcredential prompt取消と重複分類しない。
    if (IsCredentialPromptCancellation(
            RdpConnectionState::Connecting,
            kCredentialPromptCancellationReason,
            0L,
            true)) {
        return 26;
    }

    // ActiveXを所有するtest threadをCOM STAとして初期化する。
    const HRESULT com_result =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // COM初期化失敗時はATL/ActiveX操作へ進まない。
    if (FAILED(com_result)) {
        return 1;
    }

    int result = 0;
    // ATL ActiveX containmentをprocessへ登録してからhostを生成する。
    if (!ATL::AtlAxWinInit()) {
        result = 2;
    } else {
        const HWND parent = CreateWindowExW(
            0U,
            L"STATIC",
            L"ZRdp test parent",
            WS_OVERLAPPED,
            0,
            0,
            800,
            600,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        // 親window生成失敗時はActiveX hostを作らない。
        if (parent == nullptr) {
            result = 3;
        } else {
            const HWND second_parent = CreateWindowExW(
                0U,
                L"STATIC",
                L"ZRdp second test parent",
                WS_OVERLAPPED,
                0,
                0,
                800,
                600,
                nullptr,
                nullptr,
                GetModuleHandleW(nullptr),
                nullptr);
            bool second_created = false;
            {
                DiscardingObserver observer;
                DiscardingObserver second_observer;
                zrdp::client::rdp::RdpControlHost host(observer);
                zrdp::client::rdp::RdpControlHost second_host(
                    second_observer);
                const RECT bounds{0, 0, 800, 600};
                const HRESULT create_result = host.Create(parent, bounds);
                // 対応Microsoft Controlを生成できなければ設定検証を中止する。
                if (FAILED(create_result)) {
                    result = 4;
                } else if (second_parent == nullptr) {
                    result = 28;
                } else {
                    // 同じCOM STA上の別Windowへ二つ目のActiveXを同時生成する。
                    const HRESULT second_create_result =
                        second_host.Create(
                            second_parent,
                            bounds);
                    second_created =
                        SUCCEEDED(second_create_result) &&
                        host.window() !=
                            second_host.window() &&
                        IsWindow(second_host.window());
                    // 同一STA内の二つのsessionがActiveX childを共有する状態を拒否する。
                    if (!second_created) {
                        result = 29;
                    }
                }

                if (SUCCEEDED(create_result)) {
                    // 二つ目を維持したまま一つ目の既存接続前契約を回帰確認する。
                    // 空接続先は接続前設定のsecurity boundaryで拒否される。
                    zrdp::client::rdp::RdpConnectionSettings invalid_settings{};
                    if (host.ApplyConnectionSettings(invalid_settings) != E_INVALIDARG) {
                        result = 5;
                    }

                    zrdp::client::rdp::RdpConnectionSettings valid_settings{};
                    // remote desktopを維持する等倍scrollを既定modeとする。
                    if (valid_settings.display_mode !=
                        zrdp::client::rdp::RdpDisplayMode::NativeScroll) {
                        result = 6;
                    }

                    // privacy既定はマイクOFF、音声はClient側再生とする。
                    if (valid_settings.audio.redirect_microphone ||
                        valid_settings.audio.playback_mode !=
                            zrdp::client::rdp::
                                RdpAudioPlaybackMode::PlayOnClient) {
                        result = 32;
                    }

                    // 実通信せず、文書用接続先への有効な接続前設定だけを適用する。
                    valid_settings.server = L"192.0.2.1";
                    if (FAILED(host.ApplyConnectionSettings(valid_settings))) {
                        result = 7;
                    }

                    // DomainとUserNameを別propertyへ設定する接続前契約を確認する。
                    valid_settings.domain = L"EXAMPLE";
                    valid_settings.user_name = L"test-user";
                    if (FAILED(host.ApplyConnectionSettings(valid_settings))) {
                        result = 15;
                    }

                    // domainなしへ戻す設定でも前回値を明示的にclearできる。
                    valid_settings.domain.clear();
                    if (FAILED(host.ApplyConnectionSettings(valid_settings))) {
                        result = 20;
                    }

                    // 専用設定modelでは旧DOMAIN\user結合値を受け付けない。
                    valid_settings.user_name = L"EXAMPLE\\test-user";
                    if (host.ApplyConnectionSettings(valid_settings) != E_INVALIDARG) {
                        result = 16;
                    }

                    // Domainだけを指定した不完全なlogon識別子を拒否する。
                    valid_settings.domain = L"EXAMPLE";
                    valid_settings.user_name.clear();
                    if (host.ApplyConnectionSettings(valid_settings) != E_INVALIDARG) {
                        result = 17;
                    }

                    // COMへ渡すdomain上限を超える入力を接続前に拒否する。
                    valid_settings.domain.assign(256U, L'd');
                    valid_settings.user_name = L"test-user";
                    if (host.ApplyConnectionSettings(valid_settings) != E_INVALIDARG) {
                        result = 18;
                    }

                    valid_settings.domain = L"EXAMPLE";
                    valid_settings.domain.push_back(L'\0');
                    valid_settings.domain.append(L"hidden");
                    // ActiveX表示と異なるNUL後のdomainを隠せないよう拒否する。
                    if (host.ApplyConnectionSettings(valid_settings) != E_INVALIDARG) {
                        result = 19;
                    }

                    valid_settings.domain = L"EXAMPLE";
                    valid_settings.user_name = L"test-user";
                    // 接続先上限を超える入力は表示用に切り詰めて受け入れない。
                    valid_settings.server.assign(256U, L'a');
                    if (host.ApplyConnectionSettings(valid_settings) != E_INVALIDARG) {
                        result = 8;
                    }

                    // NUL後の非表示部分をconnection barへ隠せる接続先は拒否する。
                    valid_settings.server = L"test-target";
                    valid_settings.server.push_back(L'\0');
                    valid_settings.server.append(L"hidden");
                    if (host.ApplyConnectionSettings(valid_settings) != E_INVALIDARG) {
                        result = 9;
                    }

                    // 接続前設定ではClient側だけを縮小するSmart Sizingも受け付ける。
                    valid_settings.server = L"192.0.2.1";
                    valid_settings.display_mode =
                        zrdp::client::rdp::RdpDisplayMode::SmartSizing;
                    if (FAILED(host.ApplyConnectionSettings(valid_settings))) {
                        result = 10;
                    }

                    // 動的解像度はIMsRdpClient9を利用できるControlで接続前設定できる。
                    valid_settings.display_mode =
                        zrdp::client::rdp::RdpDisplayMode::DynamicResolution;
                    if (FAILED(host.ApplyConnectionSettings(valid_settings))) {
                        result = 11;
                    }

                    valid_settings.audio.redirect_microphone = true;
                    valid_settings.audio.playback_mode =
                        zrdp::client::rdp::RdpAudioPlaybackMode::
                            PlayOnRemoteComputer;
                    // マイク転送と接続先PC側の音声再生を接続前に設定できる。
                    if (FAILED(host.ApplyConnectionSettings(valid_settings))) {
                        result = 33;
                    }

                    valid_settings.audio.playback_mode =
                        zrdp::client::rdp::
                            RdpAudioPlaybackMode::DoNotPlay;
                    // 音声を再生しない選択も明示的な第三modeとして受け付ける。
                    if (FAILED(host.ApplyConnectionSettings(valid_settings))) {
                        result = 34;
                    }

                    valid_settings.audio.playback_mode =
                        static_cast<
                            zrdp::client::rdp::
                                RdpAudioPlaybackMode>(99U);
                    // 未知の音声modeはActiveX固有値へ暗黙変換せず拒否する。
                    if (host.ApplyConnectionSettings(valid_settings) !=
                        E_INVALIDARG) {
                        result = 35;
                    }
                    valid_settings.audio = {};

                    // 未知のenum値は暗黙に等倍表示へ変換せず拒否する。
                    valid_settings.display_mode =
                        static_cast<zrdp::client::rdp::RdpDisplayMode>(99U);
                    if (host.ApplyConnectionSettings(valid_settings) != E_INVALIDARG) {
                        result = 12;
                    }

                    // 実行時切替は確立済みRDP sessionだけに限定する。
                    if (host.SetSmartSizing(true) !=
                        HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) {
                        result = 13;
                    }

                    // remote desktop寸法の動的更新も未接続状態では拒否する。
                    if (host.UpdateSessionDisplaySettings(800L, 600L) !=
                        HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) {
                        result = 14;
                    }

                    constexpr std::array<std::byte, 1> test_chunk{
                        std::byte{0U},
                    };
                    // 登録済みでも未接続時のSVC送信はfail-closedにする。
                    if (host.SendStaticVirtualChannelData(test_chunk) !=
                        HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) {
                        result = 27;
                    }
                }

                // 一つ目のsession破棄が二つ目のActiveX lifetimeへ波及しないことを確認する。
                host.Destroy();
                if (second_created &&
                    !IsWindow(second_host.window()) &&
                    result == 0) {
                    result = 30;
                }

                if (second_created) {
                    zrdp::client::rdp::RdpConnectionSettings
                        remaining_settings{};
                    remaining_settings.server =
                        L"198.51.100.1";
                    // 残存sessionが破棄後も接続前設定を受け付けることを確認する。
                    if (FAILED(
                            second_host.ApplyConnectionSettings(
                                remaining_settings)) &&
                        result == 0) {
                        result = 31;
                    }
                }

                second_host.Destroy();
            }

            // ActiveX childを先に破棄してから二つのtest親Windowを解放する。
            if (second_parent != nullptr) {
                DestroyWindow(second_parent);
            }

            DestroyWindow(parent);
        }

        IsolatedHostThreadState first_isolated;
        IsolatedHostThreadState second_isolated;
        std::future<HRESULT> first_ready =
            first_isolated.ready.get_future();
        std::future<HRESULT> second_ready =
            second_isolated.ready.get_future();
        std::future<void> first_destroyed =
            first_isolated.destroyed.get_future();
        std::future<void> second_pinged =
            second_isolated.pinged.get_future();
        std::thread first_thread(
            RunIsolatedHostThread,
            std::ref(first_isolated));
        std::thread second_thread(
            RunIsolatedHostThread,
            std::ref(second_isolated));
        constexpr auto kThreadTestTimeout =
            std::chrono::seconds(5);
        const bool first_started =
            first_ready.wait_for(kThreadTestTimeout) ==
                std::future_status::ready &&
            SUCCEEDED(first_ready.get());
        const bool second_started =
            second_ready.wait_for(kThreadTestTimeout) ==
                std::future_status::ready &&
            SUCCEEDED(second_ready.get());
        // 専用STAを二つとも生成できない環境ではisolation Gateを通過させない。
        if ((!first_started || !second_started) &&
            result == 0) {
            result = 39;
        }

        if (first_started && second_started) {
            static_cast<void>(
                PostThreadMessageW(
                    first_isolated.thread_id.load(
                        std::memory_order_acquire),
                    kMessageDestroyIsolatedHost,
                    0U,
                    0L));
            // 一方のActiveX破棄完了後も他方のSTAへmessageを送れることを確認する。
            if (first_destroyed.wait_for(
                    kThreadTestTimeout) !=
                    std::future_status::ready) {
                if (result == 0) {
                    result = 40;
                }
            } else {
                static_cast<void>(
                    PostThreadMessageW(
                        second_isolated.thread_id.load(
                            std::memory_order_acquire),
                        kMessagePingIsolatedHost,
                        0U,
                        0L));
                if (second_pinged.wait_for(
                        kThreadTestTimeout) !=
                        std::future_status::ready &&
                    result == 0) {
                    result = 41;
                }
            }
        }

        // test結果にかかわらず両STAを起床してからjoinし、残留threadを作らない。
        static_cast<void>(
            PostThreadMessageW(
                first_isolated.thread_id.load(
                    std::memory_order_acquire),
                WM_QUIT,
                0U,
                0L));
        static_cast<void>(
            PostThreadMessageW(
                second_isolated.thread_id.load(
                    std::memory_order_acquire),
                WM_QUIT,
                0U,
                0L));
        first_thread.join();
        second_thread.join();

        ATL::AtlAxWinTerm();
    }

    CoUninitialize();
    return result;
}
