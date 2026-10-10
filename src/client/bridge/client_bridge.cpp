//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_bridge.cpp
 * @brief Client側HELLO retry、binary Echo、SVC送信queueを実装する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details payloadは比較と送信だけに使用し、文字列変換やlog出力を行わない。
 *          一度に一つのPINGだけを未応答とし、10 MiB検証でもmemoryとUI負荷を
 *          有限に保つ。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  限定設定controlとACK前の中継制限を追加。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/bridge/client_bridge.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace zrdp::client::bridge {
namespace {

// Echo検証が順に確認する論理data size
constexpr std::array<std::uint64_t, 5> kEchoTargetSizes{
	1U, 1024U, 64U * 1024U, 1024U * 1024U, 10U * 1024U * 1024U,
} ;

// 一つのPINGが保持するpayload上限。SVC chunkとは独立したframe payload
constexpr std::uint32_t			   kEchoPingPayloadBytes = 32U * 1024U ;

// endpointやHTTP情報を含まないAgent死活確認用の固定binary payload
constexpr std::array<std::byte, 8> kHealthCheckPayload{
	std::byte{0x5AU}, std::byte{0x52U}, std::byte{0x44U}, std::byte{0x50U},
	std::byte{0x48U}, std::byte{0x45U}, std::byte{0x41U}, std::byte{0x4CU},
} ;

// workerの4 MiB Frame queueをSVC単位へ分割して保持できるchunk件数
constexpr std::size_t				  kMaximumOutboundChunks = 4096U ;

// Frame headerとconnection controlの余白を含むSVC送信queue上限
constexpr std::size_t				  kMaximumOutboundQueuedBytes = 5U * 1024U * 1024U ;

// UI callbackからworkerへ保持できるstream Frame件数
constexpr std::size_t				  kMaximumInboundStreamFrames = 4096U ;

// UI callbackからworkerへ保持できるstream Frame byte合計
constexpr std::size_t				  kMaximumInboundStreamBytes = 4U * 1024U * 1024U ;

/**
 * @brief 全Echo targetのbyte合計をcompile timeで求める。
 * @return UI progressに使う固定総byte数。
 */
[[nodiscard]] constexpr std::uint64_t EchoTotalBytes() noexcept
{
	std::uint64_t total = 0U ;
	// size追加時もUI総量を同じ配列から導出する
	for( const std::uint64_t target_size : kEchoTargetSizes) {
		total += target_size ;
	}

	return total ;
}

} // namespace

ClientBridge::ClientBridge() noexcept
{
	local_capabilities_ = protocol::MakePhase2HelloCapabilities() ;
	local_capabilities_.feature_bits |= protocol::kFeatureAgentLifecycle ;
	local_capabilities_.maximum_streams = kMaximumClientBridgeStreams ;
}

bool ClientBridge::StartSession() noexcept
{
	StopSession() ;
	decoder_.Reset() ;
	state_				 = ClientBridgeState::WaitingForAgent ;
	error_				 = ClientBridgeError::None ;
	hello_attempt_count_ = 0U ;
	return QueueHelloFrame( protocol::MessageType::Hello) ;
}

void ClientBridge::StopSession() noexcept
{
	decoder_.Reset() ;
	ClearOutboundQueue() ;
	configuration_requested_ = false ;
	remote_stop_requested_	 = false ;
	configuration_status_	 = 0U ;
	ClearInboundStreamQueue() ;
	outstanding_payload_.clear() ;
	state_								  = ClientBridgeState::Stopped ;
	error_								  = ClientBridgeError::None ;
	hello_attempt_count_				  = 0U ;
	echo_target_index_					  = 0U ;
	echo_target_completed_bytes_		  = 0U ;
	echo_completed_bytes_				  = 0U ;
	next_sequence_						  = 1U ;
	outstanding_sequence_				  = 0U ;
	health_check_outstanding_			  = false ;
	discard_stale_frames_until_hello_ack_ = false ;
	negotiated_capabilities_			  = protocol::HelloCapabilities{} ;
}

bool ClientBridge::QueueHelloRetry() noexcept
{
	// HELLOはAgent streamをresetするため、ACK待機中以外へ周期送信しない
	if( state_ != ClientBridgeState::WaitingForAgent) {
		return false ;
	}

	return QueueHelloFrame( protocol::MessageType::Hello) ;
}

bool ClientBridge::BeginEchoTest() noexcept
{
	// handshake済みかつhealth応答待ちのないidle状態だけからEchoを開始する
	if( (state_ != ClientBridgeState::Ready && state_ != ClientBridgeState::EchoPassed) ||
	   health_check_outstanding_) {
		return false ;
	}

	outstanding_payload_.clear() ;
	echo_target_index_			 = 0U ;
	echo_target_completed_bytes_ = 0U ;
	echo_completed_bytes_		 = 0U ;
	next_sequence_				 = 1U ;
	outstanding_sequence_		 = 0U ;
	error_						 = ClientBridgeError::None ;
	state_						 = ClientBridgeState::EchoRunning ;
	// 各Echoの直前にHELLOをwire順で先行させ、Agent側sequenceを明示的に初期化する
	if( !QueueHelloFrame( protocol::MessageType::Hello)) {
		return false ;
	}

	return QueueNextEchoChunk() ;
}

bool ClientBridge::QueueHealthCheck() noexcept
{
	// 手動Echoや別PINGと重複させず、成立済みsessionだけを確認対象にする
	if( (state_ != ClientBridgeState::Ready && state_ != ClientBridgeState::EchoPassed) ||
	   health_check_outstanding_ || outstanding_sequence_ != 0U) {
		return false ;
	}

	// sequence 0はHELLO用に予約し、極端な長期稼働でもwrap後の再利用を許可しない
	if( next_sequence_ == 0U || next_sequence_ == std::numeric_limits<std::uint32_t>::max()) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::SequenceMismatch) ;
		return false ;
	}

	protocol::Frame ping{} ;
	ping.type	  = protocol::MessageType::Ping ;
	ping.sequence = next_sequence_ ;
	try {
		outstanding_payload_.assign( kHealthCheckPayload.begin(), kHealthCheckPayload.end()) ;
		ping.payload = outstanding_payload_ ;
	} catch( ...) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::QueueLimitExceeded) ;
		return false ;
	}

	outstanding_sequence_	  = next_sequence_ ;
	health_check_outstanding_ = true ;
	++next_sequence_ ;
	// queue失敗時はFailが未応答stateとpayloadを同時に破棄する
	return QueueFrame( ping) ;
}

