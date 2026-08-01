/**
 * @file client_proxy_worker.h
 * @brief loopback HTTP listenerとClient relayを所有するworker境界を定義する。
 *
 * @details UI／COM threadとは所有Frameのbounded queueだけを共有する。
 *          bind addressは設定へ公開せず、productionとtestの両方でIPv4 loopbackへ
 *          固定する。
 */
#pragma once

#include "client/proxy/client_proxy_config.h"
#include "client/proxy/client_proxy_session.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace zrdp::client::proxy {

/// UIからworkerへ未処理のまま保持できるcommand件数。
inline constexpr std::size_t kMaximumProxyWorkerCommands = 4096U;

/// UI／worker間の各Frame queueが保持できるpayload込みbyte数。
inline constexpr std::size_t kMaximumProxyWorkerFrameBytes =
    4U * 1024U * 1024U;

/// 最初の有効HTTP headerを完成させる絶対期限。
inline constexpr std::uint64_t
    kClientProxyHeaderTimeoutMilliseconds = 30000U;

/// 成立後に双方向とも進捗がないlocal connectionの保持上限。
inline constexpr std::uint64_t
    kClientProxyIdleTimeoutMilliseconds =
        30U * 60U * 1000U;

/**
 * @brief workerがownerへ状態／Frame到着を通知するcallback。
 * @param[in] context Start時の非owning context。
 * @return Window Message等を予約できた場合はtrue。
 *
 * callback内でworkerを待機したり、COM APIを呼んではならない。
 */
using ProxyOwnerNotifyCallback =
    bool (*)(void* context) noexcept;

/**
 * @brief bind addressを変更できないProxy起動設定。
 */
struct ClientProxyWorkerSettings {
    /// 既定18080。0はOS割当portを使うlocal自動test専用。
    std::uint16_t port{kDefaultClientProxyPort};

    /// trueの場合だけEXE隣接`client.ini`をworker内で任意読込みする。
    bool load_client_ini{false};

    /// product既定は30秒。短縮値はlocal自動testだけが指定する。
    std::uint64_t header_timeout_milliseconds{
        kClientProxyHeaderTimeoutMilliseconds};

    /// product既定は30分。短縮値はlocal自動testだけが指定する。
    std::uint64_t idle_timeout_milliseconds{
        kClientProxyIdleTimeoutMilliseconds};
};

/**
 * @brief listener lifecycle。
 */
enum class ClientProxyWorkerState {
    Stopped,   ///< worker未開始またはStop完了。
    Starting,  ///< Winsockとlistener初期化中。
    Listening, ///< 127.0.0.1でaccept可能。
    Failed,    ///< listenerを公開できなかった。
};

/**
 * @brief endpoint文字列を含まないworker失敗分類。
 */
enum class ClientProxyWorkerError : std::uint32_t {
    None = 0U,
    ThreadStartFailed = 1U,
    WinsockInitializationFailed = 2U,
    SocketCreateFailed = 3U,
    ExclusiveAddressFailed = 4U,
    NonBlockingFailed = 5U,
    BindFailed = 6U,
    ListenFailed = 7U,
    LocalSocketFailed = 8U,
    CommandQueueLimitExceeded = 9U,
    OutboundQueueLimitExceeded = 10U,
    SessionFailed = 11U,
    ConfigurationFailed = 12U,
};

/**
 * @brief UIとtestへ公開する非機密listener／relay snapshot。
 */
struct ClientProxyWorkerSnapshot {
    ClientProxyWorkerState state{
        ClientProxyWorkerState::Stopped};  ///< listener lifecycle。
    ClientProxyWorkerError error{
        ClientProxyWorkerError::None};  ///< endpointを含まない失敗分類。
    std::uint32_t winsock_error{0U};  ///< 分類元のWinsock error code。
    ClientProxyConfigurationError configuration_error{
        ClientProxyConfigurationError::None};  ///< client.ini検証結果。
    std::uint16_t listening_port{0U};  ///< 実際にbindしたloopback port。
    bool bridge_ready{false};  ///< HELLO済みsessionへOPEN可能。
    std::uint32_t active_streams{0U};  ///< 現在のmultiplexer stream数。
    std::uint64_t bytes_from_local{0U};  ///< localから受理した累積byte数。
    std::uint64_t bytes_to_local{0U};  ///< localへ送信済みの累積byte数。
    std::uint64_t accepted_connections{0U};  ///< 受付済みSocket累計。
    std::uint64_t rejected_connections{0U};  ///< 上限等で拒否したSocket累計。
    std::uint64_t timed_out_connections{0U};  ///< timeout終了したSocket累計。
};

/**
 * @brief IPv4 loopback listener、有限local Socket table、sessionをworkerで所有する。
 */
class ClientProxyWorker final {
public:
    /**
     * @brief listener未開始の空workerを構築する。
     */
    ClientProxyWorker() noexcept = default;

    /**
     * @brief 生存workerを停止して全Socketを閉じる。
     */
    ~ClientProxyWorker();

    ClientProxyWorker(const ClientProxyWorker&) = delete;
    ClientProxyWorker& operator=(const ClientProxyWorker&) = delete;
    ClientProxyWorker(ClientProxyWorker&&) = delete;
    ClientProxyWorker& operator=(ClientProxyWorker&&) = delete;

