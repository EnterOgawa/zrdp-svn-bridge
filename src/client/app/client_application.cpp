/**
 * @file client_application.cpp
 * @brief 接続管理UI、session専用STA、Proxy slot割当を実装する。
 */

#include "client/app/client_application.h"

#include "common/diagnostic_log.h"

#include <objbase.h>

#include <atomic>
#include <condition_variable>
#include <new>
#include <thread>
#include <utility>

namespace zrdp::client::app {
namespace {

/// session終了または履歴追加でmanager message loopを起床させるthread message。
constexpr UINT kMessageSessionStateChanged = WM_APP + 0x100U;

}  // namespace

/**
 * @brief 一つのMainWindowとRDP ActiveXを専用COM STA threadで所有する。
 *
 * ActiveXの切断・購読解除・COM解放がMicrosoft内部で待機しても、その待機を
 * 接続管理Windowや他sessionのmessage loopへ波及させない。
 */
class SessionWindowThread final {
public:
    /**
     * @brief session設定と非所有observerをthread開始前に保持する。
     * @param[in] instance Client module instance。
     * @param[in] observer 接続履歴をmanager STAへmarshalするapplication。
     * @param[in] settings 一session専用の接続条件とProxy port。
     * @param[in] manager_thread_id 完了通知先のWin32 thread ID。
     */
    SessionWindowThread(
        HINSTANCE instance,
        ISessionWindowObserver& observer,
        SessionLaunchSettings settings,
        DWORD manager_thread_id)
        : instance_(instance),
          observer_(observer),
          settings_(std::move(settings)),
          manager_thread_id_(manager_thread_id) {
    }

    /**
     * @brief 生存threadがあればWM_QUITで同一STA上の解放を要求してjoinする。
     */
    ~SessionWindowThread() {
        StopAndJoin();
    }

    SessionWindowThread(const SessionWindowThread&) = delete;
    SessionWindowThread& operator=(const SessionWindowThread&) = delete;

    /**
     * @brief 専用STAを開始し、MainWindow生成結果だけを同期的に待つ。
     * @param[in] show_command session Windowの初期表示command。
     * @return COM、thread、MainWindowを利用可能にできた場合はtrue。
     */
    [[nodiscard]] bool Start(const int show_command) noexcept {
        try {
            worker_ = std::thread(
                &SessionWindowThread::ThreadMain,
                this,
                show_command);
        } catch (...) {
            return false;
        }

        bool succeeded = false;
        {
            std::unique_lock lock(startup_mutex_);
            startup_condition_.wait(
                lock,
                [this]() noexcept {
                    return startup_complete_;
                });
            succeeded = startup_succeeded_;
        }

        // 生成失敗threadをvectorへ公開せず、COM解放完了までここで回収する。
        if (!succeeded) {
            JoinCompleted();
        }

        return succeeded;
    }

    /**
     * @brief session STAがMainWindowとCOMをすべて解放済みか返す。
     * @return 全所有resourceを解放してthread終了直前ならtrue。
     */
    [[nodiscard]] bool finished() const noexcept {
        return finished_.load(std::memory_order_acquire);
    }

