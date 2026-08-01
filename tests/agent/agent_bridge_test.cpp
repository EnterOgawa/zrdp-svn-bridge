/**
 * @file agent_bridge_test.cpp
 * @brief AgentBridgeのHELLO、binary Echo、sequence、queue上限を検証する。
 */

#include "agent/bridge/agent_bridge.h"
#include "agent/bridge/agent_stream_relay.h"
#include "agent/network/allowlist.h"
#include "agent/network/upstream_connection.h"
#include "protocol/frame.h"
#include "protocol/hello.h"
#include "protocol/multiplexer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

/**
 * @brief 接続開始を固定失敗させ、Bridgeからの呼出し回数だけを数えるfactory。
 */
class RejectingConnector final
    : public zrdp::agent::network::
          IUpstreamConnector {
public:
    std::uint32_t begin_count{0U};

    [[nodiscard]]
    zrdp::agent::network::UpstreamIoResult
    BeginConnect(
        const zrdp::protocol::OpenTarget&,
        std::unique_ptr<
            zrdp::agent::network::
                IUpstreamConnection>& connection)
        noexcept override {
        ++begin_count;
        connection.reset();
        return zrdp::agent::network::
            UpstreamIoResult{
                zrdp::agent::network::
                    UpstreamIoStatus::Failed,
                0U,
                1U,
            };
    }
};

/**
 * @brief frameを不規則なfragmentとしてAgentへ渡す。
 * @param[in,out] bridge 検証対象。
 * @param[in] frame encodeするClient frame。
 * @return 全fragmentを受理できた場合はtrue。
 */
[[nodiscard]] bool DeliverFrame(
    zrdp::agent::bridge::AgentBridge& bridge,
    const zrdp::protocol::Frame& frame) {
    std::vector<std::byte> encoded;
    if (zrdp::protocol::EncodeFrame(
            frame,
            encoded) !=
        zrdp::protocol::FrameError::None) {
        return false;
    }

    std::size_t offset = 0U;
    // headerとpayload境界に一致しない113-byte fragmentで再構成を確認する。
    while (offset < encoded.size()) {
        const std::size_t size = std::min(
            static_cast<std::size_t>(113U),
            encoded.size() - offset);
        if (!bridge.OnChannelData(
                std::span<const std::byte>(encoded).subspan(
                    offset,
                    size))) {
            return false;
        }

        offset += size;
    }

    return true;
}

/**
 * @brief Agent writer queueを一つのdecoderへ連続入力する。
 * @param[in,out] bridge AgentBridge。
 * @param[in,out] decoder Client役decoder。
 * @param[out] frames 完成したAgent frame群。
 * @return queue全体をdecodeできた場合はtrue。
 */
