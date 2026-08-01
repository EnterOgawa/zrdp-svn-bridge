/**
 * @file agent_bridge.cpp
 * @brief Agent側HELLO_ACK、binary PONG、Phase 5 stream routingを実装する。
 *
 * @details Clientだけがprotocol sessionを開始し、Agentから新規streamや任意接続を
 *          開始しない。payloadは同一byte列として返す以外に解釈・記録しない。
 */

#include "agent/bridge/agent_bridge.h"

#include "agent/bridge/agent_stream_relay.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace zrdp::agent::bridge {
namespace {

/// Agent writer queueが同時保持できるtransport chunk件数。
constexpr std::size_t kMaximumOutboundChunks = 256U;

/// Agent writer queueが所有できるbinary data総量。
constexpr std::size_t kMaximumOutboundQueuedBytes = 256U * 1024U;

}  // namespace

void AgentBridge::ConfigureStreamRelay(
    AgentStreamRelay* const relay) noexcept {
    // 旧relayがactive Socketを保持し得るためpointer置換前にsessionを停止する。
    if (stream_relay_ != nullptr) {
        stream_relay_->StopSession();
    }

    stream_relay_ = relay;
    Reset();
}

void AgentBridge::Reset() noexcept {
    // RDP再接続相当では旧streamとSocketを新HELLOへ持ち越さない。
    if (stream_relay_ != nullptr) {
        stream_relay_->StopSession();
    }

    decoder_.Reset();
    local_capabilities_ =
        protocol::HelloCapabilities{};
    if (stream_relay_ != nullptr) {
        local_capabilities_ =
            protocol::MakePhase2HelloCapabilities();
        // Phase 5 production relayは有限8 Socket tableまでを同時所有する。
        local_capabilities_.maximum_streams =
            kMaximumAgentRelayStreams;
    }

    negotiated_capabilities_ =
        protocol::HelloCapabilities{};
    state_ = AgentBridgeState::WaitingForHello;
    error_ = AgentBridgeError::None;
    last_ping_sequence_ = 0U;
    echoed_bytes_ = 0U;
    outbound_chunks_.clear();
    outbound_queued_bytes_ = 0U;
}

bool AgentBridge::OnChannelData(
    const std::span<const std::byte> data) noexcept {
    // error後の残dataを新しいframe境界として再利用しない。
    if (state_ == AgentBridgeState::Failed ||
        state_ == AgentBridgeState::Incompatible) {
        return false;
    }

    std::vector<protocol::Frame> frames;
    const protocol::FrameError decode_result =
        decoder_.Push(data, frames);
    // version不一致は一般破損と区別してClient／Agent交換を促す。
    if (decode_result ==
        protocol::FrameError::UnsupportedVersion) {
        Fail(
            AgentBridgeState::Incompatible,
            AgentBridgeError::DecodeFailed);
        return false;
    }

    // magic、length、type等の異常後は暗黙再同期しない。
    if (decode_result != protocol::FrameError::None) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::DecodeFailed);
        return false;
    }

    // 一受信に連結されたframeをwire順のまま応答queueへ反映する。
    for (protocol::Frame& frame : frames) {
        switch (frame.type) {
            case protocol::MessageType::Hello:
                if (!HandleHello(frame)) {
                    return false;
                }
                break;
            case protocol::MessageType::Ping:
                if (!HandlePing(frame)) {
                    return false;
                }
                break;
            case protocol::MessageType::Open:
            case protocol::MessageType::OpenOk:
            case protocol::MessageType::OpenError:
            case protocol::MessageType::Data:
            case protocol::MessageType::WindowUpdate:
            case protocol::MessageType::HalfClose:
                if (!HandleStreamFrame(
                        std::move(frame))) {
                    return false;
                }
                break;
            case protocol::MessageType::Close:
                // connection-level CLOSEはstream engineへ渡さない。
                if (frame.stream_id == 0U) {
                    Fail(
                        AgentBridgeState::Failed,
                        AgentBridgeError::
                            UnexpectedMessage);
                    return false;
                }

                if (!HandleStreamFrame(
                        std::move(frame))) {
                    return false;
                }
                break;
            case protocol::MessageType::ProtocolError:
                if (!HandleStreamFrame(
                        std::move(frame))) {
                    return false;
                }
                break;
            case protocol::MessageType::HelloAck:
            case protocol::MessageType::Pong:
                // Agent起点のhandshake／Echo responseをClientから受け入れない。
                Fail(
                    AgentBridgeState::Failed,
                    AgentBridgeError::UnexpectedMessage);
                return false;
        }
    }

    return true;
}

