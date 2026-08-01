/**
 * @file client_proxy_worker.cpp
 * @brief 127.0.0.1限定Winsock listenerとClient Proxy workerを実装する。
 *
 * @details DNS、OS Proxy、IPv6、0.0.0.0、任意bind addressを使用しない。
 */

#include <WinSock2.h>
#include <WS2tcpip.h>

#include "client/proxy/client_proxy_worker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <map>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace zrdp::client::proxy {
namespace {

/// local Socket一回のreceive buffer。
constexpr std::size_t kLocalReceiveChunkBytes = 16U * 1024U;

/// 一loopで各Socket方向を処理する最大operation数。
constexpr std::size_t kMaximumSocketOperationsPerPump = 8U;

/// network readinessをpollするidle間隔。
constexpr auto kWorkerIdleWait =
    std::chrono::milliseconds(5);

/// Agent未readyまたは二接続目へ返す固定response。
constexpr std::string_view kServiceUnavailableResponse =
    "HTTP/1.1 503 Service Unavailable\r\n"
    "Connection: close\r\n"
    "Content-Length: 0\r\n\r\n";

/**
 * @brief `SOCKET`を一所有者でclosesocketするRAII wrapper。
 */
class UniqueSocket final {
public:
    UniqueSocket() noexcept = default;

    explicit UniqueSocket(
        const SOCKET socket) noexcept
        : socket_(socket) {
    }

    ~UniqueSocket() {
        Reset();
    }

    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;

    UniqueSocket(UniqueSocket&& other) noexcept
        : socket_(other.Release()) {
    }

    UniqueSocket& operator=(UniqueSocket&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }

        return *this;
    }

    /**
     * @brief 現在の非owning Socket値を返す。
     * @return 未所有時は`INVALID_SOCKET`。
     */
    [[nodiscard]] SOCKET Get() const noexcept {
        return socket_;
    }

    /**
     * @brief 所有Socketを閉じ、新しい値へ置換する。
     * @param[in] socket 新しい所有値。
     */
    void Reset(
        const SOCKET socket = INVALID_SOCKET) noexcept {
        if (socket_ != INVALID_SOCKET) {
            static_cast<void>(closesocket(socket_));
        }

        socket_ = socket;
    }

private:
    /**
     * @brief close責任をmove先へ移す。
     * @return 以前の所有値。
     */
    [[nodiscard]] SOCKET Release() noexcept {
        const SOCKET released = socket_;
        socket_ = INVALID_SOCKET;
        return released;
    }

    SOCKET socket_{INVALID_SOCKET};
};

/**
 * @brief workerが一local connectionについて所有するSocket側状態。
 */
struct LocalClientSocket {
    LocalConnectionId connection_id{0U};
    UniqueSocket socket;
    bool session_owned{false};
    bool receive_closed{false};
    std::vector<std::byte> fixed_response;
    std::size_t fixed_response_offset{0U};
    std::uint64_t accepted_tick{0U};
    std::uint64_t last_activity_tick{0U};
};

/**
 * @brief WSAStartup成功分だけWSACleanupするRAII wrapper。
 */
class WinsockRuntime final {
public:
    ~WinsockRuntime() {
        Stop();
    }

    /**
     * @brief Winsock 2.2を一度開始する。
     * @return 成功時0、失敗時のWinsock code。
     */
    [[nodiscard]] std::uint32_t Start() noexcept {
        if (started_) {
            return 0U;
        }

        WSADATA data{};
        const int result =
            WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            return static_cast<std::uint32_t>(result);
        }

        // 要求version以外を成功扱いせずcleanup ownershipを残さない。
        if (LOBYTE(data.wVersion) != 2U ||
            HIBYTE(data.wVersion) != 2U) {
            static_cast<void>(WSACleanup());
            return static_cast<std::uint32_t>(
                WSAVERNOTSUPPORTED);
        }

        started_ = true;
        return 0U;
    }

    /**
     * @brief Start成功分だけcleanupする。
     */
    void Stop() noexcept {
        if (started_) {
            static_cast<void>(WSACleanup());
            started_ = false;
        }
    }

private:
    bool started_{false};
};

