/**
 * @file phase5_http_svn_compatibility_test.cpp
 * @brief production Proxy／SVC経路でMock WebDAV／Trac sequenceを検証する。
 *
 * @details test process内の127.0.0.1 listenerとin-memory Mock upstreamだけを使う。
 *          DNS、OS Proxy、実SVN／Trac、取引先endpoint、Zscaler設定には
 *          アクセスしない。
 */

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include "agent/bridge/agent_bridge.h"
#include "agent/bridge/agent_stream_relay.h"
#include "agent/network/allowlist.h"
#include "agent/network/upstream_connection.h"
#include "client/bridge/client_bridge.h"
#include "client/proxy/client_proxy_worker.h"
#include "protocol/hello.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using zrdp::agent::bridge::AgentStreamRelay;
using zrdp::agent::network::IUpstreamConnection;
using zrdp::agent::network::IUpstreamConnector;
using zrdp::agent::network::UpstreamIoResult;
using zrdp::agent::network::UpstreamIoStatus;
using zrdp::client::proxy::ClientProxyWorker;
using zrdp::protocol::Frame;
using zrdp::protocol::OpenTarget;

/// Mock Socketが一回に送受信する公平な部分I/O上限。
constexpr std::size_t kMockIoChunkBytes = 97U;

/**
 * @brief test local Socketを一所有者で閉じる。
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
 * @brief 一upstream Socketで順に期待するrequest／response transcript。
 */
struct MockWebDavScenario {
    OpenTarget target{};
    std::vector<std::string> expected_segments;
    std::vector<std::string> response_segments;
    std::size_t request_segment{0U};
    std::size_t request_offset{0U};
    std::deque<std::size_t> ready_responses;
    std::size_t response_offset{0U};
    std::uint32_t connection_count{0U};
    std::uint32_t close_count{0U};
    bool mismatch{false};
};

/**
 * @brief 一scenarioを部分I/Oで処理するin-memory upstream Socket。
 */