[[nodiscard]] bool DrainFrames(
    zrdp::agent::bridge::AgentBridge& bridge,
    zrdp::protocol::FrameDecoder& decoder,
    std::vector<zrdp::protocol::Frame>& frames) {
    frames.clear();
    bool more_data = false;
    // serialized writer queueが空になるまで全chunkを同じdecoderへ連結する。
    do {
        std::vector<std::byte> chunk;
        if (!bridge.TakeOutboundChunk(
                chunk,
                more_data)) {
            return false;
        }

        std::vector<zrdp::protocol::Frame> decoded;
        if (decoder.Push(chunk, decoded) !=
            zrdp::protocol::FrameError::None) {
            return false;
        }

        frames.insert(
            frames.end(),
            std::make_move_iterator(decoded.begin()),
            std::make_move_iterator(decoded.end()));
    } while (more_data);

    return true;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Agent BridgeのHELLO、PING、stream転送、session終了を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::agent::bridge::AgentBridge;
    using zrdp::agent::bridge::AgentBridgeError;
    using zrdp::agent::bridge::AgentBridgeState;
    using zrdp::protocol::Frame;
    using zrdp::protocol::FrameDecoder;
    using zrdp::protocol::FrameError;
    using zrdp::protocol::HelloCapabilities;
    using zrdp::protocol::MessageType;

    AgentBridge bridge;
    bridge.Reset();
    if (bridge.snapshot().state !=
        AgentBridgeState::WaitingForHello) {
        return 1;
    }

    Frame hello{};
    hello.type = MessageType::Hello;
    if (zrdp::protocol::EncodeHelloCapabilities(
            HelloCapabilities{},
            hello.payload) != FrameError::None ||
        !DeliverFrame(bridge, hello) ||
        bridge.snapshot().state != AgentBridgeState::Ready) {
        return 2;
    }

    FrameDecoder client_decoder;
    std::vector<Frame> frames;
    if (!DrainFrames(
            bridge,
            client_decoder,
            frames) ||
        frames.size() != 1U ||
        frames.front().type != MessageType::HelloAck) {
        return 3;
    }

    // Ctrl+C正常終了ではconnection-level CLOSEを最後のframeとして生成する。
    if (!bridge.QueueSessionClose() ||
        !DrainFrames(
            bridge,
            client_decoder,
            frames) ||
        frames.size() != 1U ||
        frames.front().type != MessageType::Close ||
        frames.front().stream_id != 0U ||
        frames.front().sequence != 0U ||
        !frames.front().payload.empty()) {
        return 4;
    }

    Frame ping{};
    ping.type = MessageType::Ping;
    ping.sequence = 1U;
    ping.payload.resize(32768U);
    // 埋込みNULを含むbinary patternを文字列化せず生成する。
    for (std::size_t index = 0U;
         index < ping.payload.size();
         ++index) {
        ping.payload[index] =
            static_cast<std::byte>(index % 251U);
    }

    if (!DeliverFrame(bridge, ping) ||
        !DrainFrames(
            bridge,
            client_decoder,
            frames) ||
        frames.size() != 1U ||
        frames.front().type != MessageType::Pong ||
        frames.front().sequence != 1U ||
        frames.front().payload != ping.payload ||
        bridge.snapshot().echoed_bytes !=
            ping.payload.size()) {
        return 5;
    }

    // idle HELLOを新しいsequence境界とし、再起動後と同じsequence 1を受け入れる。
    if (!DeliverFrame(bridge, hello) ||
        !DrainFrames(
            bridge,
            client_decoder,
            frames) ||
        frames.size() != 1U ||
        frames.front().type != MessageType::HelloAck ||
        bridge.snapshot().last_ping_sequence != 0U ||
        bridge.snapshot().echoed_bytes != 0U ||
        !DeliverFrame(bridge, ping)) {
        return 6;
    }

    if (!DrainFrames(
            bridge,
            client_decoder,
            frames) ||
        frames.size() != 1U ||
        frames.front().type != MessageType::Pong ||
        frames.front().sequence != 1U) {
        return 7;
    }

    // sequence欠落を正常Echoとして近似しない。
    ping.sequence = 3U;
    if (DeliverFrame(bridge, ping) ||
        bridge.snapshot().state != AgentBridgeState::Failed ||
        bridge.snapshot().error !=
            AgentBridgeError::SequenceMismatch) {
        return 8;
    }

    bridge.Reset();
    // HELLO前のPINGでAgentから暗黙sessionを開始しない。
    ping.sequence = 1U;
    if (DeliverFrame(bridge, ping) ||
        bridge.snapshot().error !=
            AgentBridgeError::UnexpectedMessage) {
        return 9;
    }

    bridge.Reset();
    if (!DeliverFrame(bridge, hello)) {
        return 10;
    }
    // ACKと複数PONGをdrainせず蓄積し、有限writer queue上限で停止する。
    for (std::uint32_t sequence = 1U;
         sequence < 32U &&
         bridge.snapshot().state ==
            AgentBridgeState::Ready;
         ++sequence) {
        ping.sequence = sequence;
        static_cast<void>(
            DeliverFrame(bridge, ping));
    }
    if (bridge.snapshot().state != AgentBridgeState::Failed ||
        bridge.snapshot().error !=
            AgentBridgeError::QueueLimitExceeded) {
        return 11;
    }

    // Phase 3設定時はHELLOで一streamを広告し、OPENをrelayへrouteする。
    zrdp::agent::network::Allowlist allowlist;
    if (!zrdp::agent::network::ParseAllowlist(
            "[allowlist]\n"
            "endpoint.1001=192.0.2.10:80\n",
            allowlist)
            .succeeded()) {
        return 12;
    }

    RejectingConnector connector;
    zrdp::agent::bridge::AgentStreamRelay relay(
        allowlist,
        connector);
    AgentBridge phase3_bridge;
    phase3_bridge.ConfigureStreamRelay(&relay);
    HelloCapabilities phase3_capabilities =
        zrdp::protocol::
            MakePhase2HelloCapabilities();
    phase3_capabilities.maximum_streams =
        zrdp::agent::bridge::
            kMaximumAgentRelayStreams;
    Frame phase3_hello{};
    phase3_hello.type = MessageType::Hello;
    if (zrdp::protocol::EncodeHelloCapabilities(
            phase3_capabilities,
            phase3_hello.payload) !=
            FrameError::None ||
        !DeliverFrame(
            phase3_bridge,
            phase3_hello) ||
        !DrainFrames(
            phase3_bridge,
            client_decoder,
            frames) ||
        frames.size() != 1U ||
        frames.front().type !=
            MessageType::HelloAck) {
        return 13;
    }

    HelloCapabilities agent_capabilities{};
    if (zrdp::protocol::DecodeHelloCapabilities(
            frames.front().payload,
            agent_capabilities) !=
            FrameError::None ||
        agent_capabilities.maximum_streams !=
            zrdp::agent::bridge::
                kMaximumAgentRelayStreams ||
        (agent_capabilities.feature_bits &
         zrdp::protocol::
             kFeatureMultiplexedStreams) == 0U) {
        return 14;
    }

    zrdp::protocol::StreamMultiplexer client(
        zrdp::protocol::MultiplexerRole::Client);
    if (client.StartSession(
            phase3_capabilities) !=
        zrdp::protocol::MultiplexerResult::Ok) {
        return 15;
    }

    std::uint32_t stream_id = 0U;
    if (client.OpenStream(
            zrdp::protocol::OpenTarget{
                {192U, 0U, 2U, 10U},
                80U},
            stream_id) !=
            zrdp::protocol::
                MultiplexerResult::Ok) {
        return 16;
    }

    Frame open{};
    if (!client.TakeOutboundFrame(open) ||
        !DeliverFrame(phase3_bridge, open) ||
        !DrainFrames(
            phase3_bridge,
            client_decoder,
            frames) ||
        frames.size() != 1U ||
        frames.front().type !=
            MessageType::OpenError ||
        client.OnFrame(
            std::move(frames.front())) !=
            zrdp::protocol::
                MultiplexerResult::Ok ||
        connector.begin_count != 1U) {
        return 17;
    }

    zrdp::protocol::StreamEvent open_event{};
    if (!client.TakeEvent(open_event) ||
        open_event.type !=
            zrdp::protocol::
                StreamEventType::OpenFailed ||
        open_event.open_error !=
            zrdp::protocol::
                OpenErrorCode::ConnectFailed) {
        return 18;
    }

    return 0;
}
