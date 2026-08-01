/**
 * @file upstream_connection.cpp
 * @brief DNSを使用しないWinsock outbound TCP adapterを実装する。
 *
 * @details SOCKETはRAII wrapperが所有し、作成直後からnon-blockingとする。
 *          production codeにはinbound `bind`／`listen`／`accept`経路を持たない。
 */

#include "agent/network/upstream_connection.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace zrdp::agent::network {
namespace {

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
     * @brief 所有Socket値を返す。
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
        // sentinel以外だけを一度closesocketする。
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

    /// `INVALID_SOCKET`またはこのinstanceだけがcloseするSocket。
    SOCKET socket_{INVALID_SOCKET};
};

/**
 * @brief Winsock errorをpeer reset系へ分類する。
 * @param[in] error `WSAGetLastError`または`SO_ERROR`。
 * @return reset／abort／network resetならtrue。
 */
[[nodiscard]] bool IsPeerResetError(
    const int error) noexcept {
    return error == WSAECONNRESET ||
        error == WSAECONNABORTED ||
        error == WSAENETRESET ||
        error == WSAESHUTDOWN;
}

/**
 * @brief Winsock errorをendpoint文字列なしの固定I/O結果へ変換する。
 * @param[in] error Winsock error code。
 * @return WouldBlock、PeerReset、Failedのいずれか。
 */
[[nodiscard]] UpstreamIoResult ClassifySocketError(
    const int error) noexcept {
    if (error == WSAEWOULDBLOCK ||
        error == WSAEINPROGRESS ||
        error == WSAEALREADY) {
        return UpstreamIoResult{
            UpstreamIoStatus::WouldBlock,
            0U,
            static_cast<std::uint32_t>(error),
        };
    }

    if (IsPeerResetError(error)) {
        return UpstreamIoResult{
            UpstreamIoStatus::PeerReset,
            0U,
            static_cast<std::uint32_t>(error),
        };
    }

    if (error == WSAETIMEDOUT) {
        return UpstreamIoResult{
            UpstreamIoStatus::TimedOut,
            0U,
            static_cast<std::uint32_t>(error),
        };
    }

    return UpstreamIoResult{
        UpstreamIoStatus::Failed,
        0U,
        static_cast<std::uint32_t>(error),
    };
}

/**
 * @brief 一つのnon-blocking Winsock TCP connectionを所有する。
 */
