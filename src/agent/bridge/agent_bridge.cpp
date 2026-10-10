//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_bridge.cpp
 * @brief Agent側HELLO_ACK、binary PONG、Phase 5 stream routingを実装する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details Clientだけがprotocol sessionを開始し、Agentから新規streamや任意接続を
 *          開始しない。payloadは同一byte列として返す以外に解釈・記録しない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  限定設定controlとACK前の中継制限を追加。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "agent/bridge/agent_bridge.h"

#include "agent/bridge/agent_stream_relay.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace zrdp::agent::bridge {
namespace {

// Agent writer queueが同時保持できるtransport chunk件数
constexpr std::size_t kMaximumOutboundChunks = 256U ;

// Agent writer queueが所有できるbinary data総量
constexpr std::size_t kMaximumOutboundQueuedBytes = 256U * 1024U ;

} // namespace

void AgentBridge::EnableConfigurationControl( bool enabled) noexcept
{
	configuration_control_ = enabled ;
	Reset() ;
}

bool AgentBridge::StopRequested() const noexcept
{
	return stop_requested_ ;
}

bool AgentBridge::ConfigurationReady() const noexcept
{
	return configuration_ready_ && !stop_requested_ ;
}

bool AgentBridge::TakeConfiguration( protocol::Frame &request) noexcept
{
	// 未処理の一件以外をINIとして返さない
	if( !configuration_request_) {
		return false ;
	}

	request = std::move( *configuration_request_) ;
	configuration_request_.reset() ;
	return true ;
}

bool AgentBridge::HandleConfigurationControl( protocol::Frame frame) noexcept
{
	// control featureの交渉とconnection-level方向を必須にする
	if( !configuration_control_ || state_ != AgentBridgeState::Ready || stop_requested_ ||
	   frame.stream_id != 0U ||
	   (negotiated_capabilities_.feature_bits & protocol::kFeatureAgentLifecycle) == 0U) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::UnexpectedMessage) ;
		return false ;
	}

	// 空payload・固定sequenceだけの停止を通常のCLOSEと区別する
	if( frame.type == protocol::MessageType::AgentStop) {
		// 追加引数や任意commandを停止要求に含めない
		if( frame.sequence != 0U || !frame.payload.empty()) {
			Fail( AgentBridgeState::Failed, AgentBridgeError::UnexpectedMessage) ;
			return false ;
		}

		stop_requested_		 = true ;
		configuration_ready_ = false ;
		// 設定撤回後は既存Socketも即時停止する
		if( stream_relay_ != nullptr) {
			stream_relay_->StopSession() ;
		}

		return true ;
	}

	// 一HELLO一設定、最大一frameだけを保持する
	if( frame.sequence != 1U || configuration_received_ || frame.payload.empty() ||
	   frame.payload.size() > negotiated_capabilities_.maximum_frame_payload) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::UnexpectedMessage) ;
		return false ;
	}

	configuration_received_ = true ;
	configuration_ready_	= false ;
	configuration_request_	= std::move( frame) ;
	return true ;
}

bool AgentBridge::CompleteConfiguration( std::uint8_t status) noexcept
{
	// 未受理・処理前・不正結果を設定反映成功として送信しない
	if( !configuration_received_ || configuration_request_ || configuration_ready_ ||
	   configuration_completed_ || stop_requested_ || state_ != AgentBridgeState::Ready ||
	   status < 1U || status > 3U) {
		return false ;
	}

	protocol::Frame frame{} ;
	frame.type	   = protocol::MessageType::AgentConfigureAck ;
	frame.sequence = 1U ;
	try {
		frame.payload.push_back( static_cast<std::byte>( status)) ;
		// 完全queueできた後だけrelayを利用可能にする
		if( !QueueFrame( frame)) {
			return false ;
		}

		configuration_ready_	 = status == 1U ;
		configuration_completed_ = true ;
		return true ;
	} catch( ...) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::QueueLimitExceeded) ;
		return false ;
	}
}

