//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file phase3_agent_tcp_test.cpp
 * @brief production Winsock adapterでloopback Mock Serverとの単一streamを検証する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details test process内の127.0.0.1だけを使用し、DNS、OS Proxy、実SVN、
 *          実取引先endpointへ接続しない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "agent/bridge/agent_stream_relay.h"
#include "agent/network/allowlist.h"
#include "agent/network/upstream_connection.h"
#include "protocol/multiplexer.h"

#include <WinSock2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using zrdp::agent::bridge::AgentStreamRelay ;
using zrdp::protocol::Frame ;
using zrdp::protocol::MultiplexerResult ;
using zrdp::protocol::StreamMultiplexer ;

/**
 * @brief integration test内のSOCKETを一意所有するRAII wrapper。
 */
class TestSocket final {
public:
	/**
	 * @brief 初期状態の管理instanceを構築し、所有資源の解放責任を固定する。
	 */
	TestSocket() noexcept = default ;

	/**
	 * @brief 初期状態の管理instanceを構築し、所有資源の解放責任を固定する。
	 * @param[in] socket 受け取る資源handle。無効値を許容し、有効な場合だけ終了時に解放する。
	 */
	explicit TestSocket( const SOCKET socket) noexcept : socket_( socket)
	{
	}

	/**
	 * @brief 所有資源を終了経路で解放し、後続の参照を残さない。
	 */
	~TestSocket()
	{
		Reset() ;
	}

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	TestSocket( const TestSocket &)			  = delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	TestSocket &operator=( const TestSocket &) = delete ;

	/**
	 * @brief 初期状態の管理instanceを構築し、所有資源の解放責任を固定する。
	 * @param[in] other 所有権を引き継ぐ移動元。終了時に同じ資源を重複解放しない。
	 */
	TestSocket( TestSocket &&other) noexcept : socket_( other.Release())
	{
	}

	/**
	 * @brief 現在の所有資源を解放し、移動元から単独所有権を引き継ぐ。
	 * @param[in] other 所有権を引き継ぐ移動元。終了時に同じ資源を重複解放しない。
	 * @return 更新後の自身への参照。
	 */
	TestSocket &operator=( TestSocket &&other) noexcept
	{
		// 自己moveでmock serverのSocketを閉じず、別instanceだけから移譲する
		if( this != &other) {
			Reset( other.Release()) ;
		}

		return *this ;
	}

	/**
	 * @brief 現在の非owning Socket値を返す。
	 * @return 未所有時は`INVALID_SOCKET`。
	 */
	[[nodiscard]] SOCKET Get() const noexcept
	{
		return socket_ ;
	}

	/**
	 * @brief 所有Socketを閉じてsentinelへ戻す。
	 * @param[in] socket 旧Socketのclose後に所有するSocket。既定は所有なしsentinel。
	 */
	void Reset( const SOCKET socket = INVALID_SOCKET) noexcept
	{
		// mock serverが既存Socketを所有する場合は置換前に閉じる
		if( socket_ != INVALID_SOCKET) {
			static_cast<void>( closesocket( socket_)) ;
		}

		socket_ = socket ;
	}

private:
	/**
	 * @brief move先へclose責任を移す。
	 * @return 以前の所有Socket。
	 */
	[[nodiscard]] SOCKET Release() noexcept
	{
		const SOCKET released = socket_ ;
		socket_				  = INVALID_SOCKET ;
		return released ;
	}

	/// このRAII instanceが単独所有するSocket。Closeまたは破棄で解放する。
	SOCKET socket_{INVALID_SOCKET} ;
} ;

/**
 * @brief loopback一接続をechoして両方向half-closeするMock HTTP相当server。
 *
 * production Agentとは異なり、listen／acceptはintegration test process内だけで
 * 使用する。
 */
class MockEchoServer final {
public:
	/**
	 * @brief 初期状態の管理instanceを構築し、所有資源の解放責任を固定する。
	 */
	MockEchoServer() = default ;

	/**
	 * @brief 所有資源を終了経路で解放し、後続の参照を残さない。
	 */
	~MockEchoServer()
	{
		Stop() ;
	}

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	MockEchoServer( const MockEchoServer &)				 = delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	MockEchoServer	  &operator=( const MockEchoServer &) = delete ;

