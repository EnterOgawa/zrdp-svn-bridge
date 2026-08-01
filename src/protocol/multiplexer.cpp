/**
 * @file multiplexer.cpp
 * @brief ESVNBRG Phase 2のstream多重化、状態遷移、credit制御を実装する。
 *
 * @details peer入力をallocation、状態更新、credit加算へ使う前に検証する。
 *          payloadは所有bufferとして転送するだけで、文字列化やlog出力を行わない。
 */

#include "protocol/multiplexer.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace zrdp::protocol {
namespace {

/// OPENが保持するIPv4 4 byteとport 2 byteの固定長。
constexpr std::size_t kOpenPayloadBytes = 6U;

/// WINDOW_UPDATEと固定error／reason codeのwire長。
constexpr std::size_t kCodePayloadBytes = 4U;

/**
 * @brief host 32-bit値をbig-endian fixed payloadへ変換する。
 * @param[in] value wireへ書く値。
 * @param[out] payload 成功時の4-byte所有buffer。
 * @return allocationを含め成功した場合はtrue。
 */
[[nodiscard]] bool EncodeUint32Payload(
    const std::uint32_t value,
    std::vector<std::byte>& payload) noexcept {
    payload.clear();
    try {
        payload.resize(kCodePayloadBytes);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (...) {
        return false;
    }

    payload[0U] =
        static_cast<std::byte>((value >> 24U) & 0xFFU);
    payload[1U] =
        static_cast<std::byte>((value >> 16U) & 0xFFU);
    payload[2U] =
        static_cast<std::byte>((value >> 8U) & 0xFFU);
    payload[3U] =
        static_cast<std::byte>(value & 0xFFU);
    return true;
}

/**
 * @brief 検証済み4-byte payloadをhost 32-bit値へ変換する。
 * @param[in] payload 4 byteであることをcallerが確認済みのspan。
 * @return host byte orderの値。
 */
[[nodiscard]] constexpr std::uint32_t DecodeUint32Payload(
    const std::span<const std::byte> payload) noexcept {
    return
        (std::to_integer<std::uint32_t>(payload[0U]) << 24U) |
        (std::to_integer<std::uint32_t>(payload[1U]) << 16U) |
        (std::to_integer<std::uint32_t>(payload[2U]) << 8U) |
        std::to_integer<std::uint32_t>(payload[3U]);
}

/**
 * @brief OpenTargetをDNS非対応の固定6-byte payloadへ変換する。
 * @param[in] target IPv4 literalと非0 port。
 * @param[out] payload 成功時の所有buffer。
 * @return 値とallocationが有効ならtrue。
 */
[[nodiscard]] bool EncodeOpenTarget(
    const OpenTarget& target,
    std::vector<std::byte>& payload) noexcept {
    // port 0をOS依存の任意port指定として解釈させない。
    if (target.port == 0U) {
        return false;
    }

    payload.clear();
    try {
        payload.resize(kOpenPayloadBytes);
    } catch (const std::bad_alloc&) {
        return false;
    } catch (...) {
        return false;
    }

    // IPv4 octetは文字列化せずnetwork byte orderのままcopyする。
    for (std::size_t index = 0U;
         index < target.ipv4.size();
         ++index) {
        payload[index] =
            static_cast<std::byte>(target.ipv4[index]);
    }

    payload[4U] =
        static_cast<std::byte>((target.port >> 8U) & 0xFFU);
    payload[5U] =
        static_cast<std::byte>(target.port & 0xFFU);
    return true;
}

/**
 * @brief 固定OPEN payloadをallowlist判定前のOpenTargetへ変換する。
 * @param[in] payload peer由来のbinary payload。
 * @param[out] target 成功時の不可信接続候補。
 * @return 固定長かつ非0 portならtrue。
 */
[[nodiscard]] constexpr bool DecodeOpenTarget(
    const std::span<const std::byte> payload,
    OpenTarget& target) noexcept {
    // field欠落やtrailing dataを別endpoint形式として近似解釈しない。
    if (payload.size() != kOpenPayloadBytes) {
        return false;
    }

    OpenTarget candidate{};
    // 固定4 octetだけをcopyし、host name解決をprotocolへ持ち込まない。
    for (std::size_t index = 0U;
         index < candidate.ipv4.size();
         ++index) {
        candidate.ipv4[index] =
            std::to_integer<std::uint8_t>(payload[index]);
    }

    candidate.port = static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(payload[4U]) << 8U) |
        std::to_integer<std::uint16_t>(payload[5U]));
    // port 0はallowlistへ渡さずprotocol境界で拒否する。
    if (candidate.port == 0U) {
        return false;
    }

    target = candidate;
    return true;
}

/**
 * @brief wire値を定義済みOPEN_ERROR codeへ変換する。
 * @param[in] raw_value peer由来の32-bit値。
 * @param[out] error 既知値の場合の列挙値。
 * @return 現versionで定義済みならtrue。
 */
[[nodiscard]] constexpr bool TryParseOpenError(
    const std::uint32_t raw_value,
    OpenErrorCode& error) noexcept {
    // 未知codeをOS errorやpolicy結果として推測しない。
    if (raw_value <
            static_cast<std::uint32_t>(
                OpenErrorCode::RejectedByPolicy) ||
        raw_value >
            static_cast<std::uint32_t>(
                OpenErrorCode::ResourceExhausted)) {
        return false;
    }

    error = static_cast<OpenErrorCode>(raw_value);
    return true;
}

/**
 * @brief wire値を定義済みstream CLOSE reasonへ変換する。
 * @param[in] raw_value peer由来の32-bit値。
 * @param[out] reason 既知値の場合の列挙値。
 * @return 現versionで定義済みならtrue。
 */
[[nodiscard]] constexpr bool TryParseCloseReason(
    const std::uint32_t raw_value,
    StreamCloseReason& reason) noexcept {
    // 0のnormalを含む連続した定義域だけを受け付ける。
    if (raw_value >
        static_cast<std::uint32_t>(
            StreamCloseReason::SessionStopping)) {
        return false;
    }

    reason = static_cast<StreamCloseReason>(raw_value);
    return true;
}