bool ClientBridge::OnChannelData( const std::span<const std::byte> data) noexcept
{
	// 固定停止の受付後の遅延ACK／CLOSEを新しい接続へ流さない
	if( state_ == ClientBridgeState::Stopped && remote_stop_requested_) {
		return true ;
	}

	// RDP未接続時の遅延callbackを新sessionのdataとして受け入れない
	if( state_ == ClientBridgeState::Stopped || state_ == ClientBridgeState::Failed ||
	   state_ == ClientBridgeState::Incompatible) {
		return false ;
	}

	std::vector<protocol::Frame> frames ;
	const protocol::FrameError	 decode_result = decoder_.Push( data, frames) ;
	// version不一致は一般malformed inputと区別してUIへ非互換を示す
	if( decode_result == protocol::FrameError::UnsupportedVersion) {
		Fail( ClientBridgeState::Incompatible, ClientBridgeError::DecodeFailed) ;
		return false ;
	}

	// それ以外のframe異常も境界探索で復旧せずsessionを停止する
	if( decode_result != protocol::FrameError::None) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::DecodeFailed) ;
		return false ;
	}

	// 一つのcallbackに連結されたframeをwire順のまま処理する
	for( protocol::Frame &frame : frames) {
		// timeout前sessionの遅延Frameは新HELLO_ACK境界まで一切再適用しない
		if( state_ == ClientBridgeState::WaitingForAgent && discard_stale_frames_until_hello_ack_ &&
		   frame.type != protocol::MessageType::HelloAck) {
			continue ;
		}

		// 受信frameをhandshake、Echo、stream終了、worker向け転送へ分類する
		switch( frame.type) {
		// Agentのcapability応答でClient側のBridge交渉を完了する
		case protocol::MessageType::HelloAck:
			// HELLO_ACKの状態またはpayloadが不正なら後続frameを扱わない
			if( !HandleHelloAck( frame)) {
				return false ;
			}

			break ;

		// PONGを照合してEchoまたは死活確認の応答を完了する
		case protocol::MessageType::Pong:
			// 未応答要求と一致しないPONGは検証失敗として受信処理を終了する
			if( !HandlePong( frame)) {
				return false ;
			}

			break ;

		// peerのprotocol異常通知をworkerへ移譲してstreamを停止させる
		case protocol::MessageType::ProtocolError:
			// protocol異常frameを有限queueへ渡せなければ受信を継続しない
			if( !QueueInboundStreamFrame( std::move( frame))) {
				return false ;
			}

			break ;

		// stream CLOSEとAgent全体の終了通知をstream IDで区別する
		case protocol::MessageType::Close:
			// stream CLOSEはworkerへ、connection CLOSEは新HELLO境界へ分離する
			if( frame.stream_id != 0U) {
				// stream終了frameをworkerへ渡せなければ完了として扱わない
				if( !QueueInboundStreamFrame( std::move( frame))) {
					return false ;
				}

				break ;
			}

			// connection-level CLOSE後のdataを次sessionへ持ち越さない
			return HandleAgentClose( frame) ;
		// Agentからの新しいhandshake開始は役割違反として拒否する
		case protocol::MessageType::Hello:
		// 限定設定と停止はClientだけが開始する
		case protocol::MessageType::AgentConfigure:
		// Agentから自身の停止要求をClientへ送らせない
		case protocol::MessageType::AgentStop:
		// Agent起点の到達確認要求は製品protocolで許可しない
		case protocol::MessageType::Ping:
			// Agentからconnection control requestを開始させない
			Fail( ClientBridgeState::Failed, ClientBridgeError::UnexpectedMessage) ;
			return false ;
		// 同じHELLO世代の設定確認だけをProxy gateへ反映する
		case protocol::MessageType::AgentConfigureAck:
			// 古い要求・重複・不正結果では中継を開始しない
			if( !HandleConfigurationAck( frame)) {
				return false ;
			}

			break ;

		// Agent起点のOPENもworker状態機械の方向検証へ渡す
		case protocol::MessageType::Open:
		// upstream接続成功応答を対応streamのworker処理へ渡す
		case protocol::MessageType::OpenOk:
		// upstream接続拒否応答を固定error分類のままworkerへ渡す
		case protocol::MessageType::OpenError:
		// upstreamのbinary dataは内容を解析せずworkerへ移譲する
		case protocol::MessageType::Data:
		// upstreamのcredit返却をworker側の送信制御へ渡す
		case protocol::MessageType::WindowUpdate:
		// upstreamの一方向終了をworkerでlocal SocketのFINへ対応させる
		case protocol::MessageType::HalfClose:
			// stream frameを有限queueに入れられなければ受信処理を終了する
			if( !QueueInboundStreamFrame( std::move( frame))) {
				return false ;
			}

			break ;
		}
	}

	return true ;
}

