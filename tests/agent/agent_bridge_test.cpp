//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_bridge_test.cpp
 * @brief AgentBridgeのHELLO、binary Echo、sequence、queue上限を検証する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  接続別設定・自動Agent管理の回帰検証を追加。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "agent/bridge/agent_bridge.h"
#include "agent/bridge/agent_stream_relay.h"
#include "agent/network/allowlist.h"
#include "agent/network/upstream_connection.h"
#include "protocol/frame.h"
#include "protocol/hello.h"
#include "protocol/multiplexer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

/**
 * @brief 接続開始を固定失敗させ、Bridgeからの呼出し回数だけを数えるfactory。
 */
class RejectingConnector final : public zrdp::agent::network::IUpstreamConnector {
public:
	/// connectorへの接続開始要求の件数。拒否後も別経路を試さないことを検証する。
	std::uint32_t begin_count{0U} ;

	/**
	 * @brief 拒否結果を返し、接続開始要求の件数だけをtest stateへ記録する。
	 * @param[out] connection 常に空へ戻す接続出力。このfakeはinstanceを生成しない。
	 * @return 拒否した接続開始のI/O分類。
	 * @note 無名のtarget入力は使用せず、全要求に同じ接続失敗を返す。
	 */
	[[nodiscard]]
	zrdp::agent::network::UpstreamIoResult BeginConnect(
		const zrdp::protocol::OpenTarget &,
		std::unique_ptr<zrdp::agent::network::IUpstreamConnection> &connection) noexcept override
	{
		++begin_count ;
		connection.reset() ;
		return zrdp::agent::network::UpstreamIoResult{
			zrdp::agent::network::UpstreamIoStatus::Failed,
			0U,
			1U,
		} ;
	}
} ;

/**
 * @brief frameを不規則なfragmentとしてAgentへ渡す。
 * @param[in,out] bridge 検証対象。
 * @param[in] frame encodeするClient frame。
 * @return 全fragmentを受理できた場合はtrue。
 */
[[nodiscard]] bool DeliverFrame( zrdp::agent::bridge::AgentBridge &bridge,
								const zrdp::protocol::Frame		 &frame)
{
	std::vector<std::byte> encoded ;
	// fixture frameをencodeできなければ受信分割の検証を開始しない
	if( zrdp::protocol::EncodeFrame( frame, encoded) != zrdp::protocol::FrameError::None) {
		return false ;
	}

	std::size_t offset = 0U ;
	// headerとpayload境界に一致しない113-byte fragmentで再構成を確認する
	while( offset < encoded.size()) {
		const std::size_t size = std::min( static_cast<std::size_t>( 113U), encoded.size() - offset) ;
		// fragmentをBridgeが受理しなければ配送helperの失敗を返す
		if( !bridge.OnChannelData( std::span<const std::byte>( encoded).subspan( offset, size))) {
			return false ;
		}

		offset += size ;
	}

	return true ;
}

/**
 * @brief Agent writer queueを一つのdecoderへ連続入力する。
 * @param[in,out] bridge AgentBridge。
 * @param[in,out] decoder Client役decoder。
 * @param[out] frames 完成したAgent frame群。
 * @return queue全体をdecodeできた場合はtrue。
 */
