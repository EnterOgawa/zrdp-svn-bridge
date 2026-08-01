/**
 * @file client_proxy_worker_test.cpp
 * @brief production listenerのloopback限定bindとBridge未ready即時503を検証する。
 *
 * @details test processの127.0.0.1とOS割当portだけを使用し、外部network、
 *          DNS、実SVNへ接続しない。
 */

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>

#include "client/proxy/client_proxy_worker.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

/**
 * @brief test Socketを一意所有する。
 */
class TestSocket final {
public:
    TestSocket() noexcept = default;

    explicit TestSocket(
        const SOCKET socket) noexcept
        : socket_(socket) {
    }

    ~TestSocket() {
        if (socket_ != INVALID_SOCKET) {
            static_cast<void>(closesocket(socket_));
        }
    }

    TestSocket(const TestSocket&) = delete;
    TestSocket& operator=(const TestSocket&) = delete;

    TestSocket(TestSocket&& other) noexcept
        : socket_(other.Release()) {
    }

    TestSocket& operator=(TestSocket&& other) noexcept {
        if (this != &other) {
            if (socket_ != INVALID_SOCKET) {
                static_cast<void>(closesocket(socket_));
            }

            socket_ = other.Release();
        }

        return *this;
    }

    /**
     * @brief 非owning Socket値を返す。
     * @return 現在値。
     */
    [[nodiscard]] SOCKET Get() const noexcept {
        return socket_;
    }

private:
    /**
     * @brief close責任をmove先へ移す。
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
 * @brief workerが指定stateへ到達するまで有限時間pollする。
 * @param[in] worker 検証対象。
 * @param[in] state 期待state。
 * @return 2秒以内に一致した場合はtrue。
 */
[[nodiscard]] bool WaitForState(
    const zrdp::client::proxy::ClientProxyWorker& worker,
    const zrdp::client::proxy::ClientProxyWorkerState state) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    // background listener初期化を短い間隔で有限待機する。
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.snapshot().state == state) {
            return true;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(5));
    }

    return false;
}

/**
 * @brief workerが固定503拒否counterを公開するまで有限待機する。
 * @param[in] worker 検証対象。
 * @return 2秒以内に一件以上を公開した場合はtrue。
 */
[[nodiscard]] bool WaitForRejectedConnection(
    const zrdp::client::proxy::ClientProxyWorker& worker) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    // Socket送信とsnapshot公開は別threadなのでcounter公開まで有限pollする。
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.snapshot().rejected_connections >
            0U) {
            return true;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(2));
    }

    return false;
}

/**
 * @brief workerがBridge readyを公開するまで有限待機する。
 * @param[in] worker 検証対象。
 * @return 2秒以内にreadyとなった場合はtrue。
 */
[[nodiscard]] bool WaitForBridgeReady(
    const zrdp::client::proxy::ClientProxyWorker& worker) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    // owner commandの非同期適用を短い間隔で有限pollする。
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.snapshot().bridge_ready) {
            return true;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(2));
    }

    return false;
}

/**
 * @brief 指定件数のlocal timeoutが公開されるまで有限待機する。
 * @param[in] worker 検証対象。
 * @param[in] expected 最低timeout件数。
 * @return 2秒以内に到達した場合はtrue。
 */
[[nodiscard]] bool WaitForTimeoutCount(
    const zrdp::client::proxy::ClientProxyWorker& worker,
    const std::uint64_t expected) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    // workerの5 ms pollとsnapshot公開が完了するまで有限待機する。
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.snapshot().timed_out_connections >=
            expected) {
            return true;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(2));
    }

    return false;
}

/**
 * @brief test workerのloopback listenerへ有限受信timeout付きで接続する。
 * @param[in] port host byte orderのlistener port。
 * @return 接続済みSocket。失敗時はINVALID_SOCKETを所有する。
 */
[[nodiscard]] TestSocket ConnectLoopback(
    const std::uint16_t port) {
    TestSocket socket(::socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP));
    if (socket.Get() == INVALID_SOCKET) {
        return socket;
    }

    DWORD timeout = 2000U;
    static_cast<void>(setsockopt(
        socket.Get(),
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout),
        static_cast<int>(sizeof(timeout))));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    // 外部addressへfallbackせず、loopback接続失敗を無効Socketとして返す。
    if (connect(
            socket.Get(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address))) ==
        SOCKET_ERROR) {
        return TestSocket{};
    }

    return socket;
}