void ClientBridge::OnTransportSendFailed() noexcept
{
	// transport失敗をRDP切断へ昇格せずBridgeだけを停止する
	if( state_ != ClientBridgeState::Stopped) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::TransportSendFailed) ;
	}
}

void ClientBridge::OnEchoTimeout() noexcept
{
	// 未応答PINGが存在するEcho中だけをtimeout対象とし、stale timerを無視する
	if( state_ == ClientBridgeState::EchoRunning && outstanding_sequence_ != 0U) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::ResponseTimeout) ;
	}
}

bool ClientBridge::OnHealthCheckTimeout() noexcept
{
	// stale timerや手動Echo timeoutを新しいAgent session開始へ誤適用しない
	if( (state_ != ClientBridgeState::Ready && state_ != ClientBridgeState::EchoPassed) ||
	   !health_check_outstanding_ || outstanding_sequence_ == 0U) {
		return false ;
	}

	// 旧streamとqueueを破棄してから同じRDP channel上で新HELLOを開始する
	const bool restarted				  = StartSession() ;
	// timeout直前にAgentが送った旧Frameは新HELLO_ACKまで破棄する
	discard_stale_frames_until_hello_ack_ = restarted ;
	return true ;
}

bool ClientBridge::TakeOutboundChunk( std::vector<std::byte> &chunk, bool &more_data) noexcept
{
	chunk.clear() ;
	more_data = false ;

	std::scoped_lock lock( outbound_mutex_) ;
	// queueが空ならCOM送信要求を生成しない
	if( outbound_chunks_.empty()) {
		return false ;
	}

	chunk = std::move( outbound_chunks_.front()) ;
	outbound_chunks_.pop_front() ;
	// 登録時と同じbyte数を差し引き、underflow時は安全側の0へ戻す
	if( outbound_queued_bytes_ >= chunk.size()) {
		outbound_queued_bytes_ -= chunk.size() ;
	} else {
		outbound_queued_bytes_ = 0U ;
	}

	more_data = !outbound_chunks_.empty() ;
	return true ;
}