/**
 * @brief PROTOCOL_ERROR payloadのcodeが現versionで既知か確認する。
 * @param[in] raw_value peer由来の32-bit値。
 * @return `None`を除く定義済みfatal codeならtrue。
 */
[[nodiscard]] constexpr bool IsKnownProtocolError(
    const std::uint32_t raw_value) noexcept {
    return raw_value >=
            static_cast<std::uint32_t>(
                MultiplexerError::InvalidCapabilities) &&
        raw_value <=
            static_cast<std::uint32_t>(
                MultiplexerError::ResourceExhausted);
}

/**
 * @brief negotiated capabilityがmultiplexer開始に十分か確認する。
 * @param[in] capabilities HELLO negotiationの出力。
 * @return stream featureと各有限上限が有効ならtrue。
 */
[[nodiscard]] constexpr bool IsValidMultiplexerCapabilities(
    const HelloCapabilities& capabilities) noexcept {
    // Phase 1だけのpeerをstream対応として公開しない。
    if ((capabilities.feature_bits &
         kFeatureMultiplexedStreams) == 0U) {
        return false;
    }

    // peer宣言値をallocationとmap上限へ使う前に製品絶対上限へ制限する。
    if (capabilities.maximum_frame_payload == 0U ||
        capabilities.maximum_frame_payload >
            kMaximumFramePayloadBytes ||
        capabilities.maximum_streams == 0U ||
        capabilities.maximum_streams >
            kMaximumNegotiableStreams ||
        capabilities.initial_window_bytes == 0U ||
        capabilities.initial_window_bytes >
            kMaximumNegotiableWindowBytes) {
        return false;
    }

    // SVC write上限も既存HELLO契約と同じ範囲だけを受け付ける。
    return capabilities.transport_chunk_bytes > 0U &&
        capabilities.transport_chunk_bytes <=
            kStaticVirtualChannelChunkBytes;
}

/**
 * @brief Frameがqueue内で占有するheader込みbyte数を求める。
 * @param[in] frame 対象Frame。
 * @return bounded queue計算用byte数。
 */
[[nodiscard]] constexpr std::size_t FrameStorageBytes(
    const Frame& frame) noexcept {
    return kFrameHeaderBytes + frame.payload.size();
}

/**
 * @brief local DATA送信を許可するstream stateか判定する。
 * @param[in] state 現在state。
 * @return peerが受信方向を閉じておらずlocal送信が未終了ならtrue。
 */
[[nodiscard]] constexpr bool CanSendData(
    const StreamState state) noexcept {
    return state == StreamState::Open ||
        state == StreamState::RemoteHalfClosed;
}

/**
 * @brief peer DATA受信を許可するstream stateか判定する。
 * @param[in] state 現在state。
 * @return peer送信方向が未終了ならtrue。
 */
[[nodiscard]] constexpr bool CanReceiveData(
    const StreamState state) noexcept {
    // CloseSent中はpeerがrequestを認識する前に送ったordered DATAだけを受け付ける。
    return state == StreamState::Open ||
        state == StreamState::LocalHalfClosed ||
        state == StreamState::CloseSent;
}

/**
 * @brief CLOSE requestを受理できるactive stateか判定する。
 * @param[in] state 現在state。
 * @return duplicate requestでなければtrue。
 */
[[nodiscard]] constexpr bool CanReceiveCloseRequest(
    const StreamState state) noexcept {
    return state != StreamState::CloseCrossed;
}

}  // namespace

StreamMultiplexer::StreamMultiplexer(
    const MultiplexerRole role) noexcept
    : role_(role) {
}

MultiplexerResult StreamMultiplexer::StartSession(
    const HelloCapabilities& capabilities) noexcept {
    StopSession();

    // stream featureと有限値が揃わないpeerをreadyへ遷移させない。
    if (!IsValidMultiplexerCapabilities(capabilities)) {
        error_ = MultiplexerError::InvalidCapabilities;
        return MultiplexerResult::InvalidArgument;
    }

    capabilities_ = capabilities;
    const std::uint64_t aggregate_window =
        static_cast<std::uint64_t>(
            capabilities.maximum_streams) *
        static_cast<std::uint64_t>(
            capabilities.initial_window_bytes);
    initial_connection_window_ = std::min(
        aggregate_window,
        kMaximumConnectionWindowBytes);
    // 乗算元を検証済みだが、公開前にも0 windowを拒否する。
    if (initial_connection_window_ == 0U) {
        error_ = MultiplexerError::InvalidCapabilities;
        return MultiplexerResult::InvalidArgument;
    }

    connection_send_window_ = initial_connection_window_;
    connection_receive_window_ = initial_connection_window_;
    state_ = MultiplexerState::Ready;
    error_ = MultiplexerError::None;
    return MultiplexerResult::Ok;
}

void StreamMultiplexer::StopSession() noexcept {
    streams_.clear();
    outbound_frames_.clear();
    events_.clear();
    state_ = MultiplexerState::Stopped;
    error_ = MultiplexerError::None;
    capabilities_ = HelloCapabilities{};
    next_local_stream_id_ = 1U;
    last_remote_stream_id_ = 0U;
    initial_connection_window_ = 0U;
    connection_send_window_ = 0U;
    connection_receive_window_ = 0U;
    connection_sent_unacknowledged_ = 0U;
    connection_received_unconsumed_ = 0U;
    outbound_queued_bytes_ = 0U;
    inbound_queued_bytes_ = 0U;
}

