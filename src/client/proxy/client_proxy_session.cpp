/**
 * @file client_proxy_session.cpp
 * @brief HTTP parser出力とClient multiplexerの双方向bounded relayを実装する。
 */

#include "client/proxy/client_proxy_session.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace zrdp::client::proxy {
namespace {

/**
 * @brief parser分類を利用者向け固定HTTP statusへ変換する。
 * @param[in] error URLやheader内容を含まない分類。
 * @return Phase 5の固定status code。
 */
[[nodiscard]] constexpr std::uint32_t HttpStatusForParserError(
    const HttpRequestError error) noexcept {
    switch (error) {
        case HttpRequestError::ConnectNotAllowed:
            return 405U;
        case HttpRequestError::UnsupportedScheme:
        case HttpRequestError::UpgradeNotAllowed:
        case HttpRequestError::TransferEncodingNotSupported:
            return 501U;
        case HttpRequestError::HeaderTooLarge:
        case HttpRequestError::ChunkLineTooLarge:
        case HttpRequestError::TrailerTooLarge:
            return 431U;
        case HttpRequestError::None:
        case HttpRequestError::MalformedRequestLine:
        case HttpRequestError::MalformedHeader:
        case HttpRequestError::MissingHost:
        case HttpRequestError::DuplicateHost:
        case HttpRequestError::HostMismatch:
        case HttpRequestError::InvalidIpv4:
        case HttpRequestError::InvalidPort:
        case HttpRequestError::DuplicateContentLength:
        case HttpRequestError::InvalidContentLength:
        case HttpRequestError::DestinationChanged:
        case HttpRequestError::ResourceExhausted:
        case HttpRequestError::InvalidTransferEncoding:
        case HttpRequestError::ConflictingMessageFraming:
        case HttpRequestError::InvalidChunkSize:
        case HttpRequestError::InvalidChunkTerminator:
        case HttpRequestError::MalformedTrailer:
            return 400U;
    }

    return 400U;
}

/**
 * @brief Agent OPEN_ERRORを固定HTTP statusへ変換する。
 * @param[in] error protocolで検証済みの固定code。
 * @return endpoint情報を含まないstatus code。
 */
[[nodiscard]] constexpr std::uint32_t HttpStatusForOpenError(
    const protocol::OpenErrorCode error) noexcept {
    switch (error) {
        case protocol::OpenErrorCode::RejectedByPolicy:
            return 403U;
        case protocol::OpenErrorCode::ConnectFailed:
            return 502U;
        case protocol::OpenErrorCode::Timeout:
            return 504U;
        case protocol::OpenErrorCode::ResourceExhausted:
            return 503U;
    }

    return 502U;
}

/**
 * @brief status codeに対応するpayloadなし固定HTTP responseを返す。
 * @param[in] status_code 対応済みcode。
 * @return path、header、endpointを含まないresponse。
 */
[[nodiscard]] constexpr std::string_view FixedHttpResponse(
    const std::uint32_t status_code) noexcept {
    switch (status_code) {
        case 400U:
            return
                "HTTP/1.1 400 Bad Request\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n\r\n";
        case 403U:
            return
                "HTTP/1.1 403 Forbidden\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n\r\n";
        case 405U:
            return
                "HTTP/1.1 405 Method Not Allowed\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n\r\n";
        case 431U:
            return
                "HTTP/1.1 431 Request Header Fields Too Large\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n\r\n";
        case 501U:
            return
                "HTTP/1.1 501 Not Implemented\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n\r\n";
        case 503U:
            return
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n\r\n";
        case 504U:
            return
                "HTTP/1.1 504 Gateway Timeout\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n\r\n";
        case 502U:
        default:
            return
                "HTTP/1.1 502 Bad Gateway\r\n"
                "Connection: close\r\n"
                "Content-Length: 0\r\n\r\n";
    }
}

}  // namespace

ClientProxySession::ClientProxySession() noexcept
    : multiplexer_(protocol::MultiplexerRole::Client) {
}