	/**
	 * @brief 127.0.0.1のOS割当portで一接続listenを開始する。
	 * @return listenerとworkerを開始できた場合はtrue。
	 */
	[[nodiscard]] bool Start() noexcept
	{
		listener_ = TestSocket( ::socket( AF_INET, SOCK_STREAM, IPPROTO_TCP)) ;
		// loopback試験Socketを生成できなければmock serverを開始しない
		if( listener_.Get() == INVALID_SOCKET) {
			return false ;
		}

		sockaddr_in address{} ;
		address.sin_family		= AF_INET ;
		address.sin_addr.s_addr = htonl( INADDR_LOOPBACK) ;
		address.sin_port		= 0U ;
		// loopbackへのbind・listen失敗ではSocketを回収して検証を中止する
		if( bind( listener_.Get(), reinterpret_cast<const sockaddr *>( &address),
				static_cast<int>( sizeof( address))) == SOCKET_ERROR ||
		   listen( listener_.Get(), 1) == SOCKET_ERROR) {
			listener_.Reset() ;
			return false ;
		}

		int address_size = static_cast<int>( sizeof( address)) ;
		// mock serverの実割当portを確認できなければ接続を開始しない
		if( getsockname( listener_.Get(), reinterpret_cast<sockaddr *>( &address), &address_size) ==
		   SOCKET_ERROR) {
			listener_.Reset() ;
			return false ;
		}

		port_ = ntohs( address.sin_port) ;
		// test thread生成失敗時はlistenerを閉じ、部分開始を公開しない
		try {
			worker_ = std::thread( &MockEchoServer::WorkerLoop, this) ;
		} catch( ...) {
			listener_.Reset() ;
			port_ = 0U ;
			return false ;
		}

		return true ;
	}

	/**
	 * @brief workerへ停止を通知し、全test Socketをjoin後に閉じる。
	 */
	void Stop() noexcept
	{
		stop_requested_.store( true, std::memory_order_release) ;
		// 起動済みmock workerをjoinし、threadを残してlistenerを破棄しない
		if( worker_.joinable()) {
			worker_.join() ;
		}

		listener_.Reset() ;
	}

	/**
	 * @brief OSが割り当てたloopback portを返す。
	 * @return Start成功後の非0 port。
	 */
	[[nodiscard]] std::uint16_t port() const noexcept
	{
		return port_ ;
	}

	/**
	 * @brief serverが受信した全binary dataのcopyを返す。
	 * @return mutexで同期した所有copy。
	 */
	[[nodiscard]] std::vector<std::byte> received() const
	{
		std::scoped_lock lock( received_mutex_) ;
		return received_ ;
	}

	/**
	 * @brief server loopにSocket errorがなかったか返す。
	 * @return 正常処理または停止要求ならtrue。
	 */
	[[nodiscard]] bool succeeded() const noexcept
	{
		return succeeded_.load( std::memory_order_acquire) ;
	}

