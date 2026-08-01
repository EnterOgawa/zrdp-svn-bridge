/**
 * @file client_proxy_session_test.cpp
 * @brief HTTP parser、Client multiplexer、local Socket credit境界を一体検証する。
 *
 * @details Socket APIを使用せず、Agent role multiplexerをfake transportとする。
 */

#include "client/proxy/client_proxy_session.h"
#include "protocol/hello.h"
#include "protocol/multiplexer.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using zrdp::client::proxy::ClientProxySession;
using zrdp::protocol::Frame;
using zrdp::protocol::MultiplexerResult;
using zrdp::protocol::StreamEvent;
using zrdp::protocol::StreamEventType;
using zrdp::protocol::StreamMultiplexer;

/**
 * @brief ASCII test dataをbinary spanへ変換する。
 * @param[in] text test wire data。
 * @return 同一memoryを参照するspan。
 */
[[nodiscard]] std::span<const std::byte> Bytes(
    const std::string_view text) noexcept {
    return std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()),
        text.size());
}

/**
 * @brief Client outbound FrameをAgent multiplexerへ全件渡す。
 * @param[in,out] client Client relay。
 * @param[in,out] agent fake Agent multiplexer。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferClientToAgent(
    ClientProxySession& client,
    StreamMultiplexer& agent) {
    Frame frame{};
    // bounded queueを空になるまでwire順に移す。
    while (client.TakeOutboundFrame(frame)) {
        if (agent.OnFrame(std::move(frame)) !=
            MultiplexerResult::Ok) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Agent outbound FrameをClient relayへ全件渡す。
 * @param[in,out] agent fake Agent multiplexer。
 * @param[in,out] client Client relay。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferAgentToClient(
    StreamMultiplexer& agent,
    ClientProxySession& client) {
    Frame frame{};
    // Agent応答をClient stream stateへwire順に移す。
    while (agent.TakeOutboundFrame(frame)) {
        if (!client.OnBridgeFrame(std::move(frame))) {
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
 * @brief 一Proxy sessionのHTTP／Bridge状態遷移とhalf-closeを検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int main() {
    using zrdp::client::proxy::ClientProxySessionState;
    using zrdp::client::proxy::LocalConnectionResult;
    using zrdp::protocol::MultiplexerRole;
    using zrdp::protocol::OpenTarget;

    auto capabilities =
        zrdp::protocol::MakePhase2HelloCapabilities();
    capabilities.maximum_streams = 1U;

    ClientProxySession client;
    // HELLO未成立ではlocal requestをBridge待ちへhangさせない。
    if (client.BeginLocalConnection() !=
        LocalConnectionResult::BridgeUnavailable) {
        return 1;
    }

    StreamMultiplexer agent(MultiplexerRole::Agent);
    if (!client.StartBridgeSession(capabilities) ||
        agent.StartSession(capabilities) !=
            MultiplexerResult::Ok ||
        client.BeginLocalConnection() !=
            LocalConnectionResult::Accepted ||
        client.BeginLocalConnection() !=
            LocalConnectionResult::Busy) {
        return 2;
    }

    constexpr std::string_view request_first =
        "REPORT http://192.0.2.40:8080/svn/Test HTTP/1.1\r\n"
        "Host: 192.0.2.40:8080\r\n"
        "Content-Length: 6\r\n\r\nabc";
    constexpr std::string_view request_second = "def";
    if (!client.OnLocalData(Bytes(request_first)) ||
        !client.OnLocalData(Bytes(request_second)) ||
        client.snapshot().state !=
            ClientProxySessionState::Opening ||
        !TransferClientToAgent(client, agent)) {
        return 3;
    }

    StreamEvent event{};
    if (!agent.TakeEvent(event) ||
        event.type != StreamEventType::OpenRequested ||
        event.target != OpenTarget{
            {192U, 0U, 2U, 40U},
            8080U}) {
        return 4;
    }

    const std::uint32_t stream_id = event.stream_id;
    if (agent.AcceptStream(stream_id) !=
            MultiplexerResult::Ok ||
        !TransferAgentToClient(agent, client) ||
        !TransferClientToAgent(client, agent)) {
        return 5;
    }

    std::string received_request;
    // OPEN_OK後にheaderとbodyがDATA eventとして順序どおり届く。
    while (agent.TakeEvent(event)) {
        if (event.type != StreamEventType::DataReceived) {
            return 6;
        }

        received_request.append(
            reinterpret_cast<const char*>(
                event.data.data()),
            event.data.size());
        if (agent.ConsumeReceivedData(
                event.stream_id,
                static_cast<std::uint32_t>(
                    event.data.size())) !=
            MultiplexerResult::Ok) {
            return 7;
        }
    }

    constexpr std::string_view expected_request =
        "REPORT /svn/Test HTTP/1.1\r\n"
        "Host: 192.0.2.40:8080\r\n"
        "Content-Length: 6\r\n\r\nabcdef";
    if (received_request != expected_request ||
        client.snapshot().bytes_from_local !=
            expected_request.size()) {
        return 8;
    }

    // Agentからのresponseはlocal send完了まで受信creditを返さない。
    constexpr std::string_view response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n\r\nhello";
    if (agent.SendData(
            stream_id,
            Bytes(response)) !=
            MultiplexerResult::Ok ||
        !TransferAgentToClient(agent, client)) {
        return 9;
    }

    std::span<const std::byte> local_write;
    if (!client.PeekLocalWrite(local_write) ||
        local_write.size() != response.size() ||
        !client.ConsumeLocalWrite(7U) ||
        !client.PeekLocalWrite(local_write) ||
        local_write.size() != response.size() - 7U ||
        !client.ConsumeLocalWrite(local_write.size()) ||
        client.PeekLocalWrite(local_write) ||
        client.snapshot().bytes_to_local !=
            response.size()) {
        return 10;
    }

    if (!TransferClientToAgent(client, agent)) {
        return 11;
    }

    // 完全request境界のlocal FINだけをHALF_CLOSEへ変換する。
    if (!client.OnLocalReceiveClosed() ||
        !TransferClientToAgent(client, agent) ||
        !agent.TakeEvent(event) ||
        event.type !=
            StreamEventType::RemoteHalfClosed) {
        return 12;
    }

    if (agent.HalfCloseStream(stream_id) !=
            MultiplexerResult::Ok ||
        !TransferAgentToClient(agent, client) ||
        !client.ShouldShutdownLocalSend()) {
        return 13;
    }

    client.MarkLocalSendShutdown();
    if (client.ShouldShutdownLocalSend()) {
        return 14;
    }

    // malformed requestはupstream OPENを作らず固定400をlocalへ返す。
    ClientProxySession rejected;
    if (!rejected.StartBridgeSession(capabilities) ||
        rejected.BeginLocalConnection() !=
            LocalConnectionResult::Accepted ||
        !rejected.OnLocalData(Bytes(
            "GET / HTTP/1.1\r\n\r\n")) ||
        !rejected.PeekLocalWrite(local_write)) {
        return 15;
    }

    const std::string error_response(
        reinterpret_cast<const char*>(
            local_write.data()),
        local_write.size());
    if (!error_response.starts_with(
            "HTTP/1.1 400 ") ||
        !rejected.ConsumeLocalWrite(
            local_write.size()) ||
        !rejected.ShouldCloseLocalConnection()) {
        return 16;
    }

    // Bridge resetはlocal connection、stream、payload queueを同期破棄する。
    client.StopBridgeSession();
    const auto stopped = client.snapshot();
    if (stopped.state !=
            ClientProxySessionState::Stopped ||
        stopped.active_stream_id != 0U ||
        stopped.local_connection_active ||
        stopped.upstream_pending_bytes != 0U ||
        stopped.local_write_bytes != 0U) {
        return 17;
    }

    // 二つのHTTP connectionを別streamへ割り当て、応答を交差させても混線しない。
    auto multi_capabilities =
        zrdp::protocol::MakePhase2HelloCapabilities();
    multi_capabilities.maximum_streams =
        zrdp::client::proxy::
            kMaximumClientProxyConnections;
    ClientProxySession multi_client;
    StreamMultiplexer multi_agent(
        MultiplexerRole::Agent);
    constexpr zrdp::client::proxy::LocalConnectionId
        connection_a = 101U;
    constexpr zrdp::client::proxy::LocalConnectionId
        connection_b = 202U;
    if (!multi_client.StartBridgeSession(
            multi_capabilities) ||
        multi_agent.StartSession(
            multi_capabilities) !=
            MultiplexerResult::Ok ||
        multi_client.BeginLocalConnection(
            connection_a) !=
            LocalConnectionResult::Accepted ||
        multi_client.BeginLocalConnection(
            connection_b) !=
            LocalConnectionResult::Accepted) {
        return 18;
    }

    constexpr std::string_view request_a =
        "OPTIONS http://192.0.2.41:8080/svn/A HTTP/1.1\r\n"
        "Host: 192.0.2.41:8080\r\n"
        "Authorization: NTLM opaque-token-a\r\n"
        "Content-Length: 0\r\n\r\n";
    constexpr std::string_view request_b =
        "PROPFIND http://192.0.2.42:8081/svn/B HTTP/1.1\r\n"
        "Host: 192.0.2.42:8081\r\n"
        "Depth: 1\r\n"
        "Content-Length: 0\r\n\r\n";
    if (!multi_client.OnLocalData(
            connection_a,
            Bytes(request_a)) ||
        !multi_client.OnLocalData(
            connection_b,
            Bytes(request_b)) ||
        !TransferClientToAgent(
            multi_client,
            multi_agent)) {
        return 19;
    }

    std::uint32_t stream_a = 0U;
    std::uint32_t stream_b = 0U;
    std::uint32_t open_count = 0U;
    // OPEN targetからstream routeを記録し、各streamを独立に受理する。
    while (multi_agent.TakeEvent(event)) {
        if (event.type !=
            StreamEventType::OpenRequested) {
            return 20;
        }

        if (event.target == OpenTarget{
                {192U, 0U, 2U, 41U},
                8080U}) {
            stream_a = event.stream_id;
        } else if (event.target == OpenTarget{
                       {192U, 0U, 2U, 42U},
                       8081U}) {
            stream_b = event.stream_id;
        } else {
            return 21;
        }

        if (multi_agent.AcceptStream(
                event.stream_id) !=
            MultiplexerResult::Ok) {
            return 22;
        }

        ++open_count;
    }

    if (open_count != 2U ||
        stream_a == 0U ||
        stream_b == 0U ||
        stream_a == stream_b ||
        !TransferAgentToClient(
            multi_agent,
            multi_client) ||
        !TransferClientToAgent(
            multi_client,
            multi_agent)) {
        return 23;
    }

    std::string upstream_a;
    std::string upstream_b;
    // DATA eventをstream IDで振り分け、受信creditを各streamへ返す。
    while (multi_agent.TakeEvent(event)) {
        if (event.type !=
            StreamEventType::DataReceived) {
            return 24;
        }

        std::string* destination = nullptr;
        if (event.stream_id == stream_a) {
            destination = &upstream_a;
        } else if (event.stream_id == stream_b) {
            destination = &upstream_b;
        } else {
            return 25;
        }

        destination->append(
            reinterpret_cast<const char*>(
                event.data.data()),
            event.data.size());
        if (multi_agent.ConsumeReceivedData(
                event.stream_id,
                static_cast<std::uint32_t>(
                    event.data.size())) !=
            MultiplexerResult::Ok) {
            return 26;
        }
    }

    constexpr std::string_view expected_a =
        "OPTIONS /svn/A HTTP/1.1\r\n"
        "Host: 192.0.2.41:8080\r\n"
        "Authorization: NTLM opaque-token-a\r\n"
        "Content-Length: 0\r\n\r\n";
    constexpr std::string_view expected_b =
        "PROPFIND /svn/B HTTP/1.1\r\n"
        "Host: 192.0.2.42:8081\r\n"
        "Depth: 1\r\n"
        "Content-Length: 0\r\n\r\n";
    if (upstream_a != expected_a ||
        upstream_b != expected_b ||
        multi_client.snapshot().active_streams != 2U) {
        return 27;
    }

    constexpr std::string_view response_a =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: NTLM opaque-challenge\r\n"
        "Content-Length: 0\r\n\r\n";
    constexpr std::string_view response_b =
        "HTTP/1.1 207 Multi-Status\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "4\r\nbody\r\n0\r\n\r\n";
    if (multi_agent.SendData(
            stream_b,
            Bytes(response_b)) !=
            MultiplexerResult::Ok ||
        multi_agent.SendData(
            stream_a,
            Bytes(response_a)) !=
            MultiplexerResult::Ok ||
        !TransferAgentToClient(
            multi_agent,
            multi_client)) {
        return 28;
    }

    std::span<const std::byte> write_a;
    std::span<const std::byte> write_b;
    if (!multi_client.PeekLocalWrite(
            connection_a,
            write_a) ||
        !multi_client.PeekLocalWrite(
            connection_b,
            write_b) ||
        std::string_view(
            reinterpret_cast<const char*>(
                write_a.data()),
            write_a.size()) != response_a ||
        std::string_view(
            reinterpret_cast<const char*>(
                write_b.data()),
            write_b.size()) != response_b ||
        !multi_client.ConsumeLocalWrite(
            connection_b,
            write_b.size()) ||
        !multi_client.ConsumeLocalWrite(
            connection_a,
            write_a.size())) {
        return 29;
    }

    // 一local connectionのHTTP違反は固定400だけへ閉じ、他二streamを維持する。
    constexpr zrdp::client::proxy::LocalConnectionId
        rejected_connection = 303U;
    std::span<const std::byte> rejected_write;
    if (multi_client.BeginLocalConnection(
            rejected_connection) !=
            LocalConnectionResult::Accepted ||
        !multi_client.OnLocalData(
            rejected_connection,
            Bytes("GET /relative HTTP/1.1\r\n\r\n")) ||
        !multi_client.PeekLocalWrite(
            rejected_connection,
            rejected_write) ||
        !std::string_view(
            reinterpret_cast<const char*>(
                rejected_write.data()),
            rejected_write.size())
             .starts_with("HTTP/1.1 400 ") ||
        multi_client.snapshot().active_streams != 2U ||
        !multi_client.ConsumeLocalWrite(
            rejected_connection,
            rejected_write.size()) ||
        !multi_client.ShouldCloseLocalConnection(
            rejected_connection)) {
        return 30;
    }

    multi_client.OnLocalConnectionClosed(
        rejected_connection,
        zrdp::protocol::StreamCloseReason::Normal);

    // negotiated上限の九件目は既存streamへ影響を与えずBusyで拒否する。
    for (zrdp::client::proxy::LocalConnectionId
             connection_id = 303U;
         connection_id < 309U;
         ++connection_id) {
        if (multi_client.BeginLocalConnection(
                connection_id) !=
            LocalConnectionResult::Accepted) {
            return 31;
        }
    }

    if (multi_client.BeginLocalConnection(309U) !=
            LocalConnectionResult::Busy ||
        multi_client.snapshot().active_connections !=
            zrdp::client::proxy::
                kMaximumClientProxyConnections) {
        return 32;
    }

    multi_client.StopBridgeSession();
    return 0;
}