/**
 * @brief Socketをnon-blockingへ設定する。
 * @param[in] socket 有効Socket。
 * @return 成功時true。
 */
[[nodiscard]] bool SetNonBlocking(
    const SOCKET socket) noexcept {
    u_long enabled = 1UL;
    return ioctlsocket(
               socket,
               FIONBIO,
               &enabled) != SOCKET_ERROR;
}

/**
 * @brief endpoint文字列なしでwould-block系errorを判定する。
 * @param[in] error WSAGetLastError値。
 * @return 後続pollで再試行可能ならtrue。
 */
[[nodiscard]] bool IsWouldBlock(
    const int error) noexcept {
    return error == WSAEWOULDBLOCK ||
        error == WSAEINPROGRESS ||
        error == WSAEALREADY;
}

/**
 * @brief Frame queueのaccountingに使う有限byte数を返す。
 * @param[in] frame 所有Frame。
 * @return wire header相当とpayloadの合計。
 */
[[nodiscard]] std::size_t FrameBytes(
    const protocol::Frame& frame) noexcept {
    return protocol::kFrameHeaderBytes +
        frame.payload.size();
}

/**
 * @brief 固定503 responseをbinary queueへcopyする。
 * @param[out] response 所有byte列。
 * @return allocation成功時はtrue。
 */
[[nodiscard]] bool BuildServiceUnavailableResponse(
    std::vector<std::byte>& response) noexcept {
    try {
        response.resize(
            kServiceUnavailableResponse.size());
        // 固定ASCIIだけを一対一のwire byteへcopyする。
        for (std::size_t index = 0U;
             index < kServiceUnavailableResponse.size();
             ++index) {
            response[index] =
                static_cast<std::byte>(
                    static_cast<unsigned char>(
                        kServiceUnavailableResponse[index]));
        }
    } catch (...) {
        response.clear();
        return false;
    }

    return true;
}

}  // namespace

ClientProxyWorker::~ClientProxyWorker() {
    Stop();
}

bool ClientProxyWorker::Start(
    const ClientProxyWorkerSettings settings,
    const ProxyOwnerNotifyCallback notify,
    void* const notify_context) noexcept {
    // 二重worker開始によるlistenerとqueueの複数ownerを作らない。
    if (worker_.joinable()) {
        return false;
    }

    settings_ = settings;
    notify_ = notify;
    notify_context_ = notify_context;
    stop_requested_.store(false, std::memory_order_release);
    owner_notification_pending_.store(
        false,
        std::memory_order_release);
    {
        std::scoped_lock lock(snapshot_mutex_);
        snapshot_ = ClientProxyWorkerSnapshot{};
        snapshot_.state =
            ClientProxyWorkerState::Starting;
    }

    try {
        worker_ = std::thread(
            &ClientProxyWorker::WorkerLoop,
            this);
    } catch (...) {
        std::scoped_lock lock(snapshot_mutex_);
        snapshot_.state =
            ClientProxyWorkerState::Failed;
        snapshot_.error =
            ClientProxyWorkerError::ThreadStartFailed;
        return false;
    }

    return true;
}

void ClientProxyWorker::Stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    command_ready_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }

    {
        std::scoped_lock lock(command_mutex_);
        commands_.clear();
        command_frame_bytes_ = 0U;
    }
    ClearOutboundFrames();
    {
        std::scoped_lock lock(snapshot_mutex_);
        snapshot_ = ClientProxyWorkerSnapshot{};
    }
    owner_notification_pending_.store(
        false,
        std::memory_order_release);
    notify_ = nullptr;
    notify_context_ = nullptr;
}

bool ClientProxyWorker::EnqueueBridgeStart(
    const protocol::HelloCapabilities& capabilities) noexcept {
    try {
        std::scoped_lock lock(command_mutex_);
        // 新session境界では旧Frame／stop commandを一切持ち越さない。
        commands_.clear();
        command_frame_bytes_ = 0U;
        Command command{};
        command.type = CommandType::StartBridge;
        command.capabilities = capabilities;
        commands_.push_back(std::move(command));
    } catch (...) {
        std::scoped_lock lock(snapshot_mutex_);
        snapshot_.error =
            ClientProxyWorkerError::
                CommandQueueLimitExceeded;
        return false;
    }

    ClearOutboundFrames();
    command_ready_.notify_one();
    return true;
}