bool AgentBridge::QueueSessionClose() noexcept {
    // protocol失敗後のchannelへ正常終了frameを追加して原因を上書きしない。
    if (state_ == AgentBridgeState::Failed ||
        state_ == AgentBridgeState::Incompatible) {
        return false;
    }

    // Agent終了通知より先に全upstream Socketを閉じる。
    if (stream_relay_ != nullptr) {
        stream_relay_->StopSession();
    }

    protocol::Frame close{};
    close.type = protocol::MessageType::Close;
    close.stream_id = 0U;
    close.sequence = 0U;
    return QueueFrame(close);
}

bool AgentBridge::PumpStreams(
    const std::uint64_t now_milliseconds) noexcept {
    // Echo-only交渉またはHELLO前ではSocket機能を開始しない。
    if (state_ != AgentBridgeState::Ready ||
        stream_relay_ == nullptr ||
        (negotiated_capabilities_.feature_bits &
         protocol::kFeatureMultiplexedStreams) == 0U) {
        return state_ != AgentBridgeState::Failed &&
            state_ != AgentBridgeState::Incompatible;
    }

    if (!stream_relay_->Pump(
            now_milliseconds) ||
        !DrainStreamFrames()) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::StreamRelayFailed);
        return false;
    }

    return true;
}

bool AgentBridge::TakeOutboundChunk(
    std::vector<std::byte>& chunk,
    bool& more_data) noexcept {
    chunk.clear();
    more_data = false;
    // 空queueではWTS writeを呼ばない。
    if (outbound_chunks_.empty()) {
        return false;
    }

    chunk = std::move(outbound_chunks_.front());
    outbound_chunks_.pop_front();
    // 内部値破損時もsize_t underflowを起こさず安全側へ戻す。
    if (outbound_queued_bytes_ >= chunk.size()) {
        outbound_queued_bytes_ -= chunk.size();
    } else {
        outbound_queued_bytes_ = 0U;
    }

    more_data = !outbound_chunks_.empty();
    return true;
}

AgentBridgeSnapshot AgentBridge::snapshot() const noexcept {
    return AgentBridgeSnapshot{
        state_,
        error_,
        echoed_bytes_,
        last_ping_sequence_,
    };
}

bool AgentBridge::HandleHello(
    const protocol::Frame& frame) noexcept {
    // HELLOをEcho sequenceと分離し、固定sequence 0だけ受け付ける。
    if (frame.sequence != 0U) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::InvalidHello);
        return false;
    }

    protocol::HelloCapabilities peer_capabilities{};
    if (protocol::DecodeHelloCapabilities(
            frame.payload,
            peer_capabilities) !=
        protocol::FrameError::None) {
        Fail(
            AgentBridgeState::Incompatible,
            AgentBridgeError::InvalidHello);
        return false;
    }

    protocol::HelloCapabilities negotiated{};
    if (protocol::NegotiateHelloCapabilities(
            local_capabilities_,
            peer_capabilities,
            negotiated) !=
        protocol::FrameError::None) {
        Fail(
            AgentBridgeState::Incompatible,
            AgentBridgeError::InvalidHello);
        return false;
    }

    // 新HELLOは旧Socketを破棄し、共通stream featureがある場合だけrelayを開始する。
    if (stream_relay_ != nullptr) {
        stream_relay_->StopSession();
        if ((negotiated.feature_bits &
             protocol::kFeatureMultiplexedStreams) !=
                0U &&
            !stream_relay_->StartSession(
                negotiated)) {
            Fail(
                AgentBridgeState::Failed,
                AgentBridgeError::
                    StreamRelayFailed);
            return false;
        }
    }

    protocol::Frame hello_ack{};
    hello_ack.type = protocol::MessageType::HelloAck;
    hello_ack.sequence = 0U;
    if (protocol::EncodeHelloCapabilities(
            local_capabilities_,
            hello_ack.payload) !=
        protocol::FrameError::None) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::QueueLimitExceeded);
        return false;
    }

    negotiated_capabilities_ = negotiated;
    if (!QueueFrame(hello_ack)) {
        return false;
    }

    // HELLOを新しいPhase 1 sequence境界とし、Agent再起動とEcho再実行を同じ規則にする。
    last_ping_sequence_ = 0U;
    echoed_bytes_ = 0U;
    state_ = AgentBridgeState::Ready;
    error_ = AgentBridgeError::None;
    return true;
}