bool ClientProxySession::StartBridgeSession(
    const protocol::HelloCapabilities& capabilities) noexcept {
    StopBridgeSession();
    // Echo-only、0上限、production table超過ではlocal requestを受け付けない。
    if ((capabilities.feature_bits &
         protocol::kFeatureMultiplexedStreams) == 0U ||
        capabilities.maximum_streams == 0U ||
        capabilities.maximum_streams >
            kMaximumClientProxyConnections ||
        capabilities.initial_window_bytes == 0U ||
        capabilities.maximum_frame_payload == 0U) {
        state_ = ClientProxySessionState::Failed;
        error_ =
            ClientProxySessionError::InvalidCapabilities;
        return false;
    }

    if (multiplexer_.StartSession(capabilities) !=
        protocol::MultiplexerResult::Ok) {
        state_ = ClientProxySessionState::Failed;
        error_ =
            ClientProxySessionError::InvalidCapabilities;
        return false;
    }

    maximum_frame_payload_ =
        capabilities.maximum_frame_payload;
    maximum_connections_ =
        capabilities.maximum_streams;
    state_ = ClientProxySessionState::Ready;
    error_ = ClientProxySessionError::None;
    return true;
}

void ClientProxySession::StopBridgeSession() noexcept {
    multiplexer_.StopSession();
    ResetConnections();
    maximum_frame_payload_ = 0U;
    maximum_connections_ = 0U;
    state_ = ClientProxySessionState::Stopped;
    error_ = ClientProxySessionError::None;
    http_error_ = HttpRequestError::None;
}

LocalConnectionResult
ClientProxySession::BeginLocalConnection(
    const LocalConnectionId connection_id) noexcept {
    // fatal relayはBridge session resetまでlocal connectionを受け付けない。
    if (state_ == ClientProxySessionState::Failed) {
        return LocalConnectionResult::Failed;
    }

    // compatible HELLO前のconnectionはownerが固定503で即時終了する。
    if (state_ == ClientProxySessionState::Stopped) {
        return LocalConnectionResult::BridgeUnavailable;
    }

    // 0、重複、negotiated上限超過をtableへ追加しない。
    if (connection_id == 0U ||
        connections_.contains(connection_id) ||
        maximum_connections_ == 0U ||
        connections_.size() >= maximum_connections_) {
        return LocalConnectionResult::Busy;
    }

    try {
        ConnectionRecord connection{};
        connection.connection_id = connection_id;
        connection.local_connection_active = true;
        const auto [iterator, inserted] =
            connections_.emplace(
                connection_id,
                std::move(connection));
        static_cast<void>(iterator);
        // contains確認後の重複は内部不変条件違反として扱う。
        if (!inserted) {
            Fail(ClientProxySessionError::InvalidState);
            return LocalConnectionResult::Failed;
        }
    } catch (...) {
        Fail(
            ClientProxySessionError::
                ResourceExhausted);
        return LocalConnectionResult::Failed;
    }

    return LocalConnectionResult::Accepted;
}

LocalConnectionResult
ClientProxySession::BeginLocalConnection() noexcept {
    return BeginLocalConnection(kLegacyConnectionId);
}

bool ClientProxySession::OnLocalData(
    const LocalConnectionId connection_id,
    const std::span<const std::byte> data) noexcept {
    const auto iterator =
        connections_.find(connection_id);
    // 空data、FIN後、未受付SocketをHTTP parserへ渡さない。
    if (data.empty() ||
        iterator == connections_.end() ||
        !iterator->second.local_connection_active ||
        iterator->second.local_receive_closed ||
        iterator->second.close_local_when_drained ||
        state_ == ClientProxySessionState::Stopped ||
        state_ == ClientProxySessionState::Failed) {
        Fail(ClientProxySessionError::InvalidState);
        return false;
    }

    ConnectionRecord& connection =
        iterator->second;
    std::vector<std::vector<std::byte>> chunks;
    if (!connection.parser.Push(data, chunks)) {
        return RejectHttpRequest(
            connection,
            connection.parser.snapshot().error);
    }

    if (!QueueUpstreamChunks(
            connection,
            std::move(chunks))) {
        return false;
    }

    protocol::OpenTarget target{};
    // 最初の完全headerを得た時点で一度だけAgent OPENを開始する。
    if (connection.stream_id == 0U &&
        connection.parser.target(target)) {
        std::uint32_t stream_id = 0U;
        const protocol::MultiplexerResult result =
            multiplexer_.OpenStream(
                target,
                stream_id);
        if (result != protocol::MultiplexerResult::Ok ||
            stream_id == 0U) {
            Fail(
                ClientProxySessionError::
                    MultiplexerFailed);
            return false;
        }

        try {
            const auto [route, inserted] =
                stream_routes_.emplace(
                    stream_id,
                    connection_id);
            static_cast<void>(route);
            if (!inserted) {
                Fail(
                    ClientProxySessionError::
                        InvalidState);
                return false;
            }
        } catch (...) {
            Fail(
                ClientProxySessionError::
                    ResourceExhausted);
            return false;
        }

        connection.stream_id = stream_id;
        connection.state =
            ClientProxySessionState::Opening;
    }

    return DrainUpstreamData();
}

