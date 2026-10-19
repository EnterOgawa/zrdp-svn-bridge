//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file multiplexer_test.cpp
 * @brief Phase 2 stream状態、flow control、bounded queue、異常遷移を検証する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "protocol/multiplexer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace {

using zrdp::protocol::Frame ;
using zrdp::protocol::MultiplexerResult ;
using zrdp::protocol::OpenTarget ;
using zrdp::protocol::StreamEvent ;
using zrdp::protocol::StreamMultiplexer ;

/**
 * @brief sourceの全Frameをwire順のままdestinationへ渡す。
 * @param[in,out] source 送信queueを所有するmultiplexer。
 * @param[in,out] destination 受信状態機械。
 * @return 全Frameを正常処理できた場合はtrue。
 */
[[nodiscard]] bool TransferAll( StreamMultiplexer &source, StreamMultiplexer &destination)
{
	Frame frame{} ;
	// bounded queueが空になるまで所有Frameを一件ずつmoveする
	while( source.TakeOutboundFrame( frame)) {
		// 配送先のprotocol検証が失敗したら多重化helperを中止する
		if( destination.OnFrame( std::move( frame)) != MultiplexerResult::Ok) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief Client OPENからAgent受理までを実行して確立済みstreamを返す。
 * @param[in,out] client Client role。
 * @param[in,out] agent Agent role。
 * @param[in] target 文書用IPv4 target。
 * @param[out] stream_id 成功時の非0 ID。
 * @return OPEN eventとOPEN_OK eventを確認できた場合はtrue。
 */
[[nodiscard]] bool OpenAndAccept( StreamMultiplexer &client, StreamMultiplexer &agent,
								 const OpenTarget &target, std::uint32_t &stream_id)
{
	// OPEN生成・非0 ID割当・Agent配送が成立することを確認する
	if( client.OpenStream( target, stream_id) != MultiplexerResult::Ok || stream_id == 0U ||
	   !TransferAll( client, agent)) {
		return false ;
	}

	StreamEvent event{} ;
	// Agentが同じID・targetでOPENを受け取り、許可応答を返すことを確認する
	if( !agent.TakeEvent( event) || event.type != zrdp::protocol::StreamEventType::OpenRequested ||
	   event.stream_id != stream_id || event.target != target ||
	   agent.AcceptStream( stream_id) != MultiplexerResult::Ok || !TransferAll( agent, client)) {
		return false ;
	}

	// ClientのOpened eventが要求したstream IDへ一致することを確認する
	if( !client.TakeEvent( event) || event.type != zrdp::protocol::StreamEventType::Opened ||
	   event.stream_id != stream_id) {
		return false ;
	}

	return true ;
}

/**
 * @brief 32-bit値をtest用big-endian payloadへ変換する。
 * @param[in] value encodeする値。
 * @return 4-byte payload。
 */
[[nodiscard]] std::vector<std::byte> EncodeUint32( const std::uint32_t value)
{
	return {
		static_cast<std::byte>( (value >> 24U) & 0xFFU),
		static_cast<std::byte>( (value >> 16U) & 0xFFU),
		static_cast<std::byte>( (value >> 8U) & 0xFFU),
		static_cast<std::byte>( value & 0xFFU),
	} ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief stream multiplexerの状態遷移、flow control、上限を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain()
{
	using zrdp::protocol::HelloCapabilities ;
	using zrdp::protocol::MakePhase2HelloCapabilities ;
	using zrdp::protocol::MessageType ;
	using zrdp::protocol::MultiplexerError ;
	using zrdp::protocol::MultiplexerRole ;
	using zrdp::protocol::MultiplexerState ;
	using zrdp::protocol::OpenErrorCode ;
	using zrdp::protocol::StreamCloseReason ;
	using zrdp::protocol::StreamEventType ;
	using zrdp::protocol::StreamState ;

	const OpenTarget first_target{
		{192U, 0U, 2U, 10U},
		80U,
	} ;

	HelloCapabilities phase1_only{} ;
	StreamMultiplexer invalid_client( MultiplexerRole::Client) ;
	// stream featureなしのPhase 1 capabilityでは多重化sessionを開始しない
	if( invalid_client.StartSession( phase1_only) != MultiplexerResult::InvalidArgument ||
	   invalid_client.snapshot().state != MultiplexerState::Stopped ||
	   invalid_client.snapshot().error != MultiplexerError::InvalidCapabilities) {
		return 1 ;
	}

	const HelloCapabilities capabilities = MakePhase2HelloCapabilities() ;
	StreamMultiplexer		client( MultiplexerRole::Client) ;
	StreamMultiplexer		agent( MultiplexerRole::Agent) ;
	// 双方の多重化sessionが開始できなければ通常転送試験を止める
	if( client.StartSession( capabilities) != MultiplexerResult::Ok ||
	   agent.StartSession( capabilities) != MultiplexerResult::Ok) {
		return 2 ;
	}

	std::uint32_t first_stream = 0U ;
	// 基本streamをOPEN・許可できなければDATA検証へ進めない
	if( !OpenAndAccept( client, agent, first_target, first_stream)) {
		return 3 ;
	}

	StreamState stream_state{} ;
	// OPEN往復後に両端のstream状態がOpenとなることを確認する
	if( !client.TryGetStreamState( first_stream, stream_state) || stream_state != StreamState::Open ||
	   !agent.TryGetStreamState( first_stream, stream_state) || stream_state != StreamState::Open) {
		return 4 ;
	}

	constexpr std::array<std::byte, 8> data{
		std::byte{0x00U}, std::byte{0x01U}, std::byte{0x02U}, std::byte{0x03U},
		std::byte{0x7FU}, std::byte{0x80U}, std::byte{0xFEU}, std::byte{0xFFU},
	} ;
	// 基本DATAの生成とAgentへの配送が成立することを確認する
	if( client.SendData( first_stream, data) != MultiplexerResult::Ok ||
	   !TransferAll( client, agent)) {
		return 5 ;
	}

	StreamEvent event{} ;
	// DATA eventのID・長さ・全byteが元dataと一致することを確認する
	if( !agent.TakeEvent( event) || event.type != StreamEventType::DataReceived ||
	   event.stream_id != first_stream || event.data.size() != data.size() ||
	   !std::equal( data.begin(), data.end(), event.data.begin())) {
		return 6 ;
	}

	// downstream消費後にstream／connection両方のcreditを返す
	if( agent.ConsumeReceivedData( first_stream, static_cast<std::uint32_t>( data.size())) !=
		   MultiplexerResult::Ok ||
	   !TransferAll( agent, client)) {
		return 7 ;
	}

	HelloCapabilities small_window	   = MakePhase2HelloCapabilities() ;
	small_window.maximum_streams	   = 2U ;
	small_window.initial_window_bytes  = 8U ;
	small_window.maximum_frame_payload = 8U ;
	StreamMultiplexer limited_client( MultiplexerRole::Client) ;
	StreamMultiplexer limited_agent( MultiplexerRole::Agent) ;
	// 小windowのsessionを開始できなければbackpressure試験を止める
	if( limited_client.StartSession( small_window) != MultiplexerResult::Ok ||
	   limited_agent.StartSession( small_window) != MultiplexerResult::Ok) {
		return 8 ;
	}

	std::uint32_t limited_stream = 0U ;
	// windowを使い切ると追加DATAがWouldBlockとなることを確認する
	if( !OpenAndAccept( limited_client, limited_agent, first_target, limited_stream) ||
	   limited_client.SendData( limited_stream, data) != MultiplexerResult::Ok ||
	   limited_client.SendData( limited_stream, std::span<const std::byte>( data.data(), 1U)) !=
		   MultiplexerResult::WouldBlock ||
	   !TransferAll( limited_client, limited_agent)) {
		return 9 ;
	}

	// 消費creditの返却後だけ追加DATA送信が再開することを確認する
	if( !limited_agent.TakeEvent( event) || event.type != StreamEventType::DataReceived ||
	   limited_agent.ConsumeReceivedData( limited_stream, 8U) != MultiplexerResult::Ok ||
	   !TransferAll( limited_agent, limited_client) ||
	   limited_client.SendData( limited_stream, std::span<const std::byte>( data.data(), 1U)) !=
		   MultiplexerResult::Ok) {
		return 10 ;
	}

	// peer HALF_CLOSE後も反対方向のDATA送信は継続できる
	if( client.HalfCloseStream( first_stream) != MultiplexerResult::Ok ||
	   !TransferAll( client, agent) || !agent.TakeEvent( event) ||
	   event.type != StreamEventType::RemoteHalfClosed ||
	   agent.SendData( first_stream, data) != MultiplexerResult::Ok || !TransferAll( agent, client) ||
	   !client.TakeEvent( event) || event.type != StreamEventType::DataReceived) {
		return 11 ;
	}

	// peer FINでlocal終了済みstreamが両方向half-closeへ遷移することを確認する
	if( agent.HalfCloseStream( first_stream) != MultiplexerResult::Ok ||
	   !TransferAll( agent, client) || !client.TakeEvent( event) ||
	   event.type != StreamEventType::RemoteHalfClosed ||
	   !client.TryGetStreamState( first_stream, stream_state) ||
	   stream_state != StreamState::BothHalfClosed) {
		return 12 ;
	}

	// 両方向終了後のnormal CLOSE request／ackで両端からrecordを除去する
	if( client.CloseStream( first_stream, StreamCloseReason::Normal) != MultiplexerResult::Ok ||
	   !TransferAll( client, agent) || !agent.TakeEvent( event) ||
	   event.type != StreamEventType::Closed || !TransferAll( agent, client) ||
	   !client.TakeEvent( event) || event.type != StreamEventType::Closed ||
	   client.TryGetStreamState( first_stream, stream_state) ||
	   agent.TryGetStreamState( first_stream, stream_state)) {
		return 13 ;
	}

	// 未送信byteへのcredit水増しはconnection全体をfail-closedにする
	StreamMultiplexer inflation_client( MultiplexerRole::Client) ;
	StreamMultiplexer inflation_agent( MultiplexerRole::Agent) ;
	// credit水増し拒否試験のsessionを準備できなければ失敗とする
	if( inflation_client.StartSession( capabilities) != MultiplexerResult::Ok ||
	   inflation_agent.StartSession( capabilities) != MultiplexerResult::Ok) {
		return 14 ;
	}

	std::uint32_t inflation_stream = 0U ;
	// credit違反を入力するstreamを開けなければ検証を止める
	if( !OpenAndAccept( inflation_client, inflation_agent, first_target, inflation_stream)) {
		return 15 ;
	}

	Frame invalid_update{} ;
	invalid_update.type		 = MessageType::WindowUpdate ;
	invalid_update.stream_id = inflation_stream ;
	invalid_update.payload	 = EncodeUint32( 1U) ;
	// 未確認送信量を越えるWINDOW_UPDATEが固定flow違反で拒否されることを確認する
	if( inflation_client.OnFrame( std::move( invalid_update)) != MultiplexerResult::Failed ||
	   inflation_client.snapshot().error != MultiplexerError::FlowControlViolation) {
		return 16 ;
	}

	Frame protocol_error{} ;
	// 違反後にconnection-level PROTOCOL_ERRORを生成することを確認する
	if( !inflation_client.TakeOutboundFrame( protocol_error) ||
	   protocol_error.type != MessageType::ProtocolError || protocol_error.stream_id != 0U) {
		return 17 ;
	}

	// peerはPROTOCOL_ERRORを受信して応答loopを作らず停止する
	if( inflation_agent.OnFrame( std::move( protocol_error)) != MultiplexerResult::Failed ||
	   inflation_agent.snapshot().error != MultiplexerError::PeerProtocolError ||
	   inflation_agent.TakeOutboundFrame( protocol_error)) {
		return 32 ;
	}

	// 同一OPEN frameの再送はstream ID collisionとして停止する
	StreamMultiplexer duplicate_client( MultiplexerRole::Client) ;
	StreamMultiplexer duplicate_agent( MultiplexerRole::Agent) ;
	// 重複OPEN試験のsessionを準備できなければ失敗とする
	if( duplicate_client.StartSession( capabilities) != MultiplexerResult::Ok ||
	   duplicate_agent.StartSession( capabilities) != MultiplexerResult::Ok) {
		return 18 ;
	}

	std::uint32_t duplicate_stream = 0U ;
	// 重複させるOPENを生成できなければ検証を止める
	if( duplicate_client.OpenStream( first_target, duplicate_stream) != MultiplexerResult::Ok) {
		return 19 ;
	}

	Frame open_frame{} ;
	// 元OPEN frameを取得できなければ同一ID再送を再現しない
	if( !duplicate_client.TakeOutboundFrame( open_frame)) {
		return 20 ;
	}

	const Frame duplicate_frame = open_frame ;
	// 同じOPENの二度目がDuplicateStreamIdで拒否されることを確認する
	if( duplicate_agent.OnFrame( std::move( open_frame)) != MultiplexerResult::Ok ||
	   duplicate_agent.OnFrame( duplicate_frame) != MultiplexerResult::Failed ||
	   duplicate_agent.snapshot().error != MultiplexerError::DuplicateStreamId) {
		return 21 ;
	}

	// outbound frame件数上限ではsessionを壊さずWouldBlockを返す
	HelloCapabilities queue_capabilities	 = MakePhase2HelloCapabilities() ;
	queue_capabilities.maximum_streams		 = 1U ;
	queue_capabilities.initial_window_bytes	 = zrdp::protocol::kMaximumNegotiableWindowBytes ;
	queue_capabilities.maximum_frame_payload = 1U ;
	StreamMultiplexer queue_client( MultiplexerRole::Client) ;
	StreamMultiplexer queue_agent( MultiplexerRole::Agent) ;
	// 送信queue上限試験のsession初期化が失敗したら検証を止める
	if( queue_client.StartSession( queue_capabilities) != MultiplexerResult::Ok ||
	   queue_agent.StartSession( queue_capabilities) != MultiplexerResult::Ok) {
		return 22 ;
	}

	std::uint32_t queue_stream = 0U ;
	// queueを埋めるstreamを開けなければ上限試験を開始しない
	if( !OpenAndAccept( queue_client, queue_agent, first_target, queue_stream)) {
		return 23 ;
	}

	constexpr std::array<std::byte, 1> one_byte{
		std::byte{0x5AU},
	} ;
	// 4096件の固定上限まで1-byte DATAをqueueし、件数境界を正確に満たす
	for( std::size_t index = 0U; index < zrdp::protocol::kMaximumMultiplexerOutboundFrames;
		++index) {
		// 有限queue上限までは一byte DATAを順に生成できることを確認する
		if( queue_client.SendData( queue_stream, one_byte) != MultiplexerResult::Ok) {
			return 24 ;
		}
	}

	// 送信queue満杯ではReadyを維持したままWouldBlockを返すことを確認する
	if( queue_client.SendData( queue_stream, one_byte) != MultiplexerResult::WouldBlock ||
	   queue_client.snapshot().state != MultiplexerState::Ready ||
	   queue_client.snapshot().outbound_frames !=
		   zrdp::protocol::kMaximumMultiplexerOutboundFrames) {
		return 25 ;
	}

	// receiver event件数上限も独立して検証し、超過時はfail-closedにする
	if( !TransferAll( queue_client, queue_agent) ||
	   queue_client.SendData( queue_stream, one_byte) != MultiplexerResult::Ok) {
		return 33 ;
	}

	Frame excessive_event{} ;
	// 受信event queue上限では固定QueueLimitExceededで停止することを確認する
	if( !queue_client.TakeOutboundFrame( excessive_event) ||
	   queue_agent.OnFrame( std::move( excessive_event)) != MultiplexerResult::Failed ||
	   queue_agent.snapshot().error != MultiplexerError::QueueLimitExceeded) {
		return 34 ;
	}

	// OPEN待機中のtimeoutを固定reason CLOSEとしてpeerへ通知する
	StreamMultiplexer timeout_client( MultiplexerRole::Client) ;
	StreamMultiplexer timeout_agent( MultiplexerRole::Agent) ;
	// timeout終了試験のsessionを開始できなければ失敗とする
	if( timeout_client.StartSession( capabilities) != MultiplexerResult::Ok ||
	   timeout_agent.StartSession( capabilities) != MultiplexerResult::Ok) {
		return 26 ;
	}

	std::uint32_t timeout_stream = 0U ;
	// pending OPENをtimeout CLOSEとACKで安全に終了できることを確認する
	if( timeout_client.OpenStream( first_target, timeout_stream) != MultiplexerResult::Ok ||
	   !TransferAll( timeout_client, timeout_agent) || !timeout_agent.TakeEvent( event) ||
	   timeout_client.CloseStream( timeout_stream, StreamCloseReason::Timeout) !=
		   MultiplexerResult::Ok ||
	   !TransferAll( timeout_client, timeout_agent) || !timeout_agent.TakeEvent( event) ||
	   event.type != StreamEventType::Closed || event.close_reason != StreamCloseReason::Timeout ||
	   !TransferAll( timeout_agent, timeout_client) || !timeout_client.TakeEvent( event) ||
	   event.type != StreamEventType::Closed) {
		return 27 ;
	}

	// session resetは旧ID、sequence、queueを破棄し、新sessionをID 1から開始する
	timeout_client.StopSession() ;
	// 再開始したsessionで旧stream ID状態を保持していないことを確認する
	if( timeout_client.StartSession( capabilities) != MultiplexerResult::Ok) {
		return 28 ;
	}

	std::uint32_t reset_stream = 0U ;
	// 新sessionではstream IDが1から新規割当されることを確認する
	if( timeout_client.OpenStream( first_target, reset_stream) != MultiplexerResult::Ok ||
	   reset_stream != 1U) {
		return 29 ;
	}

	// Agent roleからのOPEN開始を明示的に拒否する
	std::uint32_t forbidden_stream = 0U ;
	// Agent起点OPENがInvalidRoleとして拒否されることを確認する
	if( timeout_agent.OpenStream( first_target, forbidden_stream) != MultiplexerResult::InvalidRole) {
		return 30 ;
	}

	// 未定義OPEN errorをwireへ出力しない
	if( timeout_agent.RejectStream( timeout_stream, static_cast<OpenErrorCode>( 0U)) !=
	   MultiplexerResult::InvalidArgument) {
		return 31 ;
	}

	// connection windowは全stream共有で4 MiBを越えず、一stream消費で回復する
	HelloCapabilities connection_capabilities	 = MakePhase2HelloCapabilities() ;
	connection_capabilities.maximum_streams		 = 2U ;
	connection_capabilities.initial_window_bytes = 3U * 1024U * 1024U ;
	StreamMultiplexer connection_client( MultiplexerRole::Client) ;
	StreamMultiplexer connection_agent( MultiplexerRole::Agent) ;
	// connection共通credit試験のsessionを開始できなければ失敗とする
	if( connection_client.StartSession( connection_capabilities) != MultiplexerResult::Ok ||
	   connection_agent.StartSession( connection_capabilities) != MultiplexerResult::Ok) {
		return 35 ;
	}

	const OpenTarget second_target{
		{198U, 51U, 100U, 20U},
		8080U,
	} ;
	std::uint32_t connection_stream1 = 0U ;
	std::uint32_t connection_stream2 = 0U ;
	// 二つのstreamを開いて共通windowの競合を準備できることを確認する
	if( !OpenAndAccept( connection_client, connection_agent, first_target, connection_stream1) ||
	   !OpenAndAccept( connection_client, connection_agent, second_target, connection_stream2)) {
		return 36 ;
	}

	const std::vector<std::byte> full_frame( zrdp::protocol::kMaximumFramePayloadBytes,
											std::byte{0xA5U}) ;
	// 第一streamで3 MiB、第二streamで1 MiBを送りconnection creditを使い切る
	for( std::size_t index = 0U; index < 48U; ++index) {
		// 第一streamで共通creditを所定量消費できることを確認する
		if( connection_client.SendData( connection_stream1, full_frame) != MultiplexerResult::Ok ||
		   !TransferAll( connection_client, connection_agent)) {
			return 37 ;
		}
	}

	// 残り1 MiBを第二streamから送り、connection全体のcreditを正確に枯渇させる
	for( std::size_t index = 0U; index < 16U; ++index) {
		// 第二streamも残る共通credit内で転送できることを確認する
		if( connection_client.SendData( connection_stream2, full_frame) != MultiplexerResult::Ok ||
		   !TransferAll( connection_client, connection_agent)) {
			return 38 ;
		}
	}

	// stream creditが残ってもconnection credit枯渇でWouldBlockとなることを確認する
	if( connection_client.snapshot().connection_send_window != 0U ||
	   connection_client.SendData( connection_stream2, one_byte) != MultiplexerResult::WouldBlock) {
		return 39 ;
	}

	// 一eventをdownstream消費すると両scopeのWINDOW_UPDATEで送信を再開できる
	if( !connection_agent.TakeEvent( event) || event.type != StreamEventType::DataReceived ||
	   connection_agent.ConsumeReceivedData( event.stream_id,
											static_cast<std::uint32_t>( event.data.size())) !=
		   MultiplexerResult::Ok ||
	   !TransferAll( connection_agent, connection_client) ||
	   connection_client.SendData( connection_stream2, one_byte) != MultiplexerResult::Ok) {
		return 40 ;
	}

	// 双方が同時にCLOSEしてもrequest／ackを一回ずつ処理してrecordを残さない
	StreamMultiplexer crossed_client( MultiplexerRole::Client) ;
	StreamMultiplexer crossed_agent( MultiplexerRole::Agent) ;
	// crossed CLOSE試験のsessionを準備できなければ失敗とする
	if( crossed_client.StartSession( capabilities) != MultiplexerResult::Ok ||
	   crossed_agent.StartSession( capabilities) != MultiplexerResult::Ok) {
		return 41 ;
	}

	std::uint32_t crossed_stream = 0U ;
	// 両端から交差するCLOSE requestとACKを配送できることを確認する
	if( !OpenAndAccept( crossed_client, crossed_agent, first_target, crossed_stream) ||
	   crossed_client.CloseStream( crossed_stream, StreamCloseReason::Timeout) !=
		   MultiplexerResult::Ok ||
	   crossed_agent.CloseStream( crossed_stream, StreamCloseReason::PeerReset) !=
		   MultiplexerResult::Ok ||
	   !TransferAll( crossed_client, crossed_agent) || !TransferAll( crossed_agent, crossed_client) ||
	   !TransferAll( crossed_client, crossed_agent)) {
		return 42 ;
	}

	// 相手の終了理由を保持し、双方のstreamが完全回収されることを確認する
	if( !crossed_client.TakeEvent( event) || event.type != StreamEventType::Closed ||
	   event.close_reason != StreamCloseReason::PeerReset || !crossed_agent.TakeEvent( event) ||
	   event.type != StreamEventType::Closed || event.close_reason != StreamCloseReason::Timeout ||
	   crossed_client.snapshot().active_streams != 0U ||
	   crossed_agent.snapshot().active_streams != 0U) {
		return 43 ;
	}

	// DATAの重複sequenceは同じpayloadでも再適用せずsessionを停止する
	StreamMultiplexer sequence_client( MultiplexerRole::Client) ;
	StreamMultiplexer sequence_agent( MultiplexerRole::Agent) ;
	// 重複DATA sequence試験のsessionを準備できなければ失敗とする
	if( sequence_client.StartSession( capabilities) != MultiplexerResult::Ok ||
	   sequence_agent.StartSession( capabilities) != MultiplexerResult::Ok) {
		return 44 ;
	}

	std::uint32_t sequence_stream = 0U ;
	// 重複させるstreamとDATAを生成できることを確認する
	if( !OpenAndAccept( sequence_client, sequence_agent, first_target, sequence_stream) ||
	   sequence_client.SendData( sequence_stream, data) != MultiplexerResult::Ok) {
		return 45 ;
	}

	Frame sequence_frame{} ;
	// 元DATA frameを取得できなければsequence再送を再現しない
	if( !sequence_client.TakeOutboundFrame( sequence_frame)) {
		return 46 ;
	}

	const Frame repeated_sequence = sequence_frame ;
	// 同じDATA sequenceの再受信がSequenceMismatchで拒否されることを確認する
	if( sequence_agent.OnFrame( std::move( sequence_frame)) != MultiplexerResult::Ok ||
	   sequence_agent.OnFrame( repeated_sequence) != MultiplexerResult::Failed ||
	   sequence_agent.snapshot().error != MultiplexerError::SequenceMismatch) {
		return 47 ;
	}

	// Agentのpolicy拒否は固定OPEN_ERRORだけをClient eventへ返す
	StreamMultiplexer rejection_client( MultiplexerRole::Client) ;
	StreamMultiplexer rejection_agent( MultiplexerRole::Agent) ;
	// policy拒否往復試験のsessionを準備できなければ失敗とする
	if( rejection_client.StartSession( capabilities) != MultiplexerResult::Ok ||
	   rejection_agent.StartSession( capabilities) != MultiplexerResult::Ok) {
		return 48 ;
	}

	std::uint32_t rejected_stream = 0U ;
	// OPEN_ERRORの固定policy分類がClient eventへ保持されることを確認する
	if( rejection_client.OpenStream( first_target, rejected_stream) != MultiplexerResult::Ok ||
	   !TransferAll( rejection_client, rejection_agent) || !rejection_agent.TakeEvent( event) ||
	   rejection_agent.RejectStream( rejected_stream, OpenErrorCode::RejectedByPolicy) !=
		   MultiplexerResult::Ok ||
	   !TransferAll( rejection_agent, rejection_client) || !rejection_client.TakeEvent( event) ||
	   event.type != StreamEventType::OpenFailed ||
	   event.open_error != OpenErrorCode::RejectedByPolicy ||
	   rejection_client.snapshot().active_streams != 0U ||
	   rejection_agent.snapshot().active_streams != 0U) {
		return 49 ;
	}

	return 0 ;
}
