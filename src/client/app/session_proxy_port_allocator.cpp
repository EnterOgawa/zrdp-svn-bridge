/**
 * @file session_proxy_port_allocator.cpp
 * @brief session別loopback Proxy portの有限slot割当を実装する。
 */

#include "client/app/session_proxy_port_allocator.h"

#include <limits>

namespace zrdp::client::app {

SessionProxyPortAllocator::SessionProxyPortAllocator(
    const std::uint16_t base_port) noexcept
    : base_port_(base_port) {
}

std::optional<SessionProxyPortLease>
SessionProxyPortAllocator::Acquire() noexcept {
    // 小さいportから安定して割り当て、通常利用の一件目をbase portへ固定する。
    for (std::size_t slot = 0U;
         slot < used_.size();
         ++slot) {
        if (used_[slot]) {
            continue;
        }

        const std::optional<std::uint16_t> port =
            ResolvePort(slot);
        // base末尾でoverflowした後続slotはすべて利用不能なので探索を終了する。
        if (!port.has_value()) {
            break;
        }

        used_[slot] = true;
        return SessionProxyPortLease{
            slot,
            *port,
        };
    }

    return std::nullopt;
}

bool SessionProxyPortAllocator::Release(
    const std::size_t slot) noexcept {
    // 範囲外または解放済みslotを別sessionのleaseとして受け付けない。
    if (slot >= used_.size() ||
        !used_[slot]) {
        return false;
    }

    used_[slot] = false;
    return true;
}

std::optional<std::uint16_t>
SessionProxyPortAllocator::NextAvailablePort() const noexcept {
    // Acquireと同じ順序で最初の未使用slotだけを参照する。
    for (std::size_t slot = 0U;
         slot < used_.size();
         ++slot) {
        if (used_[slot]) {
            continue;
        }

        return ResolvePort(slot);
    }

    return std::nullopt;
}

std::size_t SessionProxyPortAllocator::active_count() const noexcept {
    std::size_t count = 0U;
    // 固定長slot table内の使用中要素だけを数える。
    for (const bool used : used_) {
        if (used) {
            ++count;
        }
    }

    return count;
}

std::optional<std::uint16_t>
SessionProxyPortAllocator::ResolvePort(
    const std::size_t slot) const noexcept {
    const std::uint32_t candidate =
        static_cast<std::uint32_t>(base_port_) +
        static_cast<std::uint32_t>(slot);
    // 0およびTCP port上限超過をsessionへ割り当てない。
    if (base_port_ == 0U ||
        candidate >
            std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
    }

    return static_cast<std::uint16_t>(candidate);
}

}  // namespace zrdp::client::app
