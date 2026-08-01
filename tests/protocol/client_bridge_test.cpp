/**
 * @file client_bridge_test.cpp
 * @brief ClientBridgeのhandshake、分割送信、全Echo size、異常遷移を検証する。
 *
 * @details ActiveXとRDP接続を使用せず、in-memory Agent役でframeを往復させる。
 */

#include "client/bridge/client_bridge.h"
#include "protocol/frame.h"
#include "protocol/hello.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace {

/**
 * @brief Clientのtransport chunkを全て取り出してAgent decoderへ渡す。
 * @param[in,out] client 検証対象ClientBridge。
 * @param[in,out] decoder Agent役のincremental decoder。
 * @param[out] frames 今回Clientが送った完全frame群。
 * @return 全chunkをerrorなくdecodeできた場合はtrue。
 */
[[nodiscard]] bool DrainClientFrames(
    zrdp::client::bridge::ClientBridge& client,
    zrdp::protocol::FrameDecoder& decoder,
    std::vector<zrdp::protocol::Frame>& frames) {
    frames.clear();
    bool more_data = false;
    // more_dataがfalseになるまで、bounded SVC chunkを一つずつdecoderへ渡す。
    do {
        std::vector<std::byte> chunk;
        if (!client.TakeOutboundChunk(chunk, more_data)) {
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

/**
 * @brief Agent応答frameを不規則な777-byte fragmentでClientへ返す。
 * @param[in,out] client 応答を受信するClientBridge。
 * @param[in] frame encodeして返すframe。
 * @return 全fragmentを受理できた場合はtrue。
 */
[[nodiscard]] bool DeliverAgentFrame(
    zrdp::client::bridge::ClientBridge& client,
    const zrdp::protocol::Frame& frame) {
    std::vector<std::byte> encoded;
    if (zrdp::protocol::EncodeFrame(frame, encoded) !=
        zrdp::protocol::FrameError::None) {
        return false;
    }

    std::size_t offset = 0U;
    // Client callbackがframe境界と一致しない状態を毎回再現する。
    while (offset < encoded.size()) {
        const std::size_t chunk_size = std::min(
            static_cast<std::size_t>(777U),
            encoded.size() - offset);
        if (!client.OnChannelData(
                std::span<const std::byte>(encoded).subspan(
                    offset,
                    chunk_size))) {
            return false;
        }

        offset += chunk_size;
    }

    return true;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Client Bridgeのhandshake、Echo、chunk化、異常遷移を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::client::bridge::ClientBridge;
    using zrdp::client::bridge::ClientBridgeError;
    using zrdp::client::bridge::ClientBridgeState;
    using zrdp::protocol::Frame;
    using zrdp::protocol::FrameDecoder;
    using zrdp::protocol::FrameError;
    using zrdp::protocol::HelloCapabilities;
    using zrdp::protocol::MessageType;

    ClientBridge client;
    // 新session開始時にAgent待機と初回HELLOを生成する。
    if (!client.StartSession() ||
        client.snapshot().state !=
            ClientBridgeState::WaitingForAgent ||
        client.snapshot().hello_attempt_count != 1U) {
        return 1;
    }

    FrameDecoder agent_decoder;
    std::vector<Frame> client_frames;
    if (!DrainClientFrames(
            client,
            agent_decoder,
            client_frames) ||
        client_frames.size() != 1U ||
        client_frames.front().type != MessageType::Hello ||
        client_frames.front().sequence != 0U) {
        return 2;
    }

    HelloCapabilities client_capabilities{};
    if (zrdp::protocol::DecodeHelloCapabilities(
            client_frames.front().payload,
            client_capabilities) != FrameError::None ||
        (client_capabilities.feature_bits &
         zrdp::protocol::kFeatureMultiplexedStreams) == 0U ||
        client_capabilities.maximum_streams !=
            zrdp::client::bridge::
                kMaximumClientBridgeStreams) {
        return 3;
    }

    Frame hello_ack{};
    hello_ack.type = MessageType::HelloAck;
    if (zrdp::protocol::EncodeHelloCapabilities(
            HelloCapabilities{},
            hello_ack.payload) != FrameError::None ||
        !DeliverAgentFrame(client, hello_ack) ||
        client.snapshot().state != ClientBridgeState::Ready) {
        return 4;
    }

    Frame agent_close{};
    agent_close.type = MessageType::Close;
    // Agent正常終了でRDPを切らずHELLO待機へ戻り、初回HELLOを即時生成する。
    if (!DeliverAgentFrame(client, agent_close) ||
        client.snapshot().state !=
            ClientBridgeState::WaitingForAgent ||
        client.snapshot().hello_attempt_count != 1U ||
        !DrainClientFrames(
            client,
            agent_decoder,
            client_frames) ||
        client_frames.size() != 1U ||
        client_frames.front().type != MessageType::Hello ||
        !DeliverAgentFrame(client, hello_ack) ||
        client.snapshot().state != ClientBridgeState::Ready) {
        return 5;
    }

    // ready後に1 byte～10 MiBの段階testを開始する。
    if (!client.BeginEchoTest() ||
        client.snapshot().state !=
            ClientBridgeState::EchoRunning) {
        return 6;
    }

    bool observed_embedded_null = false;
    std::uint32_t last_sequence = 0U;
    bool first_echo_chunk = true;
    // 一度に一PINGをAgent役でPONGへ変換し、全target完了まで往復する。
    while (client.snapshot().state ==
        ClientBridgeState::EchoRunning) {
        if (!DrainClientFrames(
                client,
                agent_decoder,
                client_frames)) {
            return 7;
        }

        // 各Echo先頭のHELLOへACKし、後続PINGより先にsequence境界を成立させる。
        if (first_echo_chunk) {
            if (client_frames.size() != 2U ||
                client_frames.front().type !=
                    MessageType::Hello ||
                !DeliverAgentFrame(client, hello_ack)) {
                return 8;
            }
            first_echo_chunk = false;
        } else if (client_frames.size() != 1U) {
            return 9;
        }

        const Frame& ping = client_frames.back();
        if (ping.type != MessageType::Ping ||
            ping.sequence <= last_sequence ||
            ping.payload.empty() ||
            ping.payload.size() > 32U * 1024U) {
            return 10;
        }

        last_sequence = ping.sequence;
        observed_embedded_null =
            observed_embedded_null ||
            std::find(
                ping.payload.begin(),
                ping.payload.end(),
                std::byte{0U}) != ping.payload.end();

        Frame pong{};
        pong.type = MessageType::Pong;
        pong.sequence = ping.sequence;
        pong.payload = ping.payload;
        if (!DeliverAgentFrame(client, pong)) {
            return 11;
        }
    }

    const auto passed = client.snapshot();
    if (passed.state != ClientBridgeState::EchoPassed ||
        passed.echo_completed_bytes !=
            passed.echo_total_bytes ||
        passed.echo_total_bytes != 11600897U ||
        !observed_embedded_null) {
        return 12;
    }

    // handshake後の周期HELLOでAgent側active streamをresetしない。
    if (client.QueueHelloRetry() ||
        client.snapshot().state !=
            ClientBridgeState::EchoPassed) {
        return 13;
    }

    // health PINGはactive sessionをHELLOでresetせず、Echo counterにも加算しない。
    if (!client.QueueHealthCheck() ||
        !client.snapshot().health_check_outstanding ||
        client.BeginEchoTest() ||
        !DrainClientFrames(
            client,
            agent_decoder,
            client_frames) ||
        client_frames.size() != 1U ||
        client_frames.front().type != MessageType::Ping) {
        return 40;
    }

    const Frame health_ping = client_frames.front();
    Frame health_pong{};
    health_pong.type = MessageType::Pong;
    health_pong.sequence = health_ping.sequence;
    health_pong.payload = health_ping.payload;
    if (!DeliverAgentFrame(client, health_pong) ||
        client.snapshot().health_check_outstanding ||
        client.snapshot().state != ClientBridgeState::EchoPassed ||
        client.snapshot().echo_completed_bytes !=
            passed.echo_completed_bytes) {
        return 41;
    }

    // Agent異常終了相当では旧stateを破棄し、同じRDP上のHELLO待機へ自動復帰する。
    if (!client.QueueHealthCheck() ||
        !DrainClientFrames(
            client,
            agent_decoder,
            client_frames) ||
        client_frames.size() != 1U ||
        !client.OnHealthCheckTimeout() ||
        client.snapshot().state !=
            ClientBridgeState::WaitingForAgent ||
        client.snapshot().health_check_outstanding ||
        !DrainClientFrames(
            client,
            agent_decoder,
            client_frames) ||
        client_frames.size() != 1U ||
        client_frames.front().type != MessageType::Hello) {
        return 42;
    }

    Frame stale_data{};
    stale_data.type = MessageType::Data;
    stale_data.stream_id = 1U;
    stale_data.sequence = 1U;
    stale_data.payload = {std::byte{0x41U}};
    Frame stale_inbound{};
    // timeout直前の旧stream FrameとPONGをACK前に破棄し、新sessionを汚染しない。
    if (!DeliverAgentFrame(client, stale_data) ||
        client.TakeInboundStreamFrame(stale_inbound) ||
        !DeliverAgentFrame(client, health_pong) ||
        client.snapshot().state !=
            ClientBridgeState::WaitingForAgent ||
        !DeliverAgentFrame(client, hello_ack) ||
        client.snapshot().state != ClientBridgeState::Ready) {
        return 43;
    }

    // 再実行時もsequenceとcounterを新しいtestとして初期化する。
    if (!client.BeginEchoTest()) {
        return 14;
    }
    if (!DrainClientFrames(
            client,
            agent_decoder,
            client_frames) ||
        client_frames.size() != 2U ||
        client_frames.front().type != MessageType::Hello ||
        client_frames.back().type != MessageType::Ping ||
        !DeliverAgentFrame(client, hello_ack)) {
        return 15;
    }

    Frame wrong_sequence{};
    wrong_sequence.type = MessageType::Pong;
    wrong_sequence.sequence =
        client_frames.back().sequence + 1U;
    wrong_sequence.payload =
        client_frames.back().payload;
    // out-of-order PONGをfail-closedにする。
    if (DeliverAgentFrame(client, wrong_sequence) ||
        client.snapshot().state != ClientBridgeState::Failed ||
        client.snapshot().error !=
            ClientBridgeError::SequenceMismatch) {
        return 16;
    }

    // Agent不在またはPONG欠落時は未応答testを有限時間で停止できる。
    client.StopSession();
    if (!client.StartSession() ||
        !DrainClientFrames(
            client,
            agent_decoder,
            client_frames) ||
        !DeliverAgentFrame(client, hello_ack) ||
        !client.BeginEchoTest()) {
        return 17;
    }
    client.OnEchoTimeout();
    if (client.snapshot().state != ClientBridgeState::Failed ||
        client.snapshot().error !=
            ClientBridgeError::ResponseTimeout) {
        return 18;
    }

    // RDP disconnect相当resetで旧frameとerrorを再利用しない。
    client.StopSession();
    if (client.snapshot().state != ClientBridgeState::Stopped ||
        client.snapshot().echo_completed_bytes != 0U) {
        return 19;
    }

    if (!client.StartSession()) {
        return 20;
    }

    // drainされないHELLO retryを無制限にbufferせずqueue上限で停止する。
    for (std::size_t attempt = 0U;
         attempt < 5000U &&
         client.snapshot().state ==
            ClientBridgeState::WaitingForAgent;
         ++attempt) {
        static_cast<void>(client.QueueHelloRetry());
    }
    if (client.snapshot().state != ClientBridgeState::Failed ||
        client.snapshot().error !=
            ClientBridgeError::QueueLimitExceeded) {
        return 21;
    }

    // Phase 2 Agentとの交渉後だけworker Frameを双方向queueへrouteする。
    ClientBridge stream_client;
    FrameDecoder stream_decoder;
    if (!stream_client.StartSession() ||
        !DrainClientFrames(
            stream_client,
            stream_decoder,
            client_frames)) {
        return 22;
    }

    HelloCapabilities phase2 =
        zrdp::protocol::MakePhase2HelloCapabilities();
    phase2.maximum_streams = 1U;
    Frame phase2_ack{};
    phase2_ack.type = MessageType::HelloAck;
    if (zrdp::protocol::EncodeHelloCapabilities(
            phase2,
            phase2_ack.payload) != FrameError::None ||
        !DeliverAgentFrame(stream_client, phase2_ack) ||
        (stream_client.negotiated_capabilities().
             feature_bits &
         zrdp::protocol::kFeatureMultiplexedStreams) == 0U) {
        return 23;
    }

    Frame open{};
    open.type = MessageType::Open;
    open.stream_id = 1U;
    open.payload = {
        std::byte{192U},
        std::byte{0U},
        std::byte{2U},
        std::byte{10U},
        std::byte{0U},
        std::byte{80U},
    };
    if (!stream_client.QueueStreamFrame(open) ||
        !DrainClientFrames(
            stream_client,
            stream_decoder,
            client_frames) ||
        client_frames.size() != 1U ||
        client_frames.front().type !=
            MessageType::Open) {
        return 24;
    }

    Frame open_ok{};
    open_ok.type = MessageType::OpenOk;
    open_ok.stream_id = 1U;
    if (!DeliverAgentFrame(
            stream_client,
            open_ok)) {
        return 25;
    }

    Frame inbound{};
    if (!stream_client.TakeInboundStreamFrame(
            inbound) ||
        inbound.type != MessageType::OpenOk ||
        inbound.stream_id != 1U ||
        stream_client.TakeInboundStreamFrame(inbound)) {
        return 26;
    }

    return 0;
}
