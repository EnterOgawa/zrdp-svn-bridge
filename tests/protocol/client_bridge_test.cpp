//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_bridge_test.cpp
 * @brief ClientBridgeのhandshake、分割送信、全Echo size、異常遷移を検証する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details ActiveXとRDP接続を使用せず、in-memory Agent役でframeを往復させる。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  接続別設定・自動Agent管理の回帰検証を追加。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/bridge/client_bridge.h"
#include "protocol/frame.h"
#include "protocol/hello.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace {

/**
 * @brief Clientのtransport chunkを全て取り出してAgent decoderへ渡す。
 * @param[in,out] client 検証対象ClientBridge。
 * @param[in,out] decoder Agent役のincremental decoder。
 * @param[out] frames 今回Clientが送った完全frame群。
 * @return 全chunkをerrorなくdecodeできた場合はtrue。
 */
[[nodiscard]] bool DrainClientFrames( zrdp::client::bridge::ClientBridge &client,
									 zrdp::protocol::FrameDecoder		&decoder,
									 std::vector<zrdp::protocol::Frame> &frames)
{
	frames.clear() ;
	bool more_data = false ;
	// more_dataがfalseになるまで、bounded SVC chunkを一つずつdecoderへ渡す
	do {
		std::vector<std::byte> chunk ;
		// 予告されたClient送信chunkを取得できなければ排出helperを失敗にする
		if( !client.TakeOutboundChunk( chunk, more_data)) {
			return false ;
		}

		std::vector<zrdp::protocol::Frame> decoded ;
		// outbound chunkのframe再構成に失敗したら期待frame比較を行わない
		if( decoder.Push( chunk, decoded) != zrdp::protocol::FrameError::None) {
			return false ;
		}

		frames.insert( frames.end(), std::make_move_iterator( decoded.begin()),
					  std::make_move_iterator( decoded.end())) ;
	} while( more_data) ;

	return true ;
}

/**
 * @brief Agent応答frameを不規則な777-byte fragmentでClientへ返す。
 * @param[in,out] client 応答を受信するClientBridge。
 * @param[in] frame encodeして返すframe。
 * @return 全fragmentを受理できた場合はtrue。
 */
