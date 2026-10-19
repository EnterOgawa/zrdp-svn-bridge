//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_stream_relay_test.cpp
 * @brief AgentStreamRelayのpolicy境界、I/O状態、cleanupをfake Socketで検証する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details 実networkへ接続せず、connector呼出し回数と所有Socketのclose回数を
 *          外部stateで確認する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "agent/bridge/agent_stream_relay.h"
#include "agent/network/allowlist.h"
#include "agent/network/upstream_connection.h"
#include "protocol/multiplexer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using zrdp::agent::bridge::AgentStreamRelay ;
using zrdp::agent::network::IUpstreamConnection ;
using zrdp::agent::network::IUpstreamConnector ;
using zrdp::agent::network::UpstreamIoResult ;
using zrdp::agent::network::UpstreamIoStatus ;
using zrdp::protocol::Frame ;
using zrdp::protocol::MultiplexerResult ;
using zrdp::protocol::StreamEvent ;
using zrdp::protocol::StreamMultiplexer ;

/**
 * @brief fake connection破棄後もtestから確認できるI/O状態。
 */
struct FakeSocketState {
	/// pollごとに順次返す固定connect結果。pendingからの遷移を再現する。
	std::deque<UpstreamIoStatus>									connect_results ;
	/// 順次返す受信結果と所有payload。chunk分割と終了条件を再現する。
	std::deque<std::pair<UpstreamIoStatus, std::vector<std::byte>>> read_results ;
	/// fake connectionへ送ったbyteの所有記録。順序と欠落を確認する。
	std::vector<std::byte>											sent ;
	/// 一回のfake sendで受け付けるbyte上限。partial sendを再現する。
	std::size_t														maximum_send_bytes{64U * 1024U} ;
	/// fake connectionのclose回数。資源解放の重複を検出する。
	std::uint32_t													close_count{0U} ;
	/// 送信half-closeの実行回数。反対方向を維持する契約を確認する。
	std::uint32_t													shutdown_send_count{0U} ;
	/// 次のfake sendだけWouldBlockを返すための一回限りのflag。
	bool															send_would_block_once{false} ;
	/// 送信または受信がclose済みSocketを参照しないことを確認するflag。
	bool															closed{false} ;
} ;

/**
 * @brief 外部stateへ決定的なnon-blocking結果を返すfake connection。
 */
class FakeConnection final : public IUpstreamConnection {
public:
	/**
	 * @brief 共有test stateを固定する。
	 * @param[in] state test所有の状態。
	 */
	explicit FakeConnection( std::shared_ptr<FakeSocketState> state) noexcept
		: state_( std::move( state))
	{
	}

	/**
	 * @brief 所有資源を終了経路で解放し、後続の参照を残さない。
	 */
	~FakeConnection() override
	{
		Close() ;
	}

	/**
	 * @brief 予定connect結果を一件ずつ消費し、pending遷移をfakeで再現する。
	 * @return 予定status。使い切った場合はPending。
	 */
	[[nodiscard]] UpstreamIoResult PollConnect() noexcept override
	{
		// connect結果を使い切ったfakeはpendingを維持し、勝手に成功しない
		if( state_->connect_results.empty()) {
			return UpstreamIoResult{
				UpstreamIoStatus::Pending,
				0U,
				0U,
			} ;
		}

		const UpstreamIoStatus status = state_->connect_results.front() ;
		state_->connect_results.pop_front() ;
		return UpstreamIoResult{status, 0U, 0U} ;
	}

	/**
	 * @brief 部分sendと一回のWouldBlockを再現し、credit先行返却を検査する。
	 * @param[in] data fakeへ渡した送信片。成功した範囲だけstateの所有記録へコピーする。
	 * @return 送信byte数、WouldBlock、またはfixtureの失敗分類。
	 */
	[[nodiscard]] UpstreamIoResult Send( const std::span<const std::byte> data) noexcept override
	{
		// 一度だけWouldBlockを返し、relayがcreditを先行返却しないことを確認する
		if( state_->send_would_block_once) {
			state_->send_would_block_once = false ;
			return UpstreamIoResult{
				UpstreamIoStatus::WouldBlock,
				0U,
				0U,
			} ;
		}

		// closed Socketや空DATAへのfake sendは実転送として受け付けない
		if( state_->closed || data.empty()) {
			return UpstreamIoResult{
				UpstreamIoStatus::Failed,
				0U,
				1U,
			} ;
		}

		const std::size_t sent = std::min( data.size(), state_->maximum_send_bytes) ;
		try {
			state_->sent.insert( state_->sent.end(), data.begin(),
								data.begin() + static_cast<std::ptrdiff_t>( sent)) ;
		} catch( ...) {
			return UpstreamIoResult{
				UpstreamIoStatus::Failed,
				0U,
				2U,
			} ;
		}

		return UpstreamIoResult{
			UpstreamIoStatus::Completed,
			sent,
			0U,
		} ;
	}