/**
 * @brief process所有listenerが指定portの127.0.0.1一件だけか確認する。
 * @param[in] port host byte orderのport。
 * @return TCP owner table上の完全一致が一件ならtrue。
 */
[[nodiscard]] bool HasOnlyLoopbackListener(
    const std::uint16_t port) {
    ULONG bytes = 0U;
    const DWORD first = GetExtendedTcpTable(
        nullptr,
        &bytes,
        FALSE,
        AF_INET,
        TCP_TABLE_OWNER_PID_LISTENER,
        0U);
    if (first != ERROR_INSUFFICIENT_BUFFER ||
        bytes == 0U) {
        return false;
    }

    std::vector<std::byte> storage(bytes);
    auto* const table =
        reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(
            storage.data());
    if (GetExtendedTcpTable(
            table,
            &bytes,
            FALSE,
            AF_INET,
            TCP_TABLE_OWNER_PID_LISTENER,
            0U) != NO_ERROR) {
        return false;
    }

    const DWORD process_id = GetCurrentProcessId();
    std::size_t matches = 0U;
    bool loopback = false;
    // 同process・同portの全listener addressを列挙する。
    for (DWORD index = 0U;
         index < table->dwNumEntries;
         ++index) {
        const MIB_TCPROW_OWNER_PID& row =
            table->table[index];
        if (row.dwOwningPid == process_id &&
            ntohs(
                static_cast<u_short>(
                    row.dwLocalPort)) == port) {
            ++matches;
            loopback =
                row.dwLocalAddr ==
                    htonl(INADDR_LOOPBACK);
            if (!loopback) {
                return false;
            }
        }
    }

    return matches == 1U && loopback;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief loopback Proxy workerのaccept、複数接続、停止cleanupを検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int main() {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        return 1;
    }

    zrdp::client::proxy::ClientProxyWorker worker;
    zrdp::client::proxy::ClientProxyWorkerSettings settings{};
    settings.port = 0U;
    settings.header_timeout_milliseconds = 100U;
    settings.idle_timeout_milliseconds = 150U;
    if (!worker.Start(settings, nullptr, nullptr) ||
        !WaitForState(
            worker,
            zrdp::client::proxy::
                ClientProxyWorkerState::Listening)) {
        worker.Stop();
        static_cast<void>(WSACleanup());
        return 2;
    }

    const auto listening = worker.snapshot();
    if (listening.listening_port == 0U ||
        listening.bridge_ready ||
        !HasOnlyLoopbackListener(
            listening.listening_port)) {
        worker.Stop();
        static_cast<void>(WSACleanup());
        return 3;
    }

    TestSocket client(::socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP));
    if (client.Get() == INVALID_SOCKET) {
        worker.Stop();
        static_cast<void>(WSACleanup());
        return 4;
    }

    DWORD timeout = 2000U;
    static_cast<void>(setsockopt(
        client.Get(),
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout),
        static_cast<int>(sizeof(timeout))));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);
    address.sin_port =
        htons(listening.listening_port);
    if (connect(
            client.Get(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address))) ==
        SOCKET_ERROR) {
        worker.Stop();
        static_cast<void>(WSACleanup());
        return 5;
    }

    std::string response(256U, '\0');
    const int received = recv(
        client.Get(),
        response.data(),
        static_cast<int>(response.size()),
        0);
    if (received <= 0) {
        worker.Stop();
        static_cast<void>(WSACleanup());
        return 6;
    }

    response.resize(
        static_cast<std::size_t>(received));
    if (!response.starts_with(
            "HTTP/1.1 503 Service Unavailable\r\n") ||
        response.find("Content-Length: 0\r\n") ==
            std::string::npos) {
        worker.Stop();
        static_cast<void>(WSACleanup());
        return 7;
    }

    if (!WaitForRejectedConnection(worker)) {
        worker.Stop();
        static_cast<void>(WSACleanup());
        return 8;
    }

    const auto rejected = worker.snapshot();
    if (rejected.rejected_connections == 0U ||
        rejected.active_streams != 0U) {
        worker.Stop();
        static_cast<void>(WSACleanup());
        return 9;
    }

    auto capabilities =
        zrdp::protocol::MakePhase2HelloCapabilities();
    capabilities.maximum_streams =
        zrdp::client::proxy::
            kMaximumClientProxyConnections;
    if (!worker.EnqueueBridgeStart(capabilities) ||
        !WaitForBridgeReady(worker)) {
        worker.Stop();
        static_cast<void>(WSACleanup());
        return 11;
    }

    {
        TestSocket slow_header =
            ConnectLoopback(listening.listening_port);
        const char partial = 'G';
        // 部分headerを一byte送っても絶対header期限を延長できないことを確認する。
        if (slow_header.Get() == INVALID_SOCKET ||
            send(
                slow_header.Get(),
                &partial,
                1,
                0) != 1 ||
            !WaitForTimeoutCount(worker, 1U)) {
            worker.Stop();
            static_cast<void>(WSACleanup());
            return 12;
        }

        std::array<char, 1> closed_probe{};
        if (recv(
                slow_header.Get(),
                closed_probe.data(),
                static_cast<int>(closed_probe.size()),
                0) > 0) {
            worker.Stop();
            static_cast<void>(WSACleanup());
            return 13;
        }
    }

    {
        TestSocket idle_connection =
            ConnectLoopback(listening.listening_port);
        const std::string request =
            "GET http://192.0.2.10/repository HTTP/1.1\r\n"
            "Host: 192.0.2.10\r\n\r\n";
        if (idle_connection.Get() == INVALID_SOCKET ||
            send(
                idle_connection.Get(),
                request.data(),
                static_cast<int>(request.size()),
                0) != static_cast<int>(request.size())) {
            worker.Stop();
            static_cast<void>(WSACleanup());
            return 14;
        }

        bool open_observed = false;
        std::uint32_t open_stream_id = 0U;
        const auto open_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        // 有効headerがOPENへ進んだことを確認し、header timeoutとの混同を防ぐ。
        while (std::chrono::steady_clock::now() <
            open_deadline) {
            zrdp::protocol::Frame frame{};
            bool more_frames = false;
            if (worker.TakeOutboundFrame(
                    frame,
                    more_frames) &&
                frame.type ==
                    zrdp::protocol::MessageType::Open) {
                open_observed = true;
                open_stream_id = frame.stream_id;
                break;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(2));
        }

        if (!open_observed ||
            !WaitForTimeoutCount(worker, 2U)) {
            worker.Stop();
            static_cast<void>(WSACleanup());
            return 15;
        }

        bool timeout_close_observed = false;
        const auto close_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        // idle期限をcounterだけでなくwireのCLOSE(Timeout)としても確認する。
        while (std::chrono::steady_clock::now() <
            close_deadline) {
            zrdp::protocol::Frame frame{};
            bool more_frames = false;
            if (worker.TakeOutboundFrame(
                    frame,
                    more_frames) &&
                frame.type ==
                    zrdp::protocol::MessageType::Close &&
                frame.stream_id == open_stream_id &&
                frame.sequence == 0U &&
                frame.payload ==
                    std::vector<std::byte>{
                        std::byte{0U},
                        std::byte{0U},
                        std::byte{0U},
                        std::byte{1U},
                    }) {
                timeout_close_observed = true;
                break;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(2));
        }

        std::array<char, 1> closed_probe{};
        if (!timeout_close_observed ||
            recv(
                idle_connection.Get(),
                closed_probe.data(),
                static_cast<int>(closed_probe.size()),
                0) != 0) {
            worker.Stop();
            static_cast<void>(WSACleanup());
            return 16;
        }
    }

    worker.Stop();
    const auto stopped = worker.snapshot();
    static_cast<void>(WSACleanup());
    if (stopped.state !=
            zrdp::client::proxy::
                ClientProxyWorkerState::Stopped ||
        stopped.listening_port != 0U) {
        return 17;
    }

    return 0;
}