void ClientProxyWorker::EnqueueBridgeStop() noexcept {
    try {
        std::scoped_lock lock(command_mutex_);
        // stopを必ずqueue可能にするため旧Frame commandを先に破棄する。
        commands_.clear();
        command_frame_bytes_ = 0U;
        Command command{};
        command.type = CommandType::StopBridge;
        commands_.push_back(std::move(command));
    } catch (...) {
        // allocation不能時もworkerが検出できる最終停止flagを使用する。
        stop_requested_.store(true, std::memory_order_release);
    }

    ClearOutboundFrames();
    command_ready_.notify_one();
}

bool ClientProxyWorker::EnqueueBridgeFrame(
    protocol::Frame frame) noexcept {
    const std::size_t frame_bytes = FrameBytes(frame);
    try {
        std::scoped_lock lock(command_mutex_);
        // UI callback由来Frameを件数とbyte数の両方で有限化する。
        if (commands_.size() >=
                kMaximumProxyWorkerCommands ||
            frame_bytes >
                kMaximumProxyWorkerFrameBytes -
                    command_frame_bytes_) {
            return false;
        }

        Command command{};
        command.type = CommandType::Frame;
        command.frame = std::move(frame);
        commands_.push_back(std::move(command));
        command_frame_bytes_ += frame_bytes;
    } catch (...) {
        return false;
    }

    command_ready_.notify_one();
    return true;
}

bool ClientProxyWorker::TakeOutboundFrame(
    protocol::Frame& frame,
    bool& more_frames) noexcept {
    more_frames = false;
    std::scoped_lock lock(outbound_mutex_);
    if (outbound_frames_.empty()) {
        return false;
    }

    const std::size_t frame_bytes =
        FrameBytes(outbound_frames_.front());
    frame = std::move(outbound_frames_.front());
    outbound_frames_.pop_front();
    // 内部counter破損時もsize_t underflowを避ける。
    if (outbound_frame_bytes_ >= frame_bytes) {
        outbound_frame_bytes_ -= frame_bytes;
    } else {
        outbound_frame_bytes_ = 0U;
    }

    more_frames = !outbound_frames_.empty();
    return true;
}

void ClientProxyWorker::AcknowledgeOwnerNotification() noexcept {
    owner_notification_pending_.store(
        false,
        std::memory_order_release);
}

ClientProxyWorkerSnapshot
ClientProxyWorker::snapshot() const noexcept {
    std::scoped_lock lock(snapshot_mutex_);
    return snapshot_;
}

