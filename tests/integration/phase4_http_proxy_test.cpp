//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file phase4_http_proxy_test.cpp
 * @brief localhost ProxyからAgent relay、Mock HTTP Serverまでを結合検証する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details test process内の127.0.0.1とOS割当portだけを使用する。DNS、OS Proxy、
 *          実SVN、取引先endpoint、Zscaler設定にはアクセスしない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * </pre>
 */
//------------------------------------------------------------------------------

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include "agent/bridge/agent_bridge.h"
#include "agent/bridge/agent_stream_relay.h"
#include "agent/network/allowlist.h"
#include "agent/network/upstream_connection.h"
#include "client/proxy/client_proxy_worker.h"
#include "client/bridge/client_bridge.h"
#include "protocol/hello.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using zrdp::agent::bridge::AgentStreamRelay ;
using zrdp::client::proxy::ClientProxyWorker ;
using zrdp::protocol::Frame ;

// initial flow-control windowを越えて送るrequest body size
constexpr std::size_t kRequestBodyBytes = 512U * 1024U + 137U ;

// 逆方向window回復も必要にするresponse body size
constexpr std::size_t kResponseBodyBytes = 384U * 1024U + 25U ;

/**
 * @brief test Socketを一意所有するRAII wrapper。
 */
class TestSocket	  final {
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
	 TestSocket( const TestSocket &)			   = delete ;
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
		 // 自己moveでmock HTTP Socketを閉じず、別instanceからの移譲だけを行う
		 if( this != &other) {
			 Reset( other.Release()) ;
		 }

		 return *this ;
	 }

	 /**
	  * @brief 非owning Socket値を返す。
	  * @return 未所有時はINVALID_SOCKET。
	  */
	 [[nodiscard]] SOCKET Get() const noexcept
	 {
		 return socket_ ;
	 }

	 /**
	  * @brief 所有Socketを閉じて置換する。
	  * @param[in] socket 新しい所有値。
	  */
	 void Reset( const SOCKET socket = INVALID_SOCKET) noexcept
	 {
		 // 既存mock Socketを所有している場合は置換前に閉じる
		 if( socket_ != INVALID_SOCKET) {
			 static_cast<void>( closesocket( socket_)) ;
		 }

		 socket_ = socket ;
	 }

private:
	 /**
	  * @brief close責任をmove先へ移す。
	  * @return 以前の値。
	  */
	 [[nodiscard]] SOCKET Release() noexcept
	 {
		 const SOCKET released = socket_ ;
		 socket_			   = INVALID_SOCKET ;
		 return released ;
	 }

	 /// このRAII instanceが単独所有するSocket。Closeまたは破棄で解放する。
	 SOCKET socket_{INVALID_SOCKET} ;
} ;

/**
 * @brief request bodyの位置依存binary patternを返す。
 * @param[in] offset body先頭からのoffset。
 * @return 埋込みNULを含む再現可能byte。
 */
[[nodiscard]] constexpr std::byte RequestPattern( const std::size_t offset) noexcept
{
	return static_cast<std::byte>( (offset * 13U + 7U) % 251U) ;
}

/**
 * @brief response bodyの位置依存binary patternを返す。
 * @param[in] offset body先頭からのoffset。
 * @return 埋込みNULを含む再現可能byte。
 */
[[nodiscard]] constexpr std::byte ResponsePattern( const std::size_t offset) noexcept
{
	return static_cast<std::byte>( (offset * 17U + 3U) % 253U) ;
}

/**
 * @brief 一接続のHTTP requestを検証し大きいresponseを返すloopback server。
 */
class MockHttpServer final {
public:
	/**
	 * @brief 初期状態の管理instanceを構築し、所有資源の解放責任を固定する。
	 */
	MockHttpServer() = default ;

	/**
	 * @brief 所有資源を終了経路で解放し、後続の参照を残さない。
	 */
	~MockHttpServer()
	{
		Stop() ;
	}

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	MockHttpServer( const MockHttpServer &)				 = delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	MockHttpServer	  &operator=( const MockHttpServer &) = delete ;