MultiplexerResult StreamMultiplexer::OpenStream(
    const OpenTarget& target,
    std::uint32_t& stream_id) noexcept {
    stream_id = 0U;
    // session外のlocal要求を次sessionへ持ち越さない。
    if (state_ != MultiplexerState::Ready) {
        return MultiplexerResult::InvalidState;
    }

    // Clientだけが新しいupstream接続候補を開始できる。
    if (role_ != MultiplexerRole::Client) {
        return MultiplexerResult::InvalidRole;
    }

    // negotiated上限を超えるID発行をqueue前に拒否する。
    if (streams_.size() >=
        static_cast<std::size_t>(
            capabilities_.maximum_streams)) {
        return MultiplexerResult::StreamLimitExceeded;
    }

    // 0はconnection control予約値であり、wrap後に再利用しない。
    if (next_local_stream_id_ == 0U) {
        return MultiplexerResult::StreamIdExhausted;
    }

    Frame open{};
    open.type = MessageType::Open;
    open.stream_id = next_local_stream_id_;
    open.sequence = 0U;
    if (!EncodeOpenTarget(target, open.payload)) {
        // port不正はlocal引数error、正しいtargetでの失敗はallocation errorとする。
        if (target.port == 0U) {
            return MultiplexerResult::InvalidArgument;
        }

        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    }

    StreamRecord record{};
    record.state = StreamState::Opening;
    record.target = target;
    record.send_window = capabilities_.initial_window_bytes;
    record.receive_window = capabilities_.initial_window_bytes;
    try {
        const auto [iterator, inserted] =
            streams_.emplace(open.stream_id, record);
        static_cast<void>(iterator);
        // local ID重複は内部invariant破損としてsessionを停止する。
        if (!inserted) {
            Fail(MultiplexerError::DuplicateStreamId, true);
            return MultiplexerResult::Failed;
        }
    } catch (const std::bad_alloc&) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    } catch (...) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    }

    const std::uint32_t allocated_id = open.stream_id;
    const MultiplexerResult queue_result =
        QueueOutboundFrame(std::move(open));
    // backpressure時は未送信streamを公開せず、同じIDで再試行できるよう戻す。
    if (queue_result != MultiplexerResult::Ok) {
        streams_.erase(allocated_id);
        return queue_result;
    }

    stream_id = allocated_id;
    // 最大値の次は0 sentinelとし、session内ID reuseを禁止する。
    if (allocated_id ==
        std::numeric_limits<std::uint32_t>::max()) {
        next_local_stream_id_ = 0U;
    } else {
        next_local_stream_id_ = allocated_id + 1U;
    }

    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::AcceptStream(
    const std::uint32_t stream_id) noexcept {
    if (state_ != MultiplexerState::Ready) {
        return MultiplexerResult::InvalidState;
    }

    // allowlist／Socket判定結果を返す役割はAgentだけに限定する。
    if (role_ != MultiplexerRole::Agent) {
        return MultiplexerResult::InvalidRole;
    }

    const auto iterator = streams_.find(stream_id);
    if (iterator == streams_.end()) {
        return MultiplexerResult::UnknownStream;
    }

    // OPEN requestを一度だけ応答し、重複OPEN_OKを送らない。
    if (iterator->second.state != StreamState::AwaitingAccept) {
        return MultiplexerResult::InvalidState;
    }

    Frame open_ok{};
    open_ok.type = MessageType::OpenOk;
    open_ok.stream_id = stream_id;
    const MultiplexerResult result =
        QueueOutboundFrame(std::move(open_ok));
    if (result != MultiplexerResult::Ok) {
        return result;
    }

    iterator->second.state = StreamState::Open;
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::RejectStream(
    const std::uint32_t stream_id,
    const OpenErrorCode error) noexcept {
    if (state_ != MultiplexerState::Ready) {
        return MultiplexerResult::InvalidState;
    }

    // Agent以外から接続拒否を偽装できないようroleを固定する。
    if (role_ != MultiplexerRole::Agent) {
        return MultiplexerResult::InvalidRole;
    }

    OpenErrorCode validated_error{};
    // 列挙型への不正castをwireへ出力しない。
    if (!TryParseOpenError(
            static_cast<std::uint32_t>(error),
            validated_error)) {
        return MultiplexerResult::InvalidArgument;
    }

    const auto iterator = streams_.find(stream_id);
    if (iterator == streams_.end()) {
        return MultiplexerResult::UnknownStream;
    }

    // pending OPEN以外をOPEN_ERRORで巻き戻さない。
    if (iterator->second.state != StreamState::AwaitingAccept) {
        return MultiplexerResult::InvalidState;
    }

    Frame open_error{};
    open_error.type = MessageType::OpenError;
    open_error.stream_id = stream_id;
    if (!EncodeUint32Payload(
            static_cast<std::uint32_t>(validated_error),
            open_error.payload)) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    }

    const MultiplexerResult result =
        QueueOutboundFrame(std::move(open_error));
    if (result != MultiplexerResult::Ok) {
        return result;
    }

    streams_.erase(iterator);
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::SendData(
    const std::uint32_t stream_id,
    const std::span<const std::byte> data) noexcept {
    if (state_ != MultiplexerState::Ready) {
        return MultiplexerResult::InvalidState;
    }

    // 空DATAとframe上限超過をtransport queueへ渡さない。
    if (data.empty() ||
        data.size() >
            static_cast<std::size_t>(
                capabilities_.maximum_frame_payload)) {
        return MultiplexerResult::InvalidArgument;
    }

    const auto iterator = streams_.find(stream_id);
    if (iterator == streams_.end()) {
        return MultiplexerResult::UnknownStream;
    }

    StreamRecord& stream = iterator->second;
    // local HALF_CLOSE後やOPEN完了前にDATAを送らない。
    if (!CanSendData(stream.state)) {
        return MultiplexerResult::InvalidState;
    }

    const auto data_bytes =
        static_cast<std::uint32_t>(data.size());
    // streamまたはconnection credit不足は非fatal backpressureとして返す。
    if (data_bytes > stream.send_window ||
        static_cast<std::uint64_t>(data_bytes) >
            connection_send_window_) {
        return MultiplexerResult::WouldBlock;
    }

    // sequence 0へのwrapと最大値後の再利用を起こす前にstreamを停止対象とする。
    if (stream.next_send_sequence == 0U ||
        stream.next_send_sequence ==
            std::numeric_limits<std::uint32_t>::max()) {
        return MultiplexerResult::SequenceExhausted;
    }

    Frame frame{};
    frame.type = MessageType::Data;
    frame.stream_id = stream_id;
    frame.sequence = stream.next_send_sequence;
    try {
        frame.payload.assign(data.begin(), data.end());
    } catch (const std::bad_alloc&) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    } catch (...) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    }

    const MultiplexerResult result =
        QueueOutboundFrame(std::move(frame));
    if (result != MultiplexerResult::Ok) {
        return result;
    }

    ++stream.next_send_sequence;
    stream.send_window -= data_bytes;
    stream.sent_unacknowledged += data_bytes;
    connection_send_window_ -=
        static_cast<std::uint64_t>(data_bytes);
    connection_sent_unacknowledged_ +=
        static_cast<std::uint64_t>(data_bytes);
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::ConsumeReceivedData(
    const std::uint32_t stream_id,
    const std::uint32_t bytes) noexcept {
    if (state_ != MultiplexerState::Ready) {
        return MultiplexerResult::InvalidState;
    }

    // 0差分はcredit inflation検証を曖昧にするため送信しない。
    if (bytes == 0U) {
        return MultiplexerResult::InvalidArgument;
    }

    const auto iterator = streams_.find(stream_id);
    if (iterator == streams_.end()) {
        return MultiplexerResult::UnknownStream;
    }

    StreamRecord& stream = iterator->second;
    // terminal CLOSE後にcallerが保持していた古いeventへcreditを再発行しない。
    if (stream.state == StreamState::CloseSent ||
        stream.state == StreamState::CloseCrossed) {
        return MultiplexerResult::InvalidState;
    }

    // callerが受け取っていないbyteへ先行してcreditを返さない。
    if (bytes > stream.received_unconsumed ||
        static_cast<std::uint64_t>(bytes) >
            connection_received_unconsumed_) {
        return MultiplexerResult::InvalidArgument;
    }

    // 内部creditが初期windowを越える状態では加算を行わない。
    if (bytes >
            capabilities_.initial_window_bytes -
                stream.receive_window ||
        static_cast<std::uint64_t>(bytes) >
            initial_connection_window_ -
                connection_receive_window_) {
        Fail(MultiplexerError::FlowControlViolation, true);
        return MultiplexerResult::Failed;
    }

    std::vector<Frame> updates;
    try {
        updates.resize(2U);
    } catch (const std::bad_alloc&) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    } catch (...) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    }

    updates[0U].type = MessageType::WindowUpdate;
    updates[0U].stream_id = stream_id;
    updates[0U].sequence = 0U;
    updates[1U].type = MessageType::WindowUpdate;
    updates[1U].stream_id = 0U;
    updates[1U].sequence = 0U;
    // streamとconnectionの同一差分を独立frameとして明示する。
    for (Frame& update : updates) {
        if (!EncodeUint32Payload(bytes, update.payload)) {
            Fail(MultiplexerError::ResourceExhausted, true);
            return MultiplexerResult::Failed;
        }
    }

    const MultiplexerResult result =
        QueueOutboundFrames(std::move(updates));
    if (result != MultiplexerResult::Ok) {
        return result;
    }

    stream.receive_window += bytes;
    stream.received_unconsumed -= bytes;
    connection_receive_window_ +=
        static_cast<std::uint64_t>(bytes);
    connection_received_unconsumed_ -=
        static_cast<std::uint64_t>(bytes);
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::HalfCloseStream(
    const std::uint32_t stream_id) noexcept {
    if (state_ != MultiplexerState::Ready) {
        return MultiplexerResult::InvalidState;
    }

    const auto iterator = streams_.find(stream_id);
    if (iterator == streams_.end()) {
        return MultiplexerResult::UnknownStream;
    }

    StreamState next_state = StreamState::LocalHalfClosed;
    // OPEN完了済みでlocal送信方向が開いている状態だけを遷移させる。
    if (iterator->second.state == StreamState::Open) {
        next_state = StreamState::LocalHalfClosed;
    } else if (iterator->second.state ==
               StreamState::RemoteHalfClosed) {
        next_state = StreamState::BothHalfClosed;
    } else {
        return MultiplexerResult::InvalidState;
    }

    Frame half_close{};
    half_close.type = MessageType::HalfClose;
    half_close.stream_id = stream_id;
    half_close.sequence = 0U;
    const MultiplexerResult result =
        QueueOutboundFrame(std::move(half_close));
    if (result != MultiplexerResult::Ok) {
        return result;
    }

    iterator->second.state = next_state;
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::CloseStream(
    const std::uint32_t stream_id,
    const StreamCloseReason reason) noexcept {
    if (state_ != MultiplexerState::Ready) {
        return MultiplexerResult::InvalidState;
    }

    StreamCloseReason validated_reason{};
    // 未定義reasonをwireへ出力しない。
    if (!TryParseCloseReason(
            static_cast<std::uint32_t>(reason),
            validated_reason)) {
        return MultiplexerResult::InvalidArgument;
    }

    const auto iterator = streams_.find(stream_id);
    if (iterator == streams_.end()) {
        return MultiplexerResult::UnknownStream;
    }

    // 一つのlocal CLOSE requestに対して一つのackだけを待つ。
    if (iterator->second.state == StreamState::CloseSent ||
        iterator->second.state == StreamState::CloseCrossed) {
        return MultiplexerResult::InvalidState;
    }

    Frame close{};
    close.type = MessageType::Close;
    close.stream_id = stream_id;
    close.sequence = 0U;
    if (!EncodeUint32Payload(
            static_cast<std::uint32_t>(validated_reason),
            close.payload)) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    }

    const MultiplexerResult result =
        QueueOutboundFrame(std::move(close));
    if (result != MultiplexerResult::Ok) {
        return result;
    }

    // CLOSE成立後にcallerへ古いDATA eventを渡さず、受信creditをlocalで解放する。
    DiscardPendingEventsForStream(
        stream_id,
        iterator->second);
    iterator->second.state = StreamState::CloseSent;
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::OnFrame(Frame frame) noexcept {
    // fail-closed後の残Frameを新sessionとして再解釈しない。
    if (state_ != MultiplexerState::Ready) {
        return MultiplexerResult::Failed;
    }

    switch (frame.type) {
        case MessageType::Open:
            return HandleOpen(std::move(frame));
        case MessageType::OpenOk:
            return HandleOpenOk(std::move(frame));
        case MessageType::OpenError:
            return HandleOpenError(std::move(frame));
        case MessageType::Data:
            return HandleData(std::move(frame));
        case MessageType::WindowUpdate:
            return HandleWindowUpdate(std::move(frame));
        case MessageType::HalfClose:
            return HandleHalfClose(std::move(frame));
        case MessageType::Close:
            return HandleClose(std::move(frame));
        case MessageType::ProtocolError:
            return HandleProtocolError(std::move(frame));
        case MessageType::Hello:
        case MessageType::HelloAck:
        case MessageType::Ping:
        case MessageType::Pong:
            // handshake／Echo messageはowner Bridgeが処理し、stream engineへ渡さない。
            Fail(MultiplexerError::UnexpectedMessage, true);
            return MultiplexerResult::Failed;
    }

    Fail(MultiplexerError::UnexpectedMessage, true);
    return MultiplexerResult::Failed;
}

bool StreamMultiplexer::TakeOutboundFrame(Frame& frame) noexcept {
    // queueが空ならtransportへ送信要求を生成しない。
    if (outbound_frames_.empty()) {
        return false;
    }

    frame = std::move(outbound_frames_.front());
    outbound_frames_.pop_front();
    const std::size_t storage_bytes = FrameStorageBytes(frame);
    // 内部値破損時もsize_t underflowを起こさず安全側の0へ戻す。
    if (outbound_queued_bytes_ >= storage_bytes) {
        outbound_queued_bytes_ -= storage_bytes;
    } else {
        outbound_queued_bytes_ = 0U;
    }

    return true;
}

bool StreamMultiplexer::TakeEvent(StreamEvent& event) noexcept {
    // pending eventがなければcallerへ空eventを生成しない。
    if (events_.empty()) {
        return false;
    }

    event = std::move(events_.front());
    events_.pop_front();
    // DATA所有権をcallerへ移した時点でmultiplexerのmemory占有だけを減らす。
    if (inbound_queued_bytes_ >= event.data.size()) {
        inbound_queued_bytes_ -= event.data.size();
    } else {
        inbound_queued_bytes_ = 0U;
    }

    return true;
}

bool StreamMultiplexer::TryGetStreamState(
    const std::uint32_t stream_id,
    StreamState& state) const noexcept {
    const auto iterator = streams_.find(stream_id);
    if (iterator == streams_.end()) {
        return false;
    }

    state = iterator->second.state;
    return true;
}

MultiplexerSnapshot StreamMultiplexer::snapshot() const noexcept {
    return MultiplexerSnapshot{
        state_,
        error_,
        static_cast<std::uint32_t>(streams_.size()),
        outbound_frames_.size(),
        outbound_queued_bytes_,
        events_.size(),
        inbound_queued_bytes_,
        connection_send_window_,
        connection_receive_window_,
    };
}

MultiplexerResult StreamMultiplexer::HandleOpen(
    Frame frame) noexcept {
    // AgentだけがClient開始のOPENを受け付ける。
    if (role_ != MultiplexerRole::Agent) {
        Fail(MultiplexerError::UnexpectedMessage, true);
        return MultiplexerResult::Failed;
    }

    // OPEN control値を固定し、DATA sequenceと混同しない。
    if (frame.sequence != 0U) {
        Fail(MultiplexerError::InvalidPayload, true);
        return MultiplexerResult::Failed;
    }

    // stream IDはsession内で単調増加し、過去IDを再利用しない。
    if (frame.stream_id <= last_remote_stream_id_ ||
        streams_.contains(frame.stream_id)) {
        Fail(MultiplexerError::DuplicateStreamId, true);
        return MultiplexerResult::Failed;
    }

    // negotiated最大数を超えたpeer OPENはresource exhaustion攻撃として停止する。
    if (streams_.size() >=
        static_cast<std::size_t>(
            capabilities_.maximum_streams)) {
        Fail(MultiplexerError::StreamLimitExceeded, true);
        return MultiplexerResult::Failed;
    }

    OpenTarget target{};
    if (!DecodeOpenTarget(frame.payload, target)) {
        Fail(MultiplexerError::InvalidPayload, true);
        return MultiplexerResult::Failed;
    }

    StreamRecord record{};
    record.state = StreamState::AwaitingAccept;
    record.target = target;
    record.send_window = capabilities_.initial_window_bytes;
    record.receive_window = capabilities_.initial_window_bytes;
    try {
        const auto [iterator, inserted] =
            streams_.emplace(frame.stream_id, record);
        static_cast<void>(iterator);
        // contains確認後の重複もfail-closedで扱う。
        if (!inserted) {
            Fail(MultiplexerError::DuplicateStreamId, true);
            return MultiplexerResult::Failed;
        }
    } catch (const std::bad_alloc&) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    } catch (...) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    }

    StreamEvent event{};
    event.type = StreamEventType::OpenRequested;
    event.stream_id = frame.stream_id;
    event.target = target;
    if (!QueueEvent(std::move(event))) {
        return MultiplexerResult::Failed;
    }

    last_remote_stream_id_ = frame.stream_id;
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::HandleOpenOk(
    Frame frame) noexcept {
    // OPEN_OKはClientのlocal OPENにだけ応答できる。
    if (role_ != MultiplexerRole::Client) {
        Fail(MultiplexerError::UnexpectedMessage, true);
        return MultiplexerResult::Failed;
    }

    // 固定control frameへpayloadやDATA sequenceを混在させない。
    if (frame.sequence != 0U ||
        !frame.payload.empty()) {
        Fail(MultiplexerError::InvalidPayload, true);
        return MultiplexerResult::Failed;
    }

    const auto iterator = streams_.find(frame.stream_id);
    if (iterator == streams_.end()) {
        Fail(MultiplexerError::UnknownStream, true);
        return MultiplexerResult::Failed;
    }

    // 一つのOPENに複数responseを適用しない。
    if (iterator->second.state != StreamState::Opening) {
        Fail(MultiplexerError::InvalidStateTransition, true);
        return MultiplexerResult::Failed;
    }

    StreamEvent event{};
    event.type = StreamEventType::Opened;
    event.stream_id = frame.stream_id;
    if (!QueueEvent(std::move(event))) {
        return MultiplexerResult::Failed;
    }

    iterator->second.state = StreamState::Open;
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::HandleOpenError(
    Frame frame) noexcept {
    // OPEN_ERRORはClientのlocal OPENにだけ応答できる。
    if (role_ != MultiplexerRole::Client) {
        Fail(MultiplexerError::UnexpectedMessage, true);
        return MultiplexerResult::Failed;
    }

    // error codeは固定4 byte、control sequenceは0に限定する。
    if (frame.sequence != 0U ||
        frame.payload.size() != kCodePayloadBytes) {
        Fail(MultiplexerError::InvalidPayload, true);
        return MultiplexerResult::Failed;
    }

    OpenErrorCode open_error{};
    if (!TryParseOpenError(
            DecodeUint32Payload(frame.payload),
            open_error)) {
        Fail(MultiplexerError::InvalidPayload, true);
        return MultiplexerResult::Failed;
    }

    const auto iterator = streams_.find(frame.stream_id);
    if (iterator == streams_.end()) {
        Fail(MultiplexerError::UnknownStream, true);
        return MultiplexerResult::Failed;
    }

    // 確立済みstreamを遅延OPEN_ERRORで削除しない。
    if (iterator->second.state != StreamState::Opening) {
        Fail(MultiplexerError::InvalidStateTransition, true);
        return MultiplexerResult::Failed;
    }

    StreamEvent event{};
    event.type = StreamEventType::OpenFailed;
    event.stream_id = frame.stream_id;
    event.open_error = open_error;
    if (!QueueEvent(std::move(event))) {
        return MultiplexerResult::Failed;
    }

    streams_.erase(iterator);
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::HandleData(
    Frame frame) noexcept {
    const auto iterator = streams_.find(frame.stream_id);
    if (iterator == streams_.end()) {
        Fail(MultiplexerError::UnknownStream, true);
        return MultiplexerResult::Failed;
    }

    StreamRecord& stream = iterator->second;
    // OPEN前またはpeer HALF_CLOSE後のDATAを受け付けない。
    if (!CanReceiveData(stream.state)) {
        Fail(MultiplexerError::InvalidStateTransition, true);
        return MultiplexerResult::Failed;
    }

    // 空DATA、frame上限超過、sequence欠落／重複をstate更新前に拒否する。
    if (frame.payload.empty() ||
        frame.payload.size() >
            static_cast<std::size_t>(
                capabilities_.maximum_frame_payload)) {
        Fail(MultiplexerError::InvalidPayload, true);
        return MultiplexerResult::Failed;
    }

    if (stream.next_receive_sequence == 0U ||
        stream.next_receive_sequence ==
            std::numeric_limits<std::uint32_t>::max() ||
        frame.sequence != stream.next_receive_sequence) {
        Fail(MultiplexerError::SequenceMismatch, true);
        return MultiplexerResult::Failed;
    }

    const auto data_bytes =
        static_cast<std::uint32_t>(frame.payload.size());
    // peerが付与済みstream／connection creditを越えた場合はfail-closedにする。
    if (data_bytes > stream.receive_window ||
        static_cast<std::uint64_t>(data_bytes) >
            connection_receive_window_) {
        Fail(MultiplexerError::FlowControlViolation, true);
        return MultiplexerResult::Failed;
    }

    // local CLOSE後にcrossしたDATAはcreditを検証して破棄し、Socketへ公開しない。
    if (stream.state == StreamState::CloseSent) {
        ++stream.next_receive_sequence;
        stream.receive_window -= data_bytes;
        stream.received_unconsumed += data_bytes;
        connection_receive_window_ -=
            static_cast<std::uint64_t>(data_bytes);
        connection_received_unconsumed_ +=
            static_cast<std::uint64_t>(data_bytes);
        return MultiplexerResult::Ok;
    }

    StreamEvent event{};
    event.type = StreamEventType::DataReceived;
    event.stream_id = frame.stream_id;
    event.data = std::move(frame.payload);
    if (!QueueEvent(std::move(event))) {
        return MultiplexerResult::Failed;
    }

    ++stream.next_receive_sequence;
    stream.receive_window -= data_bytes;
    stream.received_unconsumed += data_bytes;
    connection_receive_window_ -=
        static_cast<std::uint64_t>(data_bytes);
    connection_received_unconsumed_ +=
        static_cast<std::uint64_t>(data_bytes);
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::HandleWindowUpdate(
    Frame frame) noexcept {
    // WINDOW_UPDATEは固定4-byte差分とcontrol sequence 0だけを持つ。
    if (frame.sequence != 0U ||
        frame.payload.size() != kCodePayloadBytes) {
        Fail(MultiplexerError::InvalidPayload, true);
        return MultiplexerResult::Failed;
    }

    const std::uint32_t delta =
        DecodeUint32Payload(frame.payload);
    // 0差分と未送信byteを超えるcredit inflationを許可しない。
    if (delta == 0U) {
        Fail(MultiplexerError::FlowControlViolation, true);
        return MultiplexerResult::Failed;
    }

    if (frame.stream_id == 0U) {
        // connection creditは全streamの未確認送信量を越えて戻せない。
        if (static_cast<std::uint64_t>(delta) >
                connection_sent_unacknowledged_ ||
            static_cast<std::uint64_t>(delta) >
                initial_connection_window_ -
                    connection_send_window_) {
            Fail(MultiplexerError::FlowControlViolation, true);
            return MultiplexerResult::Failed;
        }

        connection_sent_unacknowledged_ -=
            static_cast<std::uint64_t>(delta);
        connection_send_window_ +=
            static_cast<std::uint64_t>(delta);
        return MultiplexerResult::Ok;
    }

    const auto iterator = streams_.find(frame.stream_id);
    if (iterator == streams_.end()) {
        Fail(MultiplexerError::UnknownStream, true);
        return MultiplexerResult::Failed;
    }

    StreamRecord& stream = iterator->second;
    // stream creditも実際の未確認送信量と初期上限の範囲だけ戻す。
    if (delta > stream.sent_unacknowledged ||
        delta >
            capabilities_.initial_window_bytes -
                stream.send_window) {
        Fail(MultiplexerError::FlowControlViolation, true);
        return MultiplexerResult::Failed;
    }

    stream.sent_unacknowledged -= delta;
    stream.send_window += delta;
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::HandleHalfClose(
    Frame frame) noexcept {
    // HALF_CLOSEは空payloadとcontrol sequence 0に固定する。
    if (frame.sequence != 0U ||
        !frame.payload.empty()) {
        Fail(MultiplexerError::InvalidPayload, true);
        return MultiplexerResult::Failed;
    }

    const auto iterator = streams_.find(frame.stream_id);
    if (iterator == streams_.end()) {
        Fail(MultiplexerError::UnknownStream, true);
        return MultiplexerResult::Failed;
    }

    StreamState next_state = StreamState::RemoteHalfClosed;
    // peer送信方向が一度だけ開状態から閉状態へ遷移することを確認する。
    if (iterator->second.state == StreamState::Open) {
        next_state = StreamState::RemoteHalfClosed;
    } else if (iterator->second.state ==
               StreamState::LocalHalfClosed) {
        next_state = StreamState::BothHalfClosed;
    } else if (iterator->second.state ==
               StreamState::CloseSent) {
        // local CLOSEとcrossしたHALF_CLOSEはterminal ack待ちstateを変えずに消費する。
        return MultiplexerResult::Ok;
    } else {
        Fail(MultiplexerError::InvalidStateTransition, true);
        return MultiplexerResult::Failed;
    }

    StreamEvent event{};
    event.type = StreamEventType::RemoteHalfClosed;
    event.stream_id = frame.stream_id;
    if (!QueueEvent(std::move(event))) {
        return MultiplexerResult::Failed;
    }

    iterator->second.state = next_state;
    return MultiplexerResult::Ok;
}

MultiplexerResult StreamMultiplexer::HandleClose(
    Frame frame) noexcept {
    // connection-level CLOSEはowner Bridgeが処理し、stream engineへ渡さない。
    if (frame.stream_id == 0U) {
        Fail(MultiplexerError::UnexpectedMessage, true);
        return MultiplexerResult::Failed;
    }

    const auto iterator = streams_.find(frame.stream_id);
    if (iterator == streams_.end()) {
        Fail(MultiplexerError::UnknownStream, true);
        return MultiplexerResult::Failed;
    }

    if (frame.sequence == 0U) {
        // requestは固定reasonを一つだけ持つ。
        if (frame.payload.size() != kCodePayloadBytes ||
            !CanReceiveCloseRequest(iterator->second.state)) {
            Fail(MultiplexerError::InvalidStateTransition, true);
            return MultiplexerResult::Failed;
        }

        StreamCloseReason reason{};
        if (!TryParseCloseReason(
                DecodeUint32Payload(frame.payload),
                reason)) {
            Fail(MultiplexerError::InvalidPayload, true);
            return MultiplexerResult::Failed;
        }

        Frame acknowledgement{};
        acknowledgement.type = MessageType::Close;
        acknowledgement.stream_id = frame.stream_id;
        acknowledgement.sequence = 1U;
        const MultiplexerResult queue_result =
            QueueOutboundFrame(std::move(acknowledgement));
        if (queue_result != MultiplexerResult::Ok) {
            // control responseをqueueできない状態をpeer入力後のbackpressureにしない。
            if (queue_result == MultiplexerResult::WouldBlock) {
                Fail(MultiplexerError::QueueLimitExceeded, true);
                return MultiplexerResult::Failed;
            }

            return queue_result;
        }

        // terminal eventを追加する前に古いeventを除去し、全connection creditを解放する。
        DiscardPendingEventsForStream(
            frame.stream_id,
            iterator->second);
        ReleaseConnectionCredits(iterator->second);

        StreamEvent event{};
        event.type = StreamEventType::Closed;
        event.stream_id = frame.stream_id;
        event.close_reason = reason;
        if (!QueueEvent(std::move(event))) {
            return MultiplexerResult::Failed;
        }

        // crossed requestでは双方のackを確認するまでrecordだけを保持する。
        if (iterator->second.state == StreamState::CloseSent) {
            iterator->second.state = StreamState::CloseCrossed;
        } else {
            streams_.erase(iterator);
        }

        return MultiplexerResult::Ok;
    }

    if (frame.sequence == 1U) {
        // acknowledgementは空payloadかつlocal request待ちstateに限定する。
        if (!frame.payload.empty() ||
            (iterator->second.state != StreamState::CloseSent &&
             iterator->second.state != StreamState::CloseCrossed)) {
            Fail(MultiplexerError::InvalidStateTransition, true);
            return MultiplexerResult::Failed;
        }

        // crossed closeではrequest受信時のClosed eventをqueueに残したままrecordだけを除去する。
        if (iterator->second.state == StreamState::CloseSent) {
            DiscardPendingEventsForStream(
                frame.stream_id,
                iterator->second);
            ReleaseConnectionCredits(iterator->second);

            StreamEvent event{};
            event.type = StreamEventType::Closed;
            event.stream_id = frame.stream_id;
            event.close_reason = StreamCloseReason::Normal;
            if (!QueueEvent(std::move(event))) {
                return MultiplexerResult::Failed;
            }
        }

        streams_.erase(iterator);
        return MultiplexerResult::Ok;
    }

    // CLOSE sequenceはrequest 0またはack 1以外を定義しない。
    Fail(MultiplexerError::InvalidPayload, true);
    return MultiplexerResult::Failed;
}

MultiplexerResult StreamMultiplexer::HandleProtocolError(
    Frame frame) noexcept {
    // peer errorは固定codeだけを受け付け、error応答loopを作らない。
    if (frame.sequence != 0U ||
        frame.payload.size() != kCodePayloadBytes ||
        !IsKnownProtocolError(
            DecodeUint32Payload(frame.payload))) {
        Fail(MultiplexerError::InvalidPayload, false);
        return MultiplexerResult::Failed;
    }

    Fail(MultiplexerError::PeerProtocolError, false);
    return MultiplexerResult::Failed;
}

MultiplexerResult StreamMultiplexer::QueueOutboundFrame(
    Frame frame) noexcept {
    std::vector<Frame> frames;
    try {
        frames.push_back(std::move(frame));
    } catch (const std::bad_alloc&) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    } catch (...) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    }

    return QueueOutboundFrames(std::move(frames));
}

MultiplexerResult StreamMultiplexer::QueueOutboundFrames(
    std::vector<Frame> frames) noexcept {
    // 空transactionを成功扱いにせず、caller invariant違反として返す。
    if (frames.empty()) {
        return MultiplexerResult::InvalidArgument;
    }

    std::size_t required_bytes = 0U;
    // queue変更前に全Frameの件数、payload、加算overflowを一括検証する。
    for (const Frame& frame : frames) {
        if (frame.payload.size() >
            static_cast<std::size_t>(
                kMaximumFramePayloadBytes)) {
            Fail(MultiplexerError::InvalidPayload, true);
            return MultiplexerResult::Failed;
        }

        const std::size_t frame_bytes =
            FrameStorageBytes(frame);
        if (frame_bytes >
            kMaximumMultiplexerOutboundBytes -
                required_bytes) {
            return MultiplexerResult::WouldBlock;
        }

        required_bytes += frame_bytes;
    }

    // 件数とmemory上限のどちらかに達した場合はcallerへbackpressureを返す。
    if (frames.size() >
            kMaximumMultiplexerOutboundFrames -
                outbound_frames_.size() ||
        required_bytes >
            kMaximumMultiplexerOutboundBytes -
                outbound_queued_bytes_) {
        return MultiplexerResult::WouldBlock;
    }

    try {
        // transaction内のwire順を維持して所有Frameを末尾へ追加する。
        for (Frame& frame : frames) {
            outbound_frames_.push_back(std::move(frame));
        }
    } catch (const std::bad_alloc&) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    } catch (...) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return MultiplexerResult::Failed;
    }

    outbound_queued_bytes_ += required_bytes;
    return MultiplexerResult::Ok;
}

