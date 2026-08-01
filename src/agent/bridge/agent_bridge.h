/**
 * @file agent_bridge.h
 * @brief Agent側HELLO応答、PING Echo、正常CLOSE、bounded writer queueを定義する。
 *
 * @details WTS channel APIを直接呼ばず、受信fragmentと送信chunkの所有bufferだけを
 *          扱う。これによりAgent protocolをRDP sessionなしで検証できる。
 */
#pragma once

#include "protocol/frame.h"
#include "protocol/hello.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace zrdp::agent::bridge {

class AgentStreamRelay;

/**
 * @brief Agent Bridgeのconnection-level lifecycle。
 */
enum class AgentBridgeState {
    WaitingForHello,  ///< channel open後、Client HELLO待ち。
    Ready,            ///< compatible HELLOへACK済み。
    Failed,           ///< malformed frameまたはstate違反で停止。
    Incompatible,     ///< protocol versionまたはcapability不一致。
};

/**
 * @brief Agent console／診断へpayloadなしで公開できる失敗分類。
 */
enum class AgentBridgeError {
    None,
    DecodeFailed,
    InvalidHello,
    UnexpectedMessage,
    SequenceMismatch,
    QueueLimitExceeded,
    StreamRelayFailed,
};

/**
 * @brief Agent runtimeが状態遷移とcounterを確認する非機密snapshot。
 */
struct AgentBridgeSnapshot {
    /// 現在のprotocol state。
    AgentBridgeState state{AgentBridgeState::WaitingForHello};

    /// 最後の固定失敗分類。
    AgentBridgeError error{AgentBridgeError::None};

    /// 正しいPINGとして受理しPONGへqueueした累積byte数。
    std::uint64_t echoed_bytes{0U};

    /// 順序検証済みの最後のPING sequence。
    std::uint32_t last_ping_sequence{0U};
};

/**
 * @brief Agent側のconnection-control protocolとserialized writer queueを所有する。
 *
 * 全methodはWTS channelを所有する同一I/O threadから呼ぶ。writer queueを
 * 一つに集約し、WTSVirtualChannelWriteの呼出し順序をwire順へ固定する。
 */
class AgentBridge final {
public:
    /**
     * @brief optional Phase 3 relayを設定し、新しいHELLO待機状態へ戻す。
     * @param[in] relay 有効設定時の非owning pointer。Echo-onlyではnullptr。
     *
     * @note relayはこのAgentBridgeより長く生存し、同じI/O threadから呼ばれる。
     */
    void ConfigureStreamRelay(
        AgentStreamRelay* relay) noexcept;

    /**
     * @brief 新しいchannel sessionのdecoder、sequence、queueを初期化する。
     */
    void Reset() noexcept;

    /**
     * @brief Clientからの任意fragmentをdecodeしてHELLOまたはPINGを処理する。
     * @param[in] data WTS readから所有copyしたbinary fragment。
     * @return sessionを継続できる場合はtrue。
     */
    [[nodiscard]] bool OnChannelData(
        std::span<const std::byte> data) noexcept;

    /**
     * @brief 正常終了前のconnection-level CLOSEをwriter queueへ登録する。
     * @return 空payloadのCLOSE全体をqueueできた場合はtrue。
     *
     * ClientへAgent終了を通知し、RDPを維持したままHELLO待機へ戻せるようにする。
     */
    [[nodiscard]] bool QueueSessionClose() noexcept;

    /**
     * @brief optional non-blocking Socket relayを有限回進める。
     * @param[in] now_milliseconds `GetTickCount64`相当の単調時刻。
     * @return Bridge sessionを継続できる場合はtrue。
     */
    [[nodiscard]] bool PumpStreams(
        std::uint64_t now_milliseconds) noexcept;

    /**
     * @brief serialized writer queueの先頭SVC chunkを取り出す。
     * @param[out] chunk 成功時に最大1600 byteを所有するdata。
     * @param[out] more_data 取出し後にもqueueが残る場合はtrue。
     * @return 一件取得できた場合はtrue。
     */
    [[nodiscard]] bool TakeOutboundChunk(
        std::vector<std::byte>& chunk,
        bool& more_data) noexcept;

    /**
     * @brief runtimeとtestへ非機密state copyを返す。
     * @return 現在のAgent Bridge snapshot。
     */
    [[nodiscard]] AgentBridgeSnapshot snapshot() const noexcept;

private:
    /**
     * @brief HELLOを検証しHELLO_ACKをwriter queueへ登録する。
     * @param[in] frame decode済みHELLO。
     * @return compatibleでACK全体をqueueできた場合はtrue。
     */
    [[nodiscard]] bool HandleHello(
        const protocol::Frame& frame) noexcept;

    /**
     * @brief PINGの順序を検証し同じbinary payloadのPONGをqueueする。
     * @param[in] frame decode済みPING。
     * @return PONG全体をqueueできた場合はtrue。
     */
    [[nodiscard]] bool HandlePing(
        const protocol::Frame& frame) noexcept;

    /**
     * @brief Phase 2 stream frameをrelayへ渡して生成frameをwriter queueへ戻す。
     * @param[in] frame decode済み所有stream frame。
     * @return relayとwriter queueが継続可能ならtrue。
     */
    [[nodiscard]] bool HandleStreamFrame(
        protocol::Frame frame) noexcept;

    /**
     * @brief relay outbound frameを既存serialized transport queueへ移す。
     * @return 全Frameをqueueできた場合はtrue。
     */
    [[nodiscard]] bool DrainStreamFrames() noexcept;

    /**
     * @brief 一frameをencodeしてSVC transport chunkへ分割する。
     * @param[in] frame encode対象。
     * @return 件数・総byte上限内で全chunkをqueueできた場合はtrue。
     */
    [[nodiscard]] bool QueueFrame(
        const protocol::Frame& frame) noexcept;

    /**
     * @brief Agent Bridgeをfail-closed状態へ移し、残queueを破棄する。
     * @param[in] state `Failed`または`Incompatible`。
     * @param[in] error 固定失敗分類。
     */
    void Fail(
        AgentBridgeState state,
        AgentBridgeError error) noexcept;

    /// current channel sessionのincremental frame decoder。
    protocol::FrameDecoder decoder_;

    /// Agentが提示するPhase 1 capability。
    protocol::HelloCapabilities local_capabilities_;

    /// Client HELLOと交渉済みの有限上限。
    protocol::HelloCapabilities negotiated_capabilities_;

    /// I/O threadだけが変更するprotocol lifecycle。
    AgentBridgeState state_{AgentBridgeState::WaitingForHello};

    /// payloadを含まない最後の失敗分類。
    AgentBridgeError error_{AgentBridgeError::None};

    /// Client開始sequenceの単調増加を確認する直前値。
    std::uint32_t last_ping_sequence_{0U};

    /// 正常Echoとしてwriter queueへ登録した累積byte数。
    std::uint64_t echoed_bytes_{0U};

    /// 一つのWTS writer経路だけがdrainするtransport chunk列。
    std::deque<std::vector<std::byte>> outbound_chunks_;

    /// 件数と独立して検証するqueue memory合計。
    std::size_t outbound_queued_bytes_{0U};

    /// mainが所有し、設定有効時だけ接続するPhase 3 relay。
    AgentStreamRelay* stream_relay_{nullptr};
};

}  // namespace zrdp::agent::bridge