	/**
	 * @brief 127.0.0.1のOS割当portでlistenerとworkerを開始する。
	 * @return 開始成功時はtrue。
	 */
	[[nodiscard]] bool Start() noexcept
	{
		listener_ = TestSocket( ::socket( AF_INET, SOCK_STREAM, IPPROTO_TCP)) ;
		// loopback listenerを作れなければmock HTTP serverを開始しない
		if( listener_.Get() == INVALID_SOCKET) {
			return false ;
		}

		sockaddr_in address{} ;
		address.sin_family		= AF_INET ;
		address.sin_addr.s_addr = htonl( INADDR_LOOPBACK) ;
		address.sin_port		= 0U ;
		// mock listenerのbind・listen失敗ではSocketを回収して試験を止める
		if( bind( listener_.Get(), reinterpret_cast<const sockaddr *>( &address),
				static_cast<int>( sizeof( address))) == SOCKET_ERROR ||
		   listen( listener_.Get(), 1) == SOCKET_ERROR) {
			listener_.Reset() ;
			return false ;
		}

		int address_size = static_cast<int>( sizeof( address)) ;
		// mock serverの実割当portを取得できなければ接続先を公開しない
		if( getsockname( listener_.Get(), reinterpret_cast<sockaddr *>( &address), &address_size) ==
		   SOCKET_ERROR) {
			listener_.Reset() ;
			return false ;
		}

		port_				= ntohs( address.sin_port) ;
		u_long non_blocking = 1UL ;
		// non-blocking listenerへ設定できなければ有限accept loopを開始しない
		if( ioctlsocket( listener_.Get(), FIONBIO, &non_blocking) == SOCKET_ERROR) {
			listener_.Reset() ;
			port_ = 0U ;
			return false ;
		}

		try {
			worker_ = std::thread( &MockHttpServer::WorkerLoop, this) ;
		} catch( ...) {
			listener_.Reset() ;
			port_ = 0U ;
			return false ;
		}

		return true ;
	}

	/**
	 * @brief workerを有限pollから終了させてjoinする。
	 */
	void Stop() noexcept
	{
		stop_requested_.store( true, std::memory_order_release) ;
		// mock workerの終了を待ってからlistenerを破棄する
		if( worker_.joinable()) {
			worker_.join() ;
		}

		listener_.Reset() ;
	}

	/**
	 * @brief listener portを返す。
	 * @return Start成功後の非0port。
	 */
	[[nodiscard]] std::uint16_t port() const noexcept
	{
		return port_ ;
	}

	/**
	 * @brief request検証とresponse送信が完了したか返す。
	 * @return 完全成功時はtrue。
	 */
	[[nodiscard]] bool succeeded() const noexcept
	{
		return succeeded_.load( std::memory_order_acquire) ;
	}

	/**
	 * @brief server workerがterminalになったか返す。
	 * @return 成否確定後はtrue。
	 */
	[[nodiscard]] bool finished() const noexcept
	{
		return finished_.load( std::memory_order_acquire) ;
	}

private:
	/**
	 * @brief requestをstream検証してresponseを分割送信する。
	 */
	void WorkerLoop() noexcept
	{
		TestSocket client ;
		const auto accept_deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5) ;
		// stopまたは期限までnon-blocking acceptを有限pollする
		while( !stop_requested_.load( std::memory_order_acquire) &&
			  std::chrono::steady_clock::now() < accept_deadline) {
			client = TestSocket( accept( listener_.Get(), nullptr, nullptr)) ;
			// mock acceptが成立した場合だけ接続待ちloopを抜ける
			if( client.Get() != INVALID_SOCKET) {
				break ;
			}

			const int error = WSAGetLastError() ;
			// 一時的なaccept待ち以外のerrorはmock serverの終了として公開する
			if( error != WSAEWOULDBLOCK) {
				finished_.store( true, std::memory_order_release) ;
				return ;
			}

			std::this_thread::sleep_for( std::chrono::milliseconds( 2)) ;
		}

		// 有限待機でも接続を受け付けられなければmock serverを終了する
		if( client.Get() == INVALID_SOCKET) {
			finished_.store( true, std::memory_order_release) ;
			return ;
		}

