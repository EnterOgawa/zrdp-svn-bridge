/**
 * @file client_proxy_session.h
 * @brief 複数local HTTP connectionとClient multiplexerの中継状態を定義する。
 *
 * @details Socket APIとCOM APIを呼ばず、local byte列とBridge Frameの所有queueだけを
 *          扱う。production workerとin-memory integration testで同じ状態機械を使う。
 */
#pragma once

#include "client/proxy/http_request_parser.h"
#include "protocol/multiplexer.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <vector>

namespace zrdp::client::proxy {

/// local clientからAgentへ未送信のまま保持できる最大byte数。
inline constexpr std::size_t kMaximumProxyUpstreamPendingBytes =
    256U * 1024U;

/// Agentからlocal clientへ未送信のまま保持できる最大byte数。
inline constexpr std::size_t kMaximumProxyLocalWriteBytes =
    256U * 1024U;

/// production Clientが同時所有できるlocal connection数。
inline constexpr std::uint32_t kMaximumClientProxyConnections = 8U;

/// 全connectionのupstream送信待ちbyte合計上限。
inline constexpr std::size_t kMaximumProxyTotalUpstreamPendingBytes =
    kMaximumProxyUpstreamPendingBytes *
    kMaximumClientProxyConnections;

/// 全connectionのlocal送信待ちbyte合計上限。
inline constexpr std::size_t kMaximumProxyTotalLocalWriteBytes =
    kMaximumProxyLocalWriteBytes *
    kMaximumClientProxyConnections;

/// 一回の状態更新でmultiplexerへ送る最大DATA operation数。
inline constexpr std::size_t kMaximumProxyOperationsPerPump = 32U;

/// workerが一local Socketへ割り当てる非0識別子。
using LocalConnectionId = std::uint64_t;

/**
 * @brief local connection受付結果。
 */
enum class LocalConnectionResult {
    Accepted,          ///< Bridge readyかつactive connectionなし。
    BridgeUnavailable, ///< HELLO未成立またはsession停止中。
    Busy,              ///< 既存local connectionまたはstreamを処理中。
    Failed,            ///< relay自体がfatal状態。
};

/**
 * @brief Client Proxy relay全体のlifecycle。
 */
enum class ClientProxySessionState {
    Stopped,  ///< compatible Bridge sessionなし。
    Ready,    ///< local connectionを受け付けられる。
    Opening,  ///< HTTP targetを検証しOPEN応答待ち。
    Open,     ///< 双方向DATAを中継中。
    Closing,  ///< CLOSE ackまたはlocal error response排出待ち。
    Failed,   ///< internal／protocol不変条件違反。
};

/**
 * @brief payloadやendpointを含まないrelay失敗分類。
 */
enum class ClientProxySessionError : std::uint32_t {
    None = 0U,
    InvalidCapabilities = 1U,
    InvalidState = 2U,
    HttpRejected = 3U,
    MultiplexerFailed = 4U,
    QueueLimitExceeded = 5U,
    UnexpectedEvent = 6U,
    ResourceExhausted = 7U,
};

/**
 * @brief worker／UI／testへ公開する非機密counter。
 */
struct ClientProxySessionSnapshot {
    ClientProxySessionState state{
        ClientProxySessionState::Stopped};  ///< relay全体のlifecycle。
    ClientProxySessionError error{
        ClientProxySessionError::None};  ///< 最後のfatal relay分類。
    HttpRequestError http_error{HttpRequestError::None};  ///< 最後のHTTP拒否分類。
    std::uint32_t active_stream_id{0U};  ///< legacy API向けactive stream ID。
    std::size_t upstream_pending_bytes{0U};  ///< Agent送信待ちbyte合計。
    std::size_t local_write_bytes{0U};  ///< local Socket送信待ちbyte合計。
    std::uint64_t bytes_from_local{0U};  ///< localから受理した累積byte数。
    std::uint64_t bytes_to_local{0U};  ///< localへ送信済みの累積byte数。
    bool local_connection_active{false};  ///< legacy connectionがSocketを所有中。
    bool local_send_shutdown_required{false};  ///< legacy SocketへFINが必要。
    bool local_close_required{false};  ///< legacy Socketのcloseが必要。