bool AgentBridge::HandleStreamFrame(
    protocol::Frame frame) noexcept {
    // compatible HELLOと共通stream capabilityなしではupstream処理を開始しない。
    if (state_ != AgentBridgeState::Ready ||
        stream_relay_ == nullptr ||
        (negotiated_capabilities_.feature_bits &
         protocol::kFeatureMultiplexedStreams) == 0U) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::UnexpectedMessage);
        return false;
    }

    if (!stream_relay_->OnFrame(
            std::move(frame)) ||
        !DrainStreamFrames()) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::StreamRelayFailed);
        return false;
    }

    return true;
}

bool AgentBridge::DrainStreamFrames() noexcept {
    // multiplexerが生成したFrameを既存の単一WTS writer queueへwire順で移す。
    protocol::Frame frame{};
    // relayが公開する有限outbound queueを空になるまで直列化する。
    while (stream_relay_ != nullptr &&
        stream_relay_->TakeOutboundFrame(frame)) {
        if (!QueueFrame(frame)) {
            return false;
        }
    }

    return true;
}

bool AgentBridge::HandlePing(
    const protocol::Frame& frame) noexcept {
    // compatible HELLO前のPINGをAgent起点sessionとして受け入れない。
    if (state_ != AgentBridgeState::Ready) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::UnexpectedMessage);
        return false;
    }

    // 0はHELLO用、以降は欠落・重複・順序入替を完全一致で検出する。
    if (frame.sequence == 0U ||
        last_ping_sequence_ ==
            std::numeric_limits<std::uint32_t>::max() ||
        frame.sequence != last_ping_sequence_ + 1U) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::SequenceMismatch);
        return false;
    }

    // Phase 1 Echoはbyte一致検証用の非空payloadだけを扱う。
    if (frame.payload.empty()) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::UnexpectedMessage);
        return false;
    }

    protocol::Frame pong{};
    pong.type = protocol::MessageType::Pong;
    pong.sequence = frame.sequence;
    pong.payload = frame.payload;
    if (!QueueFrame(pong)) {
        return false;
    }

    last_ping_sequence_ = frame.sequence;
    echoed_bytes_ +=
        static_cast<std::uint64_t>(frame.payload.size());
    return true;
}

bool AgentBridge::QueueFrame(
    const protocol::Frame& frame) noexcept {
    std::vector<std::byte> encoded;
    if (protocol::EncodeFrame(frame, encoded) !=
        protocol::FrameError::None) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::QueueLimitExceeded);
        return false;
    }

    const std::size_t chunk_size = std::min(
        static_cast<std::size_t>(
            negotiated_capabilities_.transport_chunk_bytes),
        protocol::kStaticVirtualChannelChunkBytes);
    // capability破損時に0除算や空chunk loopを起こさない。
    if (chunk_size == 0U) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::InvalidHello);
        return false;
    }

    const std::size_t required_chunks =
        (encoded.size() + chunk_size - 1U) / chunk_size;
    // queue変更前に件数と総byte数の両上限を確認する。
    if (required_chunks >
            kMaximumOutboundChunks -
                outbound_chunks_.size() ||
        encoded.size() >
            kMaximumOutboundQueuedBytes -
                outbound_queued_bytes_) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::QueueLimitExceeded);
        return false;
    }

    try {
        std::size_t offset = 0U;
        // Application frameをMicrosoft SVC上限以下のwrite単位へ分割する。
        while (offset < encoded.size()) {
            const std::size_t current_size = std::min(
                chunk_size,
                encoded.size() - offset);
            outbound_chunks_.emplace_back(
                encoded.begin() +
                    static_cast<std::ptrdiff_t>(offset),
                encoded.begin() +
                    static_cast<std::ptrdiff_t>(
                        offset + current_size));
            offset += current_size;
        }
    } catch (const std::bad_alloc&) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::QueueLimitExceeded);
        return false;
    } catch (...) {
        Fail(
            AgentBridgeState::Failed,
            AgentBridgeError::QueueLimitExceeded);
        return false;
    }

    outbound_queued_bytes_ += encoded.size();
    return true;
}

void AgentBridge::Fail(
    const AgentBridgeState state,
    const AgentBridgeError error) noexcept {
    state_ = state;
    error_ = error;
    // protocol異常後にupstream Socketだけが生存し続けないよう同期停止する。
    if (stream_relay_ != nullptr) {
        stream_relay_->StopSession();
    }

    decoder_.Reset();
    outbound_chunks_.clear();
    outbound_queued_bytes_ = 0U;
}

}  // namespace zrdp::agent::bridge