	/**
	 * @brief 予定受信片を順に返してfragment・FIN・受信失敗を再現する。
	 * @param[out] buffer 今回の受信先。成功byte数の範囲だけを書き込む。
	 * @return 予定statusとbyte数。未予定ならWouldBlock。
	 */
	[[nodiscard]] UpstreamIoResult Receive( const std::span<std::byte> buffer) noexcept override
	{
		// close済みfake Socketからdataを返さない
		if( state_->closed) {
			return UpstreamIoResult{
				UpstreamIoStatus::Failed,
				0U,
				3U,
			} ;
		}

		// fake受信予定がなければWouldBlockとして次のPumpを待たせる
		if( state_->read_results.empty()) {
			return UpstreamIoResult{
				UpstreamIoStatus::WouldBlock,
				0U,
				0U,
			} ;
		}

		auto result = std::move( state_->read_results.front()) ;
		state_->read_results.pop_front() ;
		// fakeの非完了read結果はbyteを返さず指定statusだけを再現する
		if( result.first != UpstreamIoStatus::Completed) {
			return UpstreamIoResult{
				result.first,
				0U,
				0U,
			} ;
		}

		// test chunkは一回の受信buffer以下に限定する
		if( result.second.empty() || result.second.size() > buffer.size()) {
			return UpstreamIoResult{
				UpstreamIoStatus::Failed,
				0U,
				4U,
			} ;
		}

		std::copy( result.second.begin(), result.second.end(), buffer.begin()) ;
		return UpstreamIoResult{
			UpstreamIoStatus::Completed,
			result.second.size(),
			0U,
		} ;
	}

	/**
	 * @brief fakeの送信half-closeを記録し、反対方向を維持する契約を検証する。
	 * @return 未closeならCompleted、close済みならFailed。
	 */
	[[nodiscard]] UpstreamIoResult ShutdownSend() noexcept override
	{
		// close済みfake Socketでは送信終了操作を成功にしない
		if( state_->closed) {
			return UpstreamIoResult{
				UpstreamIoStatus::Failed,
				0U,
				5U,
			} ;
		}

		++state_->shutdown_send_count ;
		return UpstreamIoResult{
			UpstreamIoStatus::Completed,
			0U,
			0U,
		} ;
	}

	/**
	 * @brief 一つのfake接続のcloseを一度だけ数える。
	 * @note 共有stateはfixtureが維持し、connection破棄後もclose回数を確認できる。
	 */
	void Close() noexcept override
	{
		// production RAIIと同じく、一接続を一度だけcloseとして数える
		if( !state_->closed) {
			state_->closed = true ;
			++state_->close_count ;
		}
	}

private:
	/// fixtureと共有するfake I/O状態。connection破棄後も終了回数を検証できる。
	std::shared_ptr<FakeSocketState> state_ ;
} ;

/**
 * @brief BeginConnectの呼出し回数と固定結果を公開するfake factory。
 */
class FakeConnector final : public IUpstreamConnector {
public:
	/// 次の接続開始で返す固定I/O結果。異常経路を再現する。
	UpstreamIoStatus				 begin_status{UpstreamIoStatus::Completed} ;
	/// connectorへの接続開始要求の件数。拒否後も別経路を試さないことを検証する。
	std::uint32_t					 begin_count{0U} ;
	/// 新connectionとfixtureが共有所有するstate。connection破棄後も検査できる。
	std::shared_ptr<FakeSocketState> socket_state = std::make_shared<FakeSocketState>() ;