		DWORD timeout = 200U ;
		static_cast<void>( setsockopt( client.Get(), SOL_SOCKET, SO_RCVTIMEO,
									 reinterpret_cast<const char *>( &timeout),
									 static_cast<int>( sizeof( timeout)))) ;
		static_cast<void>( setsockopt( client.Get(), SOL_SOCKET, SO_SNDTIMEO,
									 reinterpret_cast<const char *>( &timeout),
									 static_cast<int>( sizeof( timeout)))) ;

		const std::string expected_header = "REPORT /svn/Phase4?depth=1 HTTP/1.1\r\n"
											"Host: 127.0.0.1:" +
											std::to_string( port_) +
											"\r\n"
											"Content-Type: application/octet-stream\r\n"
											"Content-Length: " +
											std::to_string( kRequestBodyBytes) + "\r\n\r\n" ;
		std::string						   header ;
		std::size_t						   body_received   = 0U ;
		bool							   header_complete = false ;
		std::array<std::byte, 16U * 1024U> buffer{} ;
		const auto receive_deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 15) ;
		// headerは有限収集し、bodyは保持せずposition patternを逐次検証する
		while( !stop_requested_.load( std::memory_order_acquire) &&
			  std::chrono::steady_clock::now() < receive_deadline &&
			  body_received < kRequestBodyBytes) {
			const int received = recv( client.Get(), reinterpret_cast<char *>( buffer.data()),
									  static_cast<int>( buffer.size()), 0) ;
			// mock recv異常は一時待機と継続不能を区別する
			if( received == SOCKET_ERROR) {
				const int error = WSAGetLastError() ;
				// 一時timeout・WouldBlockでは未受信dataの位置を保って再試行する
				if( error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
					continue ;
				}

				finished_.store( true, std::memory_order_release) ;
				return ;
			}

			// peer FINを受けたらrequest収集を終えて完全性を検証する
			if( received == 0) {
				break ;
			}

			std::size_t offset = 0U ;
			// header未完成の間だけ文字列へ収集し、bodyをheaderへ混入させない
			if( !header_complete) {
				// header終端までだけ文字列へ追加し、64 KiBを越えさせない
				while( offset < static_cast<std::size_t>( received) && !header_complete) {
					header.push_back(
						static_cast<char>( std::to_integer<unsigned char>( buffer[offset]))) ;
					++offset ;
					// mock header上限を越えた場合は無制限に保持せず試験を終了する
					if( header.size() > 64U * 1024U) {
						finished_.store( true, std::memory_order_release) ;
						return ;
					}

					header_complete = header.ends_with( "\r\n\r\n") ;
				}
			}

			// header後の全byteをbody patternと一致させる
			while( offset < static_cast<std::size_t>( received)) {
				// request bodyの超過またはbyte pattern不一致はmock検証失敗にする
				if( body_received >= kRequestBodyBytes ||
				   buffer[offset] != RequestPattern( body_received)) {
					finished_.store( true, std::memory_order_release) ;
					return ;
				}

				++body_received ;
				++offset ;
			}
		}

		// origin-form headerとbodyの完全受信が揃わなければresponseを返さない
		if( !header_complete || header != expected_header || body_received != kRequestBodyBytes) {
			finished_.store( true, std::memory_order_release) ;
			return ;
		}

		const std::string response_header = "HTTP/1.1 207 Multi-Status\r\n"
											"Content-Length: " +
											std::to_string( kResponseBodyBytes) + "\r\n\r\n" ;
		// 固定response headerを完送できなければmock serverを失敗終了する
		if( !SendAll( client.Get(), std::span<const std::byte>(
									  reinterpret_cast<const std::byte *>( response_header.data()),
									  response_header.size()))) {
			finished_.store( true, std::memory_order_release) ;
			return ;
		}

		std::size_t response_sent = 0U ;
		// response bodyを固定chunkへ生成し、全量bufferを作らず送信する
		while( !stop_requested_.load( std::memory_order_acquire) &&
			  response_sent < kResponseBodyBytes) {
			const std::size_t size = std::min( buffer.size(), kResponseBodyBytes - response_sent) ;
			// 現chunkをabsolute offsetから再現する
			for( std::size_t index = 0U; index < size; ++index) {
				buffer[index] = ResponsePattern( response_sent + index) ;
			}

			// 大容量responseの各有限chunkを完送できなければ失敗終了する
			if( !SendAll( client.Get(), std::span<const std::byte>( buffer.data(), size))) {
				finished_.store( true, std::memory_order_release) ;
				return ;
			}

			response_sent += size ;
		}

		// response完送とserver FINが成立しなければ正常終了を公開しない
		if( response_sent != kResponseBodyBytes || shutdown( client.Get(), SD_SEND) == SOCKET_ERROR) {
			finished_.store( true, std::memory_order_release) ;
			return ;
		}

		// Client HALF_CLOSEが到着するまで有限待機し、双方向FINを検証する
		bool	   peer_closed	  = false ;
		const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 5) ;
		// server側がClient HALF_CLOSEを確認するまで有限timeout内で待つ
		while( !stop_requested_.load( std::memory_order_acquire) &&
			  std::chrono::steady_clock::now() < close_deadline) {
			const int received = recv( client.Get(), reinterpret_cast<char *>( buffer.data()),
									  static_cast<int>( buffer.size()), 0) ;
			// peerのFINだけをrequest側の正常終了として採用する
			if( received == 0) {
				peer_closed = true ;
				break ;
			}

			// peer終了待ちのrecv異常は再試行可能性を確認する
			if( received == SOCKET_ERROR) {
				const int error = WSAGetLastError() ;
				// timeout・WouldBlockの間だけ有限期限内でpeer FINを待つ
				if( error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
					continue ;
				}

				break ;
			}

			// declared request body後の余剰byteを許可しない
			if( received > 0) {
				break ;
			}
		}

		succeeded_.store( peer_closed, std::memory_order_release) ;
		finished_.store( true, std::memory_order_release) ;
	}

	/**
	 * @brief blocking test Socketへ有限timeout付きで全byteを送る。
	 * @param[in] socket 接続済みSocket。
	 * @param[in] data 送信範囲。
	 * @return stop前に全byteを送れた場合はtrue。
	 */
	[[nodiscard]] bool SendAll( const SOCKET						socket,
							   const std::span<const std::byte> data) const noexcept
	{
		std::size_t offset = 0U ;
		// partial sendと短いtimeoutを処理し、stop要求へ定期的に戻る
		while( !stop_requested_.load( std::memory_order_acquire) && offset < data.size()) {
			const int sent = send( socket, reinterpret_cast<const char *>( data.data() + offset),
								  static_cast<int>( data.size() - offset), 0) ;
			// mock responseのsend異常は一時待機と実転送失敗を区別する
			if( sent == SOCKET_ERROR) {
				const int error = WSAGetLastError() ;
				// 一時的な書込不可ではoffsetを進めず同じbyte列を再送する
				if( error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
					continue ;
				}

				return false ;
			}

			// 0以下のsend結果を進捗として扱わず完送helperを失敗にする
			if( sent <= 0) {
				return false ;
			}

			offset += static_cast<std::size_t>( sent) ;
		}

		return offset == data.size() ;
	}

	/// mock serverのloopback listen Socket。Stopで受付を終了する。
	TestSocket		  listener_ ;
	/// mock要求を処理するthread。Stopで停止要求を出してjoinする。
	std::thread		  worker_ ;
	/// mock workerへ停止を通知するatomic flag。
	std::atomic<bool> stop_requested_{false} ;
	/// mock HTTP sequenceが期待どおり完了したことを通知するatomic flag。
	std::atomic<bool> succeeded_{false} ;
	/// mock workerが成功・失敗を含め処理を終えたことを示すatomic flag。
	std::atomic<bool> finished_{false} ;
	/// mock listenerへOSが割り当てたloopback TCP port。
	std::uint16_t	  port_{0U} ;
} ;

