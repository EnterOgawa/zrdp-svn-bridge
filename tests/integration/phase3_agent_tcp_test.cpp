/**
 * @file phase3_agent_tcp_test.cpp
 * @brief production Winsock adapterでloopback Mock Serverとの単一streamを検証する。
 *
 * @details test process内の127.0.0.1だけを使用し、DNS、OS Proxy、実SVN、
 *          実取引先endpointへ接続しない。
 */

#include "agent/bridge/agent_stream_relay.h"
#include "agent/network/allowlist.h"
#include "agent/network/upstream_connection.h"
#include "protocol/multiplexer.h"

#include <WinSock2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using zrdp::agent::bridge::AgentStreamRelay;
using zrdp::protocol::Frame;
using zrdp::protocol::MultiplexerResult;
using zrdp::protocol::StreamMultiplexer;

/**
 * @brief integration test内のSOCKETを一意所有するRAII wrapper。
 */
class TestSocket final {
public:
    TestSocket() noexcept = default;

    explicit TestSocket(
        const SOCKET socket) noexcept
        : socket_(socket) {
    }

    ~TestSocket() {
        Reset();
    }

    TestSocket(const TestSocket&) = delete;
    TestSocket& operator=(const TestSocket&) = delete;

    TestSocket(TestSocket&& other) noexcept
        : socket_(other.Release()) {
    }

    TestSocket& operator=(TestSocket&& other) noexcept {
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
     * @brief 所有Socketを閉じてsentinelへ戻す。
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
     * @brief move先へclose責任を移す。
     * @return 以前の所有Socket。
     */
    [[nodiscard]] SOCKET Release() noexcept {
        const SOCKET released = socket_;
        socket_ = INVALID_SOCKET;
        return released;
    }

    SOCKET socket_{INVALID_SOCKET};
};

/**
 * @brief loopback一接続をechoして両方向half-closeするMock HTTP相当server。
 *
 * production Agentとは異なり、listen／acceptはintegration test process内だけで
 * 使用する。
 */
class MockEchoServer final {
public:
    MockEchoServer() = default;

    ~MockEchoServer() {
        Stop();
    }

    MockEchoServer(const MockEchoServer&) = delete;
    MockEchoServer& operator=(const MockEchoServer&) = delete;

    /**
     * @brief 127.0.0.1のOS割当portで一接続listenを開始する。
     * @return listenerとworkerを開始できた場合はtrue。
     */
    [[nodiscard]] bool Start() noexcept {
        listener_ = TestSocket(::socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP));
        if (listener_.Get() == INVALID_SOCKET) {
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr =
            htonl(INADDR_LOOPBACK);
        address.sin_port = 0U;
        if (bind(
                listener_.Get(),
                reinterpret_cast<const sockaddr*>(
                    &address),
                static_cast<int>(sizeof(address))) ==
                SOCKET_ERROR ||
            listen(listener_.Get(), 1) ==
                SOCKET_ERROR) {
            listener_.Reset();
            return false;
        }

        int address_size =
            static_cast<int>(sizeof(address));
        if (getsockname(
                listener_.Get(),
                reinterpret_cast<sockaddr*>(&address),
                &address_size) == SOCKET_ERROR) {
            listener_.Reset();
            return false;
        }

        port_ = ntohs(address.sin_port);
        // test thread生成失敗時はlistenerを閉じ、部分開始を公開しない。
        try {
            worker_ = std::thread(
                &MockEchoServer::WorkerLoop,
                this);
        } catch (...) {
            listener_.Reset();
            port_ = 0U;
            return false;
        }

        return true;
    }

    /**
     * @brief workerへ停止を通知し、全test Socketをjoin後に閉じる。
     */
    void Stop() noexcept {
        stop_requested_.store(
            true,
            std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }

        listener_.Reset();
    }

    /**
     * @brief OSが割り当てたloopback portを返す。
     * @return Start成功後の非0 port。
     */
    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    /**
     * @brief serverが受信した全binary dataのcopyを返す。
     * @return mutexで同期した所有copy。
     */
    [[nodiscard]] std::vector<std::byte>
    received() const {
        std::scoped_lock lock(received_mutex_);
        return received_;
    }

