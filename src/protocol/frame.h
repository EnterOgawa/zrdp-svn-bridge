/**
 * @file frame.h
 * @brief ESVNBRG Application Protocolの固定frame契約を定義する。
 *
 * @details wire dataをC++構造体へcastせず、network byte orderで個別に
 *          encode／decodeする。decoderはfragmentationと複数frameを扱い、
 *          payload長と保持bufferを有限値へ制限する。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace zrdp::protocol {

/// RDP ActiveXとAgentが共有する7文字のStatic Virtual Channel名。
inline constexpr char kStaticVirtualChannelName[] = "ESVNBRG";

/// ActiveXのBSTR propertyへ渡す同一Static Virtual Channel名。
inline constexpr wchar_t kStaticVirtualChannelNameWide[] = L"ESVNBRG";

/// Microsoft SVCの一回のtransport chunk上限に合わせるbyte数。
inline constexpr std::size_t kStaticVirtualChannelChunkBytes = 1600U;

/// 現在実装するApplication Protocol version。
inline constexpr std::uint16_t kProtocolVersion = 1U;

/// 固定wire headerのbyte数。
inline constexpr std::size_t kFrameHeaderBytes = 24U;

/// 一つのApplication frameが保持できるpayload上限。
inline constexpr std::uint32_t kMaximumFramePayloadBytes = 64U * 1024U;

/// 一回のdecoder入力が占有できる有限上限。
inline constexpr std::size_t kMaximumDecoderInputBytes = 1024U * 1024U;

/**
 * @brief wire headerで識別するApplication Protocol message種別。
 *
 * Phase 1はHELLO系、PING系、Agent正常終了のconnection-level CLOSEを処理し、
 * Phase 2は予約済みstream messageを有効化する。既存type番号は変更しない。
 */
enum class MessageType : std::uint16_t {
    Hello = 1U,
    HelloAck = 2U,
    Open = 3U,
    OpenOk = 4U,
    OpenError = 5U,
    Data = 6U,
    WindowUpdate = 7U,
    HalfClose = 8U,
    Close = 9U,
    Ping = 10U,
    Pong = 11U,
    ProtocolError = 12U,
};

/**
 * @brief frame encode／decodeがfail-closedで返す分類済み結果。
 */
enum class FrameError {
    None,
    InvalidMagic,
    UnsupportedVersion,
    InvalidHeaderSize,
    UnknownMessageType,
    UnsupportedFlags,
    InvalidStreamId,
    OversizedPayload,
    InputLimitExceeded,
    ResourceExhausted,
};

/**
 * @brief wire headerとpayloadの所有表現。
 */
struct Frame {
    /// messageの意味を決める固定type。
    MessageType type{MessageType::Hello};

    /// connection controlでは0、stream messageでは非0。CLOSEとWINDOW_UPDATEは両方を許容する。
    std::uint32_t stream_id{0U};

    /// peer間で順序と応答対応を確認する単調増加値。
    std::uint32_t sequence{0U};

    /// 埋込みNULを許容する所有binary payload。
    std::vector<std::byte> payload;
};

/**
 * @brief 指定message typeがconnection-level controlか判定する。
 * @param[in] type 判定対象のwire message type。
 * @return `streamId = 0`を必須とするtypeならtrue。
 *
 * CLOSEはconnection-levelとstream-levelの両方を持つため、この判定には含めない。
 */
[[nodiscard]] constexpr bool IsConnectionControlMessage(
    const MessageType type) noexcept {
    return type == MessageType::Hello ||
        type == MessageType::HelloAck ||
        type == MessageType::Ping ||
        type == MessageType::Pong ||
        type == MessageType::ProtocolError;
}

/**
 * @brief 一つの所有Frameを固定wire formatへencodeする。
 * @param[in] frame 検証してencodeするframe。
 * @param[out] encoded 成功時にheaderとpayloadを所有するbyte列。
 * @return 成功時は`FrameError::None`、不正値または資源不足時は分類結果。
 *
 * 失敗時は`encoded`を空にし、部分frameを公開しない。
 */
[[nodiscard]] FrameError EncodeFrame(
    const Frame& frame,
    std::vector<std::byte>& encoded) noexcept;

/**
 * @brief 任意の受信fragmentから完全なframeを再構成するincremental decoder。
 *
 * 一度errorへ遷移すると`Reset`まで追加dataを受け付けない。これにより
 * malformed frame後のbyte列を新しい境界として暗黙に再同期しない。
 */
class FrameDecoder final {
public:
    /**
     * @brief 一つのtransport fragmentを追加して完全frameを取り出す。
     * @param[in] fragment 任意のframe境界で分割された受信byte列。
     * @param[out] decoded 今回完成したframe群。呼出し開始時にclearする。
     * @return すべて有効なら`FrameError::None`、最初の異常分類。
     */
    [[nodiscard]] FrameError Push(
        std::span<const std::byte> fragment,
        std::vector<Frame>& decoded) noexcept;

    /**
     * @brief 保持fragmentとerror状態を破棄して新しいBridge sessionへ戻す。
     */
    void Reset() noexcept;

    /**
     * @brief decoderがfail-closed状態か取得する。
     * @return `Reset`を必要とするerror後ならtrue。
     */
    [[nodiscard]] bool failed() const noexcept;

private:
    /// 未完成frameを先頭に持つ有限受信buffer。
    std::vector<std::byte> buffer_;

    /// malformed input後の暗黙再同期を禁止するflag。
    bool failed_{false};
};

}  // namespace zrdp::protocol