void ClientProxyWorker::WorkerLoop() noexcept {
    WinsockRuntime winsock;
    UniqueSocket listener;
    ClientProxySession session;
    std::map<LocalConnectionId, LocalClientSocket>
        local_clients;
    LocalConnectionId next_connection_id = 1U;
    ClientProxyWorkerSnapshot published{};
    published.state = ClientProxyWorkerState::Starting;

    const auto fail_listener =
        [&](const ClientProxyWorkerError error,
            const std::uint32_t winsock_error) noexcept {
            published.state =
                ClientProxyWorkerState::Failed;
            published.error = error;
            published.winsock_error = winsock_error;
            PublishSnapshot(published);
        };

    try {
        std::uint16_t effective_port = settings_.port;
        // 内部test設定を含め0 timeoutでは無期限または即時closeへ曖昧化させない。
        if (settings_.header_timeout_milliseconds == 0U ||
            settings_.idle_timeout_milliseconds == 0U) {
            fail_listener(
                ClientProxyWorkerError::ConfigurationFailed,
                static_cast<std::uint32_t>(WSAEINVAL));
            return;
        }

        if (settings_.load_client_ini) {
            ClientProxyConfiguration configuration{};
            const ClientProxyConfigurationResult result =
                LoadClientProxyConfigurationFromExecutableDirectory(
                    configuration);
            // 存在する不正設定では別portへfallbackせずlistenerを公開しない。
            if (!result.succeeded()) {
                published.configuration_error =
                    result.error;
                fail_listener(
                    ClientProxyWorkerError::
                        ConfigurationFailed,
                    result.win32_error);
                return;
            }

            effective_port = configuration.port;
        }

        const std::uint32_t winsock_result =
            winsock.Start();
        if (winsock_result != 0U) {
            fail_listener(
                ClientProxyWorkerError::
                    WinsockInitializationFailed,
                winsock_result);
            return;
        }

        listener.Reset(::socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP));
        if (listener.Get() == INVALID_SOCKET) {
            fail_listener(
                ClientProxyWorkerError::
                    SocketCreateFailed,
                static_cast<std::uint32_t>(
                    WSAGetLastError()));
            return;
        }

        BOOL exclusive = TRUE;
        // port共有で別processが同じloopback endpointを横取りしないよう排他bindする。
        if (setsockopt(
                listener.Get(),
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive),
                static_cast<int>(sizeof(exclusive))) ==
            SOCKET_ERROR) {
            fail_listener(
                ClientProxyWorkerError::
                    ExclusiveAddressFailed,
                static_cast<std::uint32_t>(
                    WSAGetLastError()));
            return;
        }

        if (!SetNonBlocking(listener.Get())) {
            fail_listener(
                ClientProxyWorkerError::
                    NonBlockingFailed,
                static_cast<std::uint32_t>(
                    WSAGetLastError()));
            return;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        // bind addressは設定やhostnameから解決せず127.0.0.1へ固定する。
        address.sin_addr.s_addr =
            htonl(INADDR_LOOPBACK);
        address.sin_port = htons(effective_port);
        if (bind(
                listener.Get(),
                reinterpret_cast<const sockaddr*>(&address),
                static_cast<int>(sizeof(address))) ==
            SOCKET_ERROR) {
            fail_listener(
                ClientProxyWorkerError::BindFailed,
                static_cast<std::uint32_t>(
                    WSAGetLastError()));
            return;
        }

        if (listen(
                listener.Get(),
                static_cast<int>(
                    kMaximumClientProxyConnections *
                    2U)) == SOCKET_ERROR) {
            fail_listener(
                ClientProxyWorkerError::ListenFailed,
                static_cast<std::uint32_t>(
                    WSAGetLastError()));
            return;
        }

        int address_size =
            static_cast<int>(sizeof(address));
        if (getsockname(
                listener.Get(),
                reinterpret_cast<sockaddr*>(&address),
                &address_size) == SOCKET_ERROR) {
            fail_listener(
                ClientProxyWorkerError::BindFailed,
                static_cast<std::uint32_t>(
                    WSAGetLastError()));
            return;
        }

        published.state =
            ClientProxyWorkerState::Listening;
        published.error = ClientProxyWorkerError::None;
        published.winsock_error = 0U;
        published.configuration_error =
            ClientProxyConfigurationError::None;
        published.listening_port =
            ntohs(address.sin_port);
        PublishSnapshot(published);

        const auto close_local =
            [&](const LocalConnectionId connection_id,
                const protocol::StreamCloseReason reason) noexcept {
                const auto iterator =
                    local_clients.find(connection_id);
                if (iterator == local_clients.end()) {
                    return;
                }

                if (iterator->second.session_owned) {
                    session.OnLocalConnectionClosed(
                        connection_id,
                        reason);
                }

                local_clients.erase(iterator);
            };

        const auto close_all =
            [&](const protocol::StreamCloseReason reason) noexcept {
                // map破棄前にsessionへ各Socket終了を一度ずつ通知する。
                for (const auto& [connection_id, client] :
                     local_clients) {
                    if (client.session_owned) {
                        session.OnLocalConnectionClosed(
                            connection_id,
                            reason);
                    }
                }

                local_clients.clear();
            };

        // stop要求までlistener、command、local Socket tableを一owner threadでpumpする。
        while (!stop_requested_.load(
            std::memory_order_acquire)) {
            bool made_progress = false;
            std::deque<Command> commands;
            {
                std::scoped_lock lock(command_mutex_);
                commands.swap(commands_);
                command_frame_bytes_ = 0U;
            }

            // UIからのsession commandをenqueue順に適用する。
            for (Command& command : commands) {
                switch (command.type) {
                    case CommandType::StartBridge:
                        close_all(
                            protocol::StreamCloseReason::
                                SessionStopping);
                        if (!session.StartBridgeSession(
                                command.capabilities)) {
                            published.error =
                                ClientProxyWorkerError::
                                    SessionFailed;
                        } else {
                            published.error =
                                ClientProxyWorkerError::None;
                        }
                        ClearOutboundFrames();
                        made_progress = true;
                        break;
                    case CommandType::StopBridge:
                        close_all(
                            protocol::StreamCloseReason::
                                SessionStopping);
                        session.StopBridgeSession();
                        ClearOutboundFrames();
                        made_progress = true;
                        break;
                    case CommandType::Frame:
                        // Stop処理後のstale Frameは新session errorへ昇格しない。
                        if (session.snapshot().state !=
                                ClientProxySessionState::Stopped &&
                            !session.OnBridgeFrame(
                                std::move(command.frame))) {
                            close_all(
                                protocol::StreamCloseReason::
                                    LocalError);
                            published.error =
                                ClientProxyWorkerError::
                                    SessionFailed;
                        }
                        made_progress = true;
                        break;
                }
            }

            // backlogを有限回acceptし、activeと固定503待ちを合わせても有限化する。
            for (std::size_t operation = 0U;
                 operation <
                     kMaximumSocketOperationsPerPump;
                 ++operation) {
                sockaddr_in peer{};
                int peer_size =
                    static_cast<int>(sizeof(peer));
                UniqueSocket accepted(accept(
                    listener.Get(),
                    reinterpret_cast<sockaddr*>(&peer),
                    &peer_size));
                if (accepted.Get() == INVALID_SOCKET) {
                    const int error = WSAGetLastError();
                    if (!IsWouldBlock(error)) {
                        published.error =
                            ClientProxyWorkerError::
                                LocalSocketFailed;
                        published.winsock_error =
                            static_cast<std::uint32_t>(
                                error);
                    }
                    break;
                }

                made_progress = true;
                // loopback bindに加えてaccept peerもIPv4 loopback完全一致で検証する。
                if (peer.sin_family != AF_INET ||
                    peer.sin_addr.s_addr !=
                        htonl(INADDR_LOOPBACK) ||
                    !SetNonBlocking(accepted.Get()) ||
                    local_clients.size() >=
                        static_cast<std::size_t>(
                            kMaximumClientProxyConnections) *
                            2U ||
                    next_connection_id == 0U ||
                    next_connection_id ==
                        std::numeric_limits<
                            LocalConnectionId>::max()) {
                    ++published.rejected_connections;
                    continue;
                }

                const LocalConnectionId connection_id =
                    next_connection_id++;
                const LocalConnectionResult result =
                    session.BeginLocalConnection(
                        connection_id);
                LocalClientSocket client{};
                client.connection_id = connection_id;
                client.socket = std::move(accepted);
                client.accepted_tick = GetTickCount64();
                client.last_activity_tick =
                    client.accepted_tick;
                if (result ==
                    LocalConnectionResult::Accepted) {
                    client.session_owned = true;
                    ++published.accepted_connections;
                } else {
                    if (!BuildServiceUnavailableResponse(
                            client.fixed_response)) {
                        published.error =
                            ClientProxyWorkerError::
                                LocalSocketFailed;
                        if (result ==
                            LocalConnectionResult::Failed) {
                            published.error =
                                ClientProxyWorkerError::
                                    SessionFailed;
                        }

                        ++published.rejected_connections;
                        continue;
                    }
                    ++published.rejected_connections;
                }

                try {
                    const auto [iterator, inserted] =
                        local_clients.emplace(
                            connection_id,
                            std::move(client));
                    static_cast<void>(iterator);
                    if (!inserted) {
                        if (result ==
                            LocalConnectionResult::Accepted) {
                            session.OnLocalConnectionClosed(
                                connection_id,
                                protocol::StreamCloseReason::
                                    LocalError);
                        }

                        published.error =
                            ClientProxyWorkerError::
                                LocalSocketFailed;
                    }
                } catch (...) {
                    if (result ==
                        LocalConnectionResult::Accepted) {
                        session.OnLocalConnectionClosed(
                            connection_id,
                            protocol::StreamCloseReason::
                                LocalError);
                    }

                    published.error =
                        ClientProxyWorkerError::
                            LocalSocketFailed;
                }
            }

            std::vector<std::pair<
                LocalConnectionId,
                protocol::StreamCloseReason>>
                close_requests;
            close_requests.reserve(local_clients.size());
            const auto schedule_close =
                [&](const LocalConnectionId connection_id,
                    const protocol::StreamCloseReason reason) {
                    // 同じiterationで一Socketを重複closeしない。
                    for (const auto& request :
                         close_requests) {
                        if (request.first == connection_id) {
                            return;
                        }
                    }

                    close_requests.emplace_back(
                        connection_id,
                        reason);
                };

            const std::uint64_t now_tick = GetTickCount64();
            // 部分headerのSlowlorisと成立後の完全無通信をconnection単位で終了する。
            for (const auto& [connection_id, client] :
                 local_clients) {
                if (!client.session_owned) {
                    continue;
                }

                ClientProxyConnectionSnapshot
                    connection_snapshot{};
                if (!session.TryGetConnectionSnapshot(
                        connection_id,
                        connection_snapshot) ||
                    connection_snapshot.local_close_required) {
                    continue;
                }

                const bool header_pending =
                    connection_snapshot.stream_id == 0U &&
                    connection_snapshot.state ==
                        ClientProxySessionState::Ready;
                const bool header_expired =
                    header_pending &&
                    now_tick - client.accepted_tick >=
                        settings_.header_timeout_milliseconds;
                const bool idle_expired =
                    !header_pending &&
                    now_tick - client.last_activity_tick >=
                        settings_.idle_timeout_milliseconds;
                if (!header_expired && !idle_expired) {
                    continue;
                }

                schedule_close(
                    connection_id,
                    protocol::StreamCloseReason::Timeout);
                // 診断へendpointやHTTP内容を渡さず、有限counterだけを公開する。
                if (published.timed_out_connections !=
                    std::numeric_limits<std::uint64_t>::max()) {
                    ++published.timed_out_connections;
                }
            }

            std::array<
                std::byte,
                kLocalReceiveChunkBytes> receive_buffer{};
            std::size_t receive_operations = 0U;
            // 各session所有Socketを一回ずつ処理し、低いconnection IDへ偏らせない。
            for (auto& [connection_id, client] :
                 local_clients) {
                if (receive_operations >=
                        kMaximumSocketOperationsPerPump ||
                    !client.session_owned ||
                    client.receive_closed) {
                    continue;
                }

                ClientProxyConnectionSnapshot
                    connection_snapshot{};
                if (!session.TryGetConnectionSnapshot(
                        connection_id,
                        connection_snapshot)) {
                    schedule_close(
                        connection_id,
                        protocol::StreamCloseReason::
                            LocalError);
                    continue;
                }

                // terminal response中またはper-stream queue上限付近ではrecvを止める。
                if (connection_snapshot.state ==
                        ClientProxySessionState::Closing ||
                    connection_snapshot.
                            local_close_required ||
                    connection_snapshot.
                            upstream_pending_bytes >=
                        kMaximumProxyUpstreamPendingBytes -
                            receive_buffer.size()) {
                    continue;
                }

                ++receive_operations;
                const int received = recv(
                    client.socket.Get(),
                    reinterpret_cast<char*>(
                        receive_buffer.data()),
                    static_cast<int>(
                        receive_buffer.size()),
                    0);
                if (received == SOCKET_ERROR) {
                    const int error = WSAGetLastError();
                    if (!IsWouldBlock(error)) {
                        published.winsock_error =
                            static_cast<std::uint32_t>(
                                error);
                        schedule_close(
                            connection_id,
                            protocol::StreamCloseReason::
                                PeerReset);
                    }
                    continue;
                }

                if (received == 0) {
                    client.receive_closed = true;
                    if (!session.OnLocalReceiveClosed(
                            connection_id)) {
                        published.error =
                            ClientProxyWorkerError::
                                SessionFailed;
                    }
                    made_progress = true;
                    continue;
                }

                if (!session.OnLocalData(
                        connection_id,
                        std::span<const std::byte>(
                            receive_buffer.data(),
                            static_cast<std::size_t>(
                                received)))) {
                    published.error =
                        ClientProxyWorkerError::
                            SessionFailed;
                    schedule_close(
                        connection_id,
                        protocol::StreamCloseReason::
                            LocalError);
                    continue;
                }

                client.last_activity_tick =
                    GetTickCount64();
                made_progress = true;
            }

            std::size_t send_operations = 0U;
            // 固定503またはAgent responseを各local Socketへ公平に一回ずつ送る。
            for (auto& [connection_id, client] :
                 local_clients) {
                if (send_operations >=
                    kMaximumSocketOperationsPerPump) {
                    break;
                }

                std::span<const std::byte> write;
                if (client.session_owned) {
                    if (!session.PeekLocalWrite(
                            connection_id,
                            write)) {
                        continue;
                    }
                } else {
                    if (client.fixed_response_offset >=
                        client.fixed_response.size()) {
                        continue;
                    }

                    write = std::span<const std::byte>(
                        client.fixed_response.data() +
                            client.fixed_response_offset,
                        client.fixed_response.size() -
                            client.fixed_response_offset);
                }

                const std::size_t bounded_size =
                    std::min(
                        write.size(),
                        static_cast<std::size_t>(
                            std::numeric_limits<int>::
                                max()));
                ++send_operations;
                const int sent = send(
                    client.socket.Get(),
                    reinterpret_cast<const char*>(
                        write.data()),
                    static_cast<int>(bounded_size),
                    0);
                if (sent == SOCKET_ERROR) {
                    const int error = WSAGetLastError();
                    if (!IsWouldBlock(error)) {
                        published.winsock_error =
                            static_cast<std::uint32_t>(
                                error);
                        schedule_close(
                            connection_id,
                            protocol::StreamCloseReason::
                                PeerReset);
                    }
                    continue;
                }

                if (sent <= 0) {
                    continue;
                }

                const std::size_t sent_size =
                    static_cast<std::size_t>(sent);
                if (client.session_owned) {
                    if (!session.ConsumeLocalWrite(
                            connection_id,
                            sent_size)) {
                        published.error =
                            ClientProxyWorkerError::
                                SessionFailed;
                        schedule_close(
                            connection_id,
                            protocol::StreamCloseReason::
                                LocalError);
                        continue;
                    }
                } else {
                    client.fixed_response_offset +=
                        sent_size;
                }

                client.last_activity_tick =
                    GetTickCount64();
                made_progress = true;
            }

            // Agent FINは各queued responseを送った後に対応local send FINへ変換する。
            for (auto& [connection_id, client] :
                 local_clients) {
                if (!client.session_owned ||
                    !session.ShouldShutdownLocalSend(
                        connection_id)) {
                    continue;
                }

                if (shutdown(
                        client.socket.Get(),
                        SD_SEND) == SOCKET_ERROR) {
                    published.winsock_error =
                        static_cast<std::uint32_t>(
                            WSAGetLastError());
                    schedule_close(
                        connection_id,
                        protocol::StreamCloseReason::
                            LocalError);
                } else {
                    session.MarkLocalSendShutdown(
                        connection_id);
                }
                made_progress = true;
            }

            // 固定responseまたはterminal responseを全送信したSocketだけ閉じる。
            for (const auto& [connection_id, client] :
                 local_clients) {
                if ((!client.session_owned &&
                     client.fixed_response_offset >=
                         client.fixed_response.size()) ||
                    (client.session_owned &&
                     session.ShouldCloseLocalConnection(
                         connection_id))) {
                    schedule_close(
                        connection_id,
                        protocol::StreamCloseReason::
                            Normal);
                }
            }

            // range iteration終了後にSocket tableとsession recordを同期破棄する。
            for (const auto& request : close_requests) {
                close_local(
                    request.first,
                    request.second);
                made_progress = true;
            }

            protocol::Frame outbound{};
            // multiplexer outboundをUI向けbounded queueへwire順に移す。
            while (session.TakeOutboundFrame(outbound)) {
                const std::size_t frame_bytes =
                    FrameBytes(outbound);
                bool queued = false;
                {
                    std::scoped_lock lock(outbound_mutex_);
                    if (outbound_frames_.size() <
                            kMaximumProxyWorkerCommands &&
                        frame_bytes <=
                            kMaximumProxyWorkerFrameBytes -
                                outbound_frame_bytes_) {
                        outbound_frame_bytes_ +=
                            frame_bytes;
                        outbound_frames_.push_back(
                            std::move(outbound));
                        queued = true;
                    }
                }

                if (!queued) {
                    session.StopBridgeSession();
                    close_all(
                        protocol::StreamCloseReason::
                            LocalError);
                    published.error =
                        ClientProxyWorkerError::
                            OutboundQueueLimitExceeded;
                    break;
                }

                NotifyOwner();
                made_progress = true;
            }

            const ClientProxySessionSnapshot
                session_snapshot = session.snapshot();
            published.bridge_ready =
                session_snapshot.state !=
                    ClientProxySessionState::Stopped &&
                session_snapshot.state !=
                    ClientProxySessionState::Failed;
            published.active_streams =
                session_snapshot.active_streams;
            published.bytes_from_local =
                session_snapshot.bytes_from_local;
            published.bytes_to_local =
                session_snapshot.bytes_to_local;
            if (session_snapshot.state ==
                    ClientProxySessionState::Failed &&
                published.error ==
                    ClientProxyWorkerError::None) {
                published.error =
                    ClientProxyWorkerError::SessionFailed;
            }
            PublishSnapshot(published);

            if (!made_progress) {
                std::unique_lock lock(command_mutex_);
                command_ready_.wait_for(
                    lock,
                    kWorkerIdleWait,
                    [&]() noexcept {
                        return stop_requested_.load(
                                   std::memory_order_acquire) ||
                            !commands_.empty();
                    });
            }
        }

        close_all(
            protocol::StreamCloseReason::
                SessionStopping);
        session.StopBridgeSession();
        listener.Reset();
        published.state =
            ClientProxyWorkerState::Stopped;
        published.bridge_ready = false;
        published.active_streams = 0U;
        PublishSnapshot(published);
    } catch (...) {
        local_clients.clear();
        listener.Reset();
        fail_listener(
            ClientProxyWorkerError::SessionFailed,
            static_cast<std::uint32_t>(
                WSAEFAULT));
    }
}

