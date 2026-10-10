//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file phase1_echo_test.cpp
 * @brief ClientBridgeとAgentBridgeをfake SVCで接続しPhase 1全Echoを検証する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details RDP、network、実接続先を使用せず、両方向のtransport chunkをさらに
 *          不規則分割してframe境界非保証と埋込みNUL保持を再現する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "agent/bridge/agent_bridge.h"
#include "client/bridge/client_bridge.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace {

/**
 * @brief 一つのtransport chunkを固定の不規則sizeへ再分割してsinkへ渡す。
 * @tparam Sink `bool(span<const byte>)`互換callable。
 * @param[in] chunk 送信元が生成した最大1600-byte chunk。
 * @param[in,out] sink fragment受取先。
 * @return 全fragmentを受理した場合はtrue。
 */
template<typename Sink>
[[nodiscard]] bool DeliverFragmented( const std::span<const std::byte> chunk, Sink &sink)
{
	constexpr std::array<std::size_t, 5> fragment_sizes{
		1U, 31U, 257U, 509U, 997U,
	} ;

	std::size_t offset	   = 0U ;
	std::size_t size_index = 0U ;
	// SVC write単位もApplication frame境界ではない状態を再現する
	while( offset < chunk.size()) {
		const std::size_t size =
			std::min( fragment_sizes[size_index % fragment_sizes.size()], chunk.size() - offset) ;
		// 分割fragmentを受信側が拒否したら配送を中止する
		if( !sink( chunk.subspan( offset, size))) {
			return false ;
		}

		offset += size ;
		++size_index ;
	}

	return true ;
}

/**
 * @brief Clientのbounded queueをfake SVC経由でAgentへ全送信する。
 * @param[in,out] client Client protocol。
 * @param[in,out] agent Agent protocol。
 * @return queue全体をAgentが受理した場合はtrue。
 */
[[nodiscard]] bool PumpClientToAgent( zrdp::client::bridge::ClientBridge &client,
									 zrdp::agent::bridge::AgentBridge	&agent)
{
	bool more_data = false ;
	// Client queueが空になるまで、各chunkをさらに小さいfragmentへ分割して渡す
	do {
		std::vector<std::byte> chunk ;
		// Clientが予告した送信chunkを取り出せなければ配送失敗にする
		if( !client.TakeOutboundChunk( chunk, more_data)) {
			return false ;
		}

		/**
		 * @cond LOCAL_CALLBACK_CONTRACT
		 * @brief fake SVCのfragmentをAgentへ同期配送して再構成を検証する。
		 * @param[in] fragment 今回の分割片。Agentは呼出し中に所有bufferへコピーする。
		 * @return Agentがfragmentを受理できた場合はtrue。
		 * @endcond
		 */
		auto sink = [&agent]( const std::span<const std::byte> fragment) {
			return agent.OnChannelData( fragment) ;
		} ;
		// Client chunkのfragment配送失敗を往復helperへ伝える
		if( !DeliverFragmented( chunk, sink)) {
			return false ;
		}
	} while( more_data) ;

	return true ;
}

/**
 * @brief Agentのserialized writer queueをfake SVC経由でClientへ全送信する。
 * @param[in,out] agent Agent protocol。
 * @param[in,out] client Client protocol。
 * @return queue全体をClientが受理した場合はtrue。
 */