void AgentBridge::ConfigureStreamRelay( AgentStreamRelay *const relay) noexcept
{
	// 旧relayがactive Socketを保持し得るためpointer置換前にsessionを停止する
	// HELLO境界では旧設定で開始した全Socketを必ず閉じる
	if( stream_relay_ != nullptr) {
		stream_relay_->StopSession() ;
	}

	stream_relay_ = relay ;
	Reset() ;
}

void AgentBridge::Reset() noexcept
{
	// RDP再接続相当では旧streamとSocketを新HELLOへ持ち越さない
	if( stream_relay_ != nullptr) {
		stream_relay_->StopSession() ;
	}

	decoder_.Reset() ;
	local_capabilities_ = protocol::HelloCapabilities{} ;
	// Socket relayがある場合だけ多重化機能と有限stream上限を宣言する
	if( stream_relay_ != nullptr) {
		local_capabilities_					= protocol::MakePhase2HelloCapabilities() ;
		// Phase 5 production relayは有限8 Socket tableまでを同時所有する
		local_capabilities_.maximum_streams = kMaximumAgentRelayStreams ;
	}

	negotiated_capabilities_ = protocol::HelloCapabilities{} ;
	// 実runtimeでは毎HELLOで設定を確認し、旧設定のstreamを再利用しない
	if( configuration_control_) {
		local_capabilities_.feature_bits |= protocol::kFeatureAgentLifecycle ;
	}

	configuration_ready_	 = !configuration_control_ ;
	configuration_received_	 = false ;
	configuration_completed_ = false ;
	configuration_request_.reset() ;
	stop_requested_		= false ;
	state_				= AgentBridgeState::WaitingForHello ;
	error_				= AgentBridgeError::None ;
	last_ping_sequence_ = 0U ;
	echoed_bytes_		= 0U ;
	outbound_chunks_.clear() ;
	outbound_queued_bytes_ = 0U ;
}

bool AgentBridge::OnChannelData( const std::span<const std::byte> data) noexcept
{
	// error後の残dataを新しいframe境界として再利用しない
	if( state_ == AgentBridgeState::Failed || state_ == AgentBridgeState::Incompatible) {
		return false ;
	}

	std::vector<protocol::Frame> frames ;
	const protocol::FrameError	 decode_result = decoder_.Push( data, frames) ;
	// version不一致は一般破損と区別してClient／Agent交換を促す
	if( decode_result == protocol::FrameError::UnsupportedVersion) {
		Fail( AgentBridgeState::Incompatible, AgentBridgeError::DecodeFailed) ;
		return false ;
	}

	// magic、length、type等の異常後は暗黙再同期しない
	if( decode_result != protocol::FrameError::None) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::DecodeFailed) ;
		return false ;
	}

	// 一受信に連結されたframeをwire順のまま応答queueへ反映する
	for( protocol::Frame &frame : frames) {
		// 受信frameの役割に応じてhandshake、Echo、stream処理へ振り分ける
		switch( frame.type) {
		// Clientが開始したHELLOで新しいBridge sessionを交渉する
		case protocol::MessageType::Hello:
			// HELLOの検証・応答生成に失敗したら後続frameを処理しない
			if( !HandleHello( frame)) {
				return false ;
			}

			break ;

		// Clientの到達確認要求へ同じbinary payloadのPONGを返す
		case protocol::MessageType::Ping:
			// PINGの順序・状態・応答queueに異常があれば受信処理を終了する
			if( !HandlePing( frame)) {
				return false ;
			}

			break ;

		// 接続要求はallowlistを持つstream relayへ渡す
		case protocol::MessageType::Open:
		// 接続完了応答はstream状態機械で方向と状態を検証する
		case protocol::MessageType::OpenOk:
		// 接続拒否応答もstream状態機械の役割検証に従う
		case protocol::MessageType::OpenError:
		// binary転送はstreamごとのcreditとsequence検証へ渡す
		case protocol::MessageType::Data:
		// credit返却はstream状態機械に反映する
		case protocol::MessageType::WindowUpdate:
		// 一方向終了はstream relayでTCP half-closeへ対応させる
		case protocol::MessageType::HalfClose:
			// stream frameの検証または応答排出に失敗したらBridgeを継続しない
			if( !HandleStreamFrame( std::move( frame))) {
				return false ;
			}

			break ;

		// CLOSEはstream終了だけを受け付け、connection-levelは拒否する
		case protocol::MessageType::Close:
			// connection-level CLOSEはstream engineへ渡さない
			if( frame.stream_id == 0U) {
				Fail( AgentBridgeState::Failed, AgentBridgeError::UnexpectedMessage) ;
				return false ;
			}

			// stream終了処理に失敗したら後続のframeを扱わない
			if( !HandleStreamFrame( std::move( frame))) {
				return false ;
			}

			break ;

		// peerのprotocol異常通知をstream状態機械へ渡して停止させる
		case protocol::MessageType::ProtocolError:
			// protocol異常の処理失敗を受信側の終了値へ伝える
			if( !HandleStreamFrame( std::move( frame))) {
				return false ;
			}

			break ;

		// Agent宛てでないhandshake応答は役割違反として拒否する
		case protocol::MessageType::HelloAck:
		// 設定結果の送信開始はAgentだけに限定する
		case protocol::MessageType::AgentConfigureAck:
		// Agent起点でないEcho応答はClientから受け付けない
		case protocol::MessageType::Pong:
			// Agent起点のhandshake／Echo responseをClientから受け入れない
			Fail( AgentBridgeState::Failed, AgentBridgeError::UnexpectedMessage) ;
			return false ;
		// 承認済みINIだけをruntimeの限定設定境界へ渡す
		case protocol::MessageType::AgentConfigure:
		// 協調停止だけを受け付け、任意process操作は行わない
		case protocol::MessageType::AgentStop:
			// 未交渉・重複・不正設定要求では後続frameを処理しない
			if( !HandleConfigurationControl( std::move( frame))) {
				return false ;
			}

			break ;
		}
	}

	return true ;
}