bool ClientProxySession::OnLocalData(
    const std::span<const std::byte> data) noexcept {
    return OnLocalData(kLegacyConnectionId, data);
}

bool ClientProxySession::OnLocalReceiveClosed() noexcept {
    return OnLocalReceiveClosed(kLegacyConnectionId);
}

bool ClientProxySession::OnLocalReceiveClosed(
    const LocalConnectionId connection_id) noexcept {
    const auto iterator =
        connections_.find(connection_id);
    // 同じlocal SocketのFINを一度だけ処理する。
    if (iterator == connections_.end() ||
        !iterator->second.local_connection_active ||
        iterator->second.local_receive_closed) {
        return state_ != ClientProxySessionState::Failed;
    }

    ConnectionRecord& connection =
        iterator->second;
    connection.local_receive_closed = true;
    const HttpRequestParserSnapshot parser_snapshot =
        connection.parser.snapshot();
    // header／Content-Length／chunk途中のFINは正常HALF_CLOSEとして扱わない。
    if (!parser_snapshot.at_request_boundary) {
        if (connection.stream_id != 0U &&
            !connection.stream_close_sent) {
            const protocol::MultiplexerResult result =
                multiplexer_.CloseStream(
                    connection.stream_id,
                    protocol::StreamCloseReason::PeerReset);
            if (result != protocol::MultiplexerResult::Ok) {
                Fail(
                    ClientProxySessionError::
                        MultiplexerFailed);
                return false;
            }

            connection.stream_close_sent = true;
            connection.state =
                ClientProxySessionState::Closing;
        }

        connection.close_local_when_drained = true;
        return true;
    }

    // requestを一件も受けずにFINしたSocketはBridge streamを作らず閉じる。
    if (connection.stream_id == 0U) {
        connection.close_local_when_drained = true;
        return true;
    }

    return ApplyPendingLocalHalfClose(connection);
}

void ClientProxySession::OnLocalConnectionClosed(
    const LocalConnectionId connection_id,
    const protocol::StreamCloseReason reason) noexcept {
    const auto iterator =
        connections_.find(connection_id);
    if (iterator == connections_.end()) {
        return;
    }

    ConnectionRecord& connection =
        iterator->second;
    // local未送信responseを破棄する時もglobal counterを同時に戻す。
    if (connection.local_write_bytes >
        local_write_bytes_) {
        Fail(ClientProxySessionError::InvalidState);
        return;
    }

    local_write_bytes_ -=
        connection.local_write_bytes;
    connection.local_writes.clear();
    connection.local_write_bytes = 0U;
    connection.local_connection_active = false;
    connection.close_local_when_drained = false;
    connection.remote_half_closed = false;
    connection.local_send_shutdown = false;
    // terminal待ちでないactive streamだけへ一度CLOSEを生成する。
    if (connection.stream_id != 0U &&
        !connection.stream_close_sent &&
        state_ != ClientProxySessionState::Failed &&
        state_ != ClientProxySessionState::Stopped) {
        const protocol::MultiplexerResult result =
            multiplexer_.CloseStream(
                connection.stream_id,
                reason);
        if (result != protocol::MultiplexerResult::Ok) {
            Fail(
                ClientProxySessionError::
                    MultiplexerFailed);
            return;
        }

        connection.stream_close_sent = true;
        connection.state =
            ClientProxySessionState::Closing;
    } else if (connection.stream_id == 0U) {
        EraseConnection(connection_id);
    }
}

void ClientProxySession::OnLocalConnectionClosed(
    const protocol::StreamCloseReason reason) noexcept {
    OnLocalConnectionClosed(
        kLegacyConnectionId,
        reason);
}

bool ClientProxySession::OnBridgeFrame(
    protocol::Frame frame) noexcept {
    // compatible HELLO前またはfatal後のFrameを新sessionへ持ち越さない。
    if (state_ == ClientProxySessionState::Stopped ||
        state_ == ClientProxySessionState::Failed) {
        return false;
    }

    if (multiplexer_.OnFrame(std::move(frame)) !=
        protocol::MultiplexerResult::Ok) {
        Fail(ClientProxySessionError::MultiplexerFailed);
        return false;
    }

    if (!DrainEvents() ||
        !DrainUpstreamData()) {
        return false;
    }

    // WINDOW_UPDATEで送信queueが空になったconnectionのFINを公平に適用する。
    for (auto& [connection_id, connection] :
         connections_) {
        static_cast<void>(connection_id);
        if (!ApplyPendingLocalHalfClose(connection)) {
            return false;
        }
    }

    return true;
}

