/**
 * @file upstream_connection.h
 * @brief Agentのallowlist検証後に使用するnon-blocking outbound TCP境界を定義する。
 *
 * @details production実装はDNS、bind、listen、accept、OS Proxyを使用しない。
 *          testでは同じinterfaceをfake接続へ差し替え、policy拒否時に接続APIが
 *          呼ばれないことを検証できる。
 */
#pragma once

#include "protocol/multiplexer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace zrdp::agent::network {

/**
 * @brief non-blocking Socket一操作の固定結果。
 */
enum class UpstreamIoStatus {
    Completed,   ///< connect／shutdown完了、または正のbyte数を処理した。
    Pending,     ///< connect完了を後続Pumpで確認する。
    WouldBlock,  ///< send／receiveが現在は進まない。
    PeerClosed,  ///< receiveで順序正しいFINを受信した。
    PeerReset,   ///< peer reset／abortを受信した。
    TimedOut,    ///< OSがtimeoutを明示した。
    Failed,      ///< その他のWinsockまたはlocal resource失敗。
};

/**
 * @brief endpoint文字列を含まないSocket操作結果。
 */
struct UpstreamIoResult {
    /// operationの状態。
    UpstreamIoStatus status{UpstreamIoStatus::Failed};

    /// send／receiveが今回処理したbyte数。
    std::size_t bytes{0U};

    /// `Failed`または`PeerReset`時のWinsock error code。
    std::uint32_t winsock_error{0U};
};

/**
 * @brief 一つのoutbound TCP Socketを所有する抽象接続。
 *
 * @note 全methodはAgentの同一I/O owner threadから呼ぶ。
 */
class IUpstreamConnection {
public:
    virtual ~IUpstreamConnection() = default;

    /**
     * @brief pending connectの完了または失敗をpollする。
     * @return `Completed`、`Pending`、`PeerReset`、`Failed`のいずれか。
     */
    [[nodiscard]] virtual UpstreamIoResult PollConnect() noexcept = 0;

    /**
     * @brief caller所有bufferの未送信部分をnon-blockingで送る。
     * @param[in] data 空でないbinary data。
     * @return 正のbyte数を持つ`Completed`、`WouldBlock`、または失敗分類。
     */
    [[nodiscard]] virtual UpstreamIoResult Send(
        std::span<const std::byte> data) noexcept = 0;

    /**
     * @brief caller所有bufferへnon-blockingで受信する。
     * @param[out] buffer 空でない書込み先。
     * @return 正のbyte数を持つ`Completed`、`WouldBlock`、FIN、または失敗分類。
     */
    [[nodiscard]] virtual UpstreamIoResult Receive(
        std::span<std::byte> buffer) noexcept = 0;

    /**
     * @brief Socketの送信方向だけをhalf-closeする。
     * @return 完了または分類済み失敗。
     */
    [[nodiscard]] virtual UpstreamIoResult ShutdownSend() noexcept = 0;

    /**
     * @brief 所有Socketをidempotentに閉じる。
     */
    virtual void Close() noexcept = 0;
};

/**
 * @brief allowlist一致後の接続開始だけを公開するfactory境界。
 */
class IUpstreamConnector {
public:
    virtual ~IUpstreamConnector() = default;

    /**
     * @brief IPv4 literalへnon-blocking connectを開始する。
     * @param[in] target allowlist完全一致済みの接続先。
     * @param[out] connection 成功またはpending時の所有接続。
     * @return `Completed`、`Pending`、`PeerReset`、`Failed`のいずれか。
     *
     * @warning このmethodをallowlist判定前に呼び出してはならない。
     */
    [[nodiscard]] virtual UpstreamIoResult BeginConnect(
        const protocol::OpenTarget& target,
        std::unique_ptr<IUpstreamConnection>& connection) noexcept = 0;
};

/**
 * @brief process内Winsock初期化をRAIIで管理する。
 */
class WinsockRuntime final {
public:
    WinsockRuntime() noexcept = default;
    ~WinsockRuntime();

    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;
    WinsockRuntime(WinsockRuntime&&) = delete;
    WinsockRuntime& operator=(WinsockRuntime&&) = delete;

    /**
     * @brief Winsock 2.2を一度だけ初期化する。
     * @return 成功時は0、失敗時はWinsock error code。
     */
    [[nodiscard]] std::uint32_t Start() noexcept;

    /**
     * @brief 対応する`WSACleanup`を一度だけ実行する。
     */
    void Stop() noexcept;

    /**
     * @brief connectorを使用可能な初期化状態か返す。
     * @return `Start`成功後ならtrue。
     */
    [[nodiscard]] bool started() const noexcept;

private:
    /// `WSAStartup`成功後だけtrueとなるcleanup ownership。
    bool started_{false};
};

/**
 * @brief WinsockによるIPv4 literal専用outbound connector。
 *
 * @pre 同じprocessで`WinsockRuntime::Start`が成功していること。
 */
class WinsockUpstreamConnector final : public IUpstreamConnector {
public:
    [[nodiscard]] UpstreamIoResult BeginConnect(
        const protocol::OpenTarget& target,
        std::unique_ptr<IUpstreamConnection>& connection) noexcept override;
};

}  // namespace zrdp::agent::network