bool AgentBridge::QueueSessionClose() noexcept
{
	// protocol失敗後のchannelへ正常終了frameを追加して原因を上書きしない
	if( state_ == AgentBridgeState::Failed || state_ == AgentBridgeState::Incompatible) {
		return false ;
	}

	// Agent終了通知より先に全upstream Socketを閉じる
	if( stream_relay_ != nullptr) {
		stream_relay_->StopSession() ;
	}

	protocol::Frame close{} ;
	close.type		= protocol::MessageType::Close ;
	close.stream_id = 0U ;
	close.sequence	= 0U ;
	return QueueFrame( close) ;
}

bool AgentBridge::PumpStreams( const std::uint64_t now_milliseconds) noexcept
{
	// Echo-only交渉またはHELLO前ではSocket機能を開始しない
	if( state_ != AgentBridgeState::Ready || stream_relay_ == nullptr || !configuration_ready_ ||
	   stop_requested_ ||
	   (negotiated_capabilities_.feature_bits & protocol::kFeatureMultiplexedStreams) == 0U) {
		return state_ != AgentBridgeState::Failed && state_ != AgentBridgeState::Incompatible ;
	}

	// Socket進捗またはchannel応答排出が失敗したらrelay全体を停止する
	if( !stream_relay_->Pump( now_milliseconds) || !DrainStreamFrames()) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::StreamRelayFailed) ;
		return false ;
	}

	return true ;
}

bool AgentBridge::TakeOutboundChunk( std::vector<std::byte> &chunk, bool &more_data) noexcept
{
	chunk.clear() ;
	more_data = false ;
	// 空queueではWTS writeを呼ばない
	if( outbound_chunks_.empty()) {
		return false ;
	}

	chunk = std::move( outbound_chunks_.front()) ;
	outbound_chunks_.pop_front() ;
	// 内部値破損時もsize_t underflowを起こさず安全側へ戻す
	if( outbound_queued_bytes_ >= chunk.size()) {
		outbound_queued_bytes_ -= chunk.size() ;
	} else {
		outbound_queued_bytes_ = 0U ;
	}

	more_data = !outbound_chunks_.empty() ;
	return true ;
}

AgentBridgeSnapshot AgentBridge::snapshot() const noexcept
{
	return AgentBridgeSnapshot{
		state_,
		error_,
		echoed_bytes_,
		last_ping_sequence_,
	} ;
}