bool ClientProxySession::TakeOutboundFrame(
    protocol::Frame& frame) noexcept {
    return multiplexer_.TakeOutboundFrame(frame);
}

bool ClientProxySession::PeekLocalWrite(
    const LocalConnectionId connection_id,
    std::span<const std::byte>& data) const noexcept {
    data = {};
    const auto iterator =
        connections_.find(connection_id);
    if (iterator == connections_.end() ||
        iterator->second.local_writes.empty()) {
        return false;
    }

    const PendingLocalWrite& pending =
        iterator->second.local_writes.front();
    // offset破損を範囲外spanとして公開しない。
    if (pending.offset >= pending.data.size()) {
        return false;
    }

    data = std::span<const std::byte>(
        pending.data.data() + pending.offset,
        pending.data.size() - pending.offset);
    return true;
}

bool ClientProxySession::PeekLocalWrite(
    std::span<const std::byte>& data) const noexcept {
    return PeekLocalWrite(
        kLegacyConnectionId,
        data);
}

bool ClientProxySession::ConsumeLocalWrite(
    const LocalConnectionId connection_id,
    const std::size_t bytes) noexcept {
    const auto iterator =
        connections_.find(connection_id);
    // Socket報告値をqueue範囲外へ適用しない。
    if (iterator == connections_.end() ||
        iterator->second.local_writes.empty() ||
        bytes == 0U) {
        Fail(ClientProxySessionError::InvalidState);
        return false;
    }

    ConnectionRecord& connection =
        iterator->second;
    PendingLocalWrite& pending =
        connection.local_writes.front();
    const std::size_t remaining =
        pending.data.size() - pending.offset;
    if (bytes > remaining ||
        bytes > connection.local_write_bytes ||
        bytes > local_write_bytes_) {
        Fail(ClientProxySessionError::InvalidState);
        return false;
    }

    pending.offset += bytes;
    connection.local_write_bytes -= bytes;
    local_write_bytes_ -= bytes;
    bytes_to_local_ +=
        static_cast<std::uint64_t>(bytes);
    // DATA event全体がSocketへ渡った時点でだけ受信creditを返す。
    if (pending.offset == pending.data.size()) {
        const std::uint32_t credit_stream_id =
            pending.credit_stream_id;
        const std::size_t consumed_size =
            pending.data.size();
        connection.local_writes.pop_front();
        if (credit_stream_id != 0U) {
            const auto consumed =
                static_cast<std::uint32_t>(
                    consumed_size);
            if (multiplexer_.ConsumeReceivedData(
                    credit_stream_id,
                    consumed) !=
                protocol::MultiplexerResult::Ok) {
                Fail(
                    ClientProxySessionError::
                        MultiplexerFailed);
                return false;
            }
        }
    }

    return true;
}

bool ClientProxySession::ConsumeLocalWrite(
    const std::size_t bytes) noexcept {
    return ConsumeLocalWrite(
        kLegacyConnectionId,
        bytes);
}

void ClientProxySession::MarkLocalSendShutdown() noexcept {
    MarkLocalSendShutdown(kLegacyConnectionId);
}

void ClientProxySession::MarkLocalSendShutdown(
    const LocalConnectionId connection_id) noexcept {
    const auto iterator =
        connections_.find(connection_id);
    // ownerが必要条件を確認してから一度だけ完了を記録する。
    if (iterator != connections_.end() &&
        ShouldShutdownLocalSend(connection_id)) {
        iterator->second.local_send_shutdown = true;
    }
}

bool ClientProxySession::ShouldCloseLocalConnection(
    const LocalConnectionId connection_id) const noexcept {
    const auto iterator =
        connections_.find(connection_id);
    return iterator != connections_.end() &&
        iterator->second.local_connection_active &&
        iterator->second.close_local_when_drained &&
        iterator->second.local_writes.empty();
}

bool ClientProxySession::ShouldCloseLocalConnection() const noexcept {
    return ShouldCloseLocalConnection(
        kLegacyConnectionId);
}