bool StreamMultiplexer::QueueEvent(
    StreamEvent event) noexcept {
    // peer credit内でもevent件数と所有payloadの独立上限を超えさせない。
    if (events_.size() >= kMaximumMultiplexerEvents ||
        event.data.size() >
            kMaximumMultiplexerInboundBytes -
                inbound_queued_bytes_) {
        Fail(MultiplexerError::QueueLimitExceeded, true);
        return false;
    }

    const std::size_t data_bytes = event.data.size();
    try {
        events_.push_back(std::move(event));
    } catch (const std::bad_alloc&) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return false;
    } catch (...) {
        Fail(MultiplexerError::ResourceExhausted, true);
        return false;
    }

    inbound_queued_bytes_ += data_bytes;
    return true;
}

void StreamMultiplexer::DiscardPendingEventsForStream(
    const std::uint32_t stream_id,
    StreamRecord& stream) noexcept {
    auto iterator = events_.begin();
    // terminal close対象の未取得eventを除去し、他streamのevent順を維持する。
    while (iterator != events_.end()) {
        if (iterator->stream_id == stream_id) {
            // DATA payloadがある場合だけmultiplexer memory counterを減らす。
            if (inbound_queued_bytes_ >=
                iterator->data.size()) {
                inbound_queued_bytes_ -=
                    iterator->data.size();
            } else {
                inbound_queued_bytes_ = 0U;
            }

            iterator = events_.erase(iterator);
        } else {
            ++iterator;
        }
    }

    // caller取得済みだが未消費のbyteもterminal closeではcredit待ちに残さない。
    const std::uint64_t received =
        static_cast<std::uint64_t>(
            stream.received_unconsumed);
    const std::uint64_t releasable = std::min(
        received,
        connection_received_unconsumed_);
    connection_received_unconsumed_ -= releasable;
    connection_receive_window_ = std::min(
        initial_connection_window_,
        connection_receive_window_ + releasable);
    stream.received_unconsumed = 0U;
    stream.receive_window =
        capabilities_.initial_window_bytes;
}

