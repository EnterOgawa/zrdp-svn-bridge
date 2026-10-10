//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file phase2_multiplexer_test.cpp
 * @brief fake Socketで複数streamの独立した双方向転送を検証する。
 * @version 01.00.00
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details 実Socket、DNS、HTTP、実endpointを使用せず、文書用IPv4 addressと
 *          in-memory bufferだけでPhase 2終了条件を確認する。
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
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace {

using zrdp::protocol::Frame ;
using zrdp::protocol::MultiplexerResult ;
using zrdp::protocol::StreamEvent ;
using zrdp::protocol::StreamMultiplexer ;

/**
 * @brief streamごとの受信byte列を所有するfake Socket集合。
 */
class FakeSockets final {
public:
	/**
	 * @brief 一streamへ受信dataを追記する。
	 * @param[in] stream_id 対象stream。
	 * @param[in] data 所有eventから受け取ったbinary data。
	 */
	void Write( const std::uint32_t stream_id, const std::vector<std::byte> &data)
	{
		std::vector<std::byte> &destination = received_[stream_id] ;
		destination.insert( destination.end(), data.begin(), data.end()) ;
	}

	/**
	 * @brief streamの受信byte列を取得する。
	 * @param[in] stream_id 対象stream。
	 * @return 存在時の所有bufferへの非owning参照。test lifetime中有効。
	 */
	[[nodiscard]] const std::vector<std::byte> &Data( const std::uint32_t stream_id) const
	{
		return received_.at( stream_id) ;
	}

private:
	/// streamIdごとに受信した所有byte列。混線しないことをfixtureで確認する。
	std::map<std::uint32_t, std::vector<std::byte>> received_ ;
} ;

/**
 * @brief sourceの全Frameをdestinationへ渡すin-memory transport。
 * @param[in,out] source 送信側。
 * @param[in,out] destination 受信側。
 * @return 全Frameを正常処理できた場合はtrue。
 */
[[nodiscard]] bool TransferAll( StreamMultiplexer &source, StreamMultiplexer &destination)
{
	Frame frame{} ;
	// outbound queueをwire順のまま完全にdrainする
	while( source.TakeOutboundFrame( frame)) {
		// 配送先のframe検証が失敗したら多重化の転送helperを中止する
		if( destination.OnFrame( std::move( frame)) != MultiplexerResult::Ok) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief pending OPEN eventをすべてfake Socketとして受理する。
 * @param[in,out] agent Agent multiplexer。
 * @param[in] expected_ids Clientが割り当てたID。
 * @return 全IDを順番どおり受理できた場合はtrue。
 */
[[nodiscard]] bool AcceptAll( StreamMultiplexer					&agent,
							 const std::array<std::uint32_t, 3> &expected_ids)
{
	StreamEvent event{} ;
	// 三つの独立endpointをOPEN event順に受理する
	for( const std::uint32_t expected_id : expected_ids) {
		// OPEN event・stream ID・Agent許可操作が揃うことを確認する
		if( !agent.TakeEvent( event) ||
		   event.type != zrdp::protocol::StreamEventType::OpenRequested ||
		   event.stream_id != expected_id ||
		   agent.AcceptStream( expected_id) != MultiplexerResult::Ok) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief DATA eventをfake Socketへ書き、同じbyte数のcreditを返す。
 * @param[in,out] receiver event所有側multiplexer。
 * @param[in,out] sockets 書込み先fake Socket集合。
 * @return pending DATAをすべて処理できた場合はtrue。
 */
[[nodiscard]] bool DrainDataEvents( StreamMultiplexer &receiver, FakeSockets &sockets)
{
	StreamEvent event{} ;
	// current batchのDATA eventをすべてdownstream消費済みへ遷移させる
	while( receiver.TakeEvent( event)) {
		// DATA以外や空dataはmock Socket転送の期待eventとして受け付けない
		if( event.type != zrdp::protocol::StreamEventType::DataReceived || event.data.empty()) {
			return false ;
		}

		sockets.Write( event.stream_id, event.data) ;
		// mock Socket書込み後のcredit返却を生成できなければ配送を失敗にする
		if( receiver.ConsumeReceivedData( event.stream_id,
										static_cast<std::uint32_t>( event.data.size())) !=
		   MultiplexerResult::Ok) {
			return false ;
		}
	}

	return true ;
}

/**
 * @brief streamごとに異なる決定的binary patternを生成する。
 * @param[in] stream_index stream配列位置。
 * @param[in] size 生成byte数。
 * @return 埋込みNULを含む所有buffer。
 */
[[nodiscard]] std::vector<std::byte> MakePayload( const std::size_t stream_index,
												 const std::size_t size)
{
	std::vector<std::byte> payload( size) ;
	// streamとoffsetを混ぜ、stream間の誤routingもbyte比較で検出する
	for( std::size_t index = 0U; index < payload.size(); ++index) {
		payload[index] = static_cast<std::byte>( (index + stream_index * 53U) % 251U) ;
	}

	return payload ;
}

/**
 * @brief payloadを最大32 KiBへ分割し、credit回復を挟みながら転送する。
 * @param[in,out] sender DATA送信側。
 * @param[in,out] receiver DATA受信側。
 * @param[in] stream_id 対象stream。
 * @param[in] payload 送信する全binary data。
 * @param[in,out] sockets receiver側fake Socket。
 * @return 全byteを送受信しWINDOW_UPDATEをsenderへ戻せた場合はtrue。
 */
[[nodiscard]] bool SendPayload( StreamMultiplexer &sender, StreamMultiplexer &receiver,
							   const std::uint32_t stream_id, const std::vector<std::byte> &payload,
							   FakeSockets &sockets)
{
	constexpr std::size_t kChunkBytes = 32U * 1024U ;
	std::size_t			  offset	  = 0U ;
	// bounded windowを継続的に回復させながら全payloadを有限chunkで送る
	while( offset < payload.size()) {
		const std::size_t current_size = std::min( kChunkBytes, payload.size() - offset) ;
		const std::span<const std::byte> chunk( payload.data() + offset, current_size) ;
		// DATA分割・受信・Socket配送・credit往復が成立することを確認する
		if( sender.SendData( stream_id, chunk) != MultiplexerResult::Ok ||
		   !TransferAll( sender, receiver) || !DrainDataEvents( receiver, sockets) ||
		   !TransferAll( receiver, sender)) {
			return false ;
		}

		offset += current_size ;
	}

	return true ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief Phase 2の複数stream多重化と双方向flow controlを結合検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain()
{
	using zrdp::protocol::MakePhase2HelloCapabilities ;
	using zrdp::protocol::MultiplexerRole ;
	using zrdp::protocol::OpenTarget ;
	using zrdp::protocol::StreamCloseReason ;
	using zrdp::protocol::StreamEventType ;

	StreamMultiplexer client( MultiplexerRole::Client) ;
	StreamMultiplexer agent( MultiplexerRole::Agent) ;
	const auto		  capabilities = MakePhase2HelloCapabilities() ;
	// 双方の多重化sessionを開始できなければ同時stream試験を実行しない
	if( client.StartSession( capabilities) != MultiplexerResult::Ok ||
	   agent.StartSession( capabilities) != MultiplexerResult::Ok) {
		return 1 ;
	}

	constexpr std::array<OpenTarget, 3> targets{
		OpenTarget{{192U, 0U, 2U, 10U}, 80U},
		OpenTarget{{198U, 51U, 100U, 20U}, 8080U},
		OpenTarget{{203U, 0U, 113U, 30U}, 18080U},
	} ;
	std::array<std::uint32_t, 3> stream_ids{} ;
	// Clientだけが三つの文書用endpointへ独立stream IDを割り当てる
	for( std::size_t index = 0U; index < targets.size(); ++index) {
		// 各OPENが有効な非0 stream IDを取得することを確認する
		if( client.OpenStream( targets[index], stream_ids[index]) != MultiplexerResult::Ok ||
		   stream_ids[index] == 0U) {
			return 2 ;
		}
	}

	// 全streamのOPENと許可応答を往復配送できることを確認する
	if( !TransferAll( client, agent) || !AcceptAll( agent, stream_ids) ||
	   !TransferAll( agent, client)) {
		return 3 ;
	}

	StreamEvent event{} ;
	// Clientが三つすべてのOPEN_OKを同じID順で受信する
	for( const std::uint32_t stream_id : stream_ids) {
		// ClientのOpened eventが期待stream IDの順に届くことを確認する
		if( !client.TakeEvent( event) || event.type != StreamEventType::Opened ||
		   event.stream_id != stream_id) {
			return 4 ;
		}
	}

	std::array<std::vector<std::byte>, 3> client_to_agent_payloads{
		MakePayload( 0U, 300U * 1024U + 17U),
		MakePayload( 1U, 420U * 1024U + 31U),
		MakePayload( 2U, 540U * 1024U + 47U),
	} ;
	std::array<std::vector<std::byte>, 3> agent_to_client_payloads{
		MakePayload( 3U, 280U * 1024U + 19U),
		MakePayload( 4U, 360U * 1024U + 37U),
		MakePayload( 5U, 460U * 1024U + 59U),
	} ;
	FakeSockets agent_sockets ;
	FakeSockets client_sockets ;

	// 各streamのClient->Agent dataをwindow回復付きで独立転送する
	for( std::size_t index = 0U; index < stream_ids.size(); ++index) {
		// 各streamのClientからAgentへの固有payload転送を確認する
		if( !SendPayload( client, agent, stream_ids[index], client_to_agent_payloads[index],
						agent_sockets)) {
			return 5 ;
		}
	}

	// 各streamのAgent->Client dataも同じconnection上で独立転送する
	for( std::size_t index = 0U; index < stream_ids.size(); ++index) {
		// 各streamのAgentからClientへの固有payload転送を確認する
		if( !SendPayload( agent, client, stream_ids[index], agent_to_client_payloads[index],
						client_sockets)) {
			return 6 ;
		}
	}

	// stream ID取り違え、欠落、順序違反を全byte比較で検出する
	for( std::size_t index = 0U; index < stream_ids.size(); ++index) {
		// 双方向の各payloadが同じstreamのmock Socketで完全一致することを確認する
		if( agent_sockets.Data( stream_ids[index]) != client_to_agent_payloads[index] ||
		   client_sockets.Data( stream_ids[index]) != agent_to_client_payloads[index]) {
			return 7 ;
		}
	}

	// 双方向HALF_CLOSEを全streamへ適用し、eventを互いに確認する
	for( const std::uint32_t stream_id : stream_ids) {
		// 全streamで両側の送信half-closeを生成できることを確認する
		if( client.HalfCloseStream( stream_id) != MultiplexerResult::Ok ||
		   agent.HalfCloseStream( stream_id) != MultiplexerResult::Ok) {
			return 8 ;
		}
	}

	// 両方向のHALF_CLOSEを配送できなければ終端eventを検証しない
	if( !TransferAll( client, agent) || !TransferAll( agent, client)) {
		return 9 ;
	}

	// 両端に各stream一件ずつRemoteHalfClosed eventが届く
	for( const std::uint32_t stream_id : stream_ids) {
		// Clientが各streamのremote FINを順序どおり受信することを確認する
		if( !client.TakeEvent( event) || event.type != StreamEventType::RemoteHalfClosed ||
		   event.stream_id != stream_id) {
			return 10 ;
		}
	}

	// Agent側にも同じ順序で全streamのRemoteHalfClosedが届いたことを確認する
	for( const std::uint32_t stream_id : stream_ids) {
		// Agentが各streamのremote FINを順序どおり受信することを確認する
		if( !agent.TakeEvent( event) || event.type != StreamEventType::RemoteHalfClosed ||
		   event.stream_id != stream_id) {
			return 11 ;
		}
	}

	// Client request／Agent ackで全streamを順序どおり終了する
	for( const std::uint32_t stream_id : stream_ids) {
		// Clientから各streamの正常終了CLOSEを生成できることを確認する
		if( client.CloseStream( stream_id, StreamCloseReason::Normal) != MultiplexerResult::Ok) {
			return 12 ;
		}
	}

	// 全CLOSEをAgentへ配送できなければ終了処理を成功にしない
	if( !TransferAll( client, agent)) {
		return 13 ;
	}

	// Agentが各close requestを順序どおりClosed eventへ変換したことを確認する
	for( const std::uint32_t stream_id : stream_ids) {
		// AgentのClosed eventが対応streamの順序を維持することを確認する
		if( !agent.TakeEvent( event) || event.type != StreamEventType::Closed ||
		   event.stream_id != stream_id) {
			return 14 ;
		}
	}

	// Agentの終了ACKをClientへ配送できることを確認する
	if( !TransferAll( agent, client)) {
		return 15 ;
	}

	// Clientが全close acknowledgementを受理し、同じ順序で終了したことを確認する
	for( const std::uint32_t stream_id : stream_ids) {
		// Clientが終了ACKを同じstream順序で受理することを確認する
		if( !client.TakeEvent( event) || event.type != StreamEventType::Closed ||
		   event.stream_id != stream_id) {
			return 16 ;
		}
	}

	// 全record回収と双方向connection creditの整合性を確認する
	if( client.snapshot().active_streams != 0U || agent.snapshot().active_streams != 0U ||
	   client.snapshot().connection_send_window != agent.snapshot().connection_receive_window ||
	   agent.snapshot().connection_send_window != client.snapshot().connection_receive_window) {
		return 17 ;
	}

	return 0 ;
}