bool ClientBridge::TakeInboundStreamFrame( protocol::Frame &frame) noexcept
{
	// worker向け受信queueが空なら所有frameを返さない
	if( inbound_stream_frames_.empty()) {
		return false ;
	}

	const std::size_t frame_bytes =
		protocol::kFrameHeaderBytes + inbound_stream_frames_.front().payload.size() ;
	frame = std::move( inbound_stream_frames_.front()) ;
	inbound_stream_frames_.pop_front() ;
	// queue登録時と同じ値を差し引き、破損時は安全側へ戻す
	if( inbound_stream_bytes_ >= frame_bytes) {
		inbound_stream_bytes_ -= frame_bytes ;
	} else {
		inbound_stream_bytes_ = 0U ;
	}

	return true ;
}

bool ClientBridge::QueueStreamFrame( const protocol::Frame &frame) noexcept
{
	// 設定確認待ちや拒否の間はworkerから新streamを送らない
	if( (negotiated_capabilities_.feature_bits & protocol::kFeatureAgentLifecycle) != 0U &&
	   configuration_status_ != 1U) {
		return false ;
	}

	// Echo中やhandshake前にworkerの旧stream Frameを新sessionへ送らない
	if( state_ != ClientBridgeState::Ready && state_ != ClientBridgeState::EchoPassed) {
		return false ;
	}

	const bool streams_enabled =
		(negotiated_capabilities_.feature_bits & protocol::kFeatureMultiplexedStreams) != 0U ;
	// Echo-only Agentへstream messageを送信しない
	if( !streams_enabled) {
		Fail( ClientBridgeState::Incompatible, ClientBridgeError::StreamRelayFailed) ;
		return false ;
	}

	// workerからconnection handshake／Echo messageを注入させない
	if( frame.type == protocol::MessageType::Hello ||
	   frame.type == protocol::MessageType::HelloAck || frame.type == protocol::MessageType::Ping ||
	   frame.type == protocol::MessageType::Pong ||
	   frame.type == protocol::MessageType::AgentConfigure ||
	   frame.type == protocol::MessageType::AgentConfigureAck ||
	   frame.type == protocol::MessageType::AgentStop) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::StreamRelayFailed) ;
		return false ;
	}

	return QueueFrame( frame) ;
}

void ClientBridge::OnStreamRelayFailed() noexcept
{
	// Bridge停止済み状態はworkerの遅延通知でFailedへ戻さない
	if( state_ != ClientBridgeState::Stopped) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::StreamRelayFailed) ;
	}
}

protocol::HelloCapabilities ClientBridge::negotiated_capabilities() const noexcept
{
	return negotiated_capabilities_ ;
}

ClientBridgeSnapshot ClientBridge::snapshot() const noexcept
{
	return ClientBridgeSnapshot{
		state_,
		error_,
		echo_completed_bytes_,
		EchoTotalBytes(),
		hello_attempt_count_,
		health_check_outstanding_,
		configuration_status_,
	} ;
}