    /// 所有中のmultiplexer stream数。
    std::uint32_t active_streams{0U};

    /// Socket activeまたはCLOSE待ちのconnection record数。
    std::uint32_t connection_records{0U};

    /// 現在workerが所有するlocal Socket数。
    std::uint32_t active_connections{0U};
};

/**
 * @brief 一local connectionの非機密状態。
 */
struct ClientProxyConnectionSnapshot {
    LocalConnectionId connection_id{0U};  ///< workerが割り当てた非0 ID。
    ClientProxySessionState state{
        ClientProxySessionState::Ready};  ///< connection単位のrelay state。
    HttpRequestError http_error{HttpRequestError::None};  ///< HTTP拒否分類。
    std::uint32_t stream_id{0U};  ///< 対応するmultiplexer stream ID。
    std::size_t upstream_pending_bytes{0U};  ///< Agent送信待ちbyte数。
    std::size_t local_write_bytes{0U};  ///< local Socket送信待ちbyte数。
    bool local_connection_active{false};  ///< workerがSocketを所有中。
    bool local_send_shutdown_required{false};  ///< SocketへFINが必要。
    bool local_close_required{false};  ///< Socketのcloseが必要。
};

/**
 * @brief 複数HTTP変換、Client multiplexer、Socket向けbounded queueを一所有者で扱う。
 *
 * @note 全public methodはProxy workerまたはtestの一つのowner threadから呼ぶ。
 */
class ClientProxySession final {
public:
    /**
     * @brief Client roleの空multiplexerを作成する。
     */
    ClientProxySession() noexcept;

    /**
     * @brief negotiated capabilityで新しいBridge sessionを開始する。
     * @param[in] capabilities HELLOで確定したPhase 2上限。
     * @return stream機能を利用可能にできた場合はtrue。
     *
     * 旧local connection、stream ID、queue、parserは破棄する。
     */
    [[nodiscard]] bool StartBridgeSession(
        const protocol::HelloCapabilities& capabilities) noexcept;

    /**
     * @brief RDP切断／Agent CLOSE相当で全stateとqueueを破棄する。
     */
    void StopBridgeSession() noexcept;

    /**
     * @brief 一つのlocal HTTP connectionをrelayへ関連付ける。
     * @param[in] connection_id workerが割り当てた非0かつ一意ID。
     * @return Bridge状態とnegotiated connection上限に基づく受付結果。
     */
    [[nodiscard]] LocalConnectionResult BeginLocalConnection(
        LocalConnectionId connection_id) noexcept;

    /**
     * @brief 旧単一connection test向けに固定ID 1を開始する。
     * @return `BeginLocalConnection(1)`の結果。
     */
    [[nodiscard]] LocalConnectionResult BeginLocalConnection() noexcept;

    /**
     * @brief local Socketから受信したHTTP request fragmentを処理する。
     * @param[in] connection_id 受付済みlocal connection。
     * @param[in] data 正のbyte数を持つ所有元buffer。
     * @return connectionまたは固定HTTP error応答を継続できる場合はtrue。
     */
    [[nodiscard]] bool OnLocalData(
        LocalConnectionId connection_id,
        std::span<const std::byte> data) noexcept;

    /**
     * @brief 旧単一connection test向けに固定ID 1へdataを渡す。
     * @param[in] data HTTP request fragment。
     * @return `OnLocalData(1, data)`の結果。
     */
    [[nodiscard]] bool OnLocalData(
        std::span<const std::byte> data) noexcept;

    /**
     * @brief local peerのorderly send FINをrequest streamへ反映する。
     * @param[in] connection_id 受付済みlocal connection。
     * @return Bridge sessionを継続できる場合はtrue。
     */
    [[nodiscard]] bool OnLocalReceiveClosed(
        LocalConnectionId connection_id) noexcept;