	/**
	 * @brief Client側FINを受けてserver側FINも送ったか返す。
	 * @return orderly shutdown完了時はtrue。
	 */
	[[nodiscard]] bool orderly_shutdown() const noexcept
	{
		return orderly_shutdown_.load( std::memory_order_acquire) ;
	}

private:
	/**
	 * @brief listener readinessを有限pollし、一接続をechoする。
	 */
	void WorkerLoop() noexcept
	{
		TestSocket accepted ;
		// stop要求を100 ms以内に確認しながら一接続だけacceptする
		while( !stop_requested_.load( std::memory_order_acquire)) {
			fd_set readable{} ;
			FD_ZERO( &readable) ;
			FD_SET( listener_.Get(), &readable) ;
			timeval timeout{} ;
			timeout.tv_usec	   = 100000L ;
			const int selected = select( 0, &readable, nullptr, nullptr, &timeout) ;
			// accept readiness照会失敗ではmock serverの失敗をownerへ公開する
			if( selected == SOCKET_ERROR) {
				succeeded_.store( false, std::memory_order_release) ;
				return ;
			}

			// 接続待ちの間は有限selectへ戻り、無効Socketでreadを始めない
			if( selected == 0) {
				continue ;
			}

			accepted = TestSocket( accept( listener_.Get(), nullptr, nullptr)) ;
			// accept失敗は接続成立として扱わずmock serverを失敗終了する
			if( accepted.Get() == INVALID_SOCKET) {
				succeeded_.store( false, std::memory_order_release) ;
				return ;
			}

			break ;
		}

		// 接続前にtest停止された場合は正常なcleanupとして終了する
		if( accepted.Get() == INVALID_SOCKET) {
			return ;
		}

		const DWORD timeout_milliseconds = 100U ;
		// mock I/Oの有限timeoutを設定できなければblocking転送を開始しない
		if( setsockopt( accepted.Get(), SOL_SOCKET, SO_RCVTIMEO,
					  reinterpret_cast<const char *>( &timeout_milliseconds),
					  static_cast<int>( sizeof( timeout_milliseconds))) == SOCKET_ERROR ||
		   setsockopt( accepted.Get(), SOL_SOCKET, SO_SNDTIMEO,
					  reinterpret_cast<const char *>( &timeout_milliseconds),
					  static_cast<int>( sizeof( timeout_milliseconds))) == SOCKET_ERROR) {
			succeeded_.store( false, std::memory_order_release) ;
			return ;
		}

		std::array<char, 8192> buffer{} ;
		// Client FINまたはtest停止まで、受信chunkを同じ順序でechoする
		while( !stop_requested_.load( std::memory_order_acquire)) {
			const int received =
				recv( accepted.Get(), buffer.data(), static_cast<int>( buffer.size()), 0) ;
			// 非空recvだけを累積して同じbyte列をEcho応答へ渡す
			if( received > 0) {
				{
					std::scoped_lock lock( received_mutex_) ;
					const auto		*begin = reinterpret_cast<const std::byte *>( buffer.data()) ;
					received_.insert( received_.end(), begin, begin + received) ;
				}

				int offset = 0 ;
				// partial sendを含め、受信chunk全体を順序どおりechoする
				while( offset < received) {
					const int sent =
						send( accepted.Get(), buffer.data() + offset, received - offset, 0) ;
					// mock send失敗は一時待機と継続不能を区別する
					if( sent == SOCKET_ERROR) {
						const int error = WSAGetLastError() ;
						// mock sendの一時timeout・WouldBlockはoffsetを保って再試行する
						if( error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
							continue ;
						}

						succeeded_.store( false, std::memory_order_release) ;
						return ;
					}

					// blocking Socketで0-byte進捗を無限loopとして扱わない
					if( sent == 0) {
						succeeded_.store( false, std::memory_order_release) ;
						return ;
					}

					offset += sent ;
				}

				continue ;
			}

			// peer FIN後はserver側も送信FINを返して正常な両方向終了を検証する
			if( received == 0) {
				// Client送信FINへserver送信FINを返して双方のhalf-closeを成立させる
				if( shutdown( accepted.Get(), SD_SEND) == SOCKET_ERROR) {
					succeeded_.store( false, std::memory_order_release) ;
					return ;
				}

				orderly_shutdown_.store( true, std::memory_order_release) ;
				return ;
			}

			const int error = WSAGetLastError() ;
			// 短いreceive timeoutではstop flag確認へ戻る
			if( error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
				continue ;
			}

			succeeded_.store( false, std::memory_order_release) ;
			return ;
		}
	}

	/// main test threadだけがcloseするloopback listener。
	TestSocket			   listener_ ;

	/// listenerとaccepted Socketを処理する一thread。
	std::thread			   worker_ ;

	/// workerの有限poll loopを停止するatomic flag。
	std::atomic_bool	   stop_requested_{false} ;

	/// Socket errorなしの初期値から失敗時だけfalseへ遷移する。
	std::atomic_bool	   succeeded_{true} ;

	/// Client FINとserver FINの両処理完了を示す。
	std::atomic_bool	   orderly_shutdown_{false} ;

	/// Start成功後に固定するloopback port。
	std::uint16_t		   port_{0U} ;

	/// received_のappendとcopyを同期する。
	mutable std::mutex	   received_mutex_ ;

	/// Mock Serverが受信したbinary data。
	std::vector<std::byte> received_ ;
} ;

