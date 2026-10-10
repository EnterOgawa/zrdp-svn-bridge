//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_stream_relay.cpp
 * @brief Agent streamとnon-blocking outbound TCPの複数stream relayを実装する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details policy拒否はSocket factory呼出し前に完了し、Socket I/Oの進捗だけに
 *          対してflow-control creditを返す。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "agent/bridge/agent_stream_relay.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <span>
#include <utility>

namespace zrdp::agent::bridge {
namespace {

// 一回のSocket receiveと一つのDATA frameに使用する公平なchunk上限
constexpr std::size_t				  kRelayReceiveChunkBytes = 16U * 1024U ;

/**
 * @brief connect失敗分類をwireの固定OPEN_ERRORへ変換する。
 * @param[in] status connectorまたはpoll結果。
 * @return timeoutだけを区別し、それ以外は接続失敗。
 */
[[nodiscard]] protocol::OpenErrorCode MapOpenError( const network::UpstreamIoStatus status) noexcept
{
	// 接続期限超過は一般接続失敗と区別してClientへ通知する
	if( status == network::UpstreamIoStatus::TimedOut) {
		return protocol::OpenErrorCode::Timeout ;
	}

	return protocol::OpenErrorCode::ConnectFailed ;
}

/**
 * @brief Socket I/O失敗をstream CLOSE reasonへ変換する。
 * @param[in] status send／receive／shutdown結果。
 * @return reset、timeout、その他local errorの固定分類。
 */
[[nodiscard]] protocol::StreamCloseReason
MapCloseReason( const network::UpstreamIoStatus status) noexcept
{
	// peer resetは通常終了と区別してstream終了理由へ反映する
	if( status == network::UpstreamIoStatus::PeerReset) {
		return protocol::StreamCloseReason::PeerReset ;
	}

	// I/O期限超過をClient側でも識別できる終了理由へ変換する
	if( status == network::UpstreamIoStatus::TimedOut) {
		return protocol::StreamCloseReason::Timeout ;
	}

	return protocol::StreamCloseReason::LocalError ;
}

} // namespace

AgentStreamRelay::AgentStreamRelay( const network::Allowlist	   &allowlist,
								   network::IUpstreamConnector &connector) noexcept
	: allowlist_( allowlist), connector_( connector)
{
}

bool AgentStreamRelay::StartSession( const protocol::HelloCapabilities &capabilities) noexcept
{
	StopSession() ;
	// production resource上限を超えるcapabilityを部分的に有効化しない
	if( capabilities.maximum_streams == 0U ||
	   capabilities.maximum_streams > kMaximumAgentRelayStreams) {
		error_ = AgentStreamRelayError::InvalidCapabilities ;
		state_ = AgentStreamRelayState::Failed ;
		return false ;
	}

	const protocol::MultiplexerResult result = multiplexer_.StartSession( capabilities) ;
	// invalid capabilityでSocket機能を部分的に有効化しない
	if( result != protocol::MultiplexerResult::Ok) {
		error_ = AgentStreamRelayError::InvalidCapabilities ;
		state_ = AgentStreamRelayState::Failed ;
		return false ;
	}

	capabilities_ = capabilities ;
	state_		  = AgentStreamRelayState::Ready ;
	error_		  = AgentStreamRelayError::None ;
	return true ;
}

void AgentStreamRelay::StopSession() noexcept
{
	// 全active Socketを最初に閉じ、旧sessionからのI/Oを停止する
	for( auto &[stream_id, stream] : active_streams_) {
		static_cast<void>( stream_id) ;
		// 所有Socketが残るstreamだけを閉じ、停止後の接続を残さない
		if( stream != nullptr && stream->connection != nullptr) {
			stream->connection->Close() ;
		}
	}

	active_streams_.clear() ;
	total_socket_write_bytes_ = 0U ;
	last_pump_stream_id_	  = 0U ;
	timed_out_streams_		  = 0U ;
	multiplexer_.StopSession() ;
	capabilities_ = protocol::HelloCapabilities{} ;
	state_		  = AgentStreamRelayState::Stopped ;
	error_		  = AgentStreamRelayError::None ;
}

bool AgentStreamRelay::OnFrame( protocol::Frame frame) noexcept
{
	// HELLO未成立またはfatal後のframeをSocket処理へ渡さない
	if( state_ != AgentStreamRelayState::Ready) {
		return false ;
	}

	// stream状態機械が受信frameを拒否したらSocket relayも停止する
	if( multiplexer_.OnFrame( std::move( frame)) != protocol::MultiplexerResult::Ok) {
		Fail( AgentStreamRelayError::MultiplexerFailed) ;
		return false ;
	}

	return DrainEvents() ;
}

bool AgentStreamRelay::Pump( const std::uint64_t now_milliseconds) noexcept
{
	// session外ではSocket APIもmultiplexer APIも呼ばない
	if( state_ != AgentStreamRelayState::Ready) {
		return false ;
	}

	// active streamがない場合はSocket処理を開始せず正常に戻る
	if( active_streams_.empty()) {
		return true ;
	}

	std::size_t		  operations	   = 0U ;
	const std::size_t streams_to_visit = active_streams_.size() ;
	// 直前IDの次から一streamずつ処理し、低いIDへの固定優先を避ける
	for( std::size_t visited = 0U; visited < streams_to_visit && !active_streams_.empty() &&
								  operations < kMaximumRelayOperationsPerPump;
		++visited) {
		auto iterator = active_streams_.upper_bound( last_pump_stream_id_) ;
		// 公平巡回がtable末尾へ達したら先頭から次のstreamを選ぶ
		if( iterator == active_streams_.end()) {
			iterator = active_streams_.begin() ;
		}

		const std::uint32_t stream_id = iterator->first ;
		last_pump_stream_id_		  = stream_id ;
		ActiveStream &stream		  = *iterator->second ;
		// 接続待ちstreamではdata I/Oより先にnon-blocking接続を進める
		if( stream.connecting) {
			++operations ;
			// connect処理の内部異常は他streamへ進まずownerへ返す
			if( !PumpConnect( stream, now_milliseconds)) {
				return false ;
			}

			continue ;
		}

		// connect待ちまたはCLOSE ack待ちではdata planeを進めない
		if( stream.closing) {
			continue ;
		}

		// HALF_CLOSEを一Pump先にwireへ公開してからterminal CLOSEを生成する
		if( stream.normal_close_pending) {
			// 正常終了CLOSEを生成できなければrelay全体を継続しない
			if( !CloseActiveStream( stream_id, protocol::StreamCloseReason::Normal)) {
				return false ;
			}

			continue ;
		}

		// 初回の活動時刻を基準にし、未初期化時刻でidle期限を誤判定しない
		if( !stream.activity_clock_started) {
			stream.activity_clock_started = true ;
			stream.last_activity_ms		  = now_milliseconds ;
		}

		// allowlist済みpeerでも完全無通信を無期限に保持せず対象streamだけ閉じる
		if( now_milliseconds - stream.last_activity_ms >= kAgentStreamIdleTimeoutMilliseconds) {
			// timeout件数は最大値で飽和させ、診断counterのwrapを防ぐ
			if( timed_out_streams_ != std::numeric_limits<std::uint64_t>::max()) {
				++timed_out_streams_ ;
			}

			// timeout streamの終了処理に失敗した場合は残りのI/Oを進めない
			if( !CloseActiveStream( stream_id, protocol::StreamCloseReason::Timeout)) {
				return false ;
			}

			continue ;
		}

		// writeまたはFIN適用の失敗後にreadを続けて状態を上書きしない
		if( !PumpSocketWrites( stream, operations, now_milliseconds) ||
		   !ApplyPendingHalfClose( stream, operations)) {
			return false ;
		}

		// write側のfailureでstreamをclosingへ移した場合はreadを継続しない
		const auto current = active_streams_.find( stream_id) ;
		// write中に終了したstreamは再取得後のread対象から除く
		if( current == active_streams_.end() || current->second->closing) {
			continue ;
		}

		// read側でrelayの不変条件が壊れた場合はPumpを終了する
		if( !PumpSocketReads( *current->second, operations, now_milliseconds)) {
			return false ;
		}

		const auto after_read = active_streams_.find( stream_id) ;
		// read中に終了したstreamは両方向完了の判定から除く
		if( after_read == active_streams_.end() || after_read->second->closing) {
			continue ;
		}

		ActiveStream &completed = *after_read->second ;
		// 双方向FIN確認後はnormal CLOSEを開始し、Socketを保持し続けない
		if( completed.remote_half_closed && completed.send_shutdown &&
		   completed.upstream_half_closed && completed.socket_writes.empty() &&
		   completed.pending_socket_read.empty()) {
			completed.normal_close_pending = true ;
		}
	}

	return true ;
}

bool AgentStreamRelay::TakeOutboundFrame( protocol::Frame &frame) noexcept
{
	return multiplexer_.TakeOutboundFrame( frame) ;
}

AgentStreamRelaySnapshot AgentStreamRelay::snapshot() const noexcept
{
	AgentStreamRelaySnapshot snapshot{} ;
	snapshot.state				= state_ ;
	snapshot.error				= error_ ;
	snapshot.active_streams		= static_cast<std::uint32_t>( active_streams_.size()) ;
	snapshot.socket_write_bytes = total_socket_write_bytes_ ;
	snapshot.timed_out_streams	= timed_out_streams_ ;
	// 互換fieldには最初のactive streamの匿名値だけを返す
	if( !active_streams_.empty()) {
		const ActiveStream &first = *active_streams_.begin()->second ;
		snapshot.active_stream_id = first.stream_id ;
		snapshot.endpoint_id	  = first.endpoint_id ;
		snapshot.connecting		  = first.connecting ;
	}

	// 全streamの非機密counterを有限tableから集計する
	for( const auto &[stream_id, stream] : active_streams_) {
		static_cast<void>( stream_id) ;
		// 空のrecordはcounter集計から除き、所有状態を参照しない
		if( stream == nullptr) {
			continue ;
		}

		snapshot.pending_socket_read_bytes += stream->pending_socket_read.size() ;
		// 接続完了待ちのstreamだけをpending件数へ加える
		if( stream->connecting) {
			++snapshot.connecting_streams ;
		}
	}

	return snapshot ;
}

bool AgentStreamRelay::DrainEvents() noexcept
{
	protocol::StreamEvent event{} ;
	// 一Frameが生成したeventをwire順のままSocket stateへ適用する
	while( multiplexer_.TakeEvent( event)) {
		// stream eventに応じてSocket生成、送信queue、終了処理を選ぶ
		switch( event.type) {
		// Clientの接続要求だけをallowlist確認とSocket生成へ渡す
		case protocol::StreamEventType::OpenRequested:
			// 接続要求の処理が失敗した場合は後続eventを扱わない
			if( !HandleOpenRequested( event)) {
				return false ;
			}

			break ;

		// 受信binary dataを対応するupstream送信queueへ渡す
		case protocol::StreamEventType::DataReceived:
			// dataを安全にqueueできなければrelayのevent処理を停止する
			if( !HandleDataReceived( std::move( event))) {
				return false ;
			}

			break ;

		// Clientの送信終了を同じstreamのupstream FIN予約へ変換する
		case protocol::StreamEventType::RemoteHalfClosed:
			// activeな同一streamだけをTCP送信FINへ対応させる
			{
				const auto iterator = active_streams_.find( event.stream_id) ;
				// activeでないstreamへのhalf-closeは状態違反として拒否する
				if( iterator == active_streams_.end() || iterator->second == nullptr ||
				   iterator->second->closing) {
					Fail( AgentStreamRelayError::UnexpectedEvent) ;
					return false ;
				}

				iterator->second->remote_half_closed = true ;
			}

			break ;

		// peer CLOSEまたは終了ACKでは対象Socketとqueueだけを回収する
		case protocol::StreamEventType::Closed: {
			// peer CLOSEまたはlocal CLOSE ackで対象Socketだけを確実に破棄する
			const auto iterator = active_streams_.find( event.stream_id) ;
			// recordがまだ残る場合だけ終了eventに対応する資源を回収する
			if( iterator != active_streams_.end()) {
				// 実体を持つrecordだけからSocketとbyte counterを回収する
				if( iterator->second != nullptr) {
					// 所有Socketが残っている場合はrecord削除より先に閉じる
					if( iterator->second->connection != nullptr) {
						iterator->second->connection->Close() ;
					}

					// global queue counterをstream破棄前に正確に戻す
					if( iterator->second->socket_write_bytes <= total_socket_write_bytes_) {
						total_socket_write_bytes_ -= iterator->second->socket_write_bytes ;
					} else {
						Fail( AgentStreamRelayError::InvalidSocketResult) ;
						return false ;
					}
				}

				active_streams_.erase( iterator) ;
			}

			break ;
		}

		// Client向け接続成功eventをAgentが受けた場合は役割違反で停止する
		case protocol::StreamEventType::Opened:
		// Client向け接続失敗eventもAgentの入力として受け付けない
		case protocol::StreamEventType::OpenFailed:
			// Agent roleではClient向け結果eventを受信してはならない
			Fail( AgentStreamRelayError::UnexpectedEvent) ;
			return false ;
		}
	}

	return true ;
}

bool AgentStreamRelay::HandleOpenRequested( const protocol::StreamEvent &event) noexcept
{
	// negotiated上限に加えproduction Socket table上限を接続前に適用する
	if( active_streams_.size() >= kMaximumAgentRelayStreams) {
		return multiplexer_.RejectStream( event.stream_id,
										 protocol::OpenErrorCode::ResourceExhausted) ==
			   protocol::MultiplexerResult::Ok ;
	}

	std::uint32_t endpoint_id = 0U ;
	// 完全一致しないtargetではconnectorを一度も呼ばずfail-closedにする
	if( !allowlist_.Find( event.target, endpoint_id)) {
		return multiplexer_.RejectStream( event.stream_id,
										 protocol::OpenErrorCode::RejectedByPolicy) ==
			   protocol::MultiplexerResult::Ok ;
	}

	std::unique_ptr<ActiveStream> candidate ;
	try {
		candidate = std::make_unique<ActiveStream>() ;
	} catch( const std::bad_alloc &) {
		return multiplexer_.RejectStream( event.stream_id,
										 protocol::OpenErrorCode::ResourceExhausted) ==
			   protocol::MultiplexerResult::Ok ;
	} catch( ...) {
		Fail( AgentStreamRelayError::ResourceExhausted) ;
		return false ;
	}

	candidate->stream_id   = event.stream_id ;
	candidate->endpoint_id = endpoint_id ;
	const network::UpstreamIoResult connect_result =
		connector_.BeginConnect( event.target, candidate->connection) ;
	// success／pendingには所有connectionが必須であり、fakeも同じ契約へ従う
	if( (connect_result.status == network::UpstreamIoStatus::Completed ||
		connect_result.status == network::UpstreamIoStatus::Pending) &&
	   candidate->connection == nullptr) {
		Fail( AgentStreamRelayError::InvalidSocketResult) ;
		return false ;
	}

	// 接続開始を受理できなければ候補Socketを破棄してOPEN_ERRORを返す
	if( connect_result.status != network::UpstreamIoStatus::Completed &&
	   connect_result.status != network::UpstreamIoStatus::Pending) {
		// 接続失敗の候補がSocketを所有していた場合は遅延接続を残さない
		if( candidate->connection != nullptr) {
			candidate->connection->Close() ;
		}

		return multiplexer_.RejectStream( event.stream_id, MapOpenError( connect_result.status)) ==
			   protocol::MultiplexerResult::Ok ;
	}

	candidate->connecting = connect_result.status == network::UpstreamIoStatus::Pending ;
	try {
		const auto [iterator, inserted] =
			active_streams_.emplace( event.stream_id, std::move( candidate)) ;
		static_cast<void>( iterator) ;
		// multiplexerが保証した一意stream IDをtable側でも再検証する
		if( !inserted) {
			Fail( AgentStreamRelayError::UnexpectedEvent) ;
			return false ;
		}
	} catch( ...) {
		// table登録中の例外でも未移譲の候補Socketを確実に閉じる
		if( candidate != nullptr && candidate->connection != nullptr) {
			candidate->connection->Close() ;
		}

		Fail( AgentStreamRelayError::ResourceExhausted) ;
		return false ;
	}

	// 即時接続時だけOPEN_OKを同じevent処理で返す
	if( connect_result.status == network::UpstreamIoStatus::Completed) {
		// 即時接続後のOPEN_OK生成に失敗したら利用可能streamとして続けない
		if( multiplexer_.AcceptStream( event.stream_id) != protocol::MultiplexerResult::Ok) {
			Fail( AgentStreamRelayError::MultiplexerFailed) ;
			return false ;
		}
	}

	return true ;
}

bool AgentStreamRelay::HandleDataReceived( protocol::StreamEvent event) noexcept
{
	const auto iterator = active_streams_.find( event.stream_id) ;
	// OPEN_OK済みの同一stream以外へSocket dataをrouteしない
	if( iterator == active_streams_.end() || iterator->second == nullptr ||
	   iterator->second->connecting || iterator->second->closing ||
	   iterator->second->remote_half_closed || event.data.empty()) {
		Fail( AgentStreamRelayError::UnexpectedEvent) ;
		return false ;
	}

	ActiveStream &stream = *iterator->second ;
	// per-streamと全stream合計の両方へ独立上限を適用する
	if( event.data.size() > kMaximumRelaySocketWriteBytes - stream.socket_write_bytes ||
	   event.data.size() > kMaximumRelayTotalSocketWriteBytes - total_socket_write_bytes_) {
		Fail( AgentStreamRelayError::QueueLimitExceeded) ;
		return false ;
	}

	try {
		PendingWrite pending{} ;
		pending.data = std::move( event.data) ;
		stream.socket_write_bytes += pending.data.size() ;
		total_socket_write_bytes_ += pending.data.size() ;
		stream.socket_writes.push_back( std::move( pending)) ;
	} catch( const std::bad_alloc &) {
		Fail( AgentStreamRelayError::ResourceExhausted) ;
		return false ;
	} catch( ...) {
		Fail( AgentStreamRelayError::ResourceExhausted) ;
		return false ;
	}

	return true ;
}

bool AgentStreamRelay::PumpConnect( ActiveStream		  &stream,
								   const std::uint64_t now_milliseconds) noexcept
{
	// callerはactive pending streamだけをこの処理へ渡す
	if( stream.connection == nullptr || !stream.connecting) {
		Fail( AgentStreamRelayError::InvalidSocketResult) ;
		return false ;
	}

	// 接続期限は最初のpoll時刻から計測し、未初期化値と比較しない
	if( !stream.connect_clock_started) {
		stream.connect_clock_started = true ;
		stream.connect_started_ms	 = now_milliseconds ;
	}

	// 64-bit tick差分で5秒を越えたpending connectを有限に終了する
	if( now_milliseconds - stream.connect_started_ms >= kAgentConnectTimeoutMilliseconds) {
		return RejectActiveOpen( stream.stream_id, protocol::OpenErrorCode::Timeout) ;
	}

	const network::UpstreamIoResult result = stream.connection->PollConnect() ;
	// 接続がpendingの間はblockingせず次のPumpまで待つ
	if( result.status == network::UpstreamIoStatus::Pending) {
		return true ;
	}

	// 接続完了時はdata I/Oとidle期限の計測へ遷移する
	if( result.status == network::UpstreamIoStatus::Completed) {
		stream.connecting			  = false ;
		stream.activity_clock_started = true ;
		stream.last_activity_ms		  = now_milliseconds ;
		// 接続完了をClientへ通知できなければrelay全体を停止する
		if( multiplexer_.AcceptStream( stream.stream_id) != protocol::MultiplexerResult::Ok) {
			Fail( AgentStreamRelayError::MultiplexerFailed) ;
			return false ;
		}

		return true ;
	}

	return RejectActiveOpen( stream.stream_id, MapOpenError( result.status)) ;
}

bool AgentStreamRelay::PumpSocketWrites( ActiveStream &stream, std::size_t &operations,
										const std::uint64_t now_milliseconds) noexcept
{
	// 送信dataがない場合や一回の処理上限へ達した場合は次のPumpへ戻る
	if( stream.socket_writes.empty() || operations >= kMaximumRelayOperationsPerPump) {
		return true ;
	}

	PendingWrite &pending = stream.socket_writes.front() ;
	// offsetは所有buffer範囲内というrelay invariantを常に検証する
	if( pending.offset >= pending.data.size() || stream.connection == nullptr) {
		Fail( AgentStreamRelayError::InvalidSocketResult) ;
		return false ;
	}

	const std::span<const std::byte> remaining( pending.data.data() + pending.offset,
											   pending.data.size() - pending.offset) ;
	++operations ;
	const network::UpstreamIoResult result = stream.connection->Send( remaining) ;
	// upstreamが書込不可なら未送信dataとoffsetを保って次の機会を待つ
	if( result.status == network::UpstreamIoStatus::WouldBlock) {
		return true ;
	}

	// 失敗・進捗なし・過大byte数は対象streamのI/O異常として終了する
	if( result.status != network::UpstreamIoStatus::Completed || result.bytes == 0U ||
	   result.bytes > remaining.size()) {
		return CloseActiveStream( stream.stream_id, MapCloseReason( result.status)) ;
	}

	pending.offset += result.bytes ;
	stream.last_activity_ms = now_milliseconds ;
	// creditは一つのDATA event全体がSocketへ渡った時点でだけ返す
	if( pending.offset == pending.data.size()) {
		const auto consumed = static_cast<std::uint32_t>( pending.data.size()) ;
		// 完全送信後にcreditを返せなければ状態機械の不整合として停止する
		if( multiplexer_.ConsumeReceivedData( stream.stream_id, consumed) !=
		   protocol::MultiplexerResult::Ok) {
			Fail( AgentStreamRelayError::MultiplexerFailed) ;
			return false ;
		}

		const std::size_t completed_bytes = pending.data.size() ;
		// per-streamとglobalの両counterを同じ所有buffer単位で減算する
		if( completed_bytes > stream.socket_write_bytes ||
		   completed_bytes > total_socket_write_bytes_) {
			Fail( AgentStreamRelayError::InvalidSocketResult) ;
			return false ;
		}

		stream.socket_write_bytes -= completed_bytes ;
		total_socket_write_bytes_ -= completed_bytes ;
		stream.socket_writes.pop_front() ;
	}

	return true ;
}

bool AgentStreamRelay::PumpSocketReads( ActiveStream &stream, std::size_t &operations,
									   const std::uint64_t now_milliseconds) noexcept
{
	// Socket未所有・受信終了済み・処理上限到達では追加readを行わない
	if( stream.connection == nullptr || stream.upstream_half_closed ||
	   operations >= kMaximumRelayOperationsPerPump) {
		return true ;
	}

	// 前回flow-control待ちdataを新しいSocket readより先に送る
	if( !stream.pending_socket_read.empty()) {
		const protocol::MultiplexerResult send_result =
			multiplexer_.SendData( stream.stream_id, stream.pending_socket_read) ;
		// pending dataのcreditが足りない間は新しいSocket readを開始しない
		if( send_result == protocol::MultiplexerResult::WouldBlock) {
			return true ;
		}

		// pending dataの転送が拒否された場合はrelay全体の異常へ変換する
		if( send_result != protocol::MultiplexerResult::Ok) {
			Fail( AgentStreamRelayError::MultiplexerFailed) ;
			return false ;
		}

		stream.pending_socket_read.clear() ;
	}

	std::array<std::byte, kRelayReceiveChunkBytes> buffer{} ;
	// 一stream一回のrecvでround-robin fairnessを維持する
	++operations ;
	const network::UpstreamIoResult result = stream.connection->Receive( buffer) ;
	// upstreamが読込不可ならblockingせず次のPumpへ戻る
	if( result.status == network::UpstreamIoStatus::WouldBlock) {
		return true ;
	}

	// upstreamのFINは反対方向を残したままClientへhalf-close通知する
	if( result.status == network::UpstreamIoStatus::PeerClosed) {
		stream.upstream_half_closed = true ;
		// 受信終了通知をqueueできなければstream状態を継続しない
		if( multiplexer_.HalfCloseStream( stream.stream_id) != protocol::MultiplexerResult::Ok) {
			Fail( AgentStreamRelayError::MultiplexerFailed) ;
			return false ;
		}

		return true ;
	}

	// read失敗や契約外byte数では対象streamを終了して不正dataを転送しない
	if( result.status != network::UpstreamIoStatus::Completed || result.bytes == 0U ||
	   result.bytes > buffer.size()) {
		return CloseActiveStream( stream.stream_id, MapCloseReason( result.status)) ;
	}

	const std::span<const std::byte> received( buffer.data(), result.bytes) ;
	stream.last_activity_ms = now_milliseconds ;
	const protocol::MultiplexerResult send_result =
		multiplexer_.SendData( stream.stream_id, received) ;
	// credit不足で送れない受信dataは有限bufferへ保持してbackpressureを維持する
	if( send_result == protocol::MultiplexerResult::WouldBlock) {
		try {
			stream.pending_socket_read.assign( received.begin(), received.end()) ;
		} catch( const std::bad_alloc &) {
			Fail( AgentStreamRelayError::ResourceExhausted) ;
			return false ;
		} catch( ...) {
			Fail( AgentStreamRelayError::ResourceExhausted) ;
			return false ;
		}

		return true ;
	}

	// DATA生成が拒否された場合はprotocol状態の不整合として停止する
	if( send_result != protocol::MultiplexerResult::Ok) {
		Fail( AgentStreamRelayError::MultiplexerFailed) ;
		return false ;
	}

	return true ;
}

bool AgentStreamRelay::ApplyPendingHalfClose( ActiveStream &stream, std::size_t &operations) noexcept
{
	// FIN未受理・適用済み・送信待ちdataあり・処理上限到達ではshutdownを保留する
	if( !stream.remote_half_closed || stream.send_shutdown || !stream.socket_writes.empty() ||
	   operations >= kMaximumRelayOperationsPerPump) {
		return true ;
	}

	// Clientの全DATAをSocketへ送った後にだけFINを発行する
	if( stream.connection == nullptr) {
		Fail( AgentStreamRelayError::InvalidSocketResult) ;
		return false ;
	}

	++operations ;
	const network::UpstreamIoResult result = stream.connection->ShutdownSend() ;
	// TCP送信終了が失敗した場合はHALF_CLOSE成功とみなさずstreamを閉じる
	if( result.status != network::UpstreamIoStatus::Completed) {
		return CloseActiveStream( stream.stream_id, MapCloseReason( result.status)) ;
	}

	stream.send_shutdown = true ;
	return true ;
}

bool AgentStreamRelay::RejectActiveOpen( const std::uint32_t			  stream_id,
										const protocol::OpenErrorCode error) noexcept
{
	// pending Socketをresponse queue前に閉じ、拒否後の遅延connectを防止する
	const auto iterator = active_streams_.find( stream_id) ;
	// 接続拒否対象のrecordがない場合は状態を推測してOPEN_ERRORを送らない
	if( iterator == active_streams_.end() || iterator->second == nullptr) {
		Fail( AgentStreamRelayError::InvalidSocketResult) ;
		return false ;
	}

	// pending Socketを所有している場合は拒否応答より先に閉じる
	if( iterator->second->connection != nullptr) {
		iterator->second->connection->Close() ;
	}

	// queue counterの減算でunderflowが起きる状態はrelay異常として停止する
	if( iterator->second->socket_write_bytes > total_socket_write_bytes_) {
		Fail( AgentStreamRelayError::InvalidSocketResult) ;
		return false ;
	}

	total_socket_write_bytes_ -= iterator->second->socket_write_bytes ;
	active_streams_.erase( iterator) ;
	// OPEN_ERRORの生成失敗を成功として返さずrelayを停止する
	if( multiplexer_.RejectStream( stream_id, error) != protocol::MultiplexerResult::Ok) {
		Fail( AgentStreamRelayError::MultiplexerFailed) ;
		return false ;
	}

	return true ;
}

bool AgentStreamRelay::CloseActiveStream( const std::uint32_t			   stream_id,
										 const protocol::StreamCloseReason reason) noexcept
{
	// active streamがない状態でCLOSEを生成しない
	const auto iterator = active_streams_.find( stream_id) ;
	// 終了対象のrecordがなければ未知streamへのCLOSEを生成しない
	if( iterator == active_streams_.end() || iterator->second == nullptr) {
		Fail( AgentStreamRelayError::InvalidSocketResult) ;
		return false ;
	}

	ActiveStream &stream = *iterator->second ;
	// 終了対象が所有するSocketはCLOSE待ち状態へ移す前に閉じる
	if( stream.connection != nullptr) {
		stream.connection->Close() ;
		stream.connection.reset() ;
	}

	// stream単位の未送信byteが総量を超える不整合では減算しない
	if( stream.socket_write_bytes > total_socket_write_bytes_) {
		Fail( AgentStreamRelayError::InvalidSocketResult) ;
		return false ;
	}

	total_socket_write_bytes_ -= stream.socket_write_bytes ;
	stream.socket_writes.clear() ;
	stream.socket_write_bytes = 0U ;
	stream.pending_socket_read.clear() ;
	stream.closing = true ;
	// CLOSE生成に失敗した場合は終了待ちstreamを継続させない
	if( multiplexer_.CloseStream( stream_id, reason) != protocol::MultiplexerResult::Ok) {
		Fail( AgentStreamRelayError::MultiplexerFailed) ;
		return false ;
	}

	return true ;
}

void AgentStreamRelay::Fail( const AgentStreamRelayError error) noexcept
{
	// fatal後は全Socketを先に閉じ、残eventからupstream I/Oを再開させない
	for( auto &[stream_id, stream] : active_streams_) {
		static_cast<void>( stream_id) ;
		// 異常停止では残るすべての所有Socketを閉じる
		if( stream != nullptr && stream->connection != nullptr) {
			stream->connection->Close() ;
		}
	}

	active_streams_.clear() ;
	total_socket_write_bytes_ = 0U ;
	last_pump_stream_id_	  = 0U ;
	state_					  = AgentStreamRelayState::Failed ;
	error_					  = error ;
}

} // namespace zrdp::agent::bridge