    /**
     * @brief 旧単一connection test向けに固定ID 1へFINを適用する。
     * @return `OnLocalReceiveClosed(1)`の結果。
     */
    [[nodiscard]] bool OnLocalReceiveClosed() noexcept;

    /**
     * @brief local Socketのreset／I/O失敗またはowner closeを反映する。
     * @param[in] connection_id 終了したlocal connection。
     * @param[in] reason peerへ通知する固定close reason。
     */
    void OnLocalConnectionClosed(
        LocalConnectionId connection_id,
        protocol::StreamCloseReason reason) noexcept;

    /**
     * @brief 旧単一connection test向けに固定ID 1を終了する。
     * @param[in] reason peerへ通知する固定close reason。
     */
    void OnLocalConnectionClosed(
        protocol::StreamCloseReason reason) noexcept;

    /**
     * @brief Agentから受信した一Frameをmultiplexerへ適用する。
     * @param[in] frame UI queueから移動した所有Frame。
     * @return session継続時はtrue。
     */
    [[nodiscard]] bool OnBridgeFrame(
        protocol::Frame frame) noexcept;

    /**
     * @brief UI／SVC queueへ渡す先頭Frameを取り出す。
     * @param[out] frame 所有Frame。
     * @return 一件取得できた場合はtrue。
     */
    [[nodiscard]] bool TakeOutboundFrame(
        protocol::Frame& frame) noexcept;

    /**
     * @brief local Socketへ送る未送信先頭範囲を参照する。
     * @param[in] connection_id 対象local connection。
     * @param[out] data 次sendへ渡す範囲。次の変更methodまで有効。
     * @return dataが存在する場合はtrue。
     */
    [[nodiscard]] bool PeekLocalWrite(
        LocalConnectionId connection_id,
        std::span<const std::byte>& data) const noexcept;

    /**
     * @brief 旧単一connection test向けに固定ID 1の先頭dataを返す。
     * @param[out] data 次send範囲。
     * @return `PeekLocalWrite(1, data)`の結果。
     */
    [[nodiscard]] bool PeekLocalWrite(
        std::span<const std::byte>& data) const noexcept;

    /**
     * @brief local Socketが実際に送信したbyte数をqueueとflow controlへ反映する。
     * @param[in] connection_id 対象local connection。
     * @param[in] bytes 直前のPeek範囲以下の正のbyte数。
     * @return queueとmultiplexerを継続できる場合はtrue。
     */
    [[nodiscard]] bool ConsumeLocalWrite(
        LocalConnectionId connection_id,
        std::size_t bytes) noexcept;

    /**
     * @brief 旧単一connection test向けに固定ID 1の送信を消費する。
     * @param[in] bytes 送信済みbyte数。
     * @return `ConsumeLocalWrite(1, bytes)`の結果。
     */
    [[nodiscard]] bool ConsumeLocalWrite(
        std::size_t bytes) noexcept;

    /**
     * @brief remote HALF_CLOSE後のlocal Socket送信FIN完了を記録する。
     * @param[in] connection_id 対象local connection。
     */
    void MarkLocalSendShutdown(
        LocalConnectionId connection_id) noexcept;

    /**
     * @brief 旧単一connection test向けに固定ID 1へ適用する。
     */
    void MarkLocalSendShutdown() noexcept;

    /**
     * @brief ownerがlocal Socketを閉じるべきか返す。
     * @param[in] connection_id 対象local connection。
     * @return error response排出済みまたはstream terminalならtrue。
     */
    [[nodiscard]] bool ShouldCloseLocalConnection(
        LocalConnectionId connection_id) const noexcept;

    /**
     * @brief 旧単一connection test向けに固定ID 1を確認する。
     * @return `ShouldCloseLocalConnection(1)`の結果。
     */
    [[nodiscard]] bool ShouldCloseLocalConnection() const noexcept;

    /**
     * @brief ownerがlocal Socketへ送信FINを発行すべきか返す。
     * @param[in] connection_id 対象local connection。
     * @return remote HALF_CLOSEかつ全response送信済みならtrue。
     */
    [[nodiscard]] bool ShouldShutdownLocalSend(
        LocalConnectionId connection_id) const noexcept;