void ClientProxyWorker::NotifyOwner() noexcept {
    // testでcallback未指定の場合はnotification flagを占有しない。
    if (notify_ == nullptr) {
        return;
    }

    bool expected = false;
    if (!owner_notification_pending_.
            compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
        return;
    }

    // post不能時は次状態変化で再試行できるようflagを戻す。
    if (!notify_(notify_context_)) {
        owner_notification_pending_.store(
            false,
            std::memory_order_release);
    }
}

void ClientProxyWorker::ClearOutboundFrames() noexcept {
    std::scoped_lock lock(outbound_mutex_);
    outbound_frames_.clear();
    outbound_frame_bytes_ = 0U;
}

void ClientProxyWorker::PublishSnapshot(
    const ClientProxyWorkerSnapshot& snapshot) noexcept {
    bool changed = false;
    {
        std::scoped_lock lock(snapshot_mutex_);
        changed =
            snapshot_.state != snapshot.state ||
            snapshot_.error != snapshot.error ||
            snapshot_.winsock_error !=
                snapshot.winsock_error ||
            snapshot_.configuration_error !=
                snapshot.configuration_error ||
            snapshot_.listening_port !=
                snapshot.listening_port ||
            snapshot_.bridge_ready !=
                snapshot.bridge_ready ||
            snapshot_.active_streams !=
                snapshot.active_streams ||
            snapshot_.bytes_from_local !=
                snapshot.bytes_from_local ||
            snapshot_.bytes_to_local !=
                snapshot.bytes_to_local ||
            snapshot_.accepted_connections !=
                snapshot.accepted_connections ||
            snapshot_.rejected_connections !=
                snapshot.rejected_connections ||
            snapshot_.timed_out_connections !=
                snapshot.timed_out_connections;
        snapshot_ = snapshot;
    }

    if (changed) {
        NotifyOwner();
    }
}

}  // namespace zrdp::client::proxy