	/**
	 * @brief 指定statusで接続開始を再現し、allowlist拒否後の要求回数を検証する。
	 * @param[out] connection 受理した場合だけ生成するfake connection。失敗時はnull。
	 * @return fixtureの開始statusまたは確保失敗。
	 * @note 無名targetは宛先内容でなく呼出し回数を検査するため無視する。実Socketへ接続しない。
	 */
	[[nodiscard]] UpstreamIoResult
	BeginConnect( const zrdp::protocol::OpenTarget &,
				 std::unique_ptr<IUpstreamConnection> &connection) noexcept override
	{
		++begin_count ;
		connection.reset() ;
		// 接続開始が受理された場合だけfake connectionの所有物を生成する
		if( begin_status == UpstreamIoStatus::Completed ||
		   begin_status == UpstreamIoStatus::Pending) {
			try {
				connection = std::make_unique<FakeConnection>( socket_state) ;
			} catch( ...) {
				return UpstreamIoResult{
					UpstreamIoStatus::Failed,
					0U,
					6U,
				} ;
			}
		}

		return UpstreamIoResult{
			begin_status,
			0U,
			0U,
		} ;
	}
} ;

/**
 * @brief 各OPENへ独立したfake Socketを割り当てるmulti-stream factory。
 */
class MultiFakeConnector final : public IUpstreamConnector {
public:
	/// 同時streamごとのfake I/O state。接続開始順に割り当てる。
	std::vector<std::shared_ptr<FakeSocketState>> socket_states ;

	/**
	 * @brief 同時streamごとに独立fake stateを生成し、開始順の所有一覧へ登録する。
	 * @param[out] connection 新規stateを共有所有するfake接続。確保失敗時はnull。
	 * @return 生成成功時Completed、確保失敗はFailed。
	 * @note 無名targetは順序割当の検証に不要で、実ネットワークやallowlistを探索しない。
	 */
	[[nodiscard]] UpstreamIoResult
	BeginConnect( const zrdp::protocol::OpenTarget &,
				 std::unique_ptr<IUpstreamConnection> &connection) noexcept override
	{
		connection.reset() ;
		try {
			auto state	   = std::make_shared<FakeSocketState>() ;
			auto candidate = std::make_unique<FakeConnection>( state) ;
			socket_states.push_back( state) ;
			connection = std::move( candidate) ;
		} catch( ...) {
			connection.reset() ;
			return UpstreamIoResult{
				UpstreamIoStatus::Failed,
				0U,
				7U,
			} ;
		}

		return UpstreamIoResult{
			UpstreamIoStatus::Completed,
			0U,
			0U,
		} ;
	}
} ;

/**
 * @brief 文書用target一件の検証済みallowlistを作成する。
 * @param[out] allowlist 成功時の一件設定。
 * @return parserが成功した場合はtrue。
 */
[[nodiscard]] bool MakeAllowlist( zrdp::agent::network::Allowlist &allowlist)
{
	return zrdp::agent::network::ParseAllowlist( "[allowlist]\n"
												"endpoint.1001=192.0.2.10:80\n",
												allowlist)
		.succeeded() ;
}

/**
 * @brief Client multiplexerの全FrameをAgent relayへ渡す。
 * @param[in,out] client Client role multiplexer。
 * @param[in,out] relay Agent relay。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferToRelay( StreamMultiplexer &client, AgentStreamRelay &relay)
{
	Frame frame{} ;
	// Client outbound queueをwire順のまま完全にdrainする
	while( client.TakeOutboundFrame( frame)) {
		// Client frameをrelayが拒否したら配送helperの失敗を返す
		if( !relay.OnFrame( std::move( frame))) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief Agent relayの全FrameをClient multiplexerへ渡す。
 * @param[in,out] relay Agent relay。
 * @param[in,out] client Client role multiplexer。
 * @return 全Frameを受理できた場合はtrue。
 */