bool ClientBridge::QueueAgentConfiguration( std::string_view ini) noexcept
{
	// HELLO成立前・未交渉・重複・上限外の設定は送信しない
	if( state_ != ClientBridgeState::Ready || configuration_requested_ || ini.empty() ||
	   ini.size() > negotiated_capabilities_.maximum_frame_payload ||
	   (negotiated_capabilities_.feature_bits & protocol::kFeatureAgentLifecycle) == 0U) {
		return false ;
	}

	try {
		protocol::Frame frame{} ;
		frame.type		 = protocol::MessageType::AgentConfigure ;
		frame.sequence	 = 1U ;
		// 設定controlだけがINIをbyte列へ写し、HTTP data planeは変更しない
		const auto bytes = std::as_bytes( std::span( ini.data(), ini.size())) ;
		frame.payload.assign( bytes.begin(), bytes.end()) ;
		// 完全queue前に応答待ちを公開しない
		if( !QueueFrame( frame)) {
			return false ;
		}

		configuration_requested_ = true ;
		configuration_status_	 = 0U ;
		return true ;
	} catch( ...) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::EncodeFailed) ;
		return false ;
	}
}

bool ClientBridge::QueueAgentStop() noexcept
{
	// 対象との合意がない停止要求は別経路へ送らない
	if( state_ != ClientBridgeState::Ready ||
	   (negotiated_capabilities_.feature_bits & protocol::kFeatureAgentLifecycle) == 0U) {
		return false ;
	}

	protocol::Frame frame{} ;
	frame.type = protocol::MessageType::AgentStop ;
	// 停止要求をqueueできない場合は成功と扱わない
	if( !QueueFrame( frame)) {
		return false ;
	}

	// queued STOPは維持し、以後のstreamとhealthを停止する
	state_					 = ClientBridgeState::Stopped ;
	remote_stop_requested_	 = true ;
	configuration_requested_ = false ;
	configuration_status_	 = 0U ;
	ClearInboundStreamQueue() ;
	return true ;
}

bool ClientBridge::HandleConfigurationAck( const protocol::Frame &frame) noexcept
{
	// 応答を同じ一件の要求へ固定し、重複応答でgateを開かない
	if( state_ != ClientBridgeState::Ready || !configuration_requested_ ||
	   configuration_status_ != 0U || frame.sequence != 1U || frame.stream_id != 0U ||
	   frame.payload.size() != 1U ||
	   (negotiated_capabilities_.feature_bits & protocol::kFeatureAgentLifecycle) == 0U) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::UnexpectedMessage) ;
		return false ;
	}

	const auto status = std::to_integer<std::uint8_t>( frame.payload[0]) ;
	// 未定義statusを設定反映成功と近似しない
	if( status < 1U || status > 3U) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::UnexpectedMessage) ;
		return false ;
	}

	configuration_status_ = status ;
	return true ;
}

bool ClientBridge::QueueHelloFrame( const protocol::MessageType type) noexcept
{
	protocol::Frame frame{} ;
	frame.type	   = type ;
	frame.sequence = 0U ;
	const protocol::FrameError payload_result =
		protocol::EncodeHelloCapabilities( local_capabilities_, frame.payload) ;
	// capabilityを完全encodeできない場合は部分HELLOを送らない
	if( payload_result != protocol::FrameError::None) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::EncodeFailed) ;
		return false ;
	}

	// attempt count overflow時は無期限retryを続けない
	if( hello_attempt_count_ == std::numeric_limits<std::uint32_t>::max()) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::QueueLimitExceeded) ;
		return false ;
	}

	// HELLOを送信queueへ入れられなければ試行数を進めない
	if( !QueueFrame( frame)) {
		return false ;
	}

	++hello_attempt_count_ ;
	return true ;
}