class MockWebDavConnection final :
    public IUpstreamConnection {
public:
    /**
     * @brief test所有scenarioへ固定する。
     * @param[in] scenario connectionより長く生存するstate。
     */
    explicit MockWebDavConnection(
        MockWebDavScenario& scenario) noexcept
        : scenario_(scenario) {
    }

    ~MockWebDavConnection() override {
        Close();
    }

    [[nodiscard]] UpstreamIoResult
    PollConnect() noexcept override {
        return {
            UpstreamIoStatus::Completed,
            0U,
            0U,
        };
    }

    [[nodiscard]] UpstreamIoResult Send(
        const std::span<const std::byte> data) noexcept override {
        // terminal、不正空data、想定外の追加requestを成功扱いしない。
        if (closed_ ||
            data.empty() ||
            scenario_.request_segment >=
                scenario_.expected_segments.size()) {
            scenario_.mismatch = true;
            return {
                UpstreamIoStatus::Failed,
                0U,
                1U,
            };
        }

        const std::string& expected =
            scenario_.expected_segments[
                scenario_.request_segment];
        if (scenario_.request_offset >=
            expected.size()) {
            scenario_.mismatch = true;
            return {
                UpstreamIoStatus::Failed,
                0U,
                2U,
            };
        }

        const std::size_t accepted = std::min({
            data.size(),
            expected.size() -
                scenario_.request_offset,
            kMockIoChunkBytes,
        });
        // origin-form変換後のrequest byteをpayload保存なしで逐次照合する。
        for (std::size_t index = 0U;
             index < accepted;
             ++index) {
            const auto expected_byte =
                static_cast<std::byte>(
                    static_cast<unsigned char>(
                        expected[
                            scenario_.request_offset +
                            index]));
            if (data[index] != expected_byte) {
                scenario_.mismatch = true;
                return {
                    UpstreamIoStatus::Failed,
                    0U,
                    3U,
                };
            }
        }

        scenario_.request_offset += accepted;
        if (scenario_.request_offset ==
            expected.size()) {
            try {
                scenario_.ready_responses.push_back(
                    scenario_.request_segment);
            } catch (...) {
                return {
                    UpstreamIoStatus::Failed,
                    0U,
                    4U,
                };
            }

            ++scenario_.request_segment;
            scenario_.request_offset = 0U;
        }

        return {
            UpstreamIoStatus::Completed,
            accepted,
            0U,
        };
    }

    [[nodiscard]] UpstreamIoResult Receive(
        const std::span<std::byte> buffer) noexcept override {
        if (closed_) {
            return {
                UpstreamIoStatus::Failed,
                0U,
                5U,
            };
        }

        if (scenario_.ready_responses.empty()) {
            return {
                UpstreamIoStatus::WouldBlock,
                0U,
                0U,
            };
        }

        const std::size_t response_index =
            scenario_.ready_responses.front();
        // request／response vectorの対応関係を使用直前にも再検証する。
        if (response_index >=
                scenario_.response_segments.size() ||
            buffer.empty()) {
            scenario_.mismatch = true;
            return {
                UpstreamIoStatus::Failed,
                0U,
                6U,
            };
        }

        const std::string& response =
            scenario_.response_segments[
                response_index];
        if (scenario_.response_offset >=
            response.size()) {
            scenario_.mismatch = true;
            return {
                UpstreamIoStatus::Failed,
                0U,
                7U,
            };
        }

        const std::size_t received = std::min({
            buffer.size(),
            response.size() -
                scenario_.response_offset,
            kMockIoChunkBytes,
        });
        // opaque HTTP responseを変更せずClient向けbufferへcopyする。
        for (std::size_t index = 0U;
             index < received;
             ++index) {
            buffer[index] = static_cast<std::byte>(
                static_cast<unsigned char>(
                    response[
                        scenario_.response_offset +
                        index]));
        }

        scenario_.response_offset += received;
        if (scenario_.response_offset ==
            response.size()) {
            scenario_.ready_responses.pop_front();
            scenario_.response_offset = 0U;
        }

        return {
            UpstreamIoStatus::Completed,
            received,
            0U,
        };
    }

    [[nodiscard]] UpstreamIoResult
    ShutdownSend() noexcept override {
        if (closed_) {
            return {
                UpstreamIoStatus::Failed,
                0U,
                8U,
            };
        }

        return {
            UpstreamIoStatus::Completed,
            0U,
            0U,
        };
    }

    void Close() noexcept override {
        // Agent session cleanupが同じSocketを重複closeしないことを数える。
        if (!closed_) {
            closed_ = true;
            ++scenario_.close_count;
        }
    }

private:
    MockWebDavScenario& scenario_;
    bool closed_{false};
};

/**
 * @brief allowlist一致後のtargetを二つのMock WebDAV scenarioへ割り当てる。
 */
class MockWebDavConnector final :
    public IUpstreamConnector {
public:
    /**
     * @brief 二つの異なるserver／port scenarioを固定する。
     * @param[in] first 一件目。
     * @param[in] second 二件目。
     */
    MockWebDavConnector(
        MockWebDavScenario& first,
        MockWebDavScenario& second) noexcept
        : first_(first),
          second_(second) {
    }

    [[nodiscard]] UpstreamIoResult BeginConnect(
        const OpenTarget& target,
        std::unique_ptr<IUpstreamConnection>&
            connection) noexcept override {
        connection.reset();
        MockWebDavScenario* scenario = nullptr;
        if (target == first_.target) {
            scenario = &first_;
        } else if (target == second_.target) {
            scenario = &second_;
        }

        // allowlist外またはtest scenario外へ任意connectionを生成しない。
        if (scenario == nullptr ||
            scenario->connection_count != 0U) {
            return {
                UpstreamIoStatus::Failed,
                0U,
                9U,
            };
        }

        try {
            connection =
                std::make_unique<
                    MockWebDavConnection>(
                    *scenario);
        } catch (...) {
            return {
                UpstreamIoStatus::Failed,
                0U,
                10U,
            };
        }

        ++scenario->connection_count;
        return {
            UpstreamIoStatus::Completed,
            0U,
            0U,
        };
    }

private:
    MockWebDavScenario& first_;
    MockWebDavScenario& second_;
};