    /**
     * @brief server loopにSocket errorがなかったか返す。
     * @return 正常処理または停止要求ならtrue。
     */
    [[nodiscard]] bool succeeded() const noexcept {
        return succeeded_.load(
            std::memory_order_acquire);
    }

    /**
     * @brief Client側FINを受けてserver側FINも送ったか返す。
     * @return orderly shutdown完了時はtrue。
     */
    [[nodiscard]] bool orderly_shutdown() const noexcept {
        return orderly_shutdown_.load(
            std::memory_order_acquire);
    }

private:
    /**
     * @brief listener readinessを有限pollし、一接続をechoする。
     */
    void WorkerLoop() noexcept {
        TestSocket accepted;
        // stop要求を100 ms以内に確認しながら一接続だけacceptする。
        while (!stop_requested_.load(
            std::memory_order_acquire)) {
            fd_set readable{};
            FD_ZERO(&readable);
            FD_SET(listener_.Get(), &readable);
            timeval timeout{};
            timeout.tv_usec = 100000L;
            const int selected = select(
                0,
                &readable,
                nullptr,
                nullptr,
                &timeout);
            if (selected == SOCKET_ERROR) {
                succeeded_.store(
                    false,
                    std::memory_order_release);
                return;
            }

            if (selected == 0) {
                continue;
            }

            accepted = TestSocket(accept(
                listener_.Get(),
                nullptr,
                nullptr));
            if (accepted.Get() == INVALID_SOCKET) {
                succeeded_.store(
                    false,
                    std::memory_order_release);
                return;
            }

            break;
        }

        // 接続前にtest停止された場合は正常なcleanupとして終了する。
        if (accepted.Get() == INVALID_SOCKET) {
            return;
        }

        const DWORD timeout_milliseconds = 100U;
        if (setsockopt(
                accepted.Get(),
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(
                    &timeout_milliseconds),
                static_cast<int>(
                    sizeof(timeout_milliseconds))) ==
                SOCKET_ERROR ||
            setsockopt(
                accepted.Get(),
                SOL_SOCKET,
                SO_SNDTIMEO,
                reinterpret_cast<const char*>(
                    &timeout_milliseconds),
                static_cast<int>(
                    sizeof(timeout_milliseconds))) ==
                SOCKET_ERROR) {
            succeeded_.store(
                false,
                std::memory_order_release);
            return;
        }

        std::array<char, 8192> buffer{};
        // Client FINまたはtest停止まで、受信chunkを同じ順序でechoする。
        while (!stop_requested_.load(
            std::memory_order_acquire)) {
            const int received = recv(
                accepted.Get(),
                buffer.data(),
                static_cast<int>(buffer.size()),
                0);
            if (received > 0) {
                {
                    std::scoped_lock lock(
                        received_mutex_);
                    const auto* begin =
                        reinterpret_cast<
                            const std::byte*>(
                                buffer.data());
                    received_.insert(
                        received_.end(),
                        begin,
                        begin + received);
                }

                int offset = 0;
                // partial sendを含め、受信chunk全体を順序どおりechoする。
                while (offset < received) {
                    const int sent = send(
                        accepted.Get(),
                        buffer.data() + offset,
                        received - offset,
                        0);
                    if (sent == SOCKET_ERROR) {
                        const int error =
                            WSAGetLastError();
                        if (error ==
                                WSAETIMEDOUT ||
                            error ==
                                WSAEWOULDBLOCK) {
                            continue;
                        }

                        succeeded_.store(
                            false,
                            std::memory_order_release);
                        return;
                    }

                    // blocking Socketで0-byte進捗を無限loopとして扱わない。
                    if (sent == 0) {
                        succeeded_.store(
                            false,
                            std::memory_order_release);
                        return;
                    }

                    offset += sent;
                }

                continue;
            }

            if (received == 0) {
                // Client送信FINへserver送信FINを返して双方のhalf-closeを成立させる。
                if (shutdown(
                        accepted.Get(),
                        SD_SEND) == SOCKET_ERROR) {
                    succeeded_.store(
                        false,
                        std::memory_order_release);
                    return;
                }

                orderly_shutdown_.store(
                    true,
                    std::memory_order_release);
                return;
            }

            const int error = WSAGetLastError();
            // 短いreceive timeoutではstop flag確認へ戻る。
            if (error == WSAETIMEDOUT ||
                error == WSAEWOULDBLOCK) {
                continue;
            }

            succeeded_.store(
                false,
                std::memory_order_release);
            return;
        }
    }