bool ClientBridge::QueueFrame( const protocol::Frame &frame) noexcept
{
	std::vector<std::byte>	   encoded ;
	const protocol::FrameError encode_result = protocol::EncodeFrame( frame, encoded) ;
	// 不正frameまたはallocation失敗をtransport queueへ部分登録しない
	if( encode_result != protocol::FrameError::None) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::EncodeFailed) ;
		return false ;
	}

	const std::size_t chunk_size =
		std::min( static_cast<std::size_t>( negotiated_capabilities_.transport_chunk_bytes),
				 protocol::kStaticVirtualChannelChunkBytes) ;
	// capability破損時に0除算や空chunk loopへ進まない
	if( chunk_size == 0U) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::EncodeFailed) ;
		return false ;
	}

	const std::size_t required_chunks = (encoded.size() + chunk_size - 1U) / chunk_size ;
	std::scoped_lock  lock( outbound_mutex_) ;
	// 件数と総byte数の両上限を、queue変更前に一括確認する
	if( required_chunks > kMaximumOutboundChunks - outbound_chunks_.size() ||
	   encoded.size() > kMaximumOutboundQueuedBytes - outbound_queued_bytes_) {
		state_ = ClientBridgeState::Failed ;
		error_ = ClientBridgeError::QueueLimitExceeded ;
		outbound_chunks_.clear() ;
		outbound_queued_bytes_ = 0U ;
		return false ;
	}

	const std::size_t original_chunk_count = outbound_chunks_.size() ;
	try {
		std::size_t offset = 0U ;
		// Application frame境界に依存せずMicrosoft SVC上限以下へ分割する
		while( offset < encoded.size()) {
			const std::size_t remaining	   = encoded.size() - offset ;
			const std::size_t current_size = std::min( chunk_size, remaining) ;
			outbound_chunks_.emplace_back( encoded.begin() + static_cast<std::ptrdiff_t>( offset),
										  encoded.begin() +
											  static_cast<std::ptrdiff_t>( offset + current_size)) ;
			offset += current_size ;
		}
	} catch( const std::bad_alloc &) {
		// 今回追加したchunkだけをrollbackし、以前のqueueもfail時には破棄する
		while( outbound_chunks_.size() > original_chunk_count) {
			outbound_chunks_.pop_back() ;
		}

		outbound_chunks_.clear() ;
		outbound_queued_bytes_ = 0U ;
		state_				   = ClientBridgeState::Failed ;
		error_				   = ClientBridgeError::QueueLimitExceeded ;
		return false ;
	} catch( ...) {
		// 例外型に依存せず、今回追加したchunkを同じ有限rollback手順で除去する
		while( outbound_chunks_.size() > original_chunk_count) {
			outbound_chunks_.pop_back() ;
		}

		outbound_chunks_.clear() ;
		outbound_queued_bytes_ = 0U ;
		state_				   = ClientBridgeState::Failed ;
		error_				   = ClientBridgeError::QueueLimitExceeded ;
		return false ;
	}

	outbound_queued_bytes_ += encoded.size() ;
	return true ;
}

bool ClientBridge::HandleHelloAck( const protocol::Frame &frame) noexcept
{
	// HELLO controlは固定sequence 0とし、Echo sequenceと混在させない
	if( frame.sequence != 0U) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::InvalidHello) ;
		return false ;
	}

	protocol::HelloCapabilities peer_capabilities{} ;
	const protocol::FrameError	decode_result =
		protocol::DecodeHelloCapabilities( frame.payload, peer_capabilities) ;
	// capability応答の構文が不正なら互換sessionとして採用しない
	if( decode_result != protocol::FrameError::None) {
		Fail( ClientBridgeState::Incompatible, ClientBridgeError::InvalidHello) ;
		return false ;
	}

	protocol::HelloCapabilities negotiated{} ;
	const protocol::FrameError	negotiation_result =
		protocol::NegotiateHelloCapabilities( local_capabilities_, peer_capabilities, negotiated) ;
	// 合意できないcapabilityはBridgeだけを利用不可にする
	if( negotiation_result != protocol::FrameError::None) {
		Fail( ClientBridgeState::Incompatible, ClientBridgeError::InvalidHello) ;
		return false ;
	}

	negotiated_capabilities_			  = negotiated ;
	discard_stale_frames_until_hello_ack_ = false ;
	// retry由来の重複ACKは成立済みEcho testのstateを巻き戻さない
	if( state_ == ClientBridgeState::WaitingForAgent) {
		state_ = ClientBridgeState::Ready ;
		error_ = ClientBridgeError::None ;
	}
	// HELLO_ACKを受理できないlifecycleでは状態を推測して続けない
	else if( state_ != ClientBridgeState::Ready && state_ != ClientBridgeState::EchoRunning &&
			state_ != ClientBridgeState::EchoPassed) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::UnexpectedMessage) ;
		return false ;
	}

	return true ;
}

