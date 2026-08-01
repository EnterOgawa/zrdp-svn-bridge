/**
 * @file phase6_hardening_test.cpp
 * @brief Agent異常終了、Client再起動、RDP再接続、allowlist切替を反復検証する。
 *
 * @details fake SVCと文書用IPv4だけを使用し、実RDP、外部network、実SVNへ
 *          接続しない。各再開では旧decoder、PING sequence、queueを再利用しない。
 */

#include "agent/bridge/agent_bridge.h"
#include "agent/network/allowlist.h"
#include "client/bridge/client_bridge.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace {

/**
 * @brief 一transport chunkを不規則な小片へ分けて受信境界へ渡す。
 * @tparam Sink `bool(span<const byte>)`互換callable。
 * @param[in] chunk 送信元所有chunk。
 * @param[in,out] sink 受信先。
 * @return 全fragmentを受理できた場合はtrue。
 */
template <typename Sink>
[[nodiscard]] bool DeliverFragmented(
    const std::span<const std::byte> chunk,
    Sink& sink) {
    constexpr std::array<std::size_t, 4> sizes{
        3U,
        29U,
        251U,
        701U,
    };
    std::size_t offset = 0U;
    std::size_t size_index = 0U;
    // frame境界と一致しないfragmentをwire順のまま最後まで渡す。
    while (offset < chunk.size()) {
        const std::size_t size = std::min(
            sizes[size_index % sizes.size()],
            chunk.size() - offset);
        if (!sink(chunk.subspan(offset, size))) {
            return false;
        }

        offset += size;
        ++size_index;
    }

    return true;
}

/**
 * @brief Clientの全queued chunkをAgentへ転送する。
 * @param[in,out] client Client protocol owner。
 * @param[in,out] agent Agent protocol owner。
 * @return queue全体を受理できた場合はtrue。
 */
[[nodiscard]] bool PumpClientToAgent(
    zrdp::client::bridge::ClientBridge& client,
    zrdp::agent::bridge::AgentBridge& agent) {
    bool more_data = false;
    // Clientの有限queueが空になるまでfragment化してAgentへ順次配信する。
    do {
        std::vector<std::byte> chunk;
        if (!client.TakeOutboundChunk(chunk, more_data)) {
            return false;
        }

        auto sink = [&agent](
                        const std::span<const std::byte> fragment) {
            return agent.OnChannelData(fragment);
        };
        if (!DeliverFragmented(chunk, sink)) {
            return false;
        }
    } while (more_data);

    return true;
}

/**
 * @brief Agentのserialized writer queueをClientへ転送する。
 * @param[in,out] agent Agent protocol owner。
 * @param[in,out] client Client protocol owner。
 * @return queue全体を受理できた場合はtrue。
 */
[[nodiscard]] bool PumpAgentToClient(
    zrdp::agent::bridge::AgentBridge& agent,
    zrdp::client::bridge::ClientBridge& client) {
    bool more_data = false;
    // Agentのserialized writer queueが空になるまでClientへ順次配信する。
    do {
        std::vector<std::byte> chunk;
        if (!agent.TakeOutboundChunk(chunk, more_data)) {
            return false;
        }

        auto sink = [&client](
                        const std::span<const std::byte> fragment) {
            return client.OnChannelData(fragment);
        };
        if (!DeliverFragmented(chunk, sink)) {
            return false;
        }
    } while (more_data);

    return true;
}

/**
 * @brief ClientとAgentを新HELLO境界でReadyへ遷移させる。
 * @param[in,out] client Client protocol owner。
 * @param[in,out] agent Agent protocol owner。
 * @return 両端がReadyの場合はtrue。
 */
[[nodiscard]] bool StartReadySession(
    zrdp::client::bridge::ClientBridge& client,
    zrdp::agent::bridge::AgentBridge& agent) {
    return client.StartSession() &&
        PumpClientToAgent(client, agent) &&
        PumpAgentToClient(agent, client) &&
        client.snapshot().state ==
            zrdp::client::bridge::ClientBridgeState::Ready &&
        agent.snapshot().state ==
            zrdp::agent::bridge::AgentBridgeState::Ready;
}

/**
 * @brief 一件のhealth PING／PONGを往復する。
 * @param[in,out] client Client protocol owner。
 * @param[in,out] agent Agent protocol owner。
 * @return 応答待ちが解消されReadyを維持した場合はtrue。
 */
[[nodiscard]] bool RoundTripHealth(
    zrdp::client::bridge::ClientBridge& client,
    zrdp::agent::bridge::AgentBridge& agent) {
    return client.QueueHealthCheck() &&
        PumpClientToAgent(client, agent) &&
        PumpAgentToClient(agent, client) &&
        !client.snapshot().health_check_outstanding &&
        client.snapshot().state ==
            zrdp::client::bridge::ClientBridgeState::Ready;
}