/**
 * @brief worker listenerとBridge sessionがreadyになるまで有限待機する。
 * @param[in] worker Client Proxy worker。
 * @return 2秒以内に両方readyならtrue。
 */
[[nodiscard]] bool WaitForProxyReady( const ClientProxyWorker &worker)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2) ;
	// background commandとlistener初期化を短い間隔でpollする
	while( std::chrono::steady_clock::now() < deadline) {
		const auto snapshot = worker.snapshot() ;
		// listener・Bridge成立・実portが揃った場合だけProxyを利用可能と判定する
		if( snapshot.state == zrdp::client::proxy::ClientProxyWorkerState::Listening &&
		   snapshot.bridge_ready && snapshot.listening_port != 0U) {
			return true ;
		}

		std::this_thread::sleep_for( std::chrono::milliseconds( 2)) ;
	}

	return false ;
}

/**
 * @brief worker FrameをClientBridgeでencodeしAgentBridgeへSVC相当転送する。
 * @param[in,out] worker Client owner。
 * @param[in,out] client_bridge Client SVC owner。
 * @param[in,out] agent_bridge Agent SVC owner。
 * @return 全Frame／chunkを受理できた場合はtrue。
 */
[[nodiscard]] bool PumpClientToAgent( ClientProxyWorker					&worker,
									 zrdp::client::bridge::ClientBridge &client_bridge,
									 zrdp::agent::bridge::AgentBridge	&agent_bridge)
{
	Frame frame{} ;
	bool  more_frames = false ;
	// worker Frameをproductionと同じClientBridge SVC queueへ移す
	while( worker.TakeOutboundFrame( frame, more_frames)) {
		// worker frameをClient Bridgeへqueueできなければ配送を失敗にする
		if( !client_bridge.QueueStreamFrame( frame)) {
			return false ;
		}
	}

	bool				   more_chunks = false ;
	std::vector<std::byte> chunk ;
	// ClientBridgeが分割した全chunkをAgent decoderへwire順に渡す
	while( client_bridge.TakeOutboundChunk( chunk, more_chunks)) {
		// AgentがSVC chunkを拒否した場合は往復配送を失敗にする
		if( !agent_bridge.OnChannelData( chunk)) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief Agent Socket pumpとSVC decodeを経てFrameをClient workerへ戻す。
 * @param[in,out] agent_bridge Agent SVC／relay owner。
 * @param[in,out] client_bridge Client SVC decoder。
 * @param[in,out] worker Client owner。
 * @return 全chunk／Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool PumpAgentToClient( zrdp::agent::bridge::AgentBridge	&agent_bridge,
									 zrdp::client::bridge::ClientBridge &client_bridge,
									 ClientProxyWorker					&worker)
{
	// Agent Socket relayのPump失敗時は応答排出を続けない
	if( !agent_bridge.PumpStreams( GetTickCount64())) {
		return false ;
	}

	bool				   more_chunks = false ;
	std::vector<std::byte> chunk ;
	// Agent serialized writer queueをClient decoderへwire順に戻す
	while( agent_bridge.TakeOutboundChunk( chunk, more_chunks)) {
		// ClientがAgentのSVC chunkを拒否したら配送を中止する
		if( !client_bridge.OnChannelData( chunk)) {
			return false ;
		}
	}

	Frame frame{} ;
	// ClientBridgeが所有化したstream Frameだけをworkerへ渡す
	while( client_bridge.TakeInboundStreamFrame( frame)) {
		// Client受信frameをworkerへ所有移譲できなければ試験を止める
		if( !worker.EnqueueBridgeFrame( std::move( frame))) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief non-blocking test Socketへrequestの次範囲を送る。
 * @param[in] socket Proxyへ接続済みSocket。
 * @param[in] header absolute-form request header。
 * @param[in,out] header_sent header送信済みbyte数。
 * @param[in,out] body_sent body送信済みbyte数。
 * @return fatal Socket errorがなければtrue。
 */
[[nodiscard]] bool PumpRequestSend( const SOCKET socket, const std::string_view header,
								   std::size_t &header_sent, std::size_t &body_sent) noexcept
{
	std::array<std::byte, 16U * 1024U> body{} ;
	// 一loopのsend回数を有限にしBridge Frame pumpへ制御を戻す
	for( std::size_t operation = 0U; operation < 4U; ++operation) {
		const std::byte *data = nullptr ;
		std::size_t		 size = 0U ;
		// request header未送信の間はbodyより先にheader残量を送る
		if( header_sent < header.size()) {
			data = reinterpret_cast<const std::byte *>( header.data() + header_sent) ;
			size = header.size() - header_sent ;
		}
		// header完送後は残るbodyの同じabsolute patternを生成して送る
		else if( body_sent < kRequestBodyBytes) {
			size = std::min( body.size(), kRequestBodyBytes - body_sent) ;
			// partial send再試行でも同じabsolute patternを生成する
			for( std::size_t index = 0U; index < size; ++index) {
				body[index] = RequestPattern( body_sent + index) ;
			}

			data = body.data() ;
		} else {
			break ;
		}

		const int sent =
			send( socket, reinterpret_cast<const char *>( data), static_cast<int>( size), 0) ;
		// non-blocking sendではWouldBlockだけを継続可能な結果として返す
		if( sent == SOCKET_ERROR) {
			const int error = WSAGetLastError() ;
			return error == WSAEWOULDBLOCK ;
		}

		// send進捗がない場合はheader・body offsetを変更せず失敗を返す
		if( sent <= 0) {
			return false ;
		}

		// header転送中の進捗だけをheader offsetへ計上する
		if( header_sent < header.size()) {
			header_sent += static_cast<std::size_t>( sent) ;
		} else {
			body_sent += static_cast<std::size_t>( sent) ;
		}
	}

	return true ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Phase 4のloopback HTTP Proxyからmock upstreamまでを結合検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int main()
{
	WSADATA winsock{} ;
	// Winsockを初期化できなければHTTP Proxy結合試験を開始しない
	if( WSAStartup( MAKEWORD( 2, 2), &winsock) != 0) {
		return 1 ;
	}

	MockHttpServer server ;
	// mock server起動失敗ではWinsockを終了して試験を止める
	if( !server.Start()) {
		static_cast<void>( WSACleanup()) ;
		return 2 ;
	}

	zrdp::agent::network::Allowlist allowlist ;
	const std::string				allowlist_text = "[allowlist]\n"
													 "endpoint.4001=127.0.0.1:" +
									   std::to_string( server.port()) + "\n" ;
	// loopback allowlistを構築できなければserverとWinsockを回収する
	if( !zrdp::agent::network::ParseAllowlist( allowlist_text, allowlist).succeeded()) {
		server.Stop() ;
		static_cast<void>( WSACleanup()) ;
		return 3 ;
	}

	zrdp::agent::network::WinsockRuntime agent_winsock ;
	// Agent側Winsockの開始失敗ではSocket relayを生成しない
	if( agent_winsock.Start() != 0U) {
		server.Stop() ;
		static_cast<void>( WSACleanup()) ;
		return 4 ;
	}

	zrdp::agent::network::WinsockUpstreamConnector connector ;
	AgentStreamRelay							   relay( allowlist, connector) ;
	zrdp::agent::bridge::AgentBridge			   agent_bridge ;
	agent_bridge.ConfigureStreamRelay( &relay) ;
	zrdp::client::bridge::ClientBridge client_bridge ;
	ClientProxyWorker				   worker ;
	// BridgeのHELLO往復とReady遷移が揃わなければ全試験資源を回収する
	if( !client_bridge.StartSession() || !PumpClientToAgent( worker, client_bridge, agent_bridge) ||
	   !PumpAgentToClient( agent_bridge, client_bridge, worker) ||
	   client_bridge.snapshot().state != zrdp::client::bridge::ClientBridgeState::Ready) {
		agent_winsock.Stop() ;
		server.Stop() ;
		static_cast<void>( WSACleanup()) ;
		return 5 ;
	}

	const auto capabilities = client_bridge.negotiated_capabilities() ;
	zrdp::client::proxy::ClientProxyWorkerSettings settings{} ;
	settings.port = 0U ;
	// worker開始・交渉値反映・Proxy準備完了が不成立なら試験を止める
	if( !worker.Start( settings, nullptr, nullptr) || !worker.EnqueueBridgeStart( capabilities) ||
	   !WaitForProxyReady( worker)) {
		worker.Stop() ;
		relay.StopSession() ;
		agent_winsock.Stop() ;
		server.Stop() ;
		static_cast<void>( WSACleanup()) ;
		return 6 ;
	}

	const std::uint16_t proxy_port = worker.snapshot().listening_port ;
	TestSocket			client( ::socket( AF_INET, SOCK_STREAM, IPPROTO_TCP)) ;
	sockaddr_in			proxy_address{} ;
	proxy_address.sin_family	  = AF_INET ;
	proxy_address.sin_addr.s_addr = htonl( INADDR_LOOPBACK) ;
	proxy_address.sin_port		  = htons( proxy_port) ;
	// local ProxyへのTCP接続失敗ではdata送信を開始しない
	if( client.Get() == INVALID_SOCKET ||
	   connect( client.Get(), reinterpret_cast<const sockaddr *>( &proxy_address),
			   static_cast<int>( sizeof( proxy_address))) == SOCKET_ERROR) {
		worker.Stop() ;
		relay.StopSession() ;
		agent_winsock.Stop() ;
		server.Stop() ;
		static_cast<void>( WSACleanup()) ;
		return 7 ;
	}

	u_long non_blocking = 1UL ;
	// local Socketをnon-blocking化できなければ有限Pumpを開始しない
	if( ioctlsocket( client.Get(), FIONBIO, &non_blocking) == SOCKET_ERROR) {
		worker.Stop() ;
		relay.StopSession() ;
		agent_winsock.Stop() ;
		server.Stop() ;
		static_cast<void>( WSACleanup()) ;
		return 8 ;
	}

	const std::string request_header = "REPORT http://127.0.0.1:" + std::to_string( server.port()) +
									   "/svn/Phase4?depth=1 HTTP/1.1\r\n"
									   "Host: 127.0.0.1:" +
									   std::to_string( server.port()) +
									   "\r\n"
									   "Content-Type: application/octet-stream\r\n"
									   "Proxy-Connection: keep-alive\r\n"
									   "Connection: keep-alive, X-Remove\r\n"
									   "X-Remove: removed\r\n"
									   "Content-Length: " +
									   std::to_string( kRequestBodyBytes) + "\r\n\r\n" ;
	const std::string response_header = "HTTP/1.1 207 Multi-Status\r\n"
										"Content-Length: " +
										std::to_string( kResponseBodyBytes) + "\r\n\r\n" ;
	std::size_t			   request_header_sent		= 0U ;
	std::size_t			   request_body_sent		= 0U ;
	bool				   request_shutdown			= false ;
	bool				   response_header_verified = false ;
	std::size_t			   response_body_received	= 0U ;
	bool				   response_closed			= false ;
	std::vector<std::byte> response_pending ;
	const auto			   deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 20) ;
	bool				   pump_ok	= true ;
	// local Socket、fake SVC、Agent Socketを交互に進めてbackpressureを再現する
	while( std::chrono::steady_clock::now() < deadline && pump_ok) {
		// request FIN前の間だけheaderとbodyの送信を進める
		if( !request_shutdown) {
			pump_ok = PumpRequestSend( client.Get(), request_header, request_header_sent,
									  request_body_sent) ;
			// header・body完送後だけrequest送信FINを一回要求する
			if( pump_ok && request_header_sent == request_header.size() &&
			   request_body_sent == kRequestBodyBytes) {
				// FIN送信失敗ではrequest終了済みと記録しない
				if( shutdown( client.Get(), SD_SEND) == SOCKET_ERROR) {
					pump_ok = false ;
				} else {
					request_shutdown = true ;
				}
			}
		}

		pump_ok = pump_ok && PumpClientToAgent( worker, client_bridge, agent_bridge) &&
				  PumpAgentToClient( agent_bridge, client_bridge, worker) ;

		std::array<std::byte, 16U * 1024U> received{} ;
		// local responseを有限回recvし、headerとbodyを別々に検証する
		for( std::size_t operation = 0U; operation < 8U && pump_ok; ++operation) {
			const int count = recv( client.Get(), reinterpret_cast<char *>( received.data()),
								   static_cast<int>( received.size()), 0) ;
			// local responseのrecv異常をWouldBlockと実転送失敗に分ける
			if( count == SOCKET_ERROR) {
				const int error = WSAGetLastError() ;
				// WouldBlock以外のrecv異常は結合試験の失敗として保持する
				if( error != WSAEWOULDBLOCK) {
					pump_ok = false ;
				}

				break ;
			}

			// response FINを受けた場合は受信終了を記録する
			if( count == 0) {
				response_closed = true ;
				break ;
			}

			response_pending.insert( response_pending.end(), received.begin(),
									received.begin() + count) ;
		}

		// response headerが揃った場合だけ固定headerの完全一致を検証する
		if( !response_header_verified && response_pending.size() >= response_header.size()) {
			const bool header_matches =
				std::equal( response_header.begin(), response_header.end(),
						   reinterpret_cast<const char *>( response_pending.data())) ;
			// header不一致ではbodyを正常responseとして採用しない
			if( !header_matches) {
				pump_ok = false ;
			} else {
				response_pending.erase( response_pending.begin(),
									   response_pending.begin() +
										   static_cast<std::ptrdiff_t>( response_header.size())) ;
				response_header_verified = true ;
			}
		}

		// header検証後だけbodyをabsolute offset patternで逐次検証する
		if( response_header_verified) {
			// response bodyを保持せずabsolute offset patternで逐次検証する
			for( const std::byte value : response_pending) {
				// body上限超過やpattern不一致はstreaming転送検証の失敗とする
				if( response_body_received >= kResponseBodyBytes ||
				   value != ResponsePattern( response_body_received)) {
					pump_ok = false ;
					break ;
				}

				++response_body_received ;
			}

			response_pending.clear() ;
		}

		const auto worker_snapshot = worker.snapshot() ;
		// 双方向完送・FIN・server終了・全stream回収が揃ったら有限Pumpを終える
		if( pump_ok && request_shutdown && response_header_verified &&
		   response_body_received == kResponseBodyBytes && response_closed && server.finished() &&
		   worker_snapshot.active_streams == 0U) {
			break ;
		}

		std::this_thread::sleep_for( std::chrono::milliseconds( 1)) ;
	}

	// 最終CLOSE ackをworkerへ処理させる短い猶予を与える
	const auto cleanup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2) ;
	// response完了後のCLOSE request／ackだけを追加で有限pumpする
	while( pump_ok && std::chrono::steady_clock::now() < cleanup_deadline &&
		  worker.snapshot().active_streams != 0U) {
		pump_ok = PumpClientToAgent( worker, client_bridge, agent_bridge) &&
				  PumpAgentToClient( agent_bridge, client_bridge, worker) ;
		std::this_thread::sleep_for( std::chrono::milliseconds( 1)) ;
	}

	const auto result = worker.snapshot() ;
	const bool succeeded =
		pump_ok && request_shutdown && request_header_sent == request_header.size() &&
		request_body_sent == kRequestBodyBytes && response_header_verified &&
		response_body_received == kResponseBodyBytes && response_pending.empty() &&
		response_closed && server.succeeded() && result.active_streams == 0U &&
		result.bytes_from_local ==
			request_header.size() - std::string_view( "Proxy-Connection: keep-alive\r\n").size() -
				std::string_view( "Connection: keep-alive, X-Remove\r\n").size() -
				std::string_view( "X-Remove: removed\r\n").size() -
				std::string_view( "http://127.0.0.1:" + std::to_string( server.port())).size() +
				kRequestBodyBytes &&
		result.bytes_to_local == response_header.size() + kResponseBodyBytes ;

	worker.EnqueueBridgeStop() ;
	worker.Stop() ;
	relay.StopSession() ;
	agent_winsock.Stop() ;
	server.Stop() ;
	static_cast<void>( WSACleanup()) ;
	return succeeded ? 0 : 9 ;
}