    /// main test threadだけがcloseするloopback listener。
    TestSocket listener_;

    /// listenerとaccepted Socketを処理する一thread。
    std::thread worker_;

    /// workerの有限poll loopを停止するatomic flag。
    std::atomic_bool stop_requested_{false};

    /// Socket errorなしの初期値から失敗時だけfalseへ遷移する。
    std::atomic_bool succeeded_{true};

    /// Client FINとserver FINの両処理完了を示す。
    std::atomic_bool orderly_shutdown_{false};

    /// Start成功後に固定するloopback port。
    std::uint16_t port_{0U};

    /// received_のappendとcopyを同期する。
    mutable std::mutex received_mutex_;

    /// Mock Serverが受信したbinary data。
    std::vector<std::byte> received_;
};

/**
 * @brief Client outbound FrameをAgent relayへすべて渡す。
 * @param[in,out] client Client multiplexer。
 * @param[in,out] relay Agent relay。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferToRelay(
    StreamMultiplexer& client,
    AgentStreamRelay& relay) {
    Frame frame{};
    // WTS相当のFrame列をwire順のままdrainする。
    while (client.TakeOutboundFrame(frame)) {
        if (!relay.OnFrame(std::move(frame))) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Agent outbound FrameをClientへすべて渡す。
 * @param[in,out] relay Agent relay。
 * @param[in,out] client Client multiplexer。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferToClient(
    AgentStreamRelay& relay,
    StreamMultiplexer& client) {
    Frame frame{};
    // WTS相当のFrame列をwire順のままdrainする。
    while (relay.TakeOutboundFrame(frame)) {
        if (client.OnFrame(std::move(frame)) !=
            MultiplexerResult::Ok) {
            return false;
        }
    }

    return true;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Phase 3のAgentから許可済みmock TCP endpointへの中継を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::protocol::MultiplexerRole;
    using zrdp::protocol::OpenTarget;
    using zrdp::protocol::StreamCloseReason;
    using zrdp::protocol::StreamEvent;
    using zrdp::protocol::StreamEventType;

    zrdp::agent::network::WinsockRuntime
        winsock_runtime;
    if (winsock_runtime.Start() != 0U) {
        return 1;
    }

    MockEchoServer server;
    if (!server.Start() ||
        server.port() == 0U) {
        return 2;
    }

    const std::string configuration =
        "[allowlist]\nendpoint.9001=127.0.0.1:" +
        std::to_string(server.port()) +
        "\n";
    zrdp::agent::network::Allowlist allowlist;
    if (!zrdp::agent::network::ParseAllowlist(
            configuration,
            allowlist)
            .succeeded()) {
        return 3;
    }

    zrdp::agent::network::WinsockUpstreamConnector
        connector;
    AgentStreamRelay relay(allowlist, connector);
    StreamMultiplexer client(
        MultiplexerRole::Client);
    auto capabilities =
        zrdp::protocol::MakePhase2HelloCapabilities();
    capabilities.maximum_streams = 1U;
    if (client.StartSession(capabilities) !=
            MultiplexerResult::Ok ||
        !relay.StartSession(capabilities)) {
        return 4;
    }

    const OpenTarget target{
        {127U, 0U, 0U, 1U},
        server.port()};
    std::uint32_t stream_id = 0U;
    if (client.OpenStream(
            target,
            stream_id) !=
            MultiplexerResult::Ok ||
        !TransferToRelay(client, relay)) {
        return 5;
    }

    bool opened = false;
    const std::uint64_t connect_deadline =
        GetTickCount64() + 5000U;
    // production non-blocking connectを有限時間pumpしてOPEN_OKを待つ。
    while (!opened &&
        GetTickCount64() < connect_deadline) {
        if (!relay.Pump(GetTickCount64()) ||
            !TransferToClient(relay, client)) {
            return 6;
        }

        StreamEvent event{};
        // 一Pumpで届いた有限eventを全て確認し、対象streamのOPEN_OKを探す。
        while (client.TakeEvent(event)) {
            if (event.type ==
                StreamEventType::Opened) {
                opened = true;
            } else if (event.type ==
                       StreamEventType::OpenFailed) {
                return 7;
            } else {
                return 8;
            }
        }

        if (!opened) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
    }

    if (!opened) {
        return 9;
    }

    std::vector<std::byte> request(
        512U * 1024U + 137U);
    // 複数windowを跨ぐbinary patternで順序、欠落、重複をbyte比較する。
    for (std::size_t index = 0U;
         index < request.size();
         ++index) {
        request[index] =
            static_cast<std::byte>(
                (index * 13U + 17U) % 251U);
    }

    constexpr std::size_t kClientChunkBytes =
        16U * 1024U;
    std::size_t sent_offset = 0U;
    std::vector<std::byte> response;
    response.reserve(request.size());
    bool local_half_closed = false;
    bool remote_half_closed = false;
    bool closed = false;
    const std::uint64_t transfer_deadline =
        GetTickCount64() + 15000U;

    // DATA、credit、両方向FIN、CLOSE ackを同じ有限owner loopで進める。
    while (!closed &&
        GetTickCount64() < transfer_deadline) {
        if (sent_offset < request.size()) {
            const std::size_t current_size =
                std::min(
                    kClientChunkBytes,
                    request.size() - sent_offset);
            const std::span<const std::byte> chunk(
                request.data() + sent_offset,
                current_size);
            const MultiplexerResult send_result =
                client.SendData(stream_id, chunk);
            if (send_result ==
                MultiplexerResult::Ok) {
                sent_offset += current_size;
            } else if (send_result !=
                       MultiplexerResult::WouldBlock) {
                return 10;
            }
        }

        // request全量をqueueした後にだけClient送信方向を閉じる。
        if (sent_offset == request.size() &&
            !local_half_closed) {
            if (client.HalfCloseStream(stream_id) !=
                MultiplexerResult::Ok) {
                return 11;
            }

            local_half_closed = true;
        }

        if (!TransferToRelay(client, relay) ||
            !relay.Pump(GetTickCount64()) ||
            !TransferToClient(relay, client)) {
            return 12;
        }

        StreamEvent event{};
        // Client eventを全件消費し、実際にcopyしたDATAだけcreditを返す。
        while (client.TakeEvent(event)) {
            if (event.type ==
                StreamEventType::DataReceived) {
                response.insert(
                    response.end(),
                    event.data.begin(),
                    event.data.end());
                if (client.ConsumeReceivedData(
                        stream_id,
                        static_cast<std::uint32_t>(
                            event.data.size())) !=
                    MultiplexerResult::Ok) {
                    return 13;
                }
            } else if (event.type ==
                       StreamEventType::
                           RemoteHalfClosed) {
                remote_half_closed = true;
            } else if (event.type ==
                       StreamEventType::Closed) {
                if (event.close_reason !=
                    StreamCloseReason::Normal) {
                    return 14;
                }

                closed = true;
            } else {
                return 15;
            }
        }

        // WINDOW_UPDATEとCLOSE ackを次Pumpより前にAgentへ戻す。
        if (!TransferToRelay(client, relay)) {
            return 16;
        }

        if (!closed) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1));
        }
    }

    server.Stop();
    if (!closed) {
        return 17;
    }

    if (!local_half_closed) {
        return 18;
    }

    if (!remote_half_closed) {
        return 19;
    }

    if (sent_offset != request.size()) {
        return 20;
    }

    if (response != request) {
        return 21;
    }

    if (server.received() != request) {
        return 22;
    }

    if (!server.succeeded()) {
        return 23;
    }

    if (!server.orderly_shutdown()) {
        return 24;
    }

    if (relay.snapshot().active_stream_id != 0U) {
        return 25;
    }

    return 0;
}
