/**
 * @file agent_stream_relay_test.cpp
 * @brief AgentStreamRelayのpolicy境界、I/O状態、cleanupをfake Socketで検証する。
 *
 * @details 実networkへ接続せず、connector呼出し回数と所有Socketのclose回数を
 *          外部stateで確認する。
 */

#include "agent/bridge/agent_stream_relay.h"
#include "agent/network/allowlist.h"
#include "agent/network/upstream_connection.h"
#include "protocol/multiplexer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using zrdp::agent::bridge::AgentStreamRelay;
using zrdp::agent::network::IUpstreamConnection;
using zrdp::agent::network::IUpstreamConnector;
using zrdp::agent::network::UpstreamIoResult;
using zrdp::agent::network::UpstreamIoStatus;
using zrdp::protocol::Frame;
using zrdp::protocol::MultiplexerResult;
using zrdp::protocol::StreamEvent;
using zrdp::protocol::StreamMultiplexer;

/**
 * @brief fake connection破棄後もtestから確認できるI/O状態。
 */
struct FakeSocketState {
    std::deque<UpstreamIoStatus> connect_results;
    std::deque<std::pair<
        UpstreamIoStatus,
        std::vector<std::byte>>> read_results;
    std::vector<std::byte> sent;
    std::size_t maximum_send_bytes{64U * 1024U};
    std::uint32_t close_count{0U};
    std::uint32_t shutdown_send_count{0U};
    bool send_would_block_once{false};
    bool closed{false};
};

/**
 * @brief 外部stateへ決定的なnon-blocking結果を返すfake connection。
 */
class FakeConnection final : public IUpstreamConnection {
public:
    /**
     * @brief 共有test stateを固定する。
     * @param[in] state test所有の状態。
     */
    explicit FakeConnection(
        std::shared_ptr<FakeSocketState> state) noexcept
        : state_(std::move(state)) {
    }

    ~FakeConnection() override {
        Close();
    }

    [[nodiscard]] UpstreamIoResult
    PollConnect() noexcept override {
        if (state_->connect_results.empty()) {
            return UpstreamIoResult{
                UpstreamIoStatus::Pending,
                0U,
                0U,
            };
        }

        const UpstreamIoStatus status =
            state_->connect_results.front();
        state_->connect_results.pop_front();
        return UpstreamIoResult{status, 0U, 0U};
    }

    [[nodiscard]] UpstreamIoResult Send(
        const std::span<const std::byte> data) noexcept override {
        // 一度だけWouldBlockを返し、relayがcreditを先行返却しないことを確認する。
        if (state_->send_would_block_once) {
            state_->send_would_block_once = false;
            return UpstreamIoResult{
                UpstreamIoStatus::WouldBlock,
                0U,
                0U,
            };
        }

        if (state_->closed || data.empty()) {
            return UpstreamIoResult{
                UpstreamIoStatus::Failed,
                0U,
                1U,
            };
        }

        const std::size_t sent = std::min(
            data.size(),
            state_->maximum_send_bytes);
        try {
            state_->sent.insert(
                state_->sent.end(),
                data.begin(),
                data.begin() +
                    static_cast<std::ptrdiff_t>(sent));
        } catch (...) {
            return UpstreamIoResult{
                UpstreamIoStatus::Failed,
                0U,
                2U,
            };
        }

        return UpstreamIoResult{
            UpstreamIoStatus::Completed,
            sent,
            0U,
        };
    }

    [[nodiscard]] UpstreamIoResult Receive(
        const std::span<std::byte> buffer) noexcept override {
        if (state_->closed) {
            return UpstreamIoResult{
                UpstreamIoStatus::Failed,
                0U,
                3U,
            };
        }

        if (state_->read_results.empty()) {
            return UpstreamIoResult{
                UpstreamIoStatus::WouldBlock,
                0U,
                0U,
            };
        }

        auto result =
            std::move(state_->read_results.front());
        state_->read_results.pop_front();
        if (result.first !=
            UpstreamIoStatus::Completed) {
            return UpstreamIoResult{
                result.first,
                0U,
                0U,
            };
        }

        // test chunkは一回の受信buffer以下に限定する。
        if (result.second.empty() ||
            result.second.size() > buffer.size()) {
            return UpstreamIoResult{
                UpstreamIoStatus::Failed,
                0U,
                4U,
            };
        }

        std::copy(
            result.second.begin(),
            result.second.end(),
            buffer.begin());
        return UpstreamIoResult{
            UpstreamIoStatus::Completed,
            result.second.size(),
            0U,
        };
    }