bool ClientProxySession::ShouldShutdownLocalSend(
    const LocalConnectionId connection_id) const noexcept {
    const auto iterator =
        connections_.find(connection_id);
    return iterator != connections_.end() &&
        iterator->second.local_connection_active &&
        iterator->second.remote_half_closed &&
        !iterator->second.local_send_shutdown &&
        iterator->second.local_writes.empty();
}

bool ClientProxySession::ShouldShutdownLocalSend() const noexcept {
    return ShouldShutdownLocalSend(
        kLegacyConnectionId);
}

bool ClientProxySession::TryGetConnectionSnapshot(
    const LocalConnectionId connection_id,
    ClientProxyConnectionSnapshot& snapshot) const noexcept {
    const auto iterator =
        connections_.find(connection_id);
    if (iterator == connections_.end()) {
        snapshot = ClientProxyConnectionSnapshot{};
        return false;
    }

    const ConnectionRecord& connection =
        iterator->second;
    snapshot = ClientProxyConnectionSnapshot{
        connection.connection_id,
        connection.state,
        connection.http_error,
        connection.stream_id,
        connection.upstream_pending_bytes,
        connection.local_write_bytes,
        connection.local_connection_active,
        ShouldShutdownLocalSend(connection_id),
        ShouldCloseLocalConnection(connection_id),
    };
    return true;
}

ClientProxySessionSnapshot
ClientProxySession::snapshot() const noexcept {
    std::uint32_t active_connections = 0U;
    bool any_local_active = false;
    bool any_shutdown = false;
    bool any_close = false;
    // UI snapshotはpayloadを参照せず有限connection counterだけを集計する。
    for (const auto& [connection_id, connection] :
         connections_) {
        if (connection.local_connection_active) {
            ++active_connections;
            any_local_active = true;
        }

        any_shutdown =
            any_shutdown ||
            ShouldShutdownLocalSend(connection_id);
        any_close =
            any_close ||
            ShouldCloseLocalConnection(connection_id);
    }

    ClientProxySessionState visible_state = state_;
    std::uint32_t visible_stream_id = 0U;
    // 旧単一connection test／UIでは一件だけの詳細stateを維持する。
    if (connections_.size() == 1U &&
        state_ == ClientProxySessionState::Ready) {
        visible_state =
            connections_.begin()->second.state;
        visible_stream_id =
            connections_.begin()->second.stream_id;
    } else if (!stream_routes_.empty()) {
        visible_stream_id =
            stream_routes_.begin()->first;
    }

    const protocol::MultiplexerSnapshot mux_snapshot =
        multiplexer_.snapshot();
    return ClientProxySessionSnapshot{
        visible_state,
        error_,
        http_error_,
        visible_stream_id,
        upstream_pending_bytes_,
        local_write_bytes_,
        bytes_from_local_,
        bytes_to_local_,
        any_local_active,
        any_shutdown,
        any_close,
        mux_snapshot.active_streams,
        static_cast<std::uint32_t>(
            connections_.size()),
        active_connections,
    };
}

bool ClientProxySession::QueueUpstreamChunks(
    ConnectionRecord& connection,
    std::vector<std::vector<std::byte>> chunks) noexcept {
    std::size_t additional_bytes = 0U;
    // queue変更前に全chunkの合計とoverflowを検証する。
    for (const std::vector<std::byte>& chunk : chunks) {
        if (chunk.empty() ||
            chunk.size() >
                kMaximumProxyUpstreamPendingBytes -
                    additional_bytes) {
            Fail(
                ClientProxySessionError::
                    QueueLimitExceeded);
            return false;
        }

        additional_bytes += chunk.size();
    }

    if (additional_bytes >
            kMaximumProxyUpstreamPendingBytes -
                connection.upstream_pending_bytes ||
        additional_bytes >
            kMaximumProxyTotalUpstreamPendingBytes -
                upstream_pending_bytes_) {
        Fail(
            ClientProxySessionError::
                QueueLimitExceeded);
        return false;
    }

    try {
        // parser出力の順序を維持して一件ずつ所有queueへ移す。
        for (std::vector<std::byte>& chunk : chunks) {
            PendingUpstreamWrite pending{};
            pending.data = std::move(chunk);
            connection.upstream_writes.push_back(
                std::move(pending));
        }
    } catch (const std::bad_alloc&) {
        Fail(
            ClientProxySessionError::
                ResourceExhausted);
        return false;
    } catch (...) {
        Fail(
            ClientProxySessionError::
                ResourceExhausted);
        return false;
    }

    connection.upstream_pending_bytes +=
        additional_bytes;
    upstream_pending_bytes_ += additional_bytes;
    return true;
}