    /**
     * @brief 旧単一connection test向けに固定ID 1を確認する。
     * @return `ShouldShutdownLocalSend(1)`の結果。
     */
    [[nodiscard]] bool ShouldShutdownLocalSend() const noexcept;

    /**
     * @brief 指定connectionの非機密snapshotを返す。
     * @param[in] connection_id 検索ID。
     * @param[out] snapshot 存在時の状態。
     * @return connection recordが存在する場合はtrue。
     */
    [[nodiscard]] bool TryGetConnectionSnapshot(
        LocalConnectionId connection_id,
        ClientProxyConnectionSnapshot& snapshot) const noexcept;

    /**
     * @brief 非機密状態とcounterを返す。
     * @return 現在snapshot。
     */
    [[nodiscard]] ClientProxySessionSnapshot snapshot() const noexcept;

private:
    /**
     * @brief upstreamへ未送信の一所有chunk。
     */
    struct PendingUpstreamWrite {
        std::vector<std::byte> data;  ///< HTTP変換済みの所有byte列。
        std::size_t offset{0U};  ///< multiplexerへ送信済みの先頭offset。
    };

    /**
     * @brief local Socketへ未送信の一所有chunk。
     */
    struct PendingLocalWrite {
        std::vector<std::byte> data;  ///< local Socketへ送る所有byte列。
        std::size_t offset{0U};  ///< Socketへ送信済みの先頭offset。
        std::uint32_t credit_stream_id{0U};  ///< 消費時にcreditを返すstream。
    };

    /**
     * @brief 一local Socketと一Bridge streamの所有状態。
     */
    struct ConnectionRecord {
        LocalConnectionId connection_id{0U};  ///< worker割当のtable key。
        HttpRequestTransformer parser;  ///< 一authorityへ固定するHTTP parser。
        ClientProxySessionState state{
            ClientProxySessionState::Ready};  ///< connection relay state。
        HttpRequestError http_error{
            HttpRequestError::None};  ///< connection固有のHTTP拒否分類。
        std::uint32_t stream_id{0U};  ///< OPEN後に割り当てたroute key。
        std::deque<PendingUpstreamWrite>
            upstream_writes;  ///< Agent送信待ちのbounded queue。
        std::size_t upstream_pending_bytes{0U};  ///< 上記queueのbyte合計。
        std::deque<PendingLocalWrite> local_writes;  ///< Socket送信待ちqueue。
        std::size_t local_write_bytes{0U};  ///< 上記queueのbyte合計。
        bool local_connection_active{false};  ///< workerがSocketを所有中。
        bool local_receive_closed{false};  ///< local peerのsend FIN受理済み。
        bool local_half_close_sent{false};  ///< HALF_CLOSEをpeerへ送信済み。
        bool remote_half_closed{false};  ///< peerのHALF_CLOSE受理済み。
        bool local_send_shutdown{false};  ///< local SocketへFIN発行済み。
        bool close_local_when_drained{false};  ///< write完了後にcloseするflag。
        bool stream_close_sent{false};  ///< CLOSE request送信済み。
    };

    /**
     * @brief local parser出力をbounded upstream queueへ登録する。
     * @param[in,out] connection 対象connection。
     * @param[in] chunks header／bodyの所有chunk列。
     * @return 全件をatomicに登録できた場合はtrue。
     */
    [[nodiscard]] bool QueueUpstreamChunks(
        ConnectionRecord& connection,
        std::vector<std::vector<std::byte>> chunks) noexcept;

    /**
     * @brief 全OPEN済みstreamのpending HTTP byteを公平に送る。
     * @return fatal以外はtrue。WouldBlockはpendingを維持する。
     */
    [[nodiscard]] bool DrainUpstreamData() noexcept;

    /**
     * @brief multiplexer eventを対応local connectionへwire順に適用する。
     * @return 全eventを受理できた場合はtrue。
     */
    [[nodiscard]] bool DrainEvents() noexcept;