    [[nodiscard]] UpstreamIoResult
    ShutdownSend() noexcept override {
        if (state_->closed) {
            return UpstreamIoResult{
                UpstreamIoStatus::Failed,
                0U,
                5U,
            };
        }

        ++state_->shutdown_send_count;
        return UpstreamIoResult{
            UpstreamIoStatus::Completed,
            0U,
            0U,
        };
    }

    void Close() noexcept override {
        // production RAIIと同じく、一接続を一度だけcloseとして数える。
        if (!state_->closed) {
            state_->closed = true;
            ++state_->close_count;
        }
    }

private:
    std::shared_ptr<FakeSocketState> state_;
};

/**
 * @brief BeginConnectの呼出し回数と固定結果を公開するfake factory。
 */
class FakeConnector final : public IUpstreamConnector {
public:
    UpstreamIoStatus begin_status{
        UpstreamIoStatus::Completed};
    std::uint32_t begin_count{0U};
    std::shared_ptr<FakeSocketState> socket_state =
        std::make_shared<FakeSocketState>();

    [[nodiscard]] UpstreamIoResult BeginConnect(
        const zrdp::protocol::OpenTarget&,
        std::unique_ptr<IUpstreamConnection>&
            connection) noexcept override {
        ++begin_count;
        connection.reset();
        if (begin_status ==
                UpstreamIoStatus::Completed ||
            begin_status ==
                UpstreamIoStatus::Pending) {
            try {
                connection =
                    std::make_unique<FakeConnection>(
                        socket_state);
            } catch (...) {
                return UpstreamIoResult{
                    UpstreamIoStatus::Failed,
                    0U,
                    6U,
                };
            }
        }

        return UpstreamIoResult{
            begin_status,
            0U,
            0U,
        };
    }
};

/**
 * @brief 各OPENへ独立したfake Socketを割り当てるmulti-stream factory。
 */
class MultiFakeConnector final : public IUpstreamConnector {
public:
    std::vector<std::shared_ptr<FakeSocketState>>
        socket_states;

    [[nodiscard]] UpstreamIoResult BeginConnect(
        const zrdp::protocol::OpenTarget&,
        std::unique_ptr<IUpstreamConnection>&
            connection) noexcept override {
        connection.reset();
        try {
            auto state =
                std::make_shared<FakeSocketState>();
            auto candidate =
                std::make_unique<FakeConnection>(
                    state);
            socket_states.push_back(state);
            connection = std::move(candidate);
        } catch (...) {
            connection.reset();
            return UpstreamIoResult{
                UpstreamIoStatus::Failed,
                0U,
                7U,
            };
        }

        return UpstreamIoResult{
            UpstreamIoStatus::Completed,
            0U,
            0U,
        };
    }
};

/**
 * @brief 文書用target一件の検証済みallowlistを作成する。
 * @param[out] allowlist 成功時の一件設定。
 * @return parserが成功した場合はtrue。
 */
[[nodiscard]] bool MakeAllowlist(
    zrdp::agent::network::Allowlist& allowlist) {
    return zrdp::agent::network::ParseAllowlist(
               "[allowlist]\n"
               "endpoint.1001=192.0.2.10:80\n",
               allowlist)
        .succeeded();
}

/**
 * @brief Client multiplexerの全FrameをAgent relayへ渡す。
 * @param[in,out] client Client role multiplexer。
 * @param[in,out] relay Agent relay。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferToRelay(
    StreamMultiplexer& client,
    AgentStreamRelay& relay) {
    Frame frame{};
    // Client outbound queueをwire順のまま完全にdrainする。
    while (client.TakeOutboundFrame(frame)) {
        if (!relay.OnFrame(std::move(frame))) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Agent relayの全FrameをClient multiplexerへ渡す。
 * @param[in,out] relay Agent relay。
 * @param[in,out] client Client role multiplexer。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferToClient(
    AgentStreamRelay& relay,
    StreamMultiplexer& client) {
    Frame frame{};
    // Agent outbound queueをwire順のまま完全にdrainする。
    while (relay.TakeOutboundFrame(frame)) {
        if (client.OnFrame(std::move(frame)) !=
            MultiplexerResult::Ok) {
            return false;
        }
    }

    return true;
}

/**
 * @brief ClientとrelayをPhase 5の有限multi-stream capabilityで開始する。
 * @param[in,out] client Client multiplexer。
 * @param[in,out] relay Agent relay。
 * @return 両方がReadyになった場合はtrue。
 */