bool AgentBridge::HandleHello( const protocol::Frame &frame) noexcept
{
	// HELLOをEcho sequenceと分離し、固定sequence 0だけ受け付ける
	if( frame.sequence != 0U) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::InvalidHello) ;
		return false ;
	}

	protocol::HelloCapabilities peer_capabilities{} ;
	// 不正なcapability payloadを交渉済みsessionとして採用しない
	if( protocol::DecodeHelloCapabilities( frame.payload, peer_capabilities) !=
	   protocol::FrameError::None) {
		Fail( AgentBridgeState::Incompatible, AgentBridgeError::InvalidHello) ;
		return false ;
	}

	protocol::HelloCapabilities negotiated{} ;
	// 双方のcapabilityが互換でなければBridgeを利用不可にする
	if( protocol::NegotiateHelloCapabilities( local_capabilities_, peer_capabilities, negotiated) !=
	   protocol::FrameError::None) {
		Fail( AgentBridgeState::Incompatible, AgentBridgeError::InvalidHello) ;
		return false ;
	}

	// 新HELLOは旧Socketを破棄し、共通stream featureがある場合だけrelayを開始する
	configuration_ready_	 = !configuration_control_ ;
	configuration_received_	 = false ;
	configuration_completed_ = false ;
	configuration_request_.reset() ;
	stop_requested_ = false ;
	// lifecycleを必要とする実Agentへ旧Clientからstreamを開始させない
	if( configuration_control_ &&
	   (negotiated.feature_bits & protocol::kFeatureAgentLifecycle) == 0U) {
		Fail( AgentBridgeState::Incompatible, AgentBridgeError::InvalidHello) ;
		return false ;
	}

	// 新HELLOでは旧allowlistのSocketを閉じてから設定確認を待つ
	if( stream_relay_ != nullptr) {
		stream_relay_->StopSession() ;
		// 多重化を合意してもrelay初期化が失敗したら利用可能へ遷移しない
		if( (negotiated.feature_bits & protocol::kFeatureMultiplexedStreams) != 0U &&
		   !stream_relay_->StartSession( negotiated)) {
			Fail( AgentBridgeState::Failed, AgentBridgeError::StreamRelayFailed) ;
			return false ;
		}
	}

	protocol::Frame hello_ack{} ;
	hello_ack.type	   = protocol::MessageType::HelloAck ;
	hello_ack.sequence = 0U ;
	// HELLO_ACKをencodeできなければ部分的な応答をqueueへ入れない
	if( protocol::EncodeHelloCapabilities( local_capabilities_, hello_ack.payload) !=
	   protocol::FrameError::None) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::QueueLimitExceeded) ;
		return false ;
	}

	negotiated_capabilities_ = negotiated ;
	// HELLO_ACKのqueue失敗時は新sessionをReadyとして公開しない
	if( !QueueFrame( hello_ack)) {
		return false ;
	}

	// HELLOを新しいPhase 1 sequence境界とし、Agent再起動とEcho再実行を同じ規則にする
	last_ping_sequence_ = 0U ;
	echoed_bytes_		= 0U ;
	state_				= AgentBridgeState::Ready ;
	error_				= AgentBridgeError::None ;
	return true ;
}

bool AgentBridge::HandleStreamFrame( protocol::Frame frame) noexcept
{
	// compatible HELLOと共通stream capabilityなしではupstream処理を開始しない
	if( state_ != AgentBridgeState::Ready || stream_relay_ == nullptr || !configuration_ready_ ||
	   stop_requested_ ||
	   (negotiated_capabilities_.feature_bits & protocol::kFeatureMultiplexedStreams) == 0U) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::UnexpectedMessage) ;
		return false ;
	}

	// stream状態更新または生成frameの排出失敗をBridge全体の失敗へ変換する
	if( !stream_relay_->OnFrame( std::move( frame)) || !DrainStreamFrames()) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::StreamRelayFailed) ;
		return false ;
	}

	return true ;
}

