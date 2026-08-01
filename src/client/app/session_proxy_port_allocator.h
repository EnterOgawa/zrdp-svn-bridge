/**
 * @file session_proxy_port_allocator.h
 * @brief 複数RDP sessionへ重複しないloopback Proxy portを割り当てる。
 *
 * @details network APIは呼ばず、ClientApplication内で所有する有限slotだけを
 *          管理する。実bindの排他性は従来どおりClientProxyWorkerが保証する。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace zrdp::client::app {

/// 一つのClient processで同時所有できるRDP／Bridge session上限。
inline constexpr std::size_t kMaximumConcurrentRdpSessions = 16U;

/**
 * @brief 一sessionへ割り当てたbase相対slotと実port。
 */
struct SessionProxyPortLease {
    std::size_t slot{0U};     ///< 解放時に使用する0起点slot。
    std::uint16_t port{0U};   ///< `base + slot`の1..65535。
};

/**
 * @brief base portから未使用の最小session slotを有限割当する。
 */
class SessionProxyPortAllocator final {
public:
    /**
     * @brief 一件目へ使用するbase portを固定する。
     * @param[in] base_port `client.ini`由来または既定18080。
     */
    explicit SessionProxyPortAllocator(
        std::uint16_t base_port) noexcept;

    /**
     * @brief 未使用の最小slotを取得する。
     * @return 割当成功時のlease。上限またはport overflow時はnullopt。
     */
    [[nodiscard]] std::optional<SessionProxyPortLease>
    Acquire() noexcept;

    /**
     * @brief 生存終了したsessionのslotを再利用可能にする。
     * @param[in] slot Acquireが返したslot。
     * @return 使用中slotを一度解放できた場合はtrue。
     */
    [[nodiscard]] bool Release(std::size_t slot) noexcept;

    /**
     * @brief 次の接続へ割当予定のportを変更せず取得する。
     * @return 空きがある場合のport。上限時はnullopt。
     */
    [[nodiscard]] std::optional<std::uint16_t>
    NextAvailablePort() const noexcept;

    /**
     * @brief 現在使用中のsession slot数を返す。
     * @return `used_`内でtrueのslot数。
     */
    [[nodiscard]] std::size_t active_count() const noexcept;

private:
    /**
     * @brief 指定slotをbaseへ加算可能か検証してportへ変換する。
     * @param[in] slot 0起点の有限session slot。
     * @return 1..65535へ収まる場合のport、それ以外はnullopt。
     */
    [[nodiscard]] std::optional<std::uint16_t>
    ResolvePort(std::size_t slot) const noexcept;

    /// 一件目のsessionへ割り当てる検証済みport。
    std::uint16_t base_port_{0U};

    /// slot indexごとの使用状態。allocator owner threadだけが変更する。
    std::array<bool, kMaximumConcurrentRdpSessions> used_{};
};

}  // namespace zrdp::client::app