/**
 * @brief local HTTP client一件の部分送受信状態。
 */
struct LocalClientFlow {
    TestSocket socket;
    std::string pending_send;
    std::size_t send_offset{0U};
    std::string received;
};

/**
 * @brief absolute-formまたはorigin-formの一requestを構築する。
 * @param[in] method HTTP／WebDAV method。
 * @param[in] authority IPv4 literalとoptional port。
 * @param[in] path slash始まりpath。
 * @param[in] headers Hostを含むheader列と終端CRLF。
 * @param[in] body request body。
 * @param[in] absolute absolute-formならtrue。
 * @return test所有request transcript。
 */
[[nodiscard]] std::string MakeRequest(
    const std::string_view method,
    const std::string_view authority,
    const std::string_view path,
    const std::string_view headers,
    const std::string_view body,
    const bool absolute) {
    std::string request;
    request.append(method);
    request.push_back(' ');
    if (absolute) {
        request.append("http://");
        request.append(authority);
    }
    request.append(path);
    request.append(" HTTP/1.1\r\n");
    request.append(headers);
    request.append("\r\n");
    request.append(body);
    return request;
}

/**
 * @brief bodyなし固定HTTP responseを返す。
 * @param[in] status status lineのcodeとreason。
 * @return Keep-Alive可能なresponse。
 */
[[nodiscard]] std::string EmptyResponse(
    const std::string_view status) {
    std::string response("HTTP/1.1 ");
    response.append(status);
    response.append(
        "\r\nContent-Length: 0\r\n\r\n");
    return response;
}

/**
 * @brief Client worker Frameをproduction SVC encoder経由でAgentへ渡す。
 * @param[in,out] worker Client worker。
 * @param[in,out] client_bridge Client SVC owner。
 * @param[in,out] agent_bridge Agent SVC owner。
 * @return 全Frame／chunkを受理できた場合はtrue。
 */