[[nodiscard]] bool StartSession(
    StreamMultiplexer& client,
    AgentStreamRelay& relay) {
    auto capabilities =
        zrdp::protocol::MakePhase2HelloCapabilities();
    capabilities.maximum_streams =
        zrdp::agent::bridge::kMaximumAgentRelayStreams;
    return client.StartSession(capabilities) ==
            MultiplexerResult::Ok &&
        relay.StartSession(capabilities);
}

/**
 * @brief 一targetのOPENをrelayへ渡し、生成responseをClientへ戻す。
 * @param[in,out] client Client multiplexer。
 * @param[in,out] relay Agent relay。
 * @param[in] target 接続候補。
 * @param[out] stream_id 割り当てID。
 * @return transport処理が成功した場合はtrue。
 */
[[nodiscard]] bool OpenAndTransfer(
    StreamMultiplexer& client,
    AgentStreamRelay& relay,
    const zrdp::protocol::OpenTarget& target,
    std::uint32_t& stream_id) {
    return client.OpenStream(
               target,
               stream_id) ==
            MultiplexerResult::Ok &&
        TransferToRelay(client, relay) &&
        TransferToClient(relay, client);
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Agent stream relayのallowlist接続、I/O、timeout、closeを検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::protocol::MultiplexerRole;
    using zrdp::protocol::OpenErrorCode;
    using zrdp::protocol::OpenTarget;
    using zrdp::protocol::StreamCloseReason;
    using zrdp::protocol::StreamEventType;

    constexpr OpenTarget allowed{
        {192U, 0U, 2U, 10U},
        80U};
    constexpr OpenTarget denied{
        {192U, 0U, 2U, 11U},
        80U};

    zrdp::agent::network::Allowlist allowlist;
    if (!MakeAllowlist(allowlist)) {
        return 1;
    }

    // policy拒否ではSocket factoryを呼び出さず固定OPEN_ERRORだけを返す。
    {
        FakeConnector connector;
        AgentStreamRelay relay(allowlist, connector);
        StreamMultiplexer client(
            MultiplexerRole::Client);
        if (!StartSession(client, relay)) {
            return 2;
        }

        std::uint32_t stream_id = 0U;
        if (!OpenAndTransfer(
                client,
                relay,
                denied,
                stream_id)) {
            return 3;
        }

        StreamEvent event{};
        if (connector.begin_count != 0U ||
            !client.TakeEvent(event) ||
            event.type !=
                StreamEventType::OpenFailed ||
            event.open_error !=
                OpenErrorCode::RejectedByPolicy) {
            return 4;
        }
    }

    // immediate refusedは別endpointへfallbackせずConnectFailedへ変換する。
    {
        FakeConnector connector;
        connector.begin_status =
            UpstreamIoStatus::Failed;
        AgentStreamRelay relay(allowlist, connector);
        StreamMultiplexer client(
            MultiplexerRole::Client);
        if (!StartSession(client, relay)) {
            return 5;
        }

        std::uint32_t stream_id = 0U;
        if (!OpenAndTransfer(
                client,
                relay,
                allowed,
                stream_id)) {
            return 6;
        }

        StreamEvent event{};
        if (connector.begin_count != 1U ||
            !client.TakeEvent(event) ||
            event.type !=
                StreamEventType::OpenFailed ||
            event.open_error !=
                OpenErrorCode::ConnectFailed) {
            return 7;
        }
    }

    // pending connectは単調時刻5秒でtimeoutし、所有Socketを一度閉じる。
    {
        FakeConnector connector;
        connector.begin_status =
            UpstreamIoStatus::Pending;
        AgentStreamRelay relay(allowlist, connector);
        StreamMultiplexer client(
            MultiplexerRole::Client);
        if (!StartSession(client, relay)) {
            return 8;
        }

        std::uint32_t stream_id = 0U;
        if (client.OpenStream(
                allowed,
                stream_id) !=
                MultiplexerResult::Ok ||
            !TransferToRelay(client, relay) ||
            !relay.Pump(1000U) ||
            !relay.Pump(
                1000U +
                zrdp::agent::bridge::
                    kAgentConnectTimeoutMilliseconds) ||
            !TransferToClient(relay, client)) {
            return 9;
        }

        StreamEvent event{};
        if (!client.TakeEvent(event) ||
            event.type !=
                StreamEventType::OpenFailed ||
            event.open_error !=
                OpenErrorCode::Timeout ||
            connector.socket_state->close_count !=
                1U) {
            return 10;
        }
    }

    // 接続済みupstreamが完全無通信なら30分相当の単調時刻で対象streamだけ閉じる。
    {
        FakeConnector connector;
        AgentStreamRelay relay(allowlist, connector);
        StreamMultiplexer client(
            MultiplexerRole::Client);
        if (!StartSession(client, relay)) {
            return 50;
        }

        std::uint32_t stream_id = 0U;
        if (!OpenAndTransfer(
                client,
                relay,
                allowed,
                stream_id)) {
            return 51;
        }

        StreamEvent event{};
        constexpr std::uint64_t kStartTick = 10000U;
        if (!client.TakeEvent(event) ||
            event.type != StreamEventType::Opened ||
            !relay.Pump(kStartTick) ||
            !relay.Pump(
                kStartTick +
                zrdp::agent::bridge::
                    kAgentStreamIdleTimeoutMilliseconds) ||
            !TransferToClient(relay, client) ||
            !client.TakeEvent(event) ||
            event.type != StreamEventType::Closed ||
            event.close_reason !=
                StreamCloseReason::Timeout) {
            return 52;
        }

        const auto timed_out = relay.snapshot();
        if (timed_out.timed_out_streams != 1U ||
            connector.socket_state->close_count != 1U ||
            !TransferToRelay(client, relay) ||
            relay.snapshot().active_streams != 0U) {
            return 53;
        }
    }

    // established streamでpartial send、WouldBlock、双方向FIN、normal CLOSEを処理する。
    {
        FakeConnector connector;
        connector.socket_state->
            maximum_send_bytes = 17U;
        connector.socket_state->
            send_would_block_once = true;
        AgentStreamRelay relay(allowlist, connector);
        StreamMultiplexer client(
            MultiplexerRole::Client);
        if (!StartSession(client, relay)) {
            return 11;
        }

        std::uint32_t stream_id = 0U;
        if (!OpenAndTransfer(
                client,
                relay,
                allowed,
                stream_id)) {
            return 12;
        }

        StreamEvent event{};
        if (!client.TakeEvent(event) ||
            event.type != StreamEventType::Opened) {
            return 13;
        }

        std::vector<std::byte> request(257U);
        // 埋込みNULを含むbinary patternで文字列変換がないことを確認する。
        for (std::size_t index = 0U;
             index < request.size();
             ++index) {
            request[index] =
                static_cast<std::byte>(
                    index % 251U);
        }

        if (client.SendData(
                stream_id,
                request) !=
                MultiplexerResult::Ok ||
            !TransferToRelay(client, relay)) {
            return 14;
        }

        // 17-byte partial sendと一度のWouldBlockを有限Pumpで完了する。
        for (std::size_t attempt = 0U;
             attempt < 32U &&
             connector.socket_state->sent.size() <
                 request.size();
             ++attempt) {
            if (!relay.Pump(
                    2000U + attempt) ||
                !TransferToClient(relay, client)) {
                return 15;
            }
        }

        if (connector.socket_state->sent !=
            request) {
            return 16;
        }

        std::vector<std::byte> response(333U);
        // upstream応答にも別の決定的binary patternを使用する。
        for (std::size_t index = 0U;
             index < response.size();
             ++index) {
            response[index] =
                static_cast<std::byte>(
                    (index * 7U) % 251U);
        }

        connector.socket_state->
            read_results.emplace_back(
                UpstreamIoStatus::Completed,
                response);
        connector.socket_state->
            read_results.emplace_back(
                UpstreamIoStatus::PeerClosed,
                std::vector<std::byte>{});
        // 一streamが他streamを占有しないよう、一PumpのSocket readは一回に限る。
        if (!relay.Pump(3000U) ||
            !relay.Pump(3001U) ||
            !TransferToClient(relay, client)) {
            return 17;
        }

        std::vector<std::byte> received;
        bool remote_half_closed = false;
        // DATAとHALF_CLOSEを順序に依存せず全eventから確認する。
        while (client.TakeEvent(event)) {
            if (event.type ==
                StreamEventType::DataReceived) {
                received = std::move(event.data);
                if (client.ConsumeReceivedData(
                        stream_id,
                        static_cast<std::uint32_t>(
                            received.size())) !=
                    MultiplexerResult::Ok) {
                    return 18;
                }
            } else if (event.type ==
                       StreamEventType::
                           RemoteHalfClosed) {
                remote_half_closed = true;
            } else {
                return 19;
            }
        }

        if (received != response ||
            !remote_half_closed ||
            client.HalfCloseStream(stream_id) !=
                MultiplexerResult::Ok ||
            !TransferToRelay(client, relay) ||
            !relay.Pump(4000U) ||
            !TransferToClient(relay, client) ||
            !relay.Pump(4001U) ||
            !TransferToClient(relay, client)) {
            return 20;
        }

        bool closed = false;
        // 今回届いた有限event群から正常close acknowledgementを探す。
        while (client.TakeEvent(event)) {
            if (event.type ==
                    StreamEventType::Closed &&
                event.close_reason ==
                    StreamCloseReason::Normal) {
                closed = true;
            }
        }

        if (!closed ||
            connector.socket_state->
                    shutdown_send_count !=
                1U ||
            connector.socket_state->close_count !=
                1U ||
            !TransferToRelay(client, relay) ||
            relay.snapshot().active_stream_id !=
                0U) {
            return 21;
        }
    }

    // peer resetを固定CLOSE reasonへ変換し、ack後にstream所有状態を破棄する。
    {
        FakeConnector connector;
        connector.socket_state->
            read_results.emplace_back(
                UpstreamIoStatus::PeerReset,
                std::vector<std::byte>{});
        AgentStreamRelay relay(allowlist, connector);
        StreamMultiplexer client(
            MultiplexerRole::Client);
        if (!StartSession(client, relay)) {
            return 22;
        }

        std::uint32_t stream_id = 0U;
        if (!OpenAndTransfer(
                client,
                relay,
                allowed,
                stream_id)) {
            return 23;
        }

        StreamEvent event{};
        if (!client.TakeEvent(event) ||
            event.type != StreamEventType::Opened ||
            !relay.Pump(5000U) ||
            !TransferToClient(relay, client) ||
            !client.TakeEvent(event) ||
            event.type != StreamEventType::Closed ||
            event.close_reason !=
                StreamCloseReason::PeerReset ||
            !TransferToRelay(client, relay) ||
            relay.snapshot().active_stream_id !=
                0U) {
            return 24;
        }
    }

    // RDP切断相当のStopSessionは確立Socketを一度閉じ、bufferとIDを破棄する。
    {
        FakeConnector connector;
        AgentStreamRelay relay(allowlist, connector);
        StreamMultiplexer client(
            MultiplexerRole::Client);
        if (!StartSession(client, relay)) {
            return 25;
        }

        std::uint32_t stream_id = 0U;
        if (!OpenAndTransfer(
                client,
                relay,
                allowed,
                stream_id)) {
            return 26;
        }

        relay.StopSession();
        const auto snapshot = relay.snapshot();
        if (connector.socket_state->close_count !=
                1U ||
            snapshot.state !=
                zrdp::agent::bridge::
                    AgentStreamRelayState::Stopped ||
            snapshot.active_stream_id != 0U ||
            snapshot.socket_write_bytes != 0U) {
            return 27;
        }
    }

    // 複数streamを独立Socketへrouteし、round-robin Pumpで混線なく処理する。
    {
        MultiFakeConnector connector;
        AgentStreamRelay relay(allowlist, connector);
        StreamMultiplexer client(
            MultiplexerRole::Client);
        if (!StartSession(client, relay)) {
            return 28;
        }

        std::vector<std::uint32_t> stream_ids;
        // production同時上限までOPENし、各streamに別Socketが割り当たることを確認する。
        for (std::uint32_t index = 0U;
             index <
                 zrdp::agent::bridge::
                     kMaximumAgentRelayStreams;
             ++index) {
            std::uint32_t stream_id = 0U;
            if (!OpenAndTransfer(
                    client,
                    relay,
                    allowed,
                    stream_id)) {
                return 29;
            }

            StreamEvent event{};
            if (!client.TakeEvent(event) ||
                event.type !=
                    StreamEventType::Opened ||
                event.stream_id != stream_id) {
                return 30;
            }

            stream_ids.push_back(stream_id);
        }

        std::uint32_t excess_stream_id = 0U;
        if (connector.socket_states.size() !=
                zrdp::agent::bridge::
                    kMaximumAgentRelayStreams ||
            relay.snapshot().active_streams !=
                zrdp::agent::bridge::
                    kMaximumAgentRelayStreams ||
            client.OpenStream(
                allowed,
                excess_stream_id) !=
                MultiplexerResult::
                    StreamLimitExceeded) {
            return 31;
        }

        std::vector<std::vector<std::byte>> requests(
            stream_ids.size());
        // stream固有patternを送信し、Socket tableの同じindexだけへ到達させる。
        for (std::size_t index = 0U;
             index < stream_ids.size();
             ++index) {
            requests[index].resize(31U + index);
            // streamごとに異なるrequest patternを全byteへ設定する。
            for (std::size_t offset = 0U;
                 offset < requests[index].size();
                 ++offset) {
                requests[index][offset] =
                    static_cast<std::byte>(
                        (index * 17U + offset) %
                        251U);
            }

            if (client.SendData(
                    stream_ids[index],
                    requests[index]) !=
                MultiplexerResult::Ok) {
                return 32;
            }
        }

        if (!TransferToRelay(client, relay) ||
            !relay.Pump(6000U) ||
            !TransferToClient(relay, client)) {
            return 33;
        }

        // 一Pumpで各streamを一回ずつ進める公平性とroute完全一致を確認する。
        for (std::size_t index = 0U;
             index < requests.size();
             ++index) {
            if (connector.socket_states[index]->sent !=
                requests[index]) {
                return 34;
            }
        }

        std::vector<std::vector<std::byte>> responses(
            stream_ids.size());
        // upstream応答もstream固有patternとして全Socketへ同時投入する。
        for (std::size_t index = 0U;
             index < stream_ids.size();
             ++index) {
            responses[index].resize(19U + index);
            // requestと異なるresponse patternを全byteへ設定する。
            for (std::size_t offset = 0U;
                 offset < responses[index].size();
                 ++offset) {
                responses[index][offset] =
                    static_cast<std::byte>(
                        (index * 29U + offset * 3U) %
                        251U);
            }

            connector.socket_states[index]->
                read_results.emplace_back(
                    UpstreamIoStatus::Completed,
                    responses[index]);
        }

        if (!relay.Pump(6001U) ||
            !TransferToClient(relay, client)) {
            return 35;
        }

        std::size_t received_count = 0U;
        StreamEvent event{};
        // DATAをstream IDへ照合し、誤routeと重複を検出する。
        while (client.TakeEvent(event)) {
            if (event.type !=
                StreamEventType::DataReceived) {
                return 36;
            }

            const auto stream =
                std::find(
                    stream_ids.begin(),
                    stream_ids.end(),
                    event.stream_id);
            if (stream == stream_ids.end()) {
                return 37;
            }

            const std::size_t index =
                static_cast<std::size_t>(
                    stream - stream_ids.begin());
            if (event.data != responses[index] ||
                client.ConsumeReceivedData(
                    event.stream_id,
                    static_cast<std::uint32_t>(
                        event.data.size())) !=
                    MultiplexerResult::Ok) {
                return 38;
            }

            ++received_count;
        }

        if (received_count != stream_ids.size()) {
            return 39;
        }

        connector.socket_states.front()->
            read_results.emplace_back(
                UpstreamIoStatus::PeerReset,
                std::vector<std::byte>{});
        if (!relay.Pump(6002U) ||
            !TransferToClient(relay, client) ||
            !client.TakeEvent(event) ||
            event.type !=
                StreamEventType::Closed ||
            event.stream_id != stream_ids.front() ||
            event.close_reason !=
                StreamCloseReason::PeerReset ||
            !TransferToRelay(client, relay)) {
            return 40;
        }

        // 一Socketのresetは同じstreamだけを除き、残り七streamを継続可能にする。
        if (relay.snapshot().active_streams !=
                zrdp::agent::bridge::
                    kMaximumAgentRelayStreams -
                    1U ||
            connector.socket_states.front()->
                    close_count !=
                1U) {
            return 41;
        }

        const std::vector<std::byte> surviving{
            std::byte{0x55U},
            std::byte{0x2AU},
        };
        if (client.SendData(
                stream_ids[1U],
                surviving) !=
                MultiplexerResult::Ok ||
            !TransferToRelay(client, relay) ||
            !relay.Pump(6003U) ||
            connector.socket_states[1U]->sent.size() <
                requests[1U].size() +
                    surviving.size() ||
            !std::equal(
                surviving.begin(),
                surviving.end(),
                connector.socket_states[1U]->
                    sent.end() -
                    static_cast<std::ptrdiff_t>(
                        surviving.size()))) {
            return 42;
        }

        relay.StopSession();
        // session終了は全Socketを一度だけ閉じ、tableとqueueを破棄する。
        for (const auto& state :
             connector.socket_states) {
            if (state->close_count != 1U) {
                return 43;
            }
        }

        if (relay.snapshot().active_streams != 0U) {
            return 44;
        }
    }

    return 0;
}