bool ClientProxySession::DrainUpstreamData() noexcept {
    if (state_ == ClientProxySessionState::Failed ||
        state_ == ClientProxySessionState::Stopped) {
        return false;
    }

    std::size_t no_progress = 0U;
    // 一回の処理量を有限にし、connection IDをround-robinで公平に進める。
    for (std::size_t operation = 0U;
         operation < kMaximumProxyOperationsPerPump &&
         !connections_.empty() &&
         no_progress < connections_.size();
         ++operation) {
        auto iterator =
            connections_.upper_bound(
                last_drain_connection_id_);
        if (iterator == connections_.end()) {
            iterator = connections_.begin();
        }

        last_drain_connection_id_ =
            iterator->first;
        ConnectionRecord& connection =
            iterator->second;
        if (connection.state !=
                ClientProxySessionState::Open ||
            connection.upstream_writes.empty()) {
            ++no_progress;
            continue;
        }

        PendingUpstreamWrite& pending =
            connection.upstream_writes.front();
        // offsetとframe上限を使用直前に検証する。
        if (pending.offset >= pending.data.size() ||
            maximum_frame_payload_ == 0U ||
            connection.stream_id == 0U) {
            Fail(ClientProxySessionError::InvalidState);
            return false;
        }

        const std::size_t send_size = std::min(
            pending.data.size() - pending.offset,
            static_cast<std::size_t>(
                maximum_frame_payload_));
        const std::span<const std::byte> data(
            pending.data.data() + pending.offset,
            send_size);
        const protocol::MultiplexerResult result =
            multiplexer_.SendData(
                connection.stream_id,
                data);
        if (result ==
            protocol::MultiplexerResult::WouldBlock) {
            ++no_progress;
            continue;
        }

        if (result != protocol::MultiplexerResult::Ok) {
            Fail(
                ClientProxySessionError::
                    MultiplexerFailed);
            return false;
        }

        no_progress = 0U;
        pending.offset += send_size;
        if (send_size >
                connection.upstream_pending_bytes ||
            send_size > upstream_pending_bytes_) {
            Fail(ClientProxySessionError::InvalidState);
            return false;
        }

        connection.upstream_pending_bytes -=
            send_size;
        upstream_pending_bytes_ -= send_size;
        bytes_from_local_ +=
            static_cast<std::uint64_t>(send_size);
        // 一chunkを全送信した場合だけ所有bufferを破棄する。
        if (pending.offset == pending.data.size()) {
            connection.upstream_writes.pop_front();
        }
    }

    return true;
}