[[nodiscard]] bool PumpClientToAgent(
    ClientProxyWorker& worker,
    zrdp::client::bridge::ClientBridge& client_bridge,
    zrdp::agent::bridge::AgentBridge& agent_bridge) {
    Frame frame{};
    bool more_frames = false;
    // worker outbound Frameをproduction ClientBridgeへ全件移す。
    while (worker.TakeOutboundFrame(
        frame,
        more_frames)) {
        if (!client_bridge.QueueStreamFrame(frame)) {
            return false;
        }
    }

    bool more_chunks = false;
    std::vector<std::byte> chunk;
    // SVC transport chunkをwire順でAgent decoderへ渡す。
    while (client_bridge.TakeOutboundChunk(
        chunk,
        more_chunks)) {
        if (!agent_bridge.OnChannelData(chunk)) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Agent Socket／SVCを進めてClient workerへFrameを返す。
 * @param[in,out] agent_bridge Agent SVC／relay owner。
 * @param[in,out] client_bridge Client SVC decoder。
 * @param[in,out] worker Client worker。
 * @return 全I/Oを受理できた場合はtrue。
 */
[[nodiscard]] bool PumpAgentToClient(
    zrdp::agent::bridge::AgentBridge& agent_bridge,
    zrdp::client::bridge::ClientBridge& client_bridge,
    ClientProxyWorker& worker) {
    if (!agent_bridge.PumpStreams(GetTickCount64())) {
        return false;
    }

    bool more_chunks = false;
    std::vector<std::byte> chunk;
    // Agent serialized writerからClient decoderへ全chunkを戻す。
    while (agent_bridge.TakeOutboundChunk(
        chunk,
        more_chunks)) {
        if (!client_bridge.OnChannelData(chunk)) {
            return false;
        }
    }

    Frame frame{};
    // Client decoderが所有化したstream Frameだけをworkerへ渡す。
    while (client_bridge.TakeInboundStreamFrame(frame)) {
        if (!worker.EnqueueBridgeFrame(
                std::move(frame))) {
            return false;
        }
    }

    return true;
}

/**
 * @brief worker listenerとBridge sessionのreadyを有限待機する。
 * @param[in] worker 対象worker。
 * @return 2秒以内に両方readyならtrue。
 */
[[nodiscard]] bool WaitForProxyReady(
    const ClientProxyWorker& worker) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    // background listener／commandを短い間隔でpollする。
    while (std::chrono::steady_clock::now() <
           deadline) {
        const auto snapshot = worker.snapshot();
        if (snapshot.state ==
                zrdp::client::proxy::
                    ClientProxyWorkerState::Listening &&
            snapshot.bridge_ready &&
            snapshot.listening_port != 0U) {
            return true;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(2));
    }

    return false;
}

/**
 * @brief Proxy loopback listenerへ一local clientを接続する。
 * @param[in] port host byte orderのlistener port。
 * @param[out] socket 成功時のnon-blocking所有Socket。
 * @return 接続とmode設定が成功した場合はtrue。
 */
[[nodiscard]] bool ConnectLocalClient(
    const std::uint16_t port,
    TestSocket& socket) noexcept {
    socket.Reset(::socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP));
    if (socket.Get() == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr =
        htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(
            socket.Get(),
            reinterpret_cast<const sockaddr*>(
                &address),
            static_cast<int>(sizeof(address))) ==
        SOCKET_ERROR) {
        return false;
    }

    u_long enabled = 1UL;
    return ioctlsocket(
               socket.Get(),
               FIONBIO,
               &enabled) != SOCKET_ERROR;
}

/**
 * @brief local clientの未送信requestを一回進める。
 * @param[in,out] flow 対象client。
 * @return fatal Socket errorがなければtrue。
 */
[[nodiscard]] bool PumpLocalSend(
    LocalClientFlow& flow) noexcept {
    if (flow.send_offset >=
        flow.pending_send.size()) {
        return true;
    }

    const std::size_t remaining =
        flow.pending_send.size() -
        flow.send_offset;
    const int sent = send(
        flow.socket.Get(),
        flow.pending_send.data() +
            flow.send_offset,
        static_cast<int>(remaining),
        0);
    if (sent == SOCKET_ERROR) {
        return WSAGetLastError() ==
            WSAEWOULDBLOCK;
    }

    if (sent <= 0) {
        return false;
    }

    flow.send_offset +=
        static_cast<std::size_t>(sent);
    return true;
}

/**
 * @brief local clientのresponseを有限回だけ所有bufferへ受け取る。
 * @param[in,out] flow 対象client。
 * @return fatal Socket error／予期しないFINがなければtrue。
 */
[[nodiscard]] bool PumpLocalReceive(
    LocalClientFlow& flow) noexcept {
    std::array<char, 4096U> buffer{};
    // 一clientがmain test loopを占有しないよう最大4回で制御を戻す。
    for (std::size_t operation = 0U;
         operation < 4U;
         ++operation) {
        const int received = recv(
            flow.socket.Get(),
            buffer.data(),
            static_cast<int>(buffer.size()),
            0);
        if (received == SOCKET_ERROR) {
            return WSAGetLastError() ==
                WSAEWOULDBLOCK;
        }

        if (received <= 0) {
            return false;
        }

        try {
            flow.received.append(
                buffer.data(),
                static_cast<std::size_t>(received));
        } catch (...) {
            return false;
        }
    }

    return true;
}

