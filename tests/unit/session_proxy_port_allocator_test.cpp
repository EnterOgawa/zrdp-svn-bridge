/**
 * @file session_proxy_port_allocator_test.cpp
 * @brief 複数RDP sessionのProxy port順次割当、解放、上限を検証する。
 *
 * @details Socketや実endpointを使用せず、純粋な有限slot stateだけを対象とする。
 */

#include "client/app/session_proxy_port_allocator.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

using zrdp::client::app::SessionProxyPortAllocator;
using zrdp::client::app::SessionProxyPortLease;
using zrdp::client::app::kMaximumConcurrentRdpSessions;

/**
 * @brief base 18080から全slotを重複なく昇順割当できるか検証する。
 */
[[nodiscard]] bool TestSequentialAllocation() {
    SessionProxyPortAllocator allocator(18080U);
    if (allocator.active_count() != 0U ||
        allocator.NextAvailablePort() != 18080U) {
        return false;
    }

    std::array<
        SessionProxyPortLease,
        kMaximumConcurrentRdpSessions> leases{};
    // 全有限slotを順番に取得し、base+slotの対応を確認する。
    for (std::size_t index = 0U;
         index < leases.size();
         ++index) {
        const auto lease = allocator.Acquire();
        if (!lease.has_value() ||
            lease->slot != index ||
            lease->port !=
                static_cast<std::uint16_t>(
                    18080U + index)) {
            return false;
        }

        leases[index] = *lease;
    }

    return allocator.active_count() ==
            kMaximumConcurrentRdpSessions &&
        !allocator.Acquire().has_value() &&
        !allocator.NextAvailablePort().has_value();
}

/**
 * @brief 解放した最小slotだけを次のsessionへ再利用するか検証する。
 */
[[nodiscard]] bool TestReleaseAndReuse() {
    SessionProxyPortAllocator allocator(18080U);
    const auto first = allocator.Acquire();
    const auto second = allocator.Acquire();
    const auto third = allocator.Acquire();
    if (!first.has_value() ||
        !second.has_value() ||
        !third.has_value()) {
        return false;
    }

    // 使用中の中間slotだけを一度解放し、重複解放を拒否する。
    if (!allocator.Release(second->slot) ||
        allocator.Release(second->slot) ||
        allocator.NextAvailablePort() !=
            second->port) {
        return false;
    }

    const auto reused = allocator.Acquire();
    return reused.has_value() &&
        reused->slot == second->slot &&
        reused->port == second->port &&
        allocator.active_count() == 3U &&
        !allocator.Release(
            kMaximumConcurrentRdpSessions);
}

/**
 * @brief TCP port上限を越える後続slotとbase 0を拒否する。
 */
[[nodiscard]] bool TestPortBoundary() {
    SessionProxyPortAllocator last_port(65535U);
    const auto only = last_port.Acquire();
    if (!only.has_value() ||
        only->slot != 0U ||
        only->port != 65535U ||
        last_port.Acquire().has_value() ||
        last_port.NextAvailablePort().has_value()) {
        return false;
    }

    if (!last_port.Release(only->slot) ||
        last_port.NextAvailablePort() != 65535U) {
        return false;
    }

    SessionProxyPortAllocator invalid(0U);
    return !invalid.Acquire().has_value() &&
        !invalid.NextAvailablePort().has_value() &&
        invalid.active_count() == 0U;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief session専用Proxy portの割当、解放、再利用、上限拒否を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int main() {
    if (!TestSequentialAllocation()) {
        return 1;
    }

    if (!TestReleaseAndReuse()) {
        return 2;
    }

    if (!TestPortBoundary()) {
        return 3;
    }

    return 0;
}