bool ClientBridge::HandleAgentClose( const protocol::Frame &frame) noexcept
{
	// Phase 1のAgent終了通知はconnection-level、sequence 0、空payloadに固定する
	if( frame.stream_id != 0U || frame.sequence != 0U || !frame.payload.empty()) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::UnexpectedMessage) ;
		return false ;
	}

	// 設定拒否直後のCLOSEでも拒否分類を消して自動再試行しない
	if( configuration_status_ == 3U) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::StreamRelayFailed) ;
		return false ;
	}

	// 旧decoder、Echo、queueを破棄し、同じRDP上で新Agent用HELLOを直ちに開始する
	return StartSession() ;
}

bool ClientBridge::HandlePong( const protocol::Frame &frame) noexcept
{
	// 周期health PONGはEcho進捗へ加算せず、一対一対応だけを検証する
	if( (state_ == ClientBridgeState::Ready || state_ == ClientBridgeState::EchoPassed) &&
	   health_check_outstanding_ && outstanding_sequence_ != 0U) {
		// Echoのsequence不一致は欠落・重複・順序違反として拒否する
		if( frame.sequence != outstanding_sequence_) {
			Fail( ClientBridgeState::Failed, ClientBridgeError::SequenceMismatch) ;
			return false ;
		}

		// Echo payloadが完全一致しなければ転送検証を成功にしない
		if( frame.payload != outstanding_payload_) {
			Fail( ClientBridgeState::Failed, ClientBridgeError::PayloadMismatch) ;
			return false ;
		}

		outstanding_sequence_ = 0U ;
		outstanding_payload_.clear() ;
		health_check_outstanding_ = false ;
		return true ;
	}

	// unsolicited PONGを次testの応答として再利用しない
	if( state_ != ClientBridgeState::EchoRunning || outstanding_sequence_ == 0U) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::UnexpectedMessage) ;
		return false ;
	}

	// PINGとPONGの一対一対応をsequence完全一致で検証する
	if( frame.sequence != outstanding_sequence_) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::SequenceMismatch) ;
		return false ;
	}

	// byte countと全内容を比較し、NULやfragment境界での欠落を検出する
	if( frame.payload != outstanding_payload_) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::PayloadMismatch) ;
		return false ;
	}

	const std::uint64_t confirmed_bytes = static_cast<std::uint64_t>( outstanding_payload_.size()) ;
	echo_target_completed_bytes_ += confirmed_bytes ;
	echo_completed_bytes_ += confirmed_bytes ;
	outstanding_sequence_ = 0U ;
	outstanding_payload_.clear() ;
	return QueueNextEchoChunk() ;
}