bool ClientProxySession::DrainEvents() noexcept {
    protocol::StreamEvent event{};
    // 一Frameが生成した全eventをwire順でlocal connectionへ適用する。
    while (multiplexer_.TakeEvent(event)) {
        switch (event.type) {
            case protocol::StreamEventType::Opened: {
                ConnectionRecord* const connection =
                    FindConnectionByStream(
                        event.stream_id);
                if (connection == nullptr ||
                    connection->state !=
                        ClientProxySessionState::Opening) {
                    Fail(
                        ClientProxySessionError::
                            UnexpectedEvent);
                    return false;
                }

                connection->state =
                    ClientProxySessionState::Open;
                break;
            }
            case protocol::StreamEventType::OpenFailed: {
                ConnectionRecord* const connection =
                    FindConnectionByStream(
                        event.stream_id);
                if (connection == nullptr ||
                    connection->state !=
                        ClientProxySessionState::Opening) {
                    Fail(
                        ClientProxySessionError::
                            UnexpectedEvent);
                    return false;
                }

                stream_routes_.erase(event.stream_id);
                connection->stream_id = 0U;
                if (connection->upstream_pending_bytes >
                    upstream_pending_bytes_) {
                    Fail(
                        ClientProxySessionError::
                            InvalidState);
                    return false;
                }

                upstream_pending_bytes_ -=
                    connection->
                        upstream_pending_bytes;
                connection->upstream_writes.clear();
                connection->upstream_pending_bytes = 0U;
                connection->state =
                    ClientProxySessionState::Closing;
                if (!QueueFixedHttpError(
                        *connection,
                        HttpStatusForOpenError(
                            event.open_error))) {
                    return false;
                }

                connection->
                    close_local_when_drained = true;
                break;
            }
            case protocol::StreamEventType::DataReceived: {
                ConnectionRecord* const connection =
                    FindConnectionByStream(
                        event.stream_id);
                if (connection == nullptr ||
                    (connection->state !=
                         ClientProxySessionState::Open &&
                     connection->state !=
                         ClientProxySessionState::Closing)) {
                    Fail(
                        ClientProxySessionError::
                            UnexpectedEvent);
                    return false;
                }

                if (!QueueLocalData(
                        *connection,
                        std::move(event))) {
                    return false;
                }
                break;
            }
            case protocol::StreamEventType::RemoteHalfClosed: {
                ConnectionRecord* const connection =
                    FindConnectionByStream(
                        event.stream_id);
                if (connection == nullptr ||
                    connection->remote_half_closed) {
                    Fail(
                        ClientProxySessionError::
                            UnexpectedEvent);
                    return false;
                }

                connection->remote_half_closed = true;
                break;
            }
            case protocol::StreamEventType::Closed: {
                ConnectionRecord* const connection =
                    FindConnectionByStream(
                        event.stream_id);
                if (connection == nullptr) {
                    Fail(
                        ClientProxySessionError::
                            UnexpectedEvent);
                    return false;
                }

                const LocalConnectionId connection_id =
                    connection->connection_id;
                stream_routes_.erase(event.stream_id);
                connection->stream_id = 0U;
                connection->stream_close_sent = false;
                if (connection->upstream_pending_bytes >
                    upstream_pending_bytes_) {
                    Fail(
                        ClientProxySessionError::
                            InvalidState);
                    return false;
                }

                upstream_pending_bytes_ -=
                    connection->
                        upstream_pending_bytes;
                connection->upstream_writes.clear();
                connection->upstream_pending_bytes = 0U;
                connection->local_half_close_sent = false;
                // multiplexerがstream creditを解放済みなので、受取済みresponseは
                // localへ排出してもWINDOW_UPDATEをterminal streamへ返さない。
                for (PendingLocalWrite& pending :
                     connection->local_writes) {
                    pending.credit_stream_id = 0U;
                }

                connection->state =
                    ClientProxySessionState::Closing;
                connection->close_local_when_drained =
                    connection->
                        local_connection_active;
                if (!connection->
                        local_connection_active) {
                    EraseConnection(connection_id);
                }
                break;
            }
            case protocol::StreamEventType::OpenRequested:
                // Client roleではAgent起点OPENを受け入れない。
                Fail(
                    ClientProxySessionError::
                        UnexpectedEvent);
                return false;
        }
    }

    return true;
}

bool ClientProxySession::QueueLocalData(
    ConnectionRecord& connection,
    protocol::StreamEvent event) noexcept {
    // DATAはactive streamの非空payloadだけをlocal Socketへrouteする。
    if (event.stream_id != connection.stream_id ||
        event.data.empty() ||
        event.data.size() >
            kMaximumProxyLocalWriteBytes -
                connection.local_write_bytes ||
        event.data.size() >
            kMaximumProxyTotalLocalWriteBytes -
                local_write_bytes_) {
        Fail(
            ClientProxySessionError::
                QueueLimitExceeded);
        return false;
    }

    try {
        PendingLocalWrite pending{};
        pending.data = std::move(event.data);
        pending.credit_stream_id = event.stream_id;
        connection.local_write_bytes +=
            pending.data.size();
        local_write_bytes_ += pending.data.size();
        connection.local_writes.push_back(
            std::move(pending));
    } catch (const std::bad_alloc&) {
        Fail(
            ClientProxySessionError::
                ResourceExhausted);
        return false;
    } catch (...) {
        Fail(
            ClientProxySessionError::
                ResourceExhausted);
        return false;
    }

    return true;
}

bool ClientProxySession::QueueFixedHttpError(
    ConnectionRecord& connection,
    const std::uint32_t status_code) noexcept {
    const std::string_view response =
        FixedHttpResponse(status_code);
    // 固定responseでもlocal queue上限を共通適用する。
    if (response.size() >
            kMaximumProxyLocalWriteBytes -
                connection.local_write_bytes ||
        response.size() >
            kMaximumProxyTotalLocalWriteBytes -
                local_write_bytes_) {
        Fail(
            ClientProxySessionError::
                QueueLimitExceeded);
        return false;
    }

    try {
        PendingLocalWrite pending{};
        pending.data.resize(response.size());
        // 固定ASCII responseを一対一のwire byteへcopyする。
        for (std::size_t index = 0U;
             index < response.size();
             ++index) {
            pending.data[index] =
                static_cast<std::byte>(
                    static_cast<unsigned char>(
                        response[index]));
        }

        connection.local_write_bytes +=
            pending.data.size();
        local_write_bytes_ += pending.data.size();
        connection.local_writes.push_back(
            std::move(pending));
    } catch (const std::bad_alloc&) {
        Fail(
            ClientProxySessionError::
                ResourceExhausted);
        return false;
    } catch (...) {
        Fail(
            ClientProxySessionError::
                ResourceExhausted);
        return false;
    }

    return true;
}

