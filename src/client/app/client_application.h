/**
 * @file client_application.h
 * @brief 接続管理Windowと複数RDP session Windowのprocess内所有者を定義する。
 *
 * @details 接続管理Windowと各RDP sessionを別COM STAへ分離し、sessionごとの
 *          ActiveX、Bridge、Proxy所有権と有限port slotを分離する。
 */
#pragma once

#include "client/app/connection_manager_window.h"
#include "client/app/main_window.h"
#include "client/app/session_proxy_port_allocator.h"
#include "client/proxy/client_proxy_config.h"

#include <Windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace zrdp::client::app {

/// implementation file内で一session専用COM STAを所有する内部class。
class SessionWindowThread;

/**
 * @brief Client processのmanager UI、session STA、有限Proxy slotを所有する。
 */
class ClientApplication final :
    public IConnectionManagerObserver,
    public ISessionWindowObserver {
public:
    /**
     * @brief module instanceを参照する未生成applicationを構築する。
     * @param[in] instance Client resourceを所有するmodule instance。
     */
    explicit ClientApplication(HINSTANCE instance);

    /**
     * @brief 全session Windowをmanagerより先に解放する。
     */
    ~ClientApplication() override;

    ClientApplication(const ClientApplication&) = delete;
    ClientApplication& operator=(const ClientApplication&) = delete;

    /**
     * @brief Proxy base設定を読み、接続管理Windowを生成する。
     * @param[in] show_command WinMain由来の表示command。
     * @return managerを利用可能にできた場合はtrue。
     */
    [[nodiscard]] bool Create(int show_command);

    /**
     * @brief 接続管理Windowとsession完了通知のmessage loopを実行する。
     * @return WM_QUIT codeまたはGetMessageW失敗時のWin32 error。
     */
    [[nodiscard]] int RunMessageLoop();

    /**
     * @brief manager要求から独立RDP／Bridge session Windowを生成する。
     * @param[in] request passwordを含まない接続profile。
     * @return session生成または固定拒否理由。
     */
    [[nodiscard]] SessionOpenResult OpenSession(
        SessionConnectionRequest request) noexcept override;

    /**
     * @brief 認証済み接続profileを生存中managerの共通履歴へ渡す。
     * @param[in] connection_name 任意の接続名。
     * @param[in] server RDP接続先。
     * @param[in] domain 任意domain。
     * @param[in] user_name passwordを含まないユーザー名。
     * @param[in] audio 接続時に適用した音声リダイレクト設定。
     */
    void RememberSuccessfulConnection(
        std::wstring connection_name,
        std::wstring server,
        std::wstring domain,
        std::wstring user_name,
        rdp::RdpAudioRedirectionSettings audio) noexcept override;

private:
    /**
     * @brief session STAからmanager STAへ渡す認証済み接続profile。
     */
    struct PendingSuccessfulConnection {
        /// 利用者が指定した任意の履歴表示名。
        std::wstring connection_name;

        /// passwordを含まないRDP接続先。
        std::wstring server;

        /// 任意のRDP logon domain。
        std::wstring domain;

        /// passwordを含まないRDP user name。
        std::wstring user_name;

        /// 接続前に確定した音声redirect profile。
        rdp::RdpAudioRedirectionSettings audio{};
    };

    /**
     * @brief 一つのsession専用STAと解放対象Proxy slotを所有する。
     */
    struct SessionRecord {
        /// MainWindow、ActiveX、COM STAを所有するsession thread。
        std::unique_ptr<SessionWindowThread> thread;

        /// session終了時にallocatorへ返す有限Proxy slot。
        std::size_t proxy_slot{0U};
    };

    /**
     * @brief 終了済みsession STAをjoinしてvectorから除きport slotを解放する。
     */
    void CleanupDestroyedWindows() noexcept;

    /**
     * @brief session STAがqueueした接続履歴をmanager STAへ反映する。
     */
    void DrainSuccessfulConnections() noexcept;

    /**
     * @brief 生存session数と次のportをmanagerへ反映する。
     */
    void UpdateManagerStatus() noexcept;

    /**
     * @brief manager Windowでdialog互換処理を試す。
     * @param[in] message manager message loopが取得した一件。
     * @return manager Window階層で処理した場合はtrue。
     */
    [[nodiscard]] bool ProcessDialogMessage(
        MSG& message) const noexcept;

    // -------------------------------------------------------------------------
    // manager STAが所有するprocess状態
    // -------------------------------------------------------------------------

    /// Client resourceを所有するmodule instanceへの非所有参照。
    HINSTANCE instance_;

    /// session threadからの完了通知を受けるmanager thread ID。
    DWORD manager_thread_id_{0U};

    /// 全sessionへ複製する検証済みloopback Proxy base設定。
    proxy::ClientProxyConfiguration proxy_configuration_{};

    /// 設定異常をmanager UIへ表示するfail-closed読込み結果。
    proxy::ClientProxyConfigurationResult
        proxy_configuration_result_{};

    /// manager STAだけが操作する有限Proxy port allocator。
    std::unique_ptr<SessionProxyPortAllocator>
        proxy_port_allocator_;

    /// processに一つだけ存在する接続入力Window。
    std::unique_ptr<ConnectionManagerWindow> manager_;

    /// 生存中または終了通知待ちのsession STA群。
    std::vector<SessionRecord> sessions_;

    /// session STAから渡される履歴queueだけを保護するmutex。
    std::mutex successful_connections_mutex_;

    /// manager STAが次回messageでdrainする認証済みprofile群。
    std::vector<PendingSuccessfulConnection>
        pending_successful_connections_;
};

}  // namespace zrdp::client::app