[[nodiscard]] bool PumpAgentToClient( zrdp::agent::bridge::AgentBridge	&agent,
									 zrdp::client::bridge::ClientBridge &client)
{
	bool more_data = false ;
	// Agent writer queueが空になるまで、wire順を維持してClientへ渡す
	do {
		std::vector<std::byte> chunk ;
		// Agentが予告した応答chunkを取り出せなければ配送失敗にする
		if( !agent.TakeOutboundChunk( chunk, more_data)) {
			return false ;
		}

		/**
		 * @cond LOCAL_CALLBACK_CONTRACT
		 * @brief fake SVCのfragmentをClientへ同期配送して応答再構成を検証する。
		 * @param[in] fragment 今回の応答片。呼出し終了後のspanを保持しない。
		 * @return Clientがfragmentを受理できた場合はtrue。
		 * @endcond
		 */
		auto sink = [&client]( const std::span<const std::byte> fragment) {
			return client.OnChannelData( fragment) ;
		} ;
		// Agent chunkのfragment配送失敗を往復helperへ伝える
		if( !DeliverFragmented( chunk, sink)) {
			return false ;
		}
	} while( more_data) ;

	return true ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Phase 1のHELLO／Echoをfragment化transport越しに結合検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain()
{
	using zrdp::agent::bridge::AgentBridge ;
	using zrdp::agent::bridge::AgentBridgeState ;
	using zrdp::client::bridge::ClientBridge ;
	using zrdp::client::bridge::ClientBridgeState ;

	ClientBridge client ;
	AgentBridge	 agent ;
	agent.Reset() ;

	// HELLOとHELLO_ACKを両方向に不規則分割してreadyへ遷移させる
	if( !client.StartSession() || !PumpClientToAgent( client, agent) ||
	   agent.snapshot().state != AgentBridgeState::Ready || !PumpAgentToClient( agent, client) ||
	   client.snapshot().state != ClientBridgeState::Ready) {
		return 1 ;
	}

	// Ctrl+C相当のCLOSEでClientをHELLO待機へ戻し、同じRDP内の新Agentと再接続する
	if( !agent.QueueSessionClose() || !PumpAgentToClient( agent, client) ||
	   client.snapshot().state != ClientBridgeState::WaitingForAgent) {
		return 2 ;
	}

	agent.Reset() ;
	// 分割HELLO往復で双方がReadyへ遷移することを確認する
	if( !PumpClientToAgent( client, agent) || agent.snapshot().state != AgentBridgeState::Ready ||
	   !PumpAgentToClient( agent, client) || client.snapshot().state != ClientBridgeState::Ready) {
		return 3 ;
	}

	// Echo検証を開始できなければ大容量binary転送を実行しない
	if( !client.BeginEchoTest()) {
		return 4 ;
	}

	std::size_t round_trip_count = 0U ;
	// 一度に一PING/PONGを往復し、全target完了までqueueを無制限に増やさない
	while( client.snapshot().state == ClientBridgeState::EchoRunning) {
		// 各往復の送信・再構成が成立しなければ一致検証を失敗にする
		if( !PumpClientToAgent( client, agent) || !PumpAgentToClient( agent, client)) {
			return 5 ;
		}

		++round_trip_count ;
		// state不具合による無限loopをtest自体でも有限上限へ制限する
		if( round_trip_count > 1024U) {
			return 6 ;
		}
	}

	const auto client_result = client.snapshot() ;
	const auto agent_result	 = agent.snapshot() ;
	// 大容量Echoの全byte一致と双方の累積counterが一致することを確認する
	if( client_result.state != ClientBridgeState::EchoPassed ||
	   client_result.echo_completed_bytes != client_result.echo_total_bytes ||
	   agent_result.echoed_bytes != client_result.echo_total_bytes ||
	   client_result.echo_total_bytes != 11600897U) {
		return 7 ;
	}

	// stream成立後の周期HELLOはAgent active Socketをresetするため送信しない
	agent.Reset() ;
	// Agent resetだけで成立済みClientが勝手にHELLOを再開しないことを確認する
	if( client.QueueHelloRetry() || agent.snapshot().state != AgentBridgeState::WaitingForHello ||
	   client.snapshot().state != ClientBridgeState::EchoPassed) {
		return 8 ;
	}

	// RDP disconnect相当では両端が旧sequenceとdecoder fragmentを破棄する
	client.StopSession() ;
	agent.Reset() ;
	// Client停止とAgent resetで旧Echo状態とcounterが回収されることを確認する
	if( client.snapshot().state != ClientBridgeState::Stopped ||
	   agent.snapshot().state != AgentBridgeState::WaitingForHello ||
	   agent.snapshot().echoed_bytes != 0U) {
		return 9 ;
	}

	// Agent不在相当ではHELLOをqueueしてもRDP側stateは待機のまま維持する
	if( !client.StartSession() || client.snapshot().state != ClientBridgeState::WaitingForAgent ||
	   !client.QueueHelloRetry() || client.snapshot().state != ClientBridgeState::WaitingForAgent) {
		return 10 ;
	}

	// stale timer通知はAgent待機stateを誤ってEcho timeoutへ変更しない
	client.OnEchoTimeout() ;
	// 新session開始時にClientが再びAgent待ちへ戻ることを確認する
	if( client.snapshot().state != ClientBridgeState::WaitingForAgent) {
		return 11 ;
	}

	return 0 ;
}
