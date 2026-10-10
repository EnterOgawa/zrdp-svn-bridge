//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_proxy_session_test.cpp
 * @brief HTTP parser、Client multiplexer、local Socket credit境界を一体検証する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details Socket APIを使用せず、Agent role multiplexerをfake transportとする。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Connection違反の転送前拒否とstream分離を検証。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "client/proxy/client_proxy_session.h"
#include "protocol/hello.h"
#include "protocol/multiplexer.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using zrdp::client::proxy::ClientProxySession ;
using zrdp::protocol::Frame ;
using zrdp::protocol::MultiplexerResult ;
using zrdp::protocol::StreamEvent ;
using zrdp::protocol::StreamEventType ;
using zrdp::protocol::StreamMultiplexer ;

/**
 * @brief ASCII test dataをbinary spanへ変換する。
 * @param[in] text test wire data。
 * @return 同一memoryを参照するspan。
 */
[[nodiscard]] std::span<const std::byte> Bytes( const std::string_view text) noexcept
{
	return std::span<const std::byte>( reinterpret_cast<const std::byte *>( text.data()),
									  text.size()) ;
}

/**
 * @brief Client outbound FrameをAgent multiplexerへ全件渡す。
 * @param[in,out] client Client relay。
 * @param[in,out] agent fake Agent multiplexer。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferClientToAgent( ClientProxySession &client, StreamMultiplexer &agent)
{
	Frame frame{} ;
	// bounded queueを空になるまでwire順に移す
	while( client.TakeOutboundFrame( frame)) {
		// Client送信frameをAgentが拒否した場合は配送helperを失敗にする
		if( agent.OnFrame( std::move( frame)) != MultiplexerResult::Ok) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief Agent outbound FrameをClient relayへ全件渡す。
 * @param[in,out] agent fake Agent multiplexer。
 * @param[in,out] client Client relay。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferAgentToClient( StreamMultiplexer &agent, ClientProxySession &client)
{
	Frame frame{} ;
	// Agent応答をClient stream stateへwire順に移す
	while( agent.TakeOutboundFrame( frame)) {
		// Agent応答frameをProxy sessionが拒否したら配送を中止する
		if( !client.OnBridgeFrame( std::move( frame))) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief 不正Connection要求からOPEN／DATAを作らず、固定400でlocal接続を終了する。
 * @param[in] request 文書用宛先とdummy bodyだけを使用する不正要求。
 * @param[in] split_header header終端を二回のcallbackに分ける場合はtrue。
 * @return 上流出力なし、固定400、queue排出後のclose、Bridge継続を満たす場合はtrue。
 */