[[nodiscard]] bool DrainFrames( zrdp::agent::bridge::AgentBridge	  &bridge,
							   zrdp::protocol::FrameDecoder		  &decoder,
							   std::vector<zrdp::protocol::Frame> &frames)
{
	frames.clear() ;
	bool more_data = false ;
	// serialized writer queueが空になるまで全chunkを同じdecoderへ連結する
	do {
		std::vector<std::byte> chunk ;
		// 送信予定chunkを取り出せなければframe排出の検証を失敗にする
		if( !bridge.TakeOutboundChunk( chunk, more_data)) {
			return false ;
		}

		std::vector<zrdp::protocol::Frame> decoded ;
		// chunkの再構成でwire違反があれば期待frameの比較へ進めない
		if( decoder.Push( chunk, decoded) != zrdp::protocol::FrameError::None) {
			return false ;
		}

		frames.insert( frames.end(), std::make_move_iterator( decoded.begin()),
					  std::make_move_iterator( decoded.end())) ;
	} while( more_data) ;

	return true ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Agent BridgeのHELLO、PING、stream転送、session終了を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain()
{
	using zrdp::agent::bridge::AgentBridge ;
	using zrdp::agent::bridge::AgentBridgeError ;
	using zrdp::agent::bridge::AgentBridgeState ;
	using zrdp::protocol::Frame ;
	using zrdp::protocol::FrameDecoder ;
	using zrdp::protocol::FrameError ;
	using zrdp::protocol::HelloCapabilities ;
	using zrdp::protocol::MessageType ;

	AgentBridge bridge ;
	bridge.Reset() ;
	// 初期AgentがHELLO待ち以外の状態なら検証を失敗にする
	if( bridge.snapshot().state != AgentBridgeState::WaitingForHello) {
		return 1 ;
	}

	Frame hello{} ;
	hello.type = MessageType::Hello ;
	// 有効HELLOからReadyへ遷移できることを確認し、不成立を検証失敗にする
	if( zrdp::protocol::EncodeHelloCapabilities( HelloCapabilities{}, hello.payload) !=
		   FrameError::None ||
	   !DeliverFrame( bridge, hello) || bridge.snapshot().state != AgentBridgeState::Ready) {
		return 2 ;
	}

	FrameDecoder	   client_decoder ;
	std::vector<Frame> frames ;
	// HELLOに対する応答が一件のHELLO_ACKでなければ失敗とする
	if( !DrainFrames( bridge, client_decoder, frames) || frames.size() != 1U ||
	   frames.front().type != MessageType::HelloAck) {
		return 3 ;
	}

	// Ctrl+C正常終了ではconnection-level CLOSEを最後のframeとして生成する
	if( !bridge.QueueSessionClose() || !DrainFrames( bridge, client_decoder, frames) ||
	   frames.size() != 1U || frames.front().type != MessageType::Close ||
	   frames.front().stream_id != 0U || frames.front().sequence != 0U ||
	   !frames.front().payload.empty()) {
		return 4 ;
	}

	Frame ping{} ;
	ping.type	  = MessageType::Ping ;
	ping.sequence = 1U ;
	ping.payload.resize( 32768U) ;
	// 埋込みNULを含むbinary patternを文字列化せず生成する
	for( std::size_t index = 0U; index < ping.payload.size(); ++index) {
		ping.payload[index] = static_cast<std::byte>( index % 251U) ;
	}

	// PONGのsequence・binary内容・累積転送量の完全一致を確認する
	if( !DeliverFrame( bridge, ping) || !DrainFrames( bridge, client_decoder, frames) ||
	   frames.size() != 1U || frames.front().type != MessageType::Pong ||
	   frames.front().sequence != 1U || frames.front().payload != ping.payload ||
	   bridge.snapshot().echoed_bytes != ping.payload.size()) {
		return 5 ;
	}

	// idle HELLOを新しいsequence境界とし、再起動後と同じsequence 1を受け入れる
	if( !DeliverFrame( bridge, hello) || !DrainFrames( bridge, client_decoder, frames) ||
	   frames.size() != 1U || frames.front().type != MessageType::HelloAck ||
	   bridge.snapshot().last_ping_sequence != 0U || bridge.snapshot().echoed_bytes != 0U ||
	   !DeliverFrame( bridge, ping)) {
		return 6 ;
	}

	// session reset後もEcho sequenceが1から開始されることを確認する
	if( !DrainFrames( bridge, client_decoder, frames) || frames.size() != 1U ||
	   frames.front().type != MessageType::Pong || frames.front().sequence != 1U) {
		return 7 ;
	}

	// sequence欠落を正常Echoとして近似しない
	ping.sequence = 3U ;
	// 重複PINGがsequence違反で拒否されない場合は失敗とする
	if( DeliverFrame( bridge, ping) || bridge.snapshot().state != AgentBridgeState::Failed ||
	   bridge.snapshot().error != AgentBridgeError::SequenceMismatch) {
		return 8 ;
	}

	bridge.Reset() ;
	// HELLO前のPINGでAgentから暗黙sessionを開始しない
	ping.sequence = 1U ;
	// HELLO前のPINGが予期しないmessageとして拒否されることを確認する
	if( DeliverFrame( bridge, ping) ||
	   bridge.snapshot().error != AgentBridgeError::UnexpectedMessage) {
		return 9 ;
	}

	bridge.Reset() ;
	// 送信queue上限試験の前提となるHELLOを受理できなければ検証を止める
	if( !DeliverFrame( bridge, hello)) {
		return 10 ;
	}

	// ACKと複数PONGをdrainせず蓄積し、有限writer queue上限で停止する
	for( std::uint32_t sequence = 1U;
		sequence < 32U && bridge.snapshot().state == AgentBridgeState::Ready; ++sequence) {
		ping.sequence = sequence ;
		static_cast<void>( DeliverFrame( bridge, ping)) ;
	}

	// writer queue上限を越えたAgentが固定errorで停止することを確認する
	if( bridge.snapshot().state != AgentBridgeState::Failed ||
	   bridge.snapshot().error != AgentBridgeError::QueueLimitExceeded) {
		return 11 ;
	}

	// Phase 3設定時はHELLOで一streamを広告し、OPENをrelayへrouteする
	zrdp::agent::network::Allowlist allowlist ;
	// 文書例示アドレスのallowlistを作れなければSocket routing試験を開始しない
	if( !zrdp::agent::network::ParseAllowlist( "[allowlist]\n"
											 "endpoint.1001=192.0.2.10:80\n",
											 allowlist)
			.succeeded()) {
		return 12 ;
	}

	RejectingConnector					  connector ;
	zrdp::agent::bridge::AgentStreamRelay relay( allowlist, connector) ;
	AgentBridge							  phase3_bridge ;
	phase3_bridge.ConfigureStreamRelay( &relay) ;
	HelloCapabilities phase3_capabilities = zrdp::protocol::MakePhase2HelloCapabilities() ;
	phase3_capabilities.maximum_streams	  = zrdp::agent::bridge::kMaximumAgentRelayStreams ;
	Frame phase3_hello{} ;
	phase3_hello.type = MessageType::Hello ;
	// relay付きHELLOの交渉とACK生成が成立しなければ失敗とする
	if( zrdp::protocol::EncodeHelloCapabilities( phase3_capabilities, phase3_hello.payload) !=
		   FrameError::None ||
	   !DeliverFrame( phase3_bridge, phase3_hello) ||
	   !DrainFrames( phase3_bridge, client_decoder, frames) || frames.size() != 1U ||
	   frames.front().type != MessageType::HelloAck) {
		return 13 ;
	}

	HelloCapabilities agent_capabilities{} ;
	// Agentのstream機能bitと有限Socket上限の宣言を確認する
	if( zrdp::protocol::DecodeHelloCapabilities( frames.front().payload, agent_capabilities) !=
		   FrameError::None ||
	   agent_capabilities.maximum_streams != zrdp::agent::bridge::kMaximumAgentRelayStreams ||
	   (agent_capabilities.feature_bits & zrdp::protocol::kFeatureMultiplexedStreams) == 0U) {
		return 14 ;
	}

	zrdp::protocol::StreamMultiplexer client( zrdp::protocol::MultiplexerRole::Client) ;
	// Client側の試験sessionを開始できなければOPEN検証へ進めない
	if( client.StartSession( phase3_capabilities) != zrdp::protocol::MultiplexerResult::Ok) {
		return 15 ;
	}

	std::uint32_t stream_id = 0U ;
	// 許可済み例示endpointへのOPENを生成できなければ検証を止める
	if( client.OpenStream( zrdp::protocol::OpenTarget{{192U, 0U, 2U, 10U}, 80U}, stream_id) !=
	   zrdp::protocol::MultiplexerResult::Ok) {
		return 16 ;
	}

	Frame open{} ;
	// Agent bridgeがSocket接続失敗をOPEN_ERRORとして一度だけ返すことを確認する
	if( !client.TakeOutboundFrame( open) || !DeliverFrame( phase3_bridge, open) ||
	   !DrainFrames( phase3_bridge, client_decoder, frames) || frames.size() != 1U ||
	   frames.front().type != MessageType::OpenError ||
	   client.OnFrame( std::move( frames.front())) != zrdp::protocol::MultiplexerResult::Ok ||
	   connector.begin_count != 1U) {
		return 17 ;
	}

	zrdp::protocol::StreamEvent open_event{} ;
	// 固定ConnectFailedがClientのOpenFailed eventへ対応することを確認する
	if( !client.TakeEvent( open_event) ||
	   open_event.type != zrdp::protocol::StreamEventType::OpenFailed ||
	   open_event.open_error != zrdp::protocol::OpenErrorCode::ConnectFailed) {
		return 18 ;
	}

	AgentBridge managed ;
	managed.EnableConfigurationControl( true) ;
	HelloCapabilities lifecycle = zrdp::protocol::MakePhase2HelloCapabilities() ;
	lifecycle.feature_bits |= zrdp::protocol::kFeatureAgentLifecycle ;
	Frame managed_hello{} ;
	managed_hello.type = MessageType::Hello ;
	// negotiated controlを持つHELLOでも設定確認前は利用不可とする
	if( zrdp::protocol::EncodeHelloCapabilities( lifecycle, managed_hello.payload) !=
		   FrameError::None ||
	   !DeliverFrame( managed, managed_hello) || managed.ConfigurationReady()) {
		return 40 ;
	}

	Frame configuration{} ;
	configuration.type	   = MessageType::AgentConfigure ;
	configuration.sequence = 1U ;
	const std::string ini  = "[allowlist]\nendpoint.1=192.0.2.10:80\n" ;
	configuration.payload.assign( reinterpret_cast<const std::byte *>( ini.data()),
								 reinterpret_cast<const std::byte *>( ini.data() + ini.size())) ;
	Frame received{} ;
	// 受理したINIを一回だけ取得し、処理前と重複ACKを拒否する
	if( !DeliverFrame( managed, configuration) || managed.CompleteConfiguration( 1U) ||
	   !managed.TakeConfiguration( received) || received.payload != configuration.payload ||
	   managed.TakeConfiguration( received) || !managed.CompleteConfiguration( 2U) ||
	   managed.CompleteConfiguration( 2U) || managed.ConfigurationReady()) {
		return 41 ;
	}

	FrameDecoder lifecycle_decoder ;
	// HELLO_ACKと設定ACKは同じserialized writerでwire順に出る
	if( !DrainFrames( managed, lifecycle_decoder, frames) || frames.size() != 2U ||
	   frames[1].type != MessageType::AgentConfigureAck || frames[1].sequence != 1U ||
	   frames[1].payload != std::vector<std::byte>{std::byte{2}}) {
		return 42 ;
	}

	// 新HELLOでは前回のINI要求とACKを再利用しない
	if( !DeliverFrame( managed, managed_hello) || managed.ConfigurationReady() ||
	   !DeliverFrame( managed, configuration) || !managed.TakeConfiguration( received) ||
	   !managed.CompleteConfiguration( 1U) || !managed.ConfigurationReady()) {
		return 43 ;
	}

	Frame stop_frame{} ;
	stop_frame.type = MessageType::AgentStop ;
	// 固定停止は利用可能状態を直ちに失効させる
	if( !DeliverFrame( managed, stop_frame) || !managed.StopRequested() ||
	   managed.ConfigurationReady()) {
		return 44 ;
	}

	AgentBridge unmanaged ;
	// 未交渉controlを既存Agentの通常DATAへ近似しない
	if( DeliverFrame( unmanaged, configuration)) {
		return 45 ;
	}

	managed.Reset() ;
	// 再認証しても同一HELLOの重複設定を採用しない
	if( !DeliverFrame( managed, managed_hello) || !DeliverFrame( managed, configuration) ||
	   DeliverFrame( managed, configuration)) {
		return 46 ;
	}

	RejectingConnector					  guard_connector ;
	zrdp::agent::bridge::AgentStreamRelay guard_relay( allowlist, guard_connector) ;
	AgentBridge							  guarded ;
	guarded.ConfigureStreamRelay( &guard_relay) ;
	guarded.EnableConfigurationControl( true) ;
	// HELLO成立だけでは有効allowlistへもSocket接続を開始しない
	if( !DeliverFrame( guarded, managed_hello) || DeliverFrame( guarded, open) ||
	   guard_connector.begin_count != 0U) {
		return 47 ;
	}

	return 0 ;
}