/**
 * @brief Client送信queueをAgentへ渡さず破棄し、Agent crash相当を作る。
 * @param[in,out] client Client protocol owner。
 * @return 一件以上のchunkを有限queueから破棄できた場合はtrue。
 */
[[nodiscard]] bool DiscardClientQueue(
    zrdp::client::bridge::ClientBridge& client) {
    bool more_data = false;
    bool discarded = false;
    // crash相当を再現するため、現在queue済みの有限chunkだけを全て破棄する。
    do {
        std::vector<std::byte> chunk;
        if (!client.TakeOutboundChunk(chunk, more_data)) {
            return false;
        }

        discarded = discarded || !chunk.empty();
    } while (more_data);

    return discarded;
}

}  // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Phase 6のmalformed入力、resource上限、切断cleanupを検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::agent::bridge::AgentBridge;
    using zrdp::agent::bridge::AgentBridgeState;
    using zrdp::agent::network::Allowlist;
    using zrdp::client::bridge::ClientBridge;
    using zrdp::client::bridge::ClientBridgeState;
    using zrdp::protocol::OpenTarget;

    ClientBridge client;
    AgentBridge agent;
    agent.Reset();
    if (!StartReadySession(client, agent)) {
        return 1;
    }

    // 通常health checkを反復し、周期処理でqueueやsequenceが混線しないことを確認する。
    for (std::size_t round = 0U; round < 64U; ++round) {
        if (!RoundTripHealth(client, agent)) {
            return 2;
        }
    }

    // Agent crashでは未応答PING後に旧stateを捨て、新AgentへのHELLOをqueueする。
    if (!client.QueueHealthCheck() ||
        !DiscardClientQueue(client) ||
        !client.OnHealthCheckTimeout() ||
        client.snapshot().state !=
            ClientBridgeState::WaitingForAgent) {
        return 3;
    }

    AgentBridge restarted_agent;
    restarted_agent.Reset();
    if (!PumpClientToAgent(client, restarted_agent) ||
        !PumpAgentToClient(restarted_agent, client) ||
        client.snapshot().state != ClientBridgeState::Ready ||
        !RoundTripHealth(client, restarted_agent)) {
        return 4;
    }

    // Client process再起動相当の新HELLOでAgent側旧sequenceをresetする。
    ClientBridge restarted_client;
    if (!StartReadySession(
            restarted_client,
            restarted_agent) ||
        !RoundTripHealth(
            restarted_client,
            restarted_agent)) {
        return 5;
    }
    client.StopSession();

    // RDP disconnect／reconnectを反復し、旧fragmentやhealth stateを再利用しない。
    for (std::size_t cycle = 0U; cycle < 32U; ++cycle) {
        restarted_client.StopSession();
        restarted_agent.Reset();
        if (restarted_client.snapshot().state !=
                ClientBridgeState::Stopped ||
            restarted_agent.snapshot().state !=
                AgentBridgeState::WaitingForHello ||
            !StartReadySession(
                restarted_client,
                restarted_agent) ||
            !RoundTripHealth(
                restarted_client,
                restarted_agent)) {
            return 6;
        }
    }

    constexpr std::string_view old_configuration =
        "[allowlist]\n"
        "endpoint.1001=192.0.2.10:80\n";
    constexpr std::string_view invalid_reload =
        "[allowlist]\n"
        "endpoint.1001=192.0.2.10:80\n"
        "endpoint.1002=192.0.2.10:80\n";
    constexpr std::string_view new_configuration =
        "[allowlist]\n"
        "endpoint.2001=198.51.100.20:8080\n";
    constexpr OpenTarget old_target{
        {192U, 0U, 2U, 10U},
        80U};
    constexpr OpenTarget new_target{
        {198U, 51U, 100U, 20U},
        8080U};

    Allowlist active_allowlist;
    std::uint32_t endpoint_id = 0U;
    if (!zrdp::agent::network::ParseAllowlist(
            old_configuration,
            active_allowlist).succeeded() ||
        !active_allowlist.Find(
            old_target,
            endpoint_id) ||
        endpoint_id != 1001U) {
        return 7;
    }

    // 不正reloadは途中entryを公開せず、検証済み旧設定をatomicに維持する。
    if (zrdp::agent::network::ParseAllowlist(
            invalid_reload,
            active_allowlist).succeeded() ||
        !active_allowlist.Find(
            old_target,
            endpoint_id) ||
        active_allowlist.Find(
            new_target,
            endpoint_id)) {
        return 8;
    }

    // Agent再起動相当の新instanceだけへ新allowlistを適用し、旧targetを許可しない。
    Allowlist restarted_allowlist;
    if (!zrdp::agent::network::ParseAllowlist(
            new_configuration,
            restarted_allowlist).succeeded() ||
        !restarted_allowlist.Find(
            new_target,
            endpoint_id) ||
        endpoint_id != 2001U ||
        restarted_allowlist.Find(
            old_target,
            endpoint_id)) {
        return 9;
    }

    return 0;
}