    /**
     * @brief Agent DATAをlocal Socket write queueへ登録する。
     * @param[in,out] connection route済みconnection。
     * @param[in] event 所有payloadを持つDataReceived event。
     * @return queue上限内で登録できた場合はtrue。
     */
    [[nodiscard]] bool QueueLocalData(
        ConnectionRecord& connection,
        protocol::StreamEvent event) noexcept;

    /**
     * @brief parser／OPEN拒否に対応する固定HTTP responseをqueueする。
     * @param[in,out] connection 対象connection。
     * @param[in] status_code HTTP status code。
     * @return bounded queueへ登録できた場合はtrue。
     */
    [[nodiscard]] bool QueueFixedHttpError(
        ConnectionRecord& connection,
        std::uint32_t status_code) noexcept;

    /**
     * @brief HTTP parser errorを対象stream CLOSEと固定responseへ変換する。
     * @param[in,out] connection 拒否対象connection。
     * @param[in] error parserの固定分類。
     * @return responseをqueueできた場合はtrue。
     */
    [[nodiscard]] bool RejectHttpRequest(
        ConnectionRecord& connection,
        HttpRequestError error) noexcept;

    /**
     * @brief local FIN待ち条件を満たした時だけHALF_CLOSEを生成する。
     * @param[in,out] connection 対象connection。
     * @return fatal以外はtrue。
     */
    [[nodiscard]] bool ApplyPendingLocalHalfClose(
        ConnectionRecord& connection) noexcept;

    /**
     * @brief stream IDから所有connectionを検索する。
     * @param[in] stream_id 非0 stream ID。
     * @return 存在時のnon-owning pointer。
     */
    [[nodiscard]] ConnectionRecord* FindConnectionByStream(
        std::uint32_t stream_id) noexcept;

    /**
     * @brief streamのない終了connection recordをtableから破棄する。
     * @param[in] connection_id 対象ID。
     */
    void EraseConnection(
        LocalConnectionId connection_id) noexcept;

    /**
     * @brief relayをfatal状態へ移し全local bufferを破棄する。
     * @param[in] error payloadを含まない分類。
     */
    void Fail(ClientProxySessionError error) noexcept;

    /**
     * @brief 全connection、route、queue、counterを初期化する。
     */
    void ResetConnections() noexcept;

    /// Client-only OPEN規則を持つtransport非依存multiplexer。
    protocol::StreamMultiplexer multiplexer_;

    /// HELLOで確定した一Frame payload上限。
    std::uint32_t maximum_frame_payload_{0U};

    /// negotiated production connection上限。
    std::uint32_t maximum_connections_{0U};

    /// 現relay全体のlifecycle。
    ClientProxySessionState state_{
        ClientProxySessionState::Stopped};

    /// 最後のfatal relay分類。
    ClientProxySessionError error_{
        ClientProxySessionError::None};

    /// 最後にlocal connectionを終了対象にしたHTTP分類。
    HttpRequestError http_error_{HttpRequestError::None};

    /// local connection IDごとの所有record。
    std::map<LocalConnectionId, ConnectionRecord>
        connections_;

    /// stream IDからlocal connection IDへの一意route。
    std::map<std::uint32_t, LocalConnectionId>
        stream_routes_;

    /// 全connectionのupstream送信待ちbyte合計。
    std::size_t upstream_pending_bytes_{0U};

    /// 全connectionのlocal送信待ちbyte合計。
    std::size_t local_write_bytes_{0U};

    /// round-robin request送信の直前connection ID。
    LocalConnectionId last_drain_connection_id_{0U};

    /// UIへ公開する方向別byte counter。
    std::uint64_t bytes_from_local_{0U};

    /// Agentからlocal Socketへ実送信した累積byte counter。
    std::uint64_t bytes_to_local_{0U};

    /// 旧単一connection APIが使用する固定ID。
    static constexpr LocalConnectionId
        kLegacyConnectionId = 1U;
};

}  // namespace zrdp::client::proxy