    /**
     * @brief background workerと127.0.0.1 listener初期化を開始する。
     * @param[in] settings portとproduct既定timeoutを持つ起動設定。
     * @param[in] notify owner通知callback。testではnullptr可。
     * @param[in] notify_context callbackへ渡す非owning context。
     * @return threadを開始できた場合はtrue。listen結果はsnapshotで通知する。
     */
    [[nodiscard]] bool Start(
        ClientProxyWorkerSettings settings,
        ProxyOwnerNotifyCallback notify,
        void* notify_context) noexcept;

    /**
     * @brief workerを停止してjoinし、listenerとlocal Socketを閉じる。
     */
    void Stop() noexcept;

    /**
     * @brief HELLOで確定した新Bridge sessionをworkerへ予約する。
     * @param[in] capabilities negotiated Phase 2 capability。
     * @return commandをqueueできた場合はtrue。
     *
     * 旧commandと旧outbound Frameは新sessionへ持ち越さない。
     */
    [[nodiscard]] bool EnqueueBridgeStart(
        const protocol::HelloCapabilities& capabilities) noexcept;

    /**
     * @brief RDP切断／Agent CLOSEを最優先commandとして予約する。
     */
    void EnqueueBridgeStop() noexcept;

    /**
     * @brief UI decoderが受信したstream Frameをworkerへ渡す。
     * @param[in] frame 所有Frame。
     * @return bounded command queueへ登録できた場合はtrue。
     */
    [[nodiscard]] bool EnqueueBridgeFrame(
        protocol::Frame frame) noexcept;

    /**
     * @brief UI threadへ渡す先頭outbound Frameを取り出す。
     * @param[out] frame 成功時の所有Frame。
     * @param[out] more_frames 取出し後もqueueが残る場合はtrue。
     * @return 一件取得できた場合はtrue。
     */
    [[nodiscard]] bool TakeOutboundFrame(
        protocol::Frame& frame,
        bool& more_frames) noexcept;

    /**
     * @brief owner message処理開始時に通知予約flagを解除する。
     *
     * 先に解除してからqueueをdrainすることで、処理中の新着に次messageを許可する。
     */
    void AcknowledgeOwnerNotification() noexcept;

    /**
     * @brief payloadを含まない現在状態を取得する。
     * @return mutex境界のsnapshot copy。
     */
    [[nodiscard]] ClientProxyWorkerSnapshot snapshot() const noexcept;

private:
    /**
     * @brief ownerからworkerへ渡す有限command種別。
     */
    enum class CommandType {
        StartBridge,  ///< negotiated sessionを開始する。
        StopBridge,   ///< 旧sessionを全破棄する。
        Frame,        ///< 一つのAgent Frameを適用する。
    };

    /**
     * @brief command種別ごとの所有payload。
     */
    struct Command {
        CommandType type{CommandType::StopBridge};  ///< 適用する操作。
        protocol::HelloCapabilities capabilities{};  ///< StartBridgeだけが使用。
        protocol::Frame frame{};  ///< Frameだけが使用する所有data。
    };

    /**
     * @brief Winsock listenerとrelayのworker loop。
     */
    void WorkerLoop() noexcept;

    /**
     * @brief ownerへ一件だけcoalesced通知を予約する。
     */
    void NotifyOwner() noexcept;

    /**
     * @brief owner向けFrame queueをsession境界で破棄する。
     */
    void ClearOutboundFrames() noexcept;

    /**
     * @brief workerが公開するsnapshotを置換してownerへ通知する。
     * @param[in] snapshot 新しい非機密値。
     */
    void PublishSnapshot(
        const ClientProxyWorkerSnapshot& snapshot) noexcept;

    /// worker起動時にcopyするport設定。
    ClientProxyWorkerSettings settings_{};

    /// workerからだけ呼ぶ非owning通知先。
    ProxyOwnerNotifyCallback notify_{nullptr};

    /// `notify_`へそのまま渡すowner lifetime中の非所有context。
    void* notify_context_{nullptr};

    /// Stop要求をlockなしでworker待機条件へ反映するflag。
    std::atomic<bool> stop_requested_{false};

    /// owner messageの重複postを抑制するflag。
    std::atomic<bool> owner_notification_pending_{false};

    /// WinsockとClientProxySessionを一つだけ所有するthread。
    std::thread worker_;

    /// command queueとworker wakeupを保護する境界。
    mutable std::mutex command_mutex_;

    /// command追加またはStop要求でworkerを起床するcondition variable。
    std::condition_variable command_ready_;

    /// ownerからworkerへ渡すbounded command列。
    std::deque<Command> commands_;

    /// command列内のFrame payload byte合計。
    std::size_t command_frame_bytes_{0U};

    /// workerからUIへ渡す所有Frame queue。
    mutable std::mutex outbound_mutex_;

    /// SVC送信順を維持するbounded Frame列。
    std::deque<protocol::Frame> outbound_frames_;

    /// outbound Frame payload byte合計。
    std::size_t outbound_frame_bytes_{0U};

    /// UI／testが参照する非機密状態。
    mutable std::mutex snapshot_mutex_;

    /// `snapshot_mutex_`内で一括置換する最新状態。
    ClientProxyWorkerSnapshot snapshot_{};
};

}  // namespace zrdp::client::proxy