[[nodiscard]] bool ExpectConnectionRejectedWithoutOpen( const std::string_view request,
													   const bool			  split_header)
{
	ClientProxySession client ;
	auto			   capabilities = zrdp::protocol::MakePhase2HelloCapabilities() ;
	capabilities.maximum_streams	= 1U ;
	// readyなBridgeでlocal接続を受付け、Bridge未成立による拒否と区別する
	if( !client.StartBridgeSession( capabilities) ||
	   client.BeginLocalConnection() != zrdp::client::proxy::LocalConnectionResult::Accepted) {
		return false ;
	}

	std::size_t				   offset = 0U ;
	Frame					   frame{} ;
	std::span<const std::byte> local_write ;
	// header終端のCRLFを分割し、未完成のheaderからOPENやresponseを生成しない
	if( split_header) {
		const std::size_t terminator = request.find( "\r\n\r\n") ;
		// helper入力に完全headerがない場合はfragment試験として成立させない
		if( terminator == std::string_view::npos) {
			return false ;
		}

		offset = terminator + 2U ;
		// 最後の空行前では、宛先を確定したframeもlocal responseも出力しない
		if( !client.OnLocalData( Bytes( request.substr( 0U, offset))) ||
		   client.TakeOutboundFrame( frame) || client.PeekLocalWrite( local_write) ||
		   client.snapshot().active_streams != 0U) {
			return false ;
		}
	}

	// 完全headerとbodyを同時に渡しても、不正要求を送信queueへ移さない
	if( !client.OnLocalData( Bytes( request.substr( offset))) || client.TakeOutboundFrame( frame) ||
	   client.snapshot().active_streams != 0U || client.snapshot().active_stream_id != 0U ||
	   client.snapshot().upstream_pending_bytes != 0U ||
	   client.snapshot().http_error != zrdp::client::proxy::HttpRequestError::MalformedHeader ||
	   !client.PeekLocalWrite( local_write)) {
		return false ;
	}

	constexpr std::string_view expected = "HTTP/1.1 400 Bad Request\r\n"
										  "Connection: close\r\nContent-Length: 0\r\n\r\n" ;
	// 要求内容を含まない固定responseを完送してから、対象local接続だけを閉じる
	if( std::string_view( reinterpret_cast<const char *>( local_write.data()), local_write.size()) !=
		   expected ||
	   client.ShouldCloseLocalConnection() || !client.ConsumeLocalWrite( local_write.size()) ||
	   !client.ShouldCloseLocalConnection()) {
		return false ;
	}

	client.OnLocalConnectionClosed( zrdp::protocol::StreamCloseReason::Normal) ;
	return client.snapshot().state == zrdp::client::proxy::ClientProxySessionState::Ready &&
		   client.BeginLocalConnection() == zrdp::client::proxy::LocalConnectionResult::Accepted ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief 一Proxy sessionのHTTP／Bridge状態遷移とhalf-closeを検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int main()
{
	using zrdp::client::proxy::ClientProxySessionState ;
	using zrdp::client::proxy::LocalConnectionResult ;
	using zrdp::protocol::MultiplexerRole ;
	using zrdp::protocol::OpenTarget ;

	auto capabilities			 = zrdp::protocol::MakePhase2HelloCapabilities() ;
	capabilities.maximum_streams = 1U ;

	ClientProxySession client ;
	// HELLO未成立ではlocal requestをBridge待ちへhangさせない
	if( client.BeginLocalConnection() != LocalConnectionResult::BridgeUnavailable) {
		return 1 ;
	}

	StreamMultiplexer agent( MultiplexerRole::Agent) ;
	// session開始とlegacy単一connectionの受付・Busy制約を確認する
	if( !client.StartBridgeSession( capabilities) ||
	   agent.StartSession( capabilities) != MultiplexerResult::Ok ||
	   client.BeginLocalConnection() != LocalConnectionResult::Accepted ||
	   client.BeginLocalConnection() != LocalConnectionResult::Busy) {
		return 2 ;
	}

	constexpr std::string_view request_first = "REPORT http://192.0.2.40:8080/svn/Test HTTP/1.1\r\n"
											   "Host: 192.0.2.40:8080\r\n"
											   "Content-Length: 6\r\n\r\nabc" ;
	constexpr std::string_view request_second = "def" ;
	// 分割request headerからOpeningへ遷移し、OPENを配送できることを確認する
	if( !client.OnLocalData( Bytes( request_first)) || !client.OnLocalData( Bytes( request_second)) ||
	   client.snapshot().state != ClientProxySessionState::Opening ||
	   !TransferClientToAgent( client, agent)) {
		return 3 ;
	}

	StreamEvent event{} ;
	// OPEN targetがabsolute URLのIPv4・明示portと一致することを確認する
	if( !agent.TakeEvent( event) || event.type != StreamEventType::OpenRequested ||
	   event.target != OpenTarget{{192U, 0U, 2U, 40U}, 8080U}) {
		return 4 ;
	}

	const std::uint32_t stream_id = event.stream_id ;
	// 接続許可往復後に待機requestをAgentへ排出できることを確認する
	if( agent.AcceptStream( stream_id) != MultiplexerResult::Ok ||
	   !TransferAgentToClient( agent, client) || !TransferClientToAgent( client, agent)) {
		return 5 ;
	}

	std::string received_request ;
	// OPEN_OK後にheaderとbodyがDATA eventとして順序どおり届く
	while( agent.TakeEvent( event)) {
		// Agent request収集ではDATA以外のeventを期待結果と扱わない
		if( event.type != StreamEventType::DataReceived) {
			return 6 ;
		}

		received_request.append( reinterpret_cast<const char *>( event.data.data()),
								event.data.size()) ;
		// Agentのrequest消費creditを返せなければ転送検証を失敗にする
		if( agent.ConsumeReceivedData( event.stream_id,
									 static_cast<std::uint32_t>( event.data.size())) !=
		   MultiplexerResult::Ok) {
			return 7 ;
		}
	}

	constexpr std::string_view expected_request = "REPORT /svn/Test HTTP/1.1\r\n"
												  "Host: 192.0.2.40:8080\r\n"
												  "Content-Length: 6\r\n\r\nabcdef" ;
	// origin-form要求とClient受理byte counterの完全一致を確認する
	if( received_request != expected_request ||
	   client.snapshot().bytes_from_local != expected_request.size()) {
		return 8 ;
	}

	// Agentからのresponseはlocal send完了まで受信creditを返さない
	constexpr std::string_view response = "HTTP/1.1 200 OK\r\n"
										  "Content-Length: 5\r\n\r\nhello" ;
	// Agent responseをDATAとしてProxy sessionへ配送できることを確認する
	if( agent.SendData( stream_id, Bytes( response)) != MultiplexerResult::Ok ||
	   !TransferAgentToClient( agent, client)) {
		return 9 ;
	}

	std::span<const std::byte> local_write ;
	// local partial sendの残量・queue消費・転送counterが整合することを確認する
	if( !client.PeekLocalWrite( local_write) || local_write.size() != response.size() ||
	   !client.ConsumeLocalWrite( 7U) || !client.PeekLocalWrite( local_write) ||
	   local_write.size() != response.size() - 7U ||
	   !client.ConsumeLocalWrite( local_write.size()) || client.PeekLocalWrite( local_write) ||
	   client.snapshot().bytes_to_local != response.size()) {
		return 10 ;
	}

	// localへ完送したresponseのcreditをAgentへ返せることを確認する
	if( !TransferClientToAgent( client, agent)) {
		return 11 ;
	}

	// 完全request境界のlocal FINだけをHALF_CLOSEへ変換する
	if( !client.OnLocalReceiveClosed() || !TransferClientToAgent( client, agent) ||
	   !agent.TakeEvent( event) || event.type != StreamEventType::RemoteHalfClosed) {
		return 12 ;
	}

	// upstream FINがlocal Socket送信終了の要求へ対応することを確認する
	if( agent.HalfCloseStream( stream_id) != MultiplexerResult::Ok ||
	   !TransferAgentToClient( agent, client) || !client.ShouldShutdownLocalSend()) {
		return 13 ;
	}

	client.MarkLocalSendShutdown() ;
	// 適用済みlocal FINを重複要求しないことを確認する
	if( client.ShouldShutdownLocalSend()) {
		return 14 ;
	}

	// malformed requestはupstream OPENを作らず固定400をlocalへ返す
	ClientProxySession rejected ;
	// malformed requestでも固定HTTP errorをlocal queueへ取得できることを確認する
	if( !rejected.StartBridgeSession( capabilities) ||
	   rejected.BeginLocalConnection() != LocalConnectionResult::Accepted ||
	   !rejected.OnLocalData( Bytes( "GET / HTTP/1.1\r\n\r\n")) ||
	   !rejected.PeekLocalWrite( local_write)) {
		return 15 ;
	}

	const std::string error_response( reinterpret_cast<const char *>( local_write.data()),
									 local_write.size()) ;
	// 固定400 response完送後にlocal closeが要求されることを確認する
	if( !error_response.starts_with( "HTTP/1.1 400 ") ||
	   !rejected.ConsumeLocalWrite( local_write.size()) || !rejected.ShouldCloseLocalConnection()) {
		return 16 ;
	}

	// Bridge resetはlocal connection、stream、payload queueを同期破棄する
	client.StopBridgeSession() ;
	const auto stopped = client.snapshot() ;
	// session停止でstream・Socket所有・両方向queueが残らないことを確認する
	if( stopped.state != ClientProxySessionState::Stopped || stopped.active_stream_id != 0U ||
	   stopped.local_connection_active || stopped.upstream_pending_bytes != 0U ||
	   stopped.local_write_bytes != 0U) {
		return 17 ;
	}

	// 二つのHTTP connectionを別streamへ割り当て、応答を交差させても混線しない
	auto multi_capabilities			   = zrdp::protocol::MakePhase2HelloCapabilities() ;
	multi_capabilities.maximum_streams = zrdp::client::proxy::kMaximumClientProxyConnections ;
	ClientProxySession								 multi_client ;
	StreamMultiplexer								 multi_agent( MultiplexerRole::Agent) ;
	constexpr zrdp::client::proxy::LocalConnectionId connection_a = 101U ;
	constexpr zrdp::client::proxy::LocalConnectionId connection_b = 202U ;
	// 二つのlocal connectionを独立してsessionへ登録できることを確認する
	if( !multi_client.StartBridgeSession( multi_capabilities) ||
	   multi_agent.StartSession( multi_capabilities) != MultiplexerResult::Ok ||
	   multi_client.BeginLocalConnection( connection_a) != LocalConnectionResult::Accepted ||
	   multi_client.BeginLocalConnection( connection_b) != LocalConnectionResult::Accepted) {
		return 18 ;
	}

	constexpr std::string_view request_a = "OPTIONS http://192.0.2.41:8080/svn/A HTTP/1.1\r\n"
										   "Host: 192.0.2.41:8080\r\n"
										   "Authorization: NTLM opaque-token-a\r\n"
										   "Content-Length: 0\r\n\r\n" ;
	constexpr std::string_view request_b = "PROPFIND http://192.0.2.42:8081/svn/B HTTP/1.1\r\n"
										   "Host: 192.0.2.42:8081\r\n"
										   "Depth: 1\r\n"
										   "Content-Length: 0\r\n\r\n" ;
	// 二つの異なるauthorityのrequestを同時に受理・配送できることを確認する
	if( !multi_client.OnLocalData( connection_a, Bytes( request_a)) ||
	   !multi_client.OnLocalData( connection_b, Bytes( request_b)) ||
	   !TransferClientToAgent( multi_client, multi_agent)) {
		return 19 ;
	}

	std::uint32_t stream_a	 = 0U ;
	std::uint32_t stream_b	 = 0U ;
	std::uint32_t open_count = 0U ;
	// OPEN targetからstream routeを記録し、各streamを独立に受理する
	while( multi_agent.TakeEvent( event)) {
		// 同時接続の初期Agent eventはOPEN要求だけであることを確認する
		if( event.type != StreamEventType::OpenRequested) {
			return 20 ;
		}

		// 第一targetのOPENを第一streamの対応として保持する
		if( event.target == OpenTarget{{192U, 0U, 2U, 41U}, 8080U}) {
			stream_a = event.stream_id ;
		}
		// 第二targetのOPENを別streamの対応として保持する
		else if( event.target == OpenTarget{{192U, 0U, 2U, 42U}, 8081U}) {
			stream_b = event.stream_id ;
		} else {
			return 21 ;
		}

		// 各OPENを独立に許可できなければ同時転送試験を止める
		if( multi_agent.AcceptStream( event.stream_id) != MultiplexerResult::Ok) {
			return 22 ;
		}

		++open_count ;
	}

	// 異なる非0 IDの二OPENと許可往復が揃うことを確認する
	if( open_count != 2U || stream_a == 0U || stream_b == 0U || stream_a == stream_b ||
	   !TransferAgentToClient( multi_agent, multi_client) ||
	   !TransferClientToAgent( multi_client, multi_agent)) {
		return 23 ;
	}

	std::string upstream_a ;
	std::string upstream_b ;
	// DATA eventをstream IDで振り分け、受信creditを各streamへ返す
	while( multi_agent.TakeEvent( event)) {
		// request DATA収集に他eventが混入した場合は試験を失敗にする
		if( event.type != StreamEventType::DataReceived) {
			return 24 ;
		}

		std::string *destination = nullptr ;
		// 第一streamのDATAだけを第一requestの受信記録へ加える
		if( event.stream_id == stream_a) {
			destination = &upstream_a ;
		}
		// 第二streamのDATAだけを第二requestの受信記録へ加える
		else if( event.stream_id == stream_b) {
			destination = &upstream_b ;
		} else {
			return 25 ;
		}

		destination->append( reinterpret_cast<const char *>( event.data.data()), event.data.size()) ;
		// 各streamのDATA消費creditが個別に返却できることを確認する
		if( multi_agent.ConsumeReceivedData( event.stream_id,
										   static_cast<std::uint32_t>( event.data.size())) !=
		   MultiplexerResult::Ok) {
			return 26 ;
		}
	}

	constexpr std::string_view expected_a = "OPTIONS /svn/A HTTP/1.1\r\n"
											"Host: 192.0.2.41:8080\r\n"
											"Authorization: NTLM opaque-token-a\r\n"
											"Content-Length: 0\r\n\r\n" ;
	constexpr std::string_view expected_b = "PROPFIND /svn/B HTTP/1.1\r\n"
											"Host: 192.0.2.42:8081\r\n"
											"Depth: 1\r\n"
											"Content-Length: 0\r\n\r\n" ;
	// 両requestのorigin-form変換とactive stream二件の維持を確認する
	if( upstream_a != expected_a || upstream_b != expected_b ||
	   multi_client.snapshot().active_streams != 2U) {
		return 27 ;
	}

	constexpr std::string_view response_a = "HTTP/1.1 100 Continue\r\n\r\n"
											"HTTP/1.1 401 Unauthorized\r\n"
											"WWW-Authenticate: NTLM opaque-challenge\r\n"
											"Content-Length: 0\r\n\r\n" ;
	constexpr std::string_view response_b = "HTTP/1.1 207 Multi-Status\r\n"
											"Transfer-Encoding: chunked\r\n\r\n"
											"4\r\nbody\r\n0\r\n\r\n" ;
	// 逆順のresponse配送でも二streamを交差させないことを確認する
	if( multi_agent.SendData( stream_b, Bytes( response_b)) != MultiplexerResult::Ok ||
	   multi_agent.SendData( stream_a, Bytes( response_a)) != MultiplexerResult::Ok ||
	   !TransferAgentToClient( multi_agent, multi_client)) {
		return 28 ;
	}

	std::span<const std::byte> write_a ;
	std::span<const std::byte> write_b ;
	// 各local queueが対応responseを保持し、個別に消費できることを確認する
	if( !multi_client.PeekLocalWrite( connection_a, write_a) ||
	   !multi_client.PeekLocalWrite( connection_b, write_b) ||
	   std::string_view( reinterpret_cast<const char *>( write_a.data()), write_a.size()) !=
		   response_a ||
	   std::string_view( reinterpret_cast<const char *>( write_b.data()), write_b.size()) !=
		   response_b ||
	   !multi_client.ConsumeLocalWrite( connection_b, write_b.size()) ||
	   !multi_client.ConsumeLocalWrite( connection_a, write_a.size())) {
		return 29 ;
	}

	// 一local connectionのHTTP違反は固定400だけへ閉じ、他二streamを維持する
	constexpr zrdp::client::proxy::LocalConnectionId rejected_connection = 303U ;
	std::span<const std::byte>						 rejected_write ;
	// 一connectionのHTTP拒否とcloseが他二streamを停止させないことを確認する
	if( multi_client.BeginLocalConnection( rejected_connection) != LocalConnectionResult::Accepted ||
	   !multi_client.OnLocalData( rejected_connection, Bytes( "GET /relative HTTP/1.1\r\n\r\n")) ||
	   !multi_client.PeekLocalWrite( rejected_connection, rejected_write) ||
	   !std::string_view( reinterpret_cast<const char *>( rejected_write.data()),
						 rejected_write.size())
			.starts_with( "HTTP/1.1 400 ") ||
	   multi_client.snapshot().active_streams != 2U ||
	   !multi_client.ConsumeLocalWrite( rejected_connection, rejected_write.size()) ||
	   !multi_client.ShouldCloseLocalConnection( rejected_connection)) {
		return 30 ;
	}

	multi_client.OnLocalConnectionClosed( rejected_connection,
										 zrdp::protocol::StreamCloseReason::Normal) ;

	// negotiated上限の九件目は既存streamへ影響を与えずBusyで拒否する
	for( zrdp::client::proxy::LocalConnectionId connection_id = 303U; connection_id < 309U;
		++connection_id) {
		// 有限connection上限までは新規recordを受付できることを確認する
		if( multi_client.BeginLocalConnection( connection_id) != LocalConnectionResult::Accepted) {
			return 31 ;
		}
	}

	// 上限超過connectionはBusyとなり、所有Socket数が増えないことを確認する
	if( multi_client.BeginLocalConnection( 309U) != LocalConnectionResult::Busy ||
	   multi_client.snapshot().active_connections !=
		   zrdp::client::proxy::kMaximumClientProxyConnections) {
		return 32 ;
	}

	// 既存responseのcredit frameを先に配送し、不正要求が生成するframeだけを観測する
	if( !TransferClientToAgent( multi_client, multi_agent)) {
		return 33 ;
	}

	constexpr std::string_view bad_keep_alive =
		"POST /svn/A HTTP/1.1\r\n"
		"Host: 192.0.2.41:8080\r\nConnection: Content-Length\r\nContent-Length: 4\r\n\r\nDATA" ;
	// OPEN済みstream上の不正要求も送信queueへ追加せず、固定400をlocalへ返す
	if( !multi_client.OnLocalData( connection_a, Bytes( bad_keep_alive)) ||
	   !multi_client.PeekLocalWrite( connection_a, write_a) ||
	   !std::string_view( reinterpret_cast<const char *>( write_a.data()), write_a.size())
			.starts_with( "HTTP/1.1 400 ") ||
	   multi_client.snapshot().upstream_pending_bytes != 0U) {
		return 34 ;
	}

	Frame close_frame{} ;
	Frame unexpected_frame{} ;
	// 不正要求のDATA／OPENを出さず、対象streamのCLOSEだけをAgentへ通知する
	if( !multi_client.TakeOutboundFrame( close_frame) ||
	   close_frame.type != zrdp::protocol::MessageType::Close ||
	   close_frame.stream_id != stream_a || multi_client.TakeOutboundFrame( unexpected_frame) ||
	   multi_agent.OnFrame( std::move( close_frame)) != MultiplexerResult::Ok) {
		return 35 ;
	}

	constexpr std::string_view healthy_keep_alive = "GET /svn/B HTTP/1.1\r\n"
													"Host: 192.0.2.42:8081\r\n\r\n" ;
	// 他streamのKeep-Alive要求は同じupstreamへ継続して配送できる
	if( !multi_client.OnLocalData( connection_b, Bytes( healthy_keep_alive)) ||
	   !TransferClientToAgent( multi_client, multi_agent)) {
		return 36 ;
	}

	std::string healthy_received ;
	// Agentが受けたDATAは他streamの正常要求だけであり、不正bodyを含まない
	while( multi_agent.TakeEvent( event)) {
		// 正常streamのDATAだけをwire順に収集する
		if( event.type == StreamEventType::DataReceived) {
			// 対象外streamのDATAは拒否した要求の漏出として失敗にする
			if( event.stream_id != stream_b) {
				return 37 ;
			}

			healthy_received.append( reinterpret_cast<const char *>( event.data.data()),
									event.data.size()) ;
		}
	}

	// 拒否したstreamとは独立に正常要求が完全に届いたことを確認する
	if( healthy_received != healthy_keep_alive) {
		return 38 ;
	}

	constexpr std::array<std::string_view, 3U> forbidden_requests{
		"POST /repo HTTP/1.1\r\nHost: 192.0.2.10\r\nConnection: Host\r\nContent-Length: 4\r\n\r\nDATA",
		"POST /repo HTTP/1.1\r\nHost: 192.0.2.10\r\nConnection: Content-Length\r\nContent-Length: 4\r\n\r\nDATA",
		"GET /repo HTTP/1.1\r\nHost: 192.0.2.10\r\n\r\n"
		"POST /repo HTTP/1.1\r\nHost: 192.0.2.10\r\nConnection: Content-Length\r\nContent-Length: 4\r\n\r\nDATA",
	} ;
	// 初回の指名や同一callback内のpipeline違反を、部分出力を使用せずOPEN前に拒否する
	for( const std::string_view request : forbidden_requests) {
		// 固定400、OPEN／DATAなし、local close後のBridge継続を確認する
		if( !ExpectConnectionRejectedWithoutOpen( request, false) ||
		   !ExpectConnectionRejectedWithoutOpen( request, true)) {
			return 39 ;
		}
	}

	multi_client.StopBridgeSession() ;
	return 0 ;
}