bool AgentBridge::DrainStreamFrames() noexcept
{
	// multiplexerが生成したFrameを既存の単一WTS writer queueへwire順で移す
	protocol::Frame frame{} ;
	// relayが公開する有限outbound queueを空になるまで直列化する
	while( stream_relay_ != nullptr && stream_relay_->TakeOutboundFrame( frame)) {
		// 有限送信queueにframeを追加できなければ残りを排出しない
		if( !QueueFrame( frame)) {
			return false ;
		}
	}

	return true ;
}

bool AgentBridge::HandlePing( const protocol::Frame &frame) noexcept
{
	// compatible HELLO前のPINGをAgent起点sessionとして受け入れない
	if( state_ != AgentBridgeState::Ready) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::UnexpectedMessage) ;
		return false ;
	}

	// 0はHELLO用、以降は欠落・重複・順序入替を完全一致で検出する
	if( frame.sequence == 0U || last_ping_sequence_ == std::numeric_limits<std::uint32_t>::max() ||
	   frame.sequence != last_ping_sequence_ + 1U) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::SequenceMismatch) ;
		return false ;
	}

	// Phase 1 Echoはbyte一致検証用の非空payloadだけを扱う
	if( frame.payload.empty()) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::UnexpectedMessage) ;
		return false ;
	}

	protocol::Frame pong{} ;
	pong.type	  = protocol::MessageType::Pong ;
	pong.sequence = frame.sequence ;
	pong.payload  = frame.payload ;
	// PONGをqueueへ入れられなければEchoの成功counterを進めない
	if( !QueueFrame( pong)) {
		return false ;
	}

	last_ping_sequence_ = frame.sequence ;
	echoed_bytes_ += static_cast<std::uint64_t>( frame.payload.size()) ;
	return true ;
}

bool AgentBridge::QueueFrame( const protocol::Frame &frame) noexcept
{
	std::vector<std::byte> encoded ;
	// frameをwire形式へ変換できなければ送信を開始しない
	if( protocol::EncodeFrame( frame, encoded) != protocol::FrameError::None) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::QueueLimitExceeded) ;
		return false ;
	}

	const std::size_t chunk_size =
		std::min( static_cast<std::size_t>( negotiated_capabilities_.transport_chunk_bytes),
				 protocol::kStaticVirtualChannelChunkBytes) ;
	// capability破損時に0除算や空chunk loopを起こさない
	if( chunk_size == 0U) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::InvalidHello) ;
		return false ;
	}

	const std::size_t required_chunks = (encoded.size() + chunk_size - 1U) / chunk_size ;
	// queue変更前に件数と総byte数の両上限を確認する
	if( required_chunks > kMaximumOutboundChunks - outbound_chunks_.size() ||
	   encoded.size() > kMaximumOutboundQueuedBytes - outbound_queued_bytes_) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::QueueLimitExceeded) ;
		return false ;
	}

	try {
		std::size_t offset = 0U ;
		// Application frameをMicrosoft SVC上限以下のwrite単位へ分割する
		while( offset < encoded.size()) {
			const std::size_t current_size = std::min( chunk_size, encoded.size() - offset) ;
			outbound_chunks_.emplace_back( encoded.begin() + static_cast<std::ptrdiff_t>( offset),
										  encoded.begin() +
											  static_cast<std::ptrdiff_t>( offset + current_size)) ;
			offset += current_size ;
		}
	} catch( const std::bad_alloc &) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::QueueLimitExceeded) ;
		return false ;
	} catch( ...) {
		Fail( AgentBridgeState::Failed, AgentBridgeError::QueueLimitExceeded) ;
		return false ;
	}

	outbound_queued_bytes_ += encoded.size() ;
	return true ;
}

void AgentBridge::Fail( const AgentBridgeState state, const AgentBridgeError error) noexcept
{
	state_ = state ;
	error_ = error ;
	// protocol異常後にupstream Socketだけが生存し続けないよう同期停止する
	if( stream_relay_ != nullptr) {
		stream_relay_->StopSession() ;
	}

	decoder_.Reset() ;
	outbound_chunks_.clear() ;
	outbound_queued_bytes_ = 0U ;
}

} // namespace zrdp::agent::bridge