[[nodiscard]] bool TransferToClient( AgentStreamRelay &relay, StreamMultiplexer &client)
{
	Frame frame{} ;
	// Agent outbound queueをwire順のまま完全にdrainする
	while( relay.TakeOutboundFrame( frame)) {
		// relay frameをClient状態機械が拒否したら配送helperを失敗にする
		if( client.OnFrame( std::move( frame)) != MultiplexerResult::Ok) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief ClientとrelayをPhase 5の有限multi-stream capabilityで開始する。
 * @param[in,out] client Client multiplexer。
 * @param[in,out] relay Agent relay。
 * @return 両方がReadyになった場合はtrue。
 */
[[nodiscard]] bool StartSession( StreamMultiplexer &client, AgentStreamRelay &relay)
{
	auto capabilities			 = zrdp::protocol::MakePhase2HelloCapabilities() ;
	capabilities.maximum_streams = zrdp::agent::bridge::kMaximumAgentRelayStreams ;
	return client.StartSession( capabilities) == MultiplexerResult::Ok &&
		   relay.StartSession( capabilities) ;
}

/**
 * @brief 一targetのOPENをrelayへ渡し、生成responseをClientへ戻す。
 * @param[in,out] client Client multiplexer。
 * @param[in,out] relay Agent relay。
 * @param[in] target 接続候補。
 * @param[out] stream_id 割り当てID。
 * @return transport処理が成功した場合はtrue。
 */
[[nodiscard]] bool OpenAndTransfer( StreamMultiplexer &client, AgentStreamRelay &relay,
								   const zrdp::protocol::OpenTarget &target,
								   std::uint32_t					&stream_id)
{
	return client.OpenStream( target, stream_id) == MultiplexerResult::Ok &&
		   TransferToRelay( client, relay) && TransferToClient( relay, client) ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Agent stream relayのallowlist接続、I/O、timeout、closeを検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain()
{
	using zrdp::protocol::MultiplexerRole ;
	using zrdp::protocol::OpenErrorCode ;
	using zrdp::protocol::OpenTarget ;
	using zrdp::protocol::StreamCloseReason ;
	using zrdp::protocol::StreamEventType ;

	constexpr OpenTarget			allowed{{192U, 0U, 2U, 10U}, 80U} ;
	constexpr OpenTarget			denied{{192U, 0U, 2U, 11U}, 80U} ;

	zrdp::agent::network::Allowlist allowlist ;
	// 例示allowlistを構築できなければ接続方針の検証を開始しない
	if( !MakeAllowlist( allowlist)) {
		return 1 ;
	}

	// policy拒否ではSocket factoryを呼び出さず固定OPEN_ERRORだけを返す
	{
		FakeConnector	  connector ;
		AgentStreamRelay  relay( allowlist, connector) ;
		StreamMultiplexer client( MultiplexerRole::Client) ;
		// policy拒否試験の初期sessionを作れなければ検証を止める
		if( !StartSession( client, relay)) {
			return 2 ;
		}

		std::uint32_t stream_id = 0U ;
		// 拒否対象OPENを配送できなければpolicy判定の期待値を比較しない
		if( !OpenAndTransfer( client, relay, denied, stream_id)) {
			return 3 ;
		}

		StreamEvent event{} ;
		// allowlist外ではSocket factoryを呼ばず固定拒否eventが返ることを確認する
		if( connector.begin_count != 0U || !client.TakeEvent( event) ||
		   event.type != StreamEventType::OpenFailed ||
		   event.open_error != OpenErrorCode::RejectedByPolicy) {
			return 4 ;
		}
	}

	// immediate refusedは別endpointへfallbackせずConnectFailedへ変換する
	{
		FakeConnector connector ;
		connector.begin_status = UpstreamIoStatus::Failed ;
		AgentStreamRelay  relay( allowlist, connector) ;
		StreamMultiplexer client( MultiplexerRole::Client) ;
		// 接続失敗試験の前提sessionを開始できなければ失敗とする
		if( !StartSession( client, relay)) {
			return 5 ;
		}

		std::uint32_t stream_id = 0U ;
		// 許可先へのOPEN配送が成立しなければ接続失敗通知を検証できない
		if( !OpenAndTransfer( client, relay, allowed, stream_id)) {
			return 6 ;
		}

		StreamEvent event{} ;
		// Socket接続試行一回とConnectFailed eventの対応を確認する
		if( connector.begin_count != 1U || !client.TakeEvent( event) ||
		   event.type != StreamEventType::OpenFailed ||
		   event.open_error != OpenErrorCode::ConnectFailed) {
			return 7 ;
		}
	}

	// pending connectは単調時刻5秒でtimeoutし、所有Socketを一度閉じる
	{
		FakeConnector connector ;
		connector.begin_status = UpstreamIoStatus::Pending ;
		AgentStreamRelay  relay( allowlist, connector) ;
		StreamMultiplexer client( MultiplexerRole::Client) ;
		// pending connect期限試験のsession初期化が失敗したら検証を止める
		if( !StartSession( client, relay)) {
			return 8 ;
		}

		std::uint32_t stream_id = 0U ;
		// 接続待ちのまま期限を越えるPumpと応答配送が成立することを確認する
		if( client.OpenStream( allowed, stream_id) != MultiplexerResult::Ok ||
		   !TransferToRelay( client, relay) || !relay.Pump( 1000U) ||
		   !relay.Pump( 1000U + zrdp::agent::bridge::kAgentConnectTimeoutMilliseconds) ||
		   !TransferToClient( relay, client)) {
			return 9 ;
		}

		StreamEvent event{} ;
		// timeout通知とpending Socket一回closeが揃わなければ失敗とする
		if( !client.TakeEvent( event) || event.type != StreamEventType::OpenFailed ||
		   event.open_error != OpenErrorCode::Timeout ||
		   connector.socket_state->close_count != 1U) {
			return 10 ;
		}
	}

	// 接続済みupstreamが完全無通信なら30分相当の単調時刻で対象streamだけ閉じる
	{
		FakeConnector	  connector ;
		AgentStreamRelay  relay( allowlist, connector) ;
		StreamMultiplexer client( MultiplexerRole::Client) ;
		// idle timeout試験のsessionを開始できなければ失敗とする
		if( !StartSession( client, relay)) {
			return 50 ;
		}

		std::uint32_t stream_id = 0U ;
		// idle監視用streamを開けなければ期限検証へ進めない
		if( !OpenAndTransfer( client, relay, allowed, stream_id)) {
			return 51 ;
		}

		StreamEvent				event{} ;
		constexpr std::uint64_t kStartTick = 10000U ;
		// idle期限超過がTimeout付きCLOSEへ対応することを確認する
		if( !client.TakeEvent( event) || event.type != StreamEventType::Opened ||
		   !relay.Pump( kStartTick) ||
		   !relay.Pump( kStartTick + zrdp::agent::bridge::kAgentStreamIdleTimeoutMilliseconds) ||
		   !TransferToClient( relay, client) || !client.TakeEvent( event) ||
		   event.type != StreamEventType::Closed ||
		   event.close_reason != StreamCloseReason::Timeout) {
			return 52 ;
		}

		const auto timed_out = relay.snapshot() ;
		// timeout counter・Socket close回数・ACK後のstream回収を確認する
		if( timed_out.timed_out_streams != 1U || connector.socket_state->close_count != 1U ||
		   !TransferToRelay( client, relay) || relay.snapshot().active_streams != 0U) {
			return 53 ;
		}
	}

	// established streamでpartial send、WouldBlock、双方向FIN、normal CLOSEを処理する
	{
		FakeConnector connector ;
		connector.socket_state->maximum_send_bytes	  = 17U ;
		connector.socket_state->send_would_block_once = true ;
		AgentStreamRelay  relay( allowlist, connector) ;
		StreamMultiplexer client( MultiplexerRole::Client) ;
		// binary転送試験のsessionを開始できなければ失敗とする
		if( !StartSession( client, relay)) {
			return 11 ;
		}

		std::uint32_t stream_id = 0U ;
		// binary転送用streamを開けなければdataを生成しない
		if( !OpenAndTransfer( client, relay, allowed, stream_id)) {
			return 12 ;
		}

		StreamEvent event{} ;
		// Agentの接続成功がClientのOpened eventへ対応することを確認する
		if( !client.TakeEvent( event) || event.type != StreamEventType::Opened) {
			return 13 ;
		}

		std::vector<std::byte> request( 257U) ;
		// 埋込みNULを含むbinary patternで文字列変換がないことを確認する
		for( std::size_t index = 0U; index < request.size(); ++index) {
			request[index] = static_cast<std::byte>( index % 251U) ;
		}

		// request DATAをrelayへ配送できなければpartial sendを検証しない
		if( client.SendData( stream_id, request) != MultiplexerResult::Ok ||
		   !TransferToRelay( client, relay)) {
			return 14 ;
		}

		// 17-byte partial sendと一度のWouldBlockを有限Pumpで完了する
		for( std::size_t attempt = 0U;
			attempt < 32U && connector.socket_state->sent.size() < request.size(); ++attempt) {
			// partial sendの各Pumpとcredit応答を処理できなければ失敗とする
			if( !relay.Pump( 2000U + attempt) || !TransferToClient( relay, client)) {
				return 15 ;
			}
		}

		// upstream送信の全byteが元requestと完全一致することを確認する
		if( connector.socket_state->sent != request) {
			return 16 ;
		}

		std::vector<std::byte> response( 333U) ;
		// upstream応答にも別の決定的binary patternを使用する
		for( std::size_t index = 0U; index < response.size(); ++index) {
			response[index] = static_cast<std::byte>( (index * 7U) % 251U) ;
		}

		connector.socket_state->read_results.emplace_back( UpstreamIoStatus::Completed, response) ;
		connector.socket_state->read_results.emplace_back( UpstreamIoStatus::PeerClosed,
														  std::vector<std::byte>{}) ;
		// 一streamが他streamを占有しないよう、一PumpのSocket readは一回に限る
		if( !relay.Pump( 3000U) || !relay.Pump( 3001U) || !TransferToClient( relay, client)) {
			return 17 ;
		}

		std::vector<std::byte> received ;
		bool				   remote_half_closed = false ;
		// DATAとHALF_CLOSEを順序に依存せず全eventから確認する
		while( client.TakeEvent( event)) {
			// DATA eventだけからresponseを取得し、消費creditをClientへ反映する
			if( event.type == StreamEventType::DataReceived) {
				received = std::move( event.data) ;
				// 完全受信responseのcredit返却を生成できなければ検証を失敗にする
				if( client.ConsumeReceivedData( stream_id,
											  static_cast<std::uint32_t>( received.size())) !=
				   MultiplexerResult::Ok) {
					return 18 ;
				}
			}
			// upstream FIN eventをdataとは分けて記録する
			else if( event.type == StreamEventType::RemoteHalfClosed) {
				remote_half_closed = true ;
			} else {
				return 19 ;
			}
		}

		// response完全一致と両方向half-close後のPumpが成立することを確認する
		if( received != response || !remote_half_closed ||
		   client.HalfCloseStream( stream_id) != MultiplexerResult::Ok ||
		   !TransferToRelay( client, relay) || !relay.Pump( 4000U) ||
		   !TransferToClient( relay, client) || !relay.Pump( 4001U) ||
		   !TransferToClient( relay, client)) {
			return 20 ;
		}

		bool closed = false ;
		// 今回届いた有限event群から正常close acknowledgementを探す
		while( client.TakeEvent( event)) {
			// 正常終了CLOSEだけを期待する終端eventとして採用する
			if( event.type == StreamEventType::Closed &&
			   event.close_reason == StreamCloseReason::Normal) {
				closed = true ;
			}
		}

		// TCP FIN一回・Socket close一回・ACK後のrecord回収を確認する
		if( !closed || connector.socket_state->shutdown_send_count != 1U ||
		   connector.socket_state->close_count != 1U || !TransferToRelay( client, relay) ||
		   relay.snapshot().active_stream_id != 0U) {
			return 21 ;
		}
	}

	// peer resetを固定CLOSE reasonへ変換し、ack後にstream所有状態を破棄する
	{
		FakeConnector connector ;
		connector.socket_state->read_results.emplace_back( UpstreamIoStatus::PeerReset,
														  std::vector<std::byte>{}) ;
		AgentStreamRelay  relay( allowlist, connector) ;
		StreamMultiplexer client( MultiplexerRole::Client) ;
		// peer reset試験のsession初期化が失敗したら検証を止める
		if( !StartSession( client, relay)) {
			return 22 ;
		}

		std::uint32_t stream_id = 0U ;
		// peer resetを返すstreamを開けなければ失敗とする
		if( !OpenAndTransfer( client, relay, allowed, stream_id)) {
			return 23 ;
		}

		StreamEvent event{} ;
		// upstream resetが対応streamのCLOSEと完全回収へ変換されることを確認する
		if( !client.TakeEvent( event) || event.type != StreamEventType::Opened ||
		   !relay.Pump( 5000U) || !TransferToClient( relay, client) || !client.TakeEvent( event) ||
		   event.type != StreamEventType::Closed ||
		   event.close_reason != StreamCloseReason::PeerReset || !TransferToRelay( client, relay) ||
		   relay.snapshot().active_stream_id != 0U) {
			return 24 ;
		}
	}

	// RDP切断相当のStopSessionは確立Socketを一度閉じ、bufferとIDを破棄する
	{
		FakeConnector	  connector ;
		AgentStreamRelay  relay( allowlist, connector) ;
		StreamMultiplexer client( MultiplexerRole::Client) ;
		// session停止試験の前提sessionを開始できなければ失敗とする
		if( !StartSession( client, relay)) {
			return 25 ;
		}

		std::uint32_t stream_id = 0U ;
		// 停止対象streamを開けなければ全Socket回収を検証しない
		if( !OpenAndTransfer( client, relay, allowed, stream_id)) {
			return 26 ;
		}

		relay.StopSession() ;
		const auto snapshot = relay.snapshot() ;
		// StopSessionがSocket・queue・active IDを残さないことを確認する
		if( connector.socket_state->close_count != 1U ||
		   snapshot.state != zrdp::agent::bridge::AgentStreamRelayState::Stopped ||
		   snapshot.active_stream_id != 0U || snapshot.socket_write_bytes != 0U) {
			return 27 ;
		}
	}

	// 複数streamを独立Socketへrouteし、round-robin Pumpで混線なく処理する
	{
		MultiFakeConnector connector ;
		AgentStreamRelay   relay( allowlist, connector) ;
		StreamMultiplexer  client( MultiplexerRole::Client) ;
		// 多stream公平処理試験のsession初期化が失敗したら検証を止める
		if( !StartSession( client, relay)) {
			return 28 ;
		}

		std::vector<std::uint32_t> stream_ids ;
		// production同時上限までOPENし、各streamに別Socketが割り当たることを確認する
		for( std::uint32_t index = 0U; index < zrdp::agent::bridge::kMaximumAgentRelayStreams;
			++index) {
			std::uint32_t stream_id = 0U ;
			// 各許可streamを順次開けなければ同時上限を検証しない
			if( !OpenAndTransfer( client, relay, allowed, stream_id)) {
				return 29 ;
			}

			StreamEvent event{} ;
			// Opened eventが新規stream IDへ正しく対応することを確認する
			if( !client.TakeEvent( event) || event.type != StreamEventType::Opened ||
			   event.stream_id != stream_id) {
				return 30 ;
			}

			stream_ids.push_back( stream_id) ;
		}

		std::uint32_t excess_stream_id = 0U ;
		// 同時Socket上限と上限超過OPENの拒否が一致することを確認する
		if( connector.socket_states.size() != zrdp::agent::bridge::kMaximumAgentRelayStreams ||
		   relay.snapshot().active_streams != zrdp::agent::bridge::kMaximumAgentRelayStreams ||
		   client.OpenStream( allowed, excess_stream_id) != MultiplexerResult::StreamLimitExceeded) {
			return 31 ;
		}

		std::vector<std::vector<std::byte>> requests( stream_ids.size()) ;
		// stream固有patternを送信し、Socket tableの同じindexだけへ到達させる
		for( std::size_t index = 0U; index < stream_ids.size(); ++index) {
			requests[index].resize( 31U + index) ;
			// streamごとに異なるrequest patternを全byteへ設定する
			for( std::size_t offset = 0U; offset < requests[index].size(); ++offset) {
				requests[index][offset] = static_cast<std::byte>( (index * 17U + offset) % 251U) ;
			}

			// 各streamに固有のrequestをqueueできなければ分離検証を止める
			if( client.SendData( stream_ids[index], requests[index]) != MultiplexerResult::Ok) {
				return 32 ;
			}
		}

		// 一回の公平PumpとClient側応答処理が成立しなければ失敗とする
		if( !TransferToRelay( client, relay) || !relay.Pump( 6000U) ||
		   !TransferToClient( relay, client)) {
			return 33 ;
		}

		// 一Pumpで各streamを一回ずつ進める公平性とroute完全一致を確認する
		for( std::size_t index = 0U; index < requests.size(); ++index) {
			// requestが同じstreamのSocketへだけ到達することを確認する
			if( connector.socket_states[index]->sent != requests[index]) {
				return 34 ;
			}
		}

		std::vector<std::vector<std::byte>> responses( stream_ids.size()) ;
		// upstream応答もstream固有patternとして全Socketへ同時投入する
		for( std::size_t index = 0U; index < stream_ids.size(); ++index) {
			responses[index].resize( 19U + index) ;
			// requestと異なるresponse patternを全byteへ設定する
			for( std::size_t offset = 0U; offset < responses[index].size(); ++offset) {
				responses[index][offset] =
					static_cast<std::byte>( (index * 29U + offset * 3U) % 251U) ;
			}

			connector.socket_states[index]->read_results.emplace_back( UpstreamIoStatus::Completed,
																	  responses[index]) ;
		}

		// 多Socketのresponseを一回のPumpで進められることを確認する
		if( !relay.Pump( 6001U) || !TransferToClient( relay, client)) {
			return 35 ;
		}

		std::size_t received_count = 0U ;
		StreamEvent event{} ;
		// DATAをstream IDへ照合し、誤routeと重複を検出する
		while( client.TakeEvent( event)) {
			// 多stream応答試験ではDATA以外のeventを期待結果として受け付けない
			if( event.type != StreamEventType::DataReceived) {
				return 36 ;
			}

			const auto stream = std::find( stream_ids.begin(), stream_ids.end(), event.stream_id) ;
			// eventのstream IDが所有stream一覧にない場合は交差転送として失敗とする
			if( stream == stream_ids.end()) {
				return 37 ;
			}

			const std::size_t index = static_cast<std::size_t>( stream - stream_ids.begin()) ;
			// response内容とstream対応・消費creditが揃わなければ失敗とする
			if( event.data != responses[index] ||
			   client.ConsumeReceivedData( event.stream_id,
										  static_cast<std::uint32_t>( event.data.size())) !=
				   MultiplexerResult::Ok) {
				return 38 ;
			}

			++received_count ;
		}

		// 全active streamから一件ずつresponseを受信できたことを確認する
		if( received_count != stream_ids.size()) {
			return 39 ;
		}

		connector.socket_states.front()->read_results.emplace_back( UpstreamIoStatus::PeerReset,
																   std::vector<std::byte>{}) ;
		// 一つのSocket resetが対応streamだけを終了することを確認する
		if( !relay.Pump( 6002U) || !TransferToClient( relay, client) || !client.TakeEvent( event) ||
		   event.type != StreamEventType::Closed || event.stream_id != stream_ids.front() ||
		   event.close_reason != StreamCloseReason::PeerReset || !TransferToRelay( client, relay)) {
			return 40 ;
		}

		// 一Socketのresetは同じstreamだけを除き、残り七streamを継続可能にする
		if( relay.snapshot().active_streams != zrdp::agent::bridge::kMaximumAgentRelayStreams - 1U ||
		   connector.socket_states.front()->close_count != 1U) {
			return 41 ;
		}

		const std::vector<std::byte> surviving{
			std::byte{0x55U},
			std::byte{0x2AU},
		} ;
		// 他streamがreset後も固有dataを送信できることを確認する
		if( client.SendData( stream_ids[1U], surviving) != MultiplexerResult::Ok ||
		   !TransferToRelay( client, relay) || !relay.Pump( 6003U) ||
		   connector.socket_states[1U]->sent.size() < requests[1U].size() + surviving.size() ||
		   !std::equal( surviving.begin(), surviving.end(),
					   connector.socket_states[1U]->sent.end() -
						   static_cast<std::ptrdiff_t>( surviving.size()))) {
			return 42 ;
		}

		relay.StopSession() ;
		// session終了は全Socketを一度だけ閉じ、tableとqueueを破棄する
		for( const auto &state : connector.socket_states) {
			// session停止で各所有Socketが一度だけ閉じられることを確認する
			if( state->close_count != 1U) {
				return 43 ;
			}
		}

		// session停止後のactive streamが0でなければ資源漏れとして失敗とする
		if( relay.snapshot().active_streams != 0U) {
			return 44 ;
		}
	}

	return 0 ;
}