void StreamMultiplexer::ReleaseConnectionCredits(
    StreamRecord& stream) noexcept {
    // stream終了で未確認送信byteを破棄し、他streamがconnection creditを再利用できるようにする。
    const std::uint64_t sent =
        static_cast<std::uint64_t>(
            stream.sent_unacknowledged);
    const std::uint64_t releasable_sent = std::min(
        sent,
        connection_sent_unacknowledged_);
    connection_sent_unacknowledged_ -= releasable_sent;
    connection_send_window_ = std::min(
        initial_connection_window_,
        connection_send_window_ + releasable_sent);

    // helper未経由の終了経路でもcaller未消費dataを安全側で解放する。
    const std::uint64_t received =
        static_cast<std::uint64_t>(
            stream.received_unconsumed);
    const std::uint64_t releasable_received = std::min(
        received,
        connection_received_unconsumed_);
    connection_received_unconsumed_ -= releasable_received;
    connection_receive_window_ = std::min(
        initial_connection_window_,
        connection_receive_window_ + releasable_received);
    stream.sent_unacknowledged = 0U;
    stream.received_unconsumed = 0U;
}

void StreamMultiplexer::Fail(
    const MultiplexerError error,
    const bool notify_peer) noexcept {
    state_ = MultiplexerState::Failed;
    error_ = error;
    streams_.clear();
    events_.clear();
    outbound_frames_.clear();
    inbound_queued_bytes_ = 0U;
    outbound_queued_bytes_ = 0U;
    connection_send_window_ = 0U;
    connection_receive_window_ = 0U;
    connection_sent_unacknowledged_ = 0U;
    connection_received_unconsumed_ = 0U;

    // peer由来の違反だけを固定codeで一度通知し、payloadやendpointを含めない。
    if (!notify_peer ||
        error == MultiplexerError::None) {
        return;
    }

    Frame protocol_error{};
    protocol_error.type = MessageType::ProtocolError;
    protocol_error.stream_id = 0U;
    protocol_error.sequence = 0U;
    if (!EncodeUint32Payload(
            static_cast<std::uint32_t>(error),
            protocol_error.payload)) {
        return;
    }

    try {
        outbound_queued_bytes_ =
            FrameStorageBytes(protocol_error);
        outbound_frames_.push_back(
            std::move(protocol_error));
    } catch (...) {
        // resource failure中は追加allocationへfallbackせず、通知なしで停止する。
        outbound_frames_.clear();
        outbound_queued_bytes_ = 0U;
    }
}

}  // namespace zrdp::protocol