    /**
     * @brief finished確認済みthreadをblockなしで回収する。
     */
    void JoinCompleted() noexcept {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    /**
     * @brief MainWindow生成結果を待機中managerへ一度だけ公開する。
     * @param[in] succeeded COMとMainWindowを利用可能にできた場合はtrue。
     */
    void PublishStartupResult(const bool succeeded) noexcept {
        {
            std::scoped_lock lock(startup_mutex_);
            // 例外経路と通常経路から同じ結果を二重公開しない。
            if (startup_complete_) {
                return;
            }

            startup_succeeded_ = succeeded;
            startup_complete_ = true;
        }

        startup_condition_.notify_one();
    }

    /**
     * @brief session専用COM STA、Window、message loopを順序付きで実行する。
     * @param[in] show_command session Windowの初期表示command。
     */
    void ThreadMain(const int show_command) noexcept {
        const DWORD current_thread_id = GetCurrentThreadId();
        thread_id_.store(
            current_thread_id,
            std::memory_order_release);

        // destructorからWM_QUITを必ずpostできるよう、開始直後にmessage queueを作る。
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
        // ActiveXをMTAまたは未初期化threadへ生成しない。
        if (FAILED(com_result)) {
            PublishStartupResult(false);
            finished_.store(true, std::memory_order_release);
            NotifyManager();
            return;
        }

        try {
            {
                MainWindow window(
                    instance_,
                    observer_,
                    std::move(settings_));
                const bool created =
                    window.Create(show_command);
                PublishStartupResult(created);

                // このSTAが所有する一つのWindowだけをdispatchする。
                while (created &&
                       window.window() != nullptr) {
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

                    if (!window.ProcessDialogMessage(
                            message)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
            }
        } catch (...) {
            // session thread境界から例外を漏らさず、他sessionを継続する。
            PublishStartupResult(false);
            common::WriteDiagnosticLog(
                common::DiagnosticLevel::Error,
                common::DiagnosticEvent::UnexpectedException);
        }

        CoUninitialize();
        finished_.store(true, std::memory_order_release);
        NotifyManager();
    }

    /**
     * @brief manager threadを起床し、session回収または履歴反映を促す。
     */
    void NotifyManager() const noexcept {
        // main message queueが終了済みでも、通知失敗をsession異常へ昇格しない。
        if (manager_thread_id_ != 0U) {
            static_cast<void>(
                PostThreadMessageW(
                    manager_thread_id_,
                    kMessageSessionStateChanged,
                    0U,
                    0L));
        }
    }

    /**
     * @brief application rollback時に、COM所有STAへ直接触れず終了を要求する。
     */
    void StopAndJoin() noexcept {
        if (!worker_.joinable()) {
            return;
        }

        // 終了済みでないsessionだけへWM_QUITを送り、Window破棄は所有STAで行う。
        if (!finished()) {
            const DWORD target_thread_id =
                thread_id_.load(std::memory_order_acquire);
            if (target_thread_id != 0U) {
                static_cast<void>(
                    PostThreadMessageW(
                        target_thread_id,
                        WM_QUIT,
                        0U,
                        0L));
            }
        }

        worker_.join();
    }

    // -------------------------------------------------------------------------
    // session専用STAの所有状態
    // -------------------------------------------------------------------------

    /// Client module instanceへの非所有参照。
    HINSTANCE instance_;

    /// manager STAで生存する履歴通知先への非所有参照。
    ISessionWindowObserver& observer_;

    /// thread開始後にMainWindowへ一度だけmoveするsession設定。
    SessionLaunchSettings settings_;

    /// session終了と履歴追加を通知するmanagerのWin32 thread ID。
    DWORD manager_thread_id_{0U};

    /// 一つのCOM STAとmessage loopを所有するjoin対象thread。
    std::thread worker_;

    /// WM_QUITの通知先としてmanager threadから参照するsession thread ID。
    std::atomic<DWORD> thread_id_{0U};

    /// MainWindow、ActiveX、COMを全て解放したことを公開するflag。
    std::atomic<bool> finished_{false};

    /// 起動結果の一回公開だけを保護するmutex。
    std::mutex startup_mutex_;

    /// MainWindow生成完了までStartだけを待機させるcondition variable。
    std::condition_variable startup_condition_;

    /// 正常経路と例外経路から起動結果を二重公開しないためのflag。
    bool startup_complete_{false};

    /// Startへ返すCOM／MainWindow生成結果。
    bool startup_succeeded_{false};
};

// -----------------------------------------------------------------------------
// 接続管理application
// -----------------------------------------------------------------------------

ClientApplication::ClientApplication(
    const HINSTANCE instance)
    : instance_(instance),
      manager_thread_id_(GetCurrentThreadId()) {
}

ClientApplication::~ClientApplication() {
    // 各sessionを所有STA上で解放してからmanager、ATL、COMの順序へ戻す。
    sessions_.clear();
    manager_.reset();
    proxy_port_allocator_.reset();
}

bool ClientApplication::Create(
    const int show_command) {
    // manager公開前に有限64 KiBの固定local設定を一度だけ確定し、session間で共有する。
    proxy_configuration_result_ =
        proxy::LoadClientProxyConfigurationFromExecutableDirectory(
            proxy_configuration_);

    try {
        sessions_.reserve(
            kMaximumConcurrentRdpSessions);
        pending_successful_connections_.reserve(
            kMaximumConcurrentRdpSessions);
        proxy_port_allocator_ =
            std::make_unique<SessionProxyPortAllocator>(
                proxy_configuration_.port);
        manager_ =
            std::make_unique<ConnectionManagerWindow>(
                instance_,
                *this);
        // manager生成失敗時は部分所有objectを公開せずrollbackする。
        if (!manager_->Create(show_command)) {
            manager_.reset();
            proxy_port_allocator_.reset();
            return false;
        }

        UpdateManagerStatus();
        return true;
    } catch (...) {
        // startup allocation例外をWinMainへ固定失敗として返す。
        manager_.reset();
        proxy_port_allocator_.reset();
        sessions_.clear();
        return false;
    }
}

int ClientApplication::RunMessageLoop() {
    MSG message{};

    // manager STAはmanager UIとsessionからのthread messageだけを直列化する。
    while (true) {
        const BOOL result =
            GetMessageW(&message, nullptr, 0U, 0U);
        if (result == -1) {
            return static_cast<int>(GetLastError());
        }

        if (result == 0) {
            return static_cast<int>(message.wParam);
        }

        const bool internal_notification =
            message.hwnd == nullptr &&
            message.message ==
                kMessageSessionStateChanged;
        // thread messageをUSER32既定dispatchへ渡さず、下のqueue drainだけに使用する。
        if (!internal_notification &&
            !ProcessDialogMessage(message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        DrainSuccessfulConnections();
        CleanupDestroyedWindows();
        // managerと全sessionがなくなった場合だけprocess message loopを終了する。
        if (manager_ == nullptr &&
            sessions_.empty()) {
            return 0;
        }
    }
}

SessionOpenResult ClientApplication::OpenSession(
    SessionConnectionRequest request) noexcept {
    // session数を有限上限へ固定し、UI連打でActiveX／threadを無制限生成しない。
    if (sessions_.size() >=
        kMaximumConcurrentRdpSessions) {
        return {
            SessionOpenError::SessionLimitReached,
            0U,
        };
    }

    if (proxy_port_allocator_ == nullptr) {
        return {
            SessionOpenError::ResourceExhausted,
            0U,
        };
    }

    const std::optional<SessionProxyPortLease> lease =
        proxy_port_allocator_->Acquire();
    // base末尾のoverflowまたは全slot使用中では新規sessionだけを拒否する。
    if (!lease.has_value()) {
        return {
            SessionOpenError::ProxyPortUnavailable,
            0U,
        };
    }

    try {
        SessionLaunchSettings settings{
            std::move(request),
            proxy_configuration_result_.succeeded()
                ? lease->port
                : 0U,
            lease->slot,
            proxy_configuration_result_.error,
        };
        auto session =
            std::make_unique<SessionWindowThread>(
                instance_,
                *this,
                std::move(settings),
                manager_thread_id_);
        // COMまたはActiveX生成失敗時はsession一覧とport leaseへ公開しない。
        if (!session->Start(SW_SHOWNORMAL)) {
            static_cast<void>(
                proxy_port_allocator_->Release(
                    lease->slot));
            return {
                SessionOpenError::WindowCreationFailed,
                0U,
            };
        }

        sessions_.push_back(SessionRecord{
            std::move(session),
            lease->slot,
        });
        UpdateManagerStatus();
        return {
            SessionOpenError::None,
            proxy_configuration_result_.succeeded()
                ? lease->port
                : 0U,
        };
    } catch (const std::bad_alloc&) {
        static_cast<void>(
            proxy_port_allocator_->Release(
                lease->slot));
        return {
            SessionOpenError::ResourceExhausted,
            0U,
        };
    } catch (...) {
        static_cast<void>(
            proxy_port_allocator_->Release(
                lease->slot));
        return {
            SessionOpenError::WindowCreationFailed,
            0U,
        };
    }
}

void ClientApplication::RememberSuccessfulConnection(
    std::wstring connection_name,
    std::wstring server,
    std::wstring domain,
    std::wstring user_name,
    const rdp::RdpAudioRedirectionSettings audio) noexcept {
    try {
        {
            std::scoped_lock lock(
                successful_connections_mutex_);
            pending_successful_connections_.push_back(
                PendingSuccessfulConnection{
                    std::move(connection_name),
                    std::move(server),
                    std::move(domain),
                    std::move(user_name),
                    audio,
                });
        }

        // manager objectへsession threadから触れず、main queueだけを起床する。
        static_cast<void>(
            PostThreadMessageW(
                manager_thread_id_,
                kMessageSessionStateChanged,
                0U,
                0L));
    } catch (...) {
        // 履歴保存失敗で成立済みRDP sessionを切断しない。
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Error,
            common::DiagnosticEvent::UnexpectedException);
    }
}

void ClientApplication::CleanupDestroyedWindows() noexcept {
    try {
        // managerのWM_NCDESTROY後だけ所有controllerを解放する。
        if (manager_ != nullptr &&
            manager_->window() == nullptr) {
            manager_.reset();
        }

        auto iterator = sessions_.begin();
        // 終了済みSTAだけをjoinし、対応するport slotを一度だけ解放する。
        while (iterator != sessions_.end()) {
            if (iterator->thread != nullptr &&
                !iterator->thread->finished()) {
                ++iterator;
                continue;
            }

            if (iterator->thread != nullptr) {
                iterator->thread->JoinCompleted();
            }

            if (proxy_port_allocator_ != nullptr) {
                static_cast<void>(
                    proxy_port_allocator_->Release(
                        iterator->proxy_slot));
            }

            iterator = sessions_.erase(iterator);
        }

        UpdateManagerStatus();
    } catch (...) {
        // cleanup境界で例外が出ても生存Windowのmessage dispatchを継続する。
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Error,
            common::DiagnosticEvent::UnexpectedException);
    }
}

void ClientApplication::DrainSuccessfulConnections() noexcept {
    try {
        std::vector<PendingSuccessfulConnection> pending;
        {
            std::scoped_lock lock(
                successful_connections_mutex_);
            pending.swap(
                pending_successful_connections_);
        }

        // manager終了後の履歴通知は破棄し、生存workerや無効HWNDへ渡さない。
        if (manager_ == nullptr ||
            manager_->window() == nullptr) {
            return;
        }

        // 各sessionの成功順を維持して共通DPAPI履歴workerへ渡す。
        for (PendingSuccessfulConnection& connection :
             pending) {
            manager_->RememberSuccessfulConnection(
                std::move(connection.connection_name),
                std::move(connection.server),
                std::move(connection.domain),
                std::move(connection.user_name),
                connection.audio);
        }
    } catch (...) {
        // 履歴queueのallocation失敗でsession message loopを停止しない。
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Error,
            common::DiagnosticEvent::UnexpectedException);
    }
}

void ClientApplication::UpdateManagerStatus() noexcept {
    try {
        if (manager_ == nullptr ||
            manager_->window() == nullptr ||
            proxy_port_allocator_ == nullptr) {
            return;
        }

        const std::optional<std::uint16_t> next_port =
            proxy_configuration_result_.succeeded()
                ? proxy_port_allocator_->NextAvailablePort()
                : std::nullopt;
        manager_->UpdateRuntimeStatus(
            sessions_.size(),
            next_port,
            proxy_configuration_result_.error);
    } catch (...) {
        // 表示用文字列の確保失敗でcommit済みsessionのport slotを解放しない。
        common::WriteDiagnosticLog(
            common::DiagnosticLevel::Error,
            common::DiagnosticEvent::UnexpectedException);
    }
}

bool ClientApplication::ProcessDialogMessage(
    MSG& message) const noexcept {
    // session Windowは各専用STAで処理するため、manager階層だけを対象にする。
    return manager_ != nullptr &&
        manager_->ProcessDialogMessage(message);
}

}  // namespace zrdp::client::app