/**
 * @brief Client outbound FrameをAgent relayへすべて渡す。
 * @param[in,out] client Client multiplexer。
 * @param[in,out] relay Agent relay。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferToRelay( StreamMultiplexer &client, AgentStreamRelay &relay)
{
	Frame frame{} ;
	// WTS相当のFrame列をwire順のままdrainする
	while( client.TakeOutboundFrame( frame)) {
		// Client frameをrelayが受理しなければ配送helperの失敗を返す
		if( !relay.OnFrame( std::move( frame))) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief Agent outbound FrameをClientへすべて渡す。
 * @param[in,out] relay Agent relay。
 * @param[in,out] client Client multiplexer。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferToClient( AgentStreamRelay &relay, StreamMultiplexer &client)
{
	Frame frame{} ;
	// WTS相当のFrame列をwire順のままdrainする
	while( relay.TakeOutboundFrame( frame)) {
		// relay frameをClientが受理しなければ配送helperの失敗を返す
		if( client.OnFrame( std::move( frame)) != MultiplexerResult::Ok) {
			return false ;
		}
	}

	return true ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Phase 3のAgentから許可済みmock TCP endpointへの中継を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain()
{
	using zrdp::protocol::MultiplexerRole ;
	using zrdp::protocol::OpenTarget ;
	using zrdp::protocol::StreamCloseReason ;
	using zrdp::protocol::StreamEvent ;
	using zrdp::protocol::StreamEventType ;

	zrdp::agent::network::WinsockRuntime winsock_runtime ;
	// Winsockを初期化できなければ実TCPのloopback試験を開始しない
	if( winsock_runtime.Start() != 0U) {
		return 1 ;
	}

	MockEchoServer server ;
	// mock serverの起動と有効portを確認できなければ試験を止める
	if( !server.Start() || server.port() == 0U) {
		return 2 ;
	}

	const std::string configuration =
		"[allowlist]\nendpoint.9001=127.0.0.1:" + std::to_string( server.port()) + "\n" ;
	zrdp::agent::network::Allowlist allowlist ;
	// loopback専用allowlistを構築できなければAgent Socketを作らない
	if( !zrdp::agent::network::ParseAllowlist( configuration, allowlist).succeeded()) {
		return 3 ;
	}

	zrdp::agent::network::WinsockUpstreamConnector connector ;
	AgentStreamRelay							   relay( allowlist, connector) ;
	StreamMultiplexer							   client( MultiplexerRole::Client) ;
	auto capabilities			 = zrdp::protocol::MakePhase2HelloCapabilities() ;
	capabilities.maximum_streams = 1U ;
	// Clientとrelayのsession初期化が成立しなければOPENへ進めない
	if( client.StartSession( capabilities) != MultiplexerResult::Ok ||
	   !relay.StartSession( capabilities)) {
		return 4 ;
	}

	const OpenTarget target{{127U, 0U, 0U, 1U}, server.port()} ;
	std::uint32_t	 stream_id = 0U ;
	// 許可済みloopback targetへのOPEN配送が成立することを確認する
	if( client.OpenStream( target, stream_id) != MultiplexerResult::Ok ||
	   !TransferToRelay( client, relay)) {
		return 5 ;
	}

	bool				opened			 = false ;
	const std::uint64_t connect_deadline = GetTickCount64() + 5000U ;
	// production non-blocking connectを有限時間pumpしてOPEN_OKを待つ
	while( !opened && GetTickCount64() < connect_deadline) {
		// non-blocking connectのPumpとClient応答配送を進められることを確認する
		if( !relay.Pump( GetTickCount64()) || !TransferToClient( relay, client)) {
			return 6 ;
		}

		StreamEvent event{} ;
		// 一Pumpで届いた有限eventを全て確認し、対象streamのOPEN_OKを探す
		while( client.TakeEvent( event)) {
			// Opened eventを接続成立の正本として保持する
			if( event.type == StreamEventType::Opened) {
				opened = true ;
			}
			// 許可済みmock接続へのOpenFailedは試験失敗として返す
			else if( event.type == StreamEventType::OpenFailed) {
				return 7 ;
			} else {
				return 8 ;
			}
		}

		// 接続未成立の間だけ短時間待機してtestのbusy loopを避ける
		if( !opened) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1)) ;
		}
	}

	// 有限待機でOpenedに到達しなければ検証を失敗にする
	if( !opened) {
		return 9 ;
	}

	std::vector<std::byte> request( 512U * 1024U + 137U) ;
	// 複数windowを跨ぐbinary patternで順序、欠落、重複をbyte比較する
	for( std::size_t index = 0U; index < request.size(); ++index) {
		request[index] = static_cast<std::byte>( (index * 13U + 17U) % 251U) ;
	}

	constexpr std::size_t  kClientChunkBytes = 16U * 1024U ;
	std::size_t			   sent_offset		 = 0U ;
	std::vector<std::byte> response ;
	response.reserve( request.size()) ;
	bool				local_half_closed  = false ;
	bool				remote_half_closed = false ;
	bool				closed			   = false ;
	const std::uint64_t transfer_deadline  = GetTickCount64() + 15000U ;

	// DATA、credit、両方向FIN、CLOSE ackを同じ有限owner loopで進める
	while( !closed && GetTickCount64() < transfer_deadline) {
		// request未送信部分が残る場合だけ次の有限DATA chunkを作る
		if( sent_offset < request.size()) {
			const std::size_t current_size =
				std::min( kClientChunkBytes, request.size() - sent_offset) ;
			const std::span<const std::byte> chunk( request.data() + sent_offset, current_size) ;
			const MultiplexerResult			 send_result = client.SendData( stream_id, chunk) ;
			// DATAをqueueできた場合だけrequestの送信offsetを進める
			if( send_result == MultiplexerResult::Ok) {
				sent_offset += current_size ;
			}
			// credit待ち以外のDATA生成失敗は転送試験の失敗にする
			else if( send_result != MultiplexerResult::WouldBlock) {
				return 10 ;
			}
		}

		// request全量をqueueした後にだけClient送信方向を閉じる
		if( sent_offset == request.size() && !local_half_closed) {
			// request完送後のHALF_CLOSEを生成できなければ終了検証を止める
			if( client.HalfCloseStream( stream_id) != MultiplexerResult::Ok) {
				return 11 ;
			}

			local_half_closed = true ;
		}

		// Client・relay間の往復とSocket Pumpが失敗したら大容量検証を中止する
		if( !TransferToRelay( client, relay) || !relay.Pump( GetTickCount64()) ||
		   !TransferToClient( relay, client)) {
			return 12 ;
		}

		StreamEvent event{} ;
		// Client eventを全件消費し、実際にcopyしたDATAだけcreditを返す
		while( client.TakeEvent( event)) {
			// response DATAを累積し、消費分のcreditを返して転送を継続する
			if( event.type == StreamEventType::DataReceived) {
				response.insert( response.end(), event.data.begin(), event.data.end()) ;
				// response消費のcredit返却失敗は試験失敗とする
				if( client.ConsumeReceivedData( stream_id,
											  static_cast<std::uint32_t>( event.data.size())) !=
				   MultiplexerResult::Ok) {
					return 13 ;
				}
			}
			// remote FIN eventはresponse byteとは分けて記録する
			else if( event.type == StreamEventType::RemoteHalfClosed) {
				remote_half_closed = true ;
			}
			// CLOSE eventでは正常終了理由と最終record回収を検証する
			else if( event.type == StreamEventType::Closed) {
				// 正常以外のstream終了理由は大容量Echo成功とみなさない
				if( event.close_reason != StreamCloseReason::Normal) {
					return 14 ;
				}

				closed = true ;
			} else {
				return 15 ;
			}
		}

		// WINDOW_UPDATEとCLOSE ackを次Pumpより前にAgentへ戻す
		if( !TransferToRelay( client, relay)) {
			return 16 ;
		}

		// 終了未成立の間だけ短時間待機し、有限loopで終了を待つ
		if( !closed) {
			std::this_thread::sleep_for( std::chrono::milliseconds( 1)) ;
		}
	}

	server.Stop() ;
	// 正常CLOSEが確認できなければ終了検証を失敗にする
	if( !closed) {
		return 17 ;
	}

	// Client側の送信FINが未成立なら両方向終了成功とみなさない
	if( !local_half_closed) {
		return 18 ;
	}

	// upstream側の送信FINが未確認なら両方向終了成功とみなさない
	if( !remote_half_closed) {
		return 19 ;
	}

	// request全byteを送れていなければ転送完了とみなさない
	if( sent_offset != request.size()) {
		return 20 ;
	}

	// Echo responseが元requestと完全一致しなければ失敗にする
	if( response != request) {
		return 21 ;
	}

	// mock serverの受信内容もrequestと完全一致することを確認する
	if( server.received() != request) {
		return 22 ;
	}

	// mock serverが自身の検証を成功と報告したことを確認する
	if( !server.succeeded()) {
		return 23 ;
	}

	// mock serverのorderly shutdownが成立したことを確認する
	if( !server.orderly_shutdown()) {
		return 24 ;
	}

	// relayが終端stream recordを残していないことを確認する
	if( relay.snapshot().active_stream_id != 0U) {
		return 25 ;
	}

	return 0 ;
}