[[nodiscard]] bool DeliverAgentFrame( zrdp::client::bridge::ClientBridge &client,
									 const zrdp::protocol::Frame		&frame)
{
	std::vector<std::byte> encoded ;
	// Agent fixture frameをencodeできなければ分割配送を開始しない
	if( zrdp::protocol::EncodeFrame( frame, encoded) != zrdp::protocol::FrameError::None) {
		return false ;
	}

	std::size_t offset = 0U ;
	// Client callbackがframe境界と一致しない状態を毎回再現する
	while( offset < encoded.size()) {
		const std::size_t chunk_size =
			std::min( static_cast<std::size_t>( 777U), encoded.size() - offset) ;
		// 分割Agent frameをClientが拒否したら配送helperを失敗にする
		if( !client.OnChannelData( std::span<const std::byte>( encoded).subspan( offset, chunk_size))) {
			return false ;
		}

		offset += chunk_size ;
	}

	return true ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Client Bridgeのhandshake、Echo、chunk化、異常遷移を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain()
{
	using zrdp::client::bridge::ClientBridge ;
	using zrdp::client::bridge::ClientBridgeError ;
	using zrdp::client::bridge::ClientBridgeState ;
	using zrdp::protocol::Frame ;
	using zrdp::protocol::FrameDecoder ;
	using zrdp::protocol::FrameError ;
	using zrdp::protocol::HelloCapabilities ;
	using zrdp::protocol::MessageType ;

	ClientBridge client ;
	// 新session開始時にAgent待機と初回HELLOを生成する
	if( !client.StartSession() || client.snapshot().state != ClientBridgeState::WaitingForAgent ||
	   client.snapshot().hello_attempt_count != 1U) {
		return 1 ;
	}

	FrameDecoder	   agent_decoder ;
	std::vector<Frame> client_frames ;
	// Client起動直後の一件のHELLOと固定sequence 0を確認する
	if( !DrainClientFrames( client, agent_decoder, client_frames) || client_frames.size() != 1U ||
	   client_frames.front().type != MessageType::Hello || client_frames.front().sequence != 0U) {
		return 2 ;
	}

	HelloCapabilities client_capabilities{} ;
	// Clientの多重化機能bitと有限stream上限宣言を確認する
	if( zrdp::protocol::DecodeHelloCapabilities( client_frames.front().payload,
											   client_capabilities) != FrameError::None ||
	   (client_capabilities.feature_bits & zrdp::protocol::kFeatureMultiplexedStreams) == 0U ||
	   client_capabilities.maximum_streams != zrdp::client::bridge::kMaximumClientBridgeStreams) {
		return 3 ;
	}

	Frame hello_ack{} ;
	hello_ack.type = MessageType::HelloAck ;
	// 有効HELLO_ACKの分割配送でClientがReadyへ遷移することを確認する
	if( zrdp::protocol::EncodeHelloCapabilities( HelloCapabilities{}, hello_ack.payload) !=
		   FrameError::None ||
	   !DeliverAgentFrame( client, hello_ack) ||
	   client.snapshot().state != ClientBridgeState::Ready) {
		return 4 ;
	}

	Frame agent_close{} ;
	agent_close.type = MessageType::Close ;
	// Agent正常終了でRDPを切らずHELLO待機へ戻り、初回HELLOを即時生成する
	if( !DeliverAgentFrame( client, agent_close) ||
	   client.snapshot().state != ClientBridgeState::WaitingForAgent ||
	   client.snapshot().hello_attempt_count != 1U ||
	   !DrainClientFrames( client, agent_decoder, client_frames) || client_frames.size() != 1U ||
	   client_frames.front().type != MessageType::Hello || !DeliverAgentFrame( client, hello_ack) ||
	   client.snapshot().state != ClientBridgeState::Ready) {
		return 5 ;
	}

	// ready後に1 byte～10 MiBの段階testを開始する
	if( !client.BeginEchoTest() || client.snapshot().state != ClientBridgeState::EchoRunning) {
		return 6 ;
	}

	bool		  observed_embedded_null = false ;
	std::uint32_t last_sequence			 = 0U ;
	bool		  first_echo_chunk		 = true ;
	// 一度に一PINGをAgent役でPONGへ変換し、全target完了まで往復する
	while( client.snapshot().state == ClientBridgeState::EchoRunning) {
		// Echo送信frameを排出できなければsequence一致検証を止める
		if( !DrainClientFrames( client, agent_decoder, client_frames)) {
			return 7 ;
		}

		// 各Echo先頭のHELLOへACKし、後続PINGより先にsequence境界を成立させる
		if( first_echo_chunk) {
			// Echo先頭はHELLOとPINGの二frameで新しいsequence境界を開始することを確認する
			if( client_frames.size() != 2U || client_frames.front().type != MessageType::Hello ||
			   !DeliverAgentFrame( client, hello_ack)) {
				return 8 ;
			}

			first_echo_chunk = false ;
		}
		// Echo継続chunkが一件のPING以外なら検証を失敗にする
		else if( client_frames.size() != 1U) {
			return 9 ;
		}

		const Frame &ping = client_frames.back() ;
		// PINGの種別・単調sequence・非空payload・frame上限を確認する
		if( ping.type != MessageType::Ping || ping.sequence <= last_sequence ||
		   ping.payload.empty() || ping.payload.size() > 32U * 1024U) {
			return 10 ;
		}

		last_sequence = ping.sequence ;
		observed_embedded_null =
			observed_embedded_null || std::find( ping.payload.begin(), ping.payload.end(),
												std::byte{0U}) != ping.payload.end() ;

		Frame pong{} ;
		pong.type	  = MessageType::Pong ;
		pong.sequence = ping.sequence ;
		pong.payload  = ping.payload ;
		// 同じbyte列のPONGをClientが受理しなければEcho検証を失敗にする
		if( !DeliverAgentFrame( client, pong)) {
			return 11 ;
		}
	}

	const auto passed = client.snapshot() ;
	// 埋込みNULを含む大容量Echoが全byte一致して完了することを確認する
	if( passed.state != ClientBridgeState::EchoPassed ||
	   passed.echo_completed_bytes != passed.echo_total_bytes ||
	   passed.echo_total_bytes != 11600897U || !observed_embedded_null) {
		return 12 ;
	}

	// handshake後の周期HELLOでAgent側active streamをresetしない
	if( client.QueueHelloRetry() || client.snapshot().state != ClientBridgeState::EchoPassed) {
		return 13 ;
	}

	// health PINGはactive sessionをHELLOでresetせず、Echo counterにも加算しない
	if( !client.QueueHealthCheck() || !client.snapshot().health_check_outstanding ||
	   client.BeginEchoTest() || !DrainClientFrames( client, agent_decoder, client_frames) ||
	   client_frames.size() != 1U || client_frames.front().type != MessageType::Ping) {
		return 40 ;
	}

	const Frame health_ping = client_frames.front() ;
	Frame		health_pong{} ;
	health_pong.type	 = MessageType::Pong ;
	health_pong.sequence = health_ping.sequence ;
	health_pong.payload	 = health_ping.payload ;
	// health応答でEchoの完了状態とcounterが変わらないことを確認する
	if( !DeliverAgentFrame( client, health_pong) || client.snapshot().health_check_outstanding ||
	   client.snapshot().state != ClientBridgeState::EchoPassed ||
	   client.snapshot().echo_completed_bytes != passed.echo_completed_bytes) {
		return 41 ;
	}

	// Agent異常終了相当では旧stateを破棄し、同じRDP上のHELLO待機へ自動復帰する
	if( !client.QueueHealthCheck() || !DrainClientFrames( client, agent_decoder, client_frames) ||
	   client_frames.size() != 1U || !client.OnHealthCheckTimeout() ||
	   client.snapshot().state != ClientBridgeState::WaitingForAgent ||
	   client.snapshot().health_check_outstanding ||
	   !DrainClientFrames( client, agent_decoder, client_frames) || client_frames.size() != 1U ||
	   client_frames.front().type != MessageType::Hello) {
		return 42 ;
	}

	Frame stale_data{} ;
	stale_data.type		 = MessageType::Data ;
	stale_data.stream_id = 1U ;
	stale_data.sequence	 = 1U ;
	stale_data.payload	 = {std::byte{0x41U}} ;
	Frame stale_inbound{} ;
	// timeout直前の旧stream FrameとPONGをACK前に破棄し、新sessionを汚染しない
	if( !DeliverAgentFrame( client, stale_data) || client.TakeInboundStreamFrame( stale_inbound) ||
	   !DeliverAgentFrame( client, health_pong) ||
	   client.snapshot().state != ClientBridgeState::WaitingForAgent ||
	   !DeliverAgentFrame( client, hello_ack) ||
	   client.snapshot().state != ClientBridgeState::Ready) {
		return 43 ;
	}

	// 再実行時もsequenceとcounterを新しいtestとして初期化する
	if( !client.BeginEchoTest()) {
		return 14 ;
	}

	// Echo再実行時もHELLOで境界を更新してからPINGを始めることを確認する
	if( !DrainClientFrames( client, agent_decoder, client_frames) || client_frames.size() != 2U ||
	   client_frames.front().type != MessageType::Hello ||
	   client_frames.back().type != MessageType::Ping || !DeliverAgentFrame( client, hello_ack)) {
		return 15 ;
	}

	Frame wrong_sequence{} ;
	wrong_sequence.type		= MessageType::Pong ;
	wrong_sequence.sequence = client_frames.back().sequence + 1U ;
	wrong_sequence.payload	= client_frames.back().payload ;
	// out-of-order PONGをfail-closedにする
	if( DeliverAgentFrame( client, wrong_sequence) ||
	   client.snapshot().state != ClientBridgeState::Failed ||
	   client.snapshot().error != ClientBridgeError::SequenceMismatch) {
		return 16 ;
	}

	// Agent不在またはPONG欠落時は未応答testを有限時間で停止できる
	client.StopSession() ;
	// Echo timeout試験の交渉・開始条件が成立しなければ失敗とする
	if( !client.StartSession() || !DrainClientFrames( client, agent_decoder, client_frames) ||
	   !DeliverAgentFrame( client, hello_ack) || !client.BeginEchoTest()) {
		return 17 ;
	}

	client.OnEchoTimeout() ;
	// 未応答Echoが固定ResponseTimeoutで停止することを確認する
	if( client.snapshot().state != ClientBridgeState::Failed ||
	   client.snapshot().error != ClientBridgeError::ResponseTimeout) {
		return 18 ;
	}

	// RDP disconnect相当resetで旧frameとerrorを再利用しない
	client.StopSession() ;
	// session停止でEcho状態と累積counterが初期化されることを確認する
	if( client.snapshot().state != ClientBridgeState::Stopped ||
	   client.snapshot().echo_completed_bytes != 0U) {
		return 19 ;
	}

	// queue上限試験のsessionを開始できなければ検証を止める
	if( !client.StartSession()) {
		return 20 ;
	}

	// drainされないHELLO retryを無制限にbufferせずqueue上限で停止する
	for( std::size_t attempt = 0U;
		attempt < 5000U && client.snapshot().state == ClientBridgeState::WaitingForAgent;
		++attempt) {
		static_cast<void>( client.QueueHelloRetry()) ;
	}

	// 未排出HELLOの蓄積が有限queue上限で停止することを確認する
	if( client.snapshot().state != ClientBridgeState::Failed ||
	   client.snapshot().error != ClientBridgeError::QueueLimitExceeded) {
		return 21 ;
	}

	// Phase 2 Agentとの交渉後だけworker Frameを双方向queueへrouteする
	ClientBridge stream_client ;
	FrameDecoder stream_decoder ;
	// stream転送試験のHELLO送信を準備できなければ失敗とする
	if( !stream_client.StartSession() ||
	   !DrainClientFrames( stream_client, stream_decoder, client_frames)) {
		return 22 ;
	}

	HelloCapabilities phase2 = zrdp::protocol::MakePhase2HelloCapabilities() ;
	phase2.maximum_streams	 = 1U ;
	Frame phase2_ack{} ;
	phase2_ack.type = MessageType::HelloAck ;
	// stream capabilityの共通bitが合意されることを確認する
	if( zrdp::protocol::EncodeHelloCapabilities( phase2, phase2_ack.payload) != FrameError::None ||
	   !DeliverAgentFrame( stream_client, phase2_ack) ||
	   (stream_client.negotiated_capabilities().feature_bits &
		zrdp::protocol::kFeatureMultiplexedStreams) == 0U) {
		return 23 ;
	}

	Frame open{} ;
	open.type	   = MessageType::Open ;
	open.stream_id = 1U ;
	open.payload   = {
		  std::byte{192U}, std::byte{0U}, std::byte{2U},
		  std::byte{10U},  std::byte{0U}, std::byte{80U},
	  } ;
	// OPENをSVC送信queueへ一件だけ移せることを確認する
	if( !stream_client.QueueStreamFrame( open) ||
	   !DrainClientFrames( stream_client, stream_decoder, client_frames) ||
	   client_frames.size() != 1U || client_frames.front().type != MessageType::Open) {
		return 24 ;
	}

	Frame open_ok{} ;
	open_ok.type	  = MessageType::OpenOk ;
	open_ok.stream_id = 1U ;
	// AgentのOPEN_OKをClient Bridgeが受理することを確認する
	if( !DeliverAgentFrame( stream_client, open_ok)) {
		return 25 ;
	}

	Frame inbound{} ;
	// OPEN_OKをworker向けqueueから一度だけ同じstream IDで取得できることを確認する
	if( !stream_client.TakeInboundStreamFrame( inbound) || inbound.type != MessageType::OpenOk ||
	   inbound.stream_id != 1U || stream_client.TakeInboundStreamFrame( inbound)) {
		return 26 ;
	}

	ClientBridge managed ;
	FrameDecoder managed_decoder ;
	// 新しい接続はHELLOの合意前にINIを送らない
	if( managed.QueueAgentConfiguration( "x") || !managed.StartSession() ||
	   !DrainClientFrames( managed, managed_decoder, client_frames)) {
		return 40 ;
	}

	HelloCapabilities lifecycle = zrdp::protocol::MakePhase2HelloCapabilities() ;
	lifecycle.feature_bits |= zrdp::protocol::kFeatureAgentLifecycle ;
	Frame managed_ack{} ;
	managed_ack.type = MessageType::HelloAck ;
	// 設定controlを合意した場合だけ有限INIを一回送る
	if( zrdp::protocol::EncodeHelloCapabilities( lifecycle, managed_ack.payload) !=
		   FrameError::None ||
	   !DeliverAgentFrame( managed, managed_ack) || managed.QueueAgentConfiguration( "") ||
	   managed.QueueAgentConfiguration( std::string( 65537U, 'x')) ||
	   !managed.QueueAgentConfiguration( "[allowlist]\nendpoint.1=192.0.2.10:80\n") ||
	   managed.QueueAgentConfiguration( "again") || managed.QueueStreamFrame( open)) {
		return 41 ;
	}

	Frame configured{} ;
	configured.type		= MessageType::AgentConfigureAck ;
	configured.sequence = 1U ;
	configured.payload	= {std::byte{1}} ;
	// 一度だけ正しいACKを受け、旧設定待ちを解除する
	if( !DeliverAgentFrame( managed, configured) || managed.snapshot().configuration_status != 1U ||
	   DeliverAgentFrame( managed, configured)) {
		return 42 ;
	}

	// 新HELLOで設定ACKの所有を破棄する
	if( !managed.StartSession() || !DeliverAgentFrame( managed, managed_ack) ||
	   managed.snapshot().configuration_status != 0U || !managed.QueueAgentStop() ||
	   managed.QueueAgentStop()) {
		return 43 ;
	}

	// 自身の協調停止後の遅延CLOSEだけを捨て、利用可能に戻さない
	if( !DeliverAgentFrame( managed, configured) ||
	   managed.snapshot().state != ClientBridgeState::Stopped) {
		return 44 ;
	}

	// 設定拒否直後の正常CLOSEも拒否を消して復旧へ戻さない
	if( !managed.StartSession() || !DeliverAgentFrame( managed, managed_ack) ||
	   !managed.QueueAgentConfiguration( "valid-fixture")) {
		return 45 ;
	}

	configured.payload = {std::byte{3}} ;
	Frame rejected_close{} ;
	rejected_close.type = MessageType::Close ;
	// 正常な拒否ACKとCLOSEはprotocol errorではなくfail-closedな停止として残す
	if( !DeliverAgentFrame( managed, configured) || DeliverAgentFrame( managed, rejected_close) ||
	   managed.snapshot().state != ClientBridgeState::Failed ||
	   managed.snapshot().configuration_status != 3U) {
		return 46 ;
	}

	return 0 ;
}