bool ClientProxySession::RejectHttpRequest(
    ConnectionRecord& connection,
    const HttpRequestError error) noexcept {
    http_error_ = error;
    connection.http_error = error;
    if (connection.upstream_pending_bytes >
        upstream_pending_bytes_) {
        Fail(ClientProxySessionError::InvalidState);
        return false;
    }

    upstream_pending_bytes_ -=
        connection.upstream_pending_bytes;
    connection.upstream_writes.clear();
    connection.upstream_pending_bytes = 0U;
    // 既存upstream streamへ不正な後続requestを送り続けない。
    if (connection.stream_id != 0U &&
        !connection.stream_close_sent) {
        const protocol::MultiplexerResult result =
            multiplexer_.CloseStream(
                connection.stream_id,
                protocol::StreamCloseReason::LocalError);
        if (result != protocol::MultiplexerResult::Ok) {
            Fail(
                ClientProxySessionError::
                    MultiplexerFailed);
            return false;
        }

        connection.stream_close_sent = true;
    }

    connection.state =
        ClientProxySessionState::Closing;
    if (!QueueFixedHttpError(
            connection,
            HttpStatusForParserError(error))) {
        return false;
    }

    connection.close_local_when_drained = true;
    return true;
}

bool ClientProxySession::ApplyPendingLocalHalfClose(
    ConnectionRecord& connection) noexcept {
    // FIN前の全request byteをAgentへqueueし、OPEN_OK済みの場合だけHALF_CLOSEする。
    if (!connection.local_receive_closed ||
        !connection.upstream_writes.empty() ||
        connection.stream_id == 0U ||
        connection.local_half_close_sent ||
        connection.stream_close_sent ||
        connection.state ==
            ClientProxySessionState::Opening) {
        return state_ != ClientProxySessionState::Failed;
    }

    if (connection.state !=
        ClientProxySessionState::Open) {
        return true;
    }

    const protocol::MultiplexerResult result =
        multiplexer_.HalfCloseStream(
            connection.stream_id);
    if (result != protocol::MultiplexerResult::Ok) {
        Fail(
            ClientProxySessionError::
                MultiplexerFailed);
        return false;
    }

    connection.local_half_close_sent = true;
    return true;
}

ClientProxySession::ConnectionRecord*
ClientProxySession::FindConnectionByStream(
    const std::uint32_t stream_id) noexcept {
    const auto route =
        stream_routes_.find(stream_id);
    if (route == stream_routes_.end()) {
        return nullptr;
    }

    const auto connection =
        connections_.find(route->second);
    if (connection == connections_.end() ||
        connection->second.stream_id != stream_id) {
        return nullptr;
    }

    return &connection->second;
}

void ClientProxySession::EraseConnection(
    const LocalConnectionId connection_id) noexcept {
    const auto iterator =
        connections_.find(connection_id);
    if (iterator == connections_.end() ||
        iterator->second.stream_id != 0U) {
        return;
    }

    // recordが持つ残queueをglobal counterから安全に除外する。
    upstream_pending_bytes_ =
        iterator->second.upstream_pending_bytes <=
                upstream_pending_bytes_
            ? upstream_pending_bytes_ -
                  iterator->second.
                      upstream_pending_bytes
            : 0U;
    local_write_bytes_ =
        iterator->second.local_write_bytes <=
                local_write_bytes_
            ? local_write_bytes_ -
                  iterator->second.local_write_bytes
            : 0U;
    connections_.erase(iterator);
}

void ClientProxySession::Fail(
    const ClientProxySessionError error) noexcept {
    multiplexer_.StopSession();
    ResetConnections();
    state_ = ClientProxySessionState::Failed;
    error_ = error;
}

void ClientProxySession::ResetConnections() noexcept {
    connections_.clear();
    stream_routes_.clear();
    upstream_pending_bytes_ = 0U;
    local_write_bytes_ = 0U;
    last_drain_connection_id_ = 0U;
    bytes_from_local_ = 0U;
    bytes_to_local_ = 0U;
    http_error_ = HttpRequestError::None;
}

}  // namespace zrdp::client::proxy