class WinsockUpstreamConnection final
    : public IUpstreamConnection {
public:
    WinsockUpstreamConnection(
        UniqueSocket socket,
        const bool connected) noexcept
        : socket_(std::move(socket)),
          connected_(connected) {
    }

    [[nodiscard]] UpstreamIoResult PollConnect() noexcept override {
        // connect完了後の重複pollは成功として扱い、Socket stateを変更しない。
        if (connected_) {
            return UpstreamIoResult{
                UpstreamIoStatus::Completed,
                0U,
                0U,
            };
        }

        // close済み接続をpendingとして再利用しない。
        if (socket_.Get() == INVALID_SOCKET) {
            return UpstreamIoResult{
                UpstreamIoStatus::Failed,
                0U,
                static_cast<std::uint32_t>(WSAENOTSOCK),
            };
        }

        fd_set writable{};
        fd_set exceptional{};
        FD_ZERO(&writable);
        FD_ZERO(&exceptional);
        FD_SET(socket_.Get(), &writable);
        FD_SET(socket_.Get(), &exceptional);
        timeval timeout{};
        const int selected = select(
            0,
            nullptr,
            &writable,
            &exceptional,
            &timeout);
        if (selected == SOCKET_ERROR) {
            return ClassifySocketError(
                WSAGetLastError());
        }

        // readinessなしではowner loopをblockingせず後続Pumpへ戻す。
        if (selected == 0) {
            return UpstreamIoResult{
                UpstreamIoStatus::Pending,
                0U,
                0U,
            };
        }

        int socket_error = 0;
        int option_size =
            static_cast<int>(sizeof(socket_error));
        // non-blocking connect結果はSO_ERRORで確定し、write-readyだけで成功扱いしない。
        if (getsockopt(
                socket_.Get(),
                SOL_SOCKET,
                SO_ERROR,
                reinterpret_cast<char*>(&socket_error),
                &option_size) == SOCKET_ERROR) {
            return ClassifySocketError(
                WSAGetLastError());
        }

        if (socket_error != 0) {
            return ClassifySocketError(socket_error);
        }

        connected_ = true;
        return UpstreamIoResult{
            UpstreamIoStatus::Completed,
            0U,
            0U,
        };
    }

    [[nodiscard]] UpstreamIoResult Send(
        const std::span<const std::byte> data) noexcept override {
        // 空buffer、未接続、close済みをWinsockへ渡さない。
        if (data.empty() ||
            !connected_ ||
            socket_.Get() == INVALID_SOCKET) {
            return UpstreamIoResult{
                UpstreamIoStatus::Failed,
                0U,
                static_cast<std::uint32_t>(WSAEINVAL),
            };
        }

        const std::size_t bounded_size = std::min(
            data.size(),
            static_cast<std::size_t>(
                std::numeric_limits<int>::max()));
        const int sent = send(
            socket_.Get(),
            reinterpret_cast<const char*>(data.data()),
            static_cast<int>(bounded_size),
            0);
        if (sent == SOCKET_ERROR) {
            return ClassifySocketError(
                WSAGetLastError());
        }

        // TCP sendの0-byte完了を進捗としてloopさせない。
        if (sent == 0) {
            return UpstreamIoResult{
                UpstreamIoStatus::WouldBlock,
                0U,
                0U,
            };
        }

        return UpstreamIoResult{
            UpstreamIoStatus::Completed,
            static_cast<std::size_t>(sent),
            0U,
        };
    }

    [[nodiscard]] UpstreamIoResult Receive(
        const std::span<std::byte> buffer) noexcept override {
        // 空buffer、未接続、close済みをWinsockへ渡さない。
        if (buffer.empty() ||
            !connected_ ||
            socket_.Get() == INVALID_SOCKET) {
            return UpstreamIoResult{
                UpstreamIoStatus::Failed,
                0U,
                static_cast<std::uint32_t>(WSAEINVAL),
            };
        }

        const std::size_t bounded_size = std::min(
            buffer.size(),
            static_cast<std::size_t>(
                std::numeric_limits<int>::max()));
        const int received = recv(
            socket_.Get(),
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(bounded_size),
            0);
        if (received == SOCKET_ERROR) {
            return ClassifySocketError(
                WSAGetLastError());
        }

        // TCPの0-byte receiveはorderly shutdownを意味する。
        if (received == 0) {
            return UpstreamIoResult{
                UpstreamIoStatus::PeerClosed,
                0U,
                0U,
            };
        }

        return UpstreamIoResult{
            UpstreamIoStatus::Completed,
            static_cast<std::size_t>(received),
            0U,
        };
    }

    [[nodiscard]] UpstreamIoResult ShutdownSend() noexcept override {
        // half-closeを一度だけ実行し、重複eventをWinsockへ渡さない。
        if (send_shutdown_) {
            return UpstreamIoResult{
                UpstreamIoStatus::Completed,
                0U,
                0U,
            };
        }

        if (!connected_ ||
            socket_.Get() == INVALID_SOCKET) {
            return UpstreamIoResult{
                UpstreamIoStatus::Failed,
                0U,
                static_cast<std::uint32_t>(WSAENOTCONN),
            };
        }

        if (shutdown(socket_.Get(), SD_SEND) ==
            SOCKET_ERROR) {
            return ClassifySocketError(
                WSAGetLastError());
        }

        send_shutdown_ = true;
        return UpstreamIoResult{
            UpstreamIoStatus::Completed,
            0U,
            0U,
        };
    }

    void Close() noexcept override {
        socket_.Reset();
        connected_ = false;
        send_shutdown_ = false;
    }

private:
    /// production adapterだけが所有するoutbound Socket。
    UniqueSocket socket_;

    /// `connect`完了後だけsend／receiveを許可するstate。
    bool connected_{false};

    /// TCP送信方向へ一度だけFINを送ったstate。
    bool send_shutdown_{false};
};

}  // namespace