/**
 * @brief absolute-formとorigin-formの同じrequestを各transcriptへ追加する。
 * @param[in,out] local local client送信列。
 * @param[in,out] upstream Mock upstream期待列。
 * @param[in] method HTTP method。
 * @param[in] authority IPv4 authority。
 * @param[in] path repository path。
 * @param[in] headers header列。
 * @param[in] body body wire bytes。
 */
void AppendRequestPair(
    std::string& local,
    std::string& upstream,
    const std::string_view method,
    const std::string_view authority,
    const std::string_view path,
    const std::string_view headers,
    const std::string_view body) {
    local.append(MakeRequest(
        method,
        authority,
        path,
        headers,
        body,
        true));
    upstream.append(MakeRequest(
        method,
        authority,
        path,
        headers,
        body,
        false));
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Phase 5のSVN／WebDAV相当sequenceと複数repositoryを結合検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int main() {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        return 1;
    }

    constexpr std::string_view authority_a =
        "192.0.2.10";
    constexpr std::string_view authority_b =
        "198.51.100.20:8080";
    MockWebDavScenario scenario_a{};
    scenario_a.target =
        OpenTarget{{192U, 0U, 2U, 10U}, 80U};
    MockWebDavScenario scenario_b{};
    scenario_b.target =
        OpenTarget{{198U, 51U, 100U, 20U}, 8080U};

    std::string local_a_first;
    std::string upstream_a_first;
    local_a_first = MakeRequest(
        "OPTIONS",
        authority_a,
        "/svn/RepoA",
        "Host: 192.0.2.10\r\n"
        "Connection: keep-alive\r\n"
        "Proxy-Connection: keep-alive\r\n"
        "Content-Length: 0\r\n",
        "",
        true);
    upstream_a_first = MakeRequest(
        "OPTIONS",
        authority_a,
        "/svn/RepoA",
        "Host: 192.0.2.10\r\n"
        "Content-Length: 0\r\n",
        "",
        false);
    const std::string response_a_first =
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: NTLM opaque-challenge\r\n"
        "Content-Length: 0\r\n\r\n";

    std::string local_a_second;
    std::string upstream_a_second;
    AppendRequestPair(
        local_a_second,
        upstream_a_second,
        "OPTIONS",
        authority_a,
        "/svn/RepoA",
        "Host: 192.0.2.10\r\n"
        "Authorization: NTLM opaque-response\r\n"
        "Content-Length: 0\r\n",
        "");
    AppendRequestPair(
        local_a_second,
        upstream_a_second,
        "GET",
        authority_a,
        "/svn/RepoA/file",
        "Host: 192.0.2.10\r\n"
        "Content-Length: 0\r\n",
        "");
    AppendRequestPair(
        local_a_second,
        upstream_a_second,
        "HEAD",
        authority_a,
        "/svn/RepoA/file",
        "Host: 192.0.2.10\r\n"
        "Content-Length: 0\r\n",
        "");
    AppendRequestPair(
        local_a_second,
        upstream_a_second,
        "POST",
        authority_a,
        "/svn/RepoA/hook",
        "Host: 192.0.2.10\r\n"
        "Content-Length: 4\r\n",
        "post");

    AppendRequestPair(
        local_a_second,
        upstream_a_second,
        "GET",
        authority_a,
        "/trac/ProjectA/login?referer=%2Fquery",
        "Host: 192.0.2.10\r\n"
        "User-Agent: Mylyn-Trac-Test\r\n"
        "Accept-Encoding: gzip\r\n"
        "Content-Length: 0\r\n",
        "");
    constexpr std::string_view trac_xml_body =
        "<?xml version=\"1.0\"?>"
        "<methodCall>"
        "<methodName>system.getAPIVersion</methodName>"
        "<params/>"
        "</methodCall>";
    const std::string trac_xml_headers =
        "Host: 192.0.2.10\r\n"
        "User-Agent: Mylyn-Trac-Test\r\n"
        "Content-Type: text/xml\r\n"
        "Accept-Encoding: gzip\r\n"
        "Authorization: Digest opaque-repository-response\r\n"
        "Cookie: trac_auth=opaque-session\r\n"
        "Content-Length: " +
        std::to_string(trac_xml_body.size()) +
        "\r\n";
    AppendRequestPair(
        local_a_second,
        upstream_a_second,
        "POST",
        authority_a,
        "/trac/ProjectA/xmlrpc",
        trac_xml_headers,
        trac_xml_body);

    AppendRequestPair(
        local_a_second,
        upstream_a_second,
        "PROPFIND",
        authority_a,
        "/svn/RepoA",
        "Host: 192.0.2.10\r\n"
        "Depth: 1\r\n"
        "Content-Length: 6\r\n",
        "<prop>");
    AppendRequestPair(
        local_a_second,
        upstream_a_second,
        "REPORT",
        authority_a,
        "/svn/RepoA!svn/rev/7",
        "Host: 192.0.2.10\r\n"
        "Expect: 100-continue\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Trailer: X-Trace\r\n",
        "4;kind=svndiff\r\nbody\r\n"
        "0\r\nX-Trace: done\r\n\r\n");
    AppendRequestPair(
        local_a_second,
        upstream_a_second,
        "LOCK",
        authority_a,
        "/svn/RepoA/file",
        "Host: 192.0.2.10\r\n"
        "Timeout: Second-3600\r\n"
        "Content-Length: 6\r\n",
        "<lock>");
    AppendRequestPair(
        local_a_second,
        upstream_a_second,
        "UNLOCK",
        authority_a,
        "/svn/RepoA/file",
        "Host: 192.0.2.10\r\n"
        "Lock-Token: <opaquelocktoken:test-a>\r\n"
        "Content-Length: 0\r\n",
        "");

    std::string response_a_second;
    response_a_second.append(
        EmptyResponse("200 OK"));
    response_a_second.append(
        EmptyResponse("200 OK"));
    response_a_second.append(
        EmptyResponse("200 OK"));
    response_a_second.append(
        EmptyResponse("201 Created"));
    response_a_second.append(
        "HTTP/1.1 302 Found\r\n"
        "Location: http://192.0.2.10/trac/ProjectA/login\r\n"
        "Set-Cookie: trac_auth=opaque-session; "
        "Path=/trac/ProjectA; HttpOnly\r\n"
        "Content-Length: 0\r\n\r\n");
    response_a_second.append(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/xml\r\n"
        "Content-Encoding: gzip\r\n"
        "Content-Length: 4\r\n\r\n"
        "gzip");
    response_a_second.append(
        "HTTP/1.1 207 Multi-Status\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4\r\nbody\r\n0\r\n\r\n");
    response_a_second.append(
        "HTTP/1.1 100 Continue\r\n\r\n");
    response_a_second.append(
        EmptyResponse("200 OK"));
    response_a_second.append(
        EmptyResponse("200 OK"));
    response_a_second.append(
        EmptyResponse("204 No Content"));

    std::string local_b;
    std::string upstream_b;
    AppendRequestPair(
        local_b,
        upstream_b,
        "MKACTIVITY",
        authority_b,
        "/svn/RepoB/act/1",
        "Host: 198.51.100.20:8080\r\n"
        "Content-Length: 0\r\n",
        "");
    AppendRequestPair(
        local_b,
        upstream_b,
        "CHECKOUT",
        authority_b,
        "/svn/RepoB/wrk/1",
        "Host: 198.51.100.20:8080\r\n"
        "Content-Length: 0\r\n",
        "");
    AppendRequestPair(
        local_b,
        upstream_b,
        "PUT",
        authority_b,
        "/svn/RepoB/file",
        "Host: 198.51.100.20:8080\r\n"
        "Content-Length: 3\r\n",
        "put");
    AppendRequestPair(
        local_b,
        upstream_b,
        "DELETE",
        authority_b,
        "/svn/RepoB/old",
        "Host: 198.51.100.20:8080\r\n"
        "Content-Length: 0\r\n",
        "");
    AppendRequestPair(
        local_b,
        upstream_b,
        "COPY",
        authority_b,
        "/svn/RepoB/src",
        "Host: 198.51.100.20:8080\r\n"
        "Destination: http://198.51.100.20:8080/svn/RepoB/copy\r\n"
        "Content-Length: 0\r\n",
        "");
    AppendRequestPair(
        local_b,
        upstream_b,
        "MOVE",
        authority_b,
        "/svn/RepoB/copy",
        "Host: 198.51.100.20:8080\r\n"
        "Destination: http://198.51.100.20:8080/svn/RepoB/moved\r\n"
        "Content-Length: 0\r\n",
        "");
    AppendRequestPair(
        local_b,
        upstream_b,
        "MERGE",
        authority_b,
        "/svn/RepoB",
        "Host: 198.51.100.20:8080\r\n"
        "Content-Length: 7\r\n",
        "<merge>");
    AppendRequestPair(
        local_b,
        upstream_b,
        "LOCK",
        authority_b,
        "/svn/RepoB/file",
        "Host: 198.51.100.20:8080\r\n"
        "Content-Length: 6\r\n",
        "<lock>");
    AppendRequestPair(
        local_b,
        upstream_b,
        "UNLOCK",
        authority_b,
        "/svn/RepoB/file",
        "Host: 198.51.100.20:8080\r\n"
        "Lock-Token: <opaquelocktoken:test-b>\r\n"
        "Content-Length: 0\r\n",
        "");

    std::string response_b;
    // write系WebDAV sequenceへ一request一responseを同一Socketで返す。
    for (std::size_t index = 0U;
         index < 9U;
         ++index) {
        response_b.append(
            EmptyResponse(
                index == 2U
                    ? "201 Created"
                    : "200 OK"));
    }

    scenario_a.expected_segments = {
        upstream_a_first,
        upstream_a_second,
    };
    scenario_a.response_segments = {
        response_a_first,
        response_a_second,
    };
    scenario_b.expected_segments = {upstream_b};
    scenario_b.response_segments = {response_b};

    zrdp::agent::network::Allowlist allowlist;
    if (!zrdp::agent::network::ParseAllowlist(
            "[allowlist]\n"
            "endpoint.5001=192.0.2.10:80\n"
            "endpoint.5002=198.51.100.20:8080\n",
            allowlist)
             .succeeded()) {
        static_cast<void>(WSACleanup());
        return 2;
    }

    MockWebDavConnector connector(
        scenario_a,
        scenario_b);
    AgentStreamRelay relay(allowlist, connector);
    zrdp::agent::bridge::AgentBridge agent_bridge;
    agent_bridge.ConfigureStreamRelay(&relay);
    zrdp::client::bridge::ClientBridge client_bridge;
    ClientProxyWorker worker;
    if (!client_bridge.StartSession() ||
        !PumpClientToAgent(
            worker,
            client_bridge,
            agent_bridge) ||
        !PumpAgentToClient(
            agent_bridge,
            client_bridge,
            worker) ||
        client_bridge.snapshot().state !=
            zrdp::client::bridge::
                ClientBridgeState::Ready ||
        client_bridge.negotiated_capabilities().
                maximum_streams !=
            zrdp::client::bridge::
                kMaximumClientBridgeStreams) {
        static_cast<void>(WSACleanup());
        return 3;
    }

    zrdp::client::proxy::ClientProxyWorkerSettings
        settings{};
    settings.port = 0U;
    if (!worker.Start(settings, nullptr, nullptr) ||
        !worker.EnqueueBridgeStart(
            client_bridge.
                negotiated_capabilities()) ||
        !WaitForProxyReady(worker)) {
        worker.Stop();
        relay.StopSession();
        static_cast<void>(WSACleanup());
        return 4;
    }

    LocalClientFlow client_a{};
    LocalClientFlow client_b{};
    const std::uint16_t proxy_port =
        worker.snapshot().listening_port;
    if (!ConnectLocalClient(
            proxy_port,
            client_a.socket) ||
        !ConnectLocalClient(
            proxy_port,
            client_b.socket)) {
        worker.Stop();
        relay.StopSession();
        static_cast<void>(WSACleanup());
        return 5;
    }

    client_a.pending_send = local_a_first;
    client_b.pending_send = local_b;
    bool sent_second_a = false;
    bool completed = false;
    bool pump_failed = false;
    int pump_failure_stage = 0;
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(10);
    // 二local Socket、production worker、SVC、二Mock Socketを並行pumpする。
    while (std::chrono::steady_clock::now() <
           deadline) {
        if (!PumpLocalSend(client_a)) {
            pump_failed = true;
            pump_failure_stage = 1;
            break;
        }

        if (!PumpLocalSend(client_b)) {
            pump_failed = true;
            pump_failure_stage = 2;
            break;
        }

        if (!PumpClientToAgent(
                worker,
                client_bridge,
                agent_bridge)) {
            pump_failed = true;
            pump_failure_stage = 3;
            break;
        }

        if (!PumpAgentToClient(
                agent_bridge,
                client_bridge,
                worker)) {
            pump_failed = true;
            pump_failure_stage = 4;
            break;
        }

        if (!PumpLocalReceive(client_a)) {
            pump_failed = true;
            pump_failure_stage = 5;
            break;
        }

        if (!PumpLocalReceive(client_b)) {
            pump_failed = true;
            pump_failure_stage = 6;
            break;
        }

        // 401 challengeを受信してから同じlocal Socketで認証付きsequenceを送る。
        if (!sent_second_a &&
            client_a.received.size() >=
                response_a_first.size()) {
            if (client_a.received !=
                response_a_first) {
                break;
            }

            client_a.pending_send =
                local_a_second;
            client_a.send_offset = 0U;
            sent_second_a = true;
        }

        const std::string expected_a =
            response_a_first +
            response_a_second;
        if (sent_second_a &&
            client_a.send_offset ==
                client_a.pending_send.size() &&
            client_b.send_offset ==
                client_b.pending_send.size() &&
            client_a.received.size() ==
                expected_a.size() &&
            client_b.received.size() ==
                response_b.size() &&
            scenario_a.request_segment ==
                scenario_a.expected_segments.size() &&
            scenario_b.request_segment ==
                scenario_b.expected_segments.size()) {
            completed =
                client_a.received == expected_a &&
                client_b.received == response_b;
            break;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }

    const auto before_stop = worker.snapshot();
    const bool gate_passed =
        completed &&
        !scenario_a.mismatch &&
        !scenario_b.mismatch &&
        scenario_a.connection_count == 1U &&
        scenario_b.connection_count == 1U &&
        relay.snapshot().active_streams == 2U &&
        before_stop.accepted_connections == 2U &&
        before_stop.active_streams == 2U &&
        before_stop.error ==
            zrdp::client::proxy::
                ClientProxyWorkerError::None;

    worker.Stop();
    relay.StopSession();
    const bool cleanup_passed =
        scenario_a.close_count == 1U &&
        scenario_b.close_count == 1U &&
        relay.snapshot().active_streams == 0U;
    static_cast<void>(WSACleanup());
    if (scenario_a.mismatch ||
        scenario_b.mismatch) {
        return scenario_a.mismatch
            ? 20 +
                static_cast<int>(
                    scenario_a.request_segment)
            : 30 +
                static_cast<int>(
                    scenario_b.request_segment);
    }

    if (pump_failed) {
        return 10 + pump_failure_stage;
    }

    if (!completed) {
        return 7;
    }

    if (!gate_passed) {
        return 9;
    }

    if (!cleanup_passed) {
        return 10;
    }

    return 0;
}