bool ClientBridge::QueueNextEchoChunk() noexcept
{
	// 一度に一件だけを未応答とし、queueと検証bufferを有限に保つ
	if( state_ != ClientBridgeState::EchoRunning || outstanding_sequence_ != 0U) {
		return false ;
	}

	// 完了した論理targetを順に進め、次sizeのoffsetを0へ戻す
	while( echo_target_index_ < kEchoTargetSizes.size() &&
		  echo_target_completed_bytes_ == kEchoTargetSizes[echo_target_index_]) {
		++echo_target_index_ ;
		echo_target_completed_bytes_ = 0U ;
	}

	// 最終targetまで完全一致した時だけEchoPassedへ遷移する
	if( echo_target_index_ >= kEchoTargetSizes.size()) {
		state_ = ClientBridgeState::EchoPassed ;
		return true ;
	}

	const std::uint64_t target_size = kEchoTargetSizes[echo_target_index_] ;
	// state破損で確認済みbyte数がtargetを越えた場合は継続しない
	if( echo_target_completed_bytes_ > target_size) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::PayloadMismatch) ;
		return false ;
	}

	const std::uint64_t remaining = target_size - echo_target_completed_bytes_ ;
	const std::uint32_t frame_limit =
		std::min( negotiated_capabilities_.maximum_frame_payload, kEchoPingPayloadBytes) ;
	// handshakeで0を除外済みだが、使用直前にもallocation境界を確認する
	if( frame_limit == 0U) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::InvalidHello) ;
		return false ;
	}

	const std::size_t payload_size =
		static_cast<std::size_t>( std::min<std::uint64_t>( remaining, frame_limit)) ;
	try {
		outstanding_payload_.resize( payload_size) ;
		// targetとoffsetから再現可能なbinary patternを作り、埋込みNULも含める
		for( std::size_t index = 0U; index < payload_size; ++index) {
			const std::uint64_t absolute_offset =
				echo_target_completed_bytes_ + static_cast<std::uint64_t>( index) ;
			const std::uint64_t pattern =
				absolute_offset + static_cast<std::uint64_t>( echo_target_index_) * 31U ;
			outstanding_payload_[index] = static_cast<std::byte>( pattern % 251U) ;
		}
	} catch( const std::bad_alloc &) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::QueueLimitExceeded) ;
		return false ;
	} catch( ...) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::QueueLimitExceeded) ;
		return false ;
	}

	// sequence 0はHELLO用に予約し、wrap前にsessionを停止する
	if( next_sequence_ == 0U || next_sequence_ == std::numeric_limits<std::uint32_t>::max()) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::SequenceMismatch) ;
		return false ;
	}

	protocol::Frame ping{} ;
	ping.type	  = protocol::MessageType::Ping ;
	ping.sequence = next_sequence_ ;
	try {
		ping.payload = outstanding_payload_ ;
	} catch( ...) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::QueueLimitExceeded) ;
		return false ;
	}

	outstanding_sequence_ = next_sequence_ ;
	++next_sequence_ ;
	// queue失敗時はFail内でoutstanding payloadも破棄される
	if( !QueueFrame( ping)) {
		return false ;
	}

	return true ;
}

void ClientBridge::Fail( const ClientBridgeState state, const ClientBridgeError error) noexcept
{
	state_ = state ;
	error_ = error ;
	decoder_.Reset() ;
	outstanding_sequence_ = 0U ;
	outstanding_payload_.clear() ;
	health_check_outstanding_			  = false ;
	discard_stale_frames_until_hello_ack_ = false ;
	ClearOutboundQueue() ;
	ClearInboundStreamQueue() ;
}

void ClientBridge::ClearOutboundQueue() noexcept
{
	std::scoped_lock lock( outbound_mutex_) ;
	outbound_chunks_.clear() ;
	outbound_queued_bytes_ = 0U ;
}

bool ClientBridge::QueueInboundStreamFrame( protocol::Frame frame) noexcept
{
	// stream featureとhandshake成立なしではAgent stream dataを受け付けない
	if( (state_ != ClientBridgeState::Ready && state_ != ClientBridgeState::EchoPassed) ||
	   (negotiated_capabilities_.feature_bits & protocol::kFeatureMultiplexedStreams) == 0U) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::UnexpectedMessage) ;
		return false ;
	}

	const std::size_t frame_bytes = protocol::kFrameHeaderBytes + frame.payload.size() ;
	// callbackからworkerへのqueueを件数と総byte数で有限化する
	if( inbound_stream_frames_.size() >= kMaximumInboundStreamFrames ||
	   frame_bytes > kMaximumInboundStreamBytes - inbound_stream_bytes_) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::QueueLimitExceeded) ;
		return false ;
	}

	try {
		inbound_stream_frames_.push_back( std::move( frame)) ;
	} catch( ...) {
		Fail( ClientBridgeState::Failed, ClientBridgeError::QueueLimitExceeded) ;
		return false ;
	}

	inbound_stream_bytes_ += frame_bytes ;
	return true ;
}

void ClientBridge::ClearInboundStreamQueue() noexcept
{
	inbound_stream_frames_.clear() ;
	inbound_stream_bytes_ = 0U ;
}

} // namespace zrdp::client::bridge