WinsockRuntime::~WinsockRuntime() {
    Stop();
}

std::uint32_t WinsockRuntime::Start() noexcept {
    // 二重WSAStartupによるcleanup countのずれを作らない。
    if (started_) {
        return 0U;
    }

    WSADATA data{};
    const int result = WSAStartup(
        MAKEWORD(2, 2),
        &data);
    if (result != 0) {
        return static_cast<std::uint32_t>(result);
    }

    // 要求version以外を成功扱いせず、対応するcleanupを即時実行する。
    if (LOBYTE(data.wVersion) != 2U ||
        HIBYTE(data.wVersion) != 2U) {
        static_cast<void>(WSACleanup());
        return static_cast<std::uint32_t>(
            WSAVERNOTSUPPORTED);
    }

    started_ = true;
    return 0U;
}

void WinsockRuntime::Stop() noexcept {
    // Start成功分だけcleanup ownershipを消費する。
    if (!started_) {
        return;
    }

    static_cast<void>(WSACleanup());
    started_ = false;
}

bool WinsockRuntime::started() const noexcept {
    return started_;
}

UpstreamIoResult WinsockUpstreamConnector::BeginConnect(
    const protocol::OpenTarget& target,
    std::unique_ptr<IUpstreamConnection>& connection) noexcept {
    connection.reset();
    // allowlist parserを迂回した不正portでもSocket作成へ進まない。
    if (target.port == 0U) {
        return UpstreamIoResult{
            UpstreamIoStatus::Failed,
            0U,
            static_cast<std::uint32_t>(WSAEINVAL),
        };
    }

    UniqueSocket socket(::socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP));
    if (socket.Get() == INVALID_SOCKET) {
        return ClassifySocketError(
            WSAGetLastError());
    }

    u_long non_blocking = 1UL;
    // connect前にnon-blockingへ切り替え、Agent owner threadを停止させない。
    if (ioctlsocket(
            socket.Get(),
            FIONBIO,
            &non_blocking) == SOCKET_ERROR) {
        return ClassifySocketError(
            WSAGetLastError());
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(target.port);
    const std::uint32_t host_address =
        (static_cast<std::uint32_t>(target.ipv4[0U]) << 24U) |
        (static_cast<std::uint32_t>(target.ipv4[1U]) << 16U) |
        (static_cast<std::uint32_t>(target.ipv4[2U]) << 8U) |
        static_cast<std::uint32_t>(target.ipv4[3U]);
    address.sin_addr.s_addr = htonl(host_address);

    const int connect_result = connect(
        socket.Get(),
        reinterpret_cast<const sockaddr*>(&address),
        static_cast<int>(sizeof(address)));
    bool connected = connect_result == 0;
    UpstreamIoStatus status =
        connected
            ? UpstreamIoStatus::Completed
            : UpstreamIoStatus::Pending;
    std::uint32_t error = 0U;
    if (!connected) {
        const int connect_error = WSAGetLastError();
        // non-blocking開始の規定errorだけをpendingとして保持する。
        if (connect_error != WSAEWOULDBLOCK &&
            connect_error != WSAEINPROGRESS &&
            connect_error != WSAEALREADY) {
            return ClassifySocketError(connect_error);
        }

        error = static_cast<std::uint32_t>(
            connect_error);
    }

    try {
        connection =
            std::make_unique<WinsockUpstreamConnection>(
                std::move(socket),
                connected);
    } catch (const std::bad_alloc&) {
        return UpstreamIoResult{
            UpstreamIoStatus::Failed,
            0U,
            static_cast<std::uint32_t>(WSA_NOT_ENOUGH_MEMORY),
        };
    } catch (...) {
        return UpstreamIoResult{
            UpstreamIoStatus::Failed,
            0U,
            static_cast<std::uint32_t>(WSAEFAULT),
        };
    }

    return UpstreamIoResult{
        status,
        0U,
        error,
    };
}

}  // namespace zrdp::agent::network
