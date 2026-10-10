//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file frame_test.cpp
 * @brief ESVNBRG frameのwire形式、fragmentation、異常拒否を検証する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details network接続を行わず、文書用dataだけをin-memoryで扱う。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "protocol/frame.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

/**
 * @brief 埋込みNULを含む決定的なtest payloadを作る。
 * @param[in] size 生成byte数。
 * @return 指定長の所有payload。
 */
[[nodiscard]] std::vector<std::byte> MakePayload( const std::size_t size)
{
	std::vector<std::byte> payload( size) ;
	// byte列の先頭をNULとし、文字列処理で欠落する実装を検出する
	for( std::size_t index = 0U; index < size; ++index) {
		payload[index] = static_cast<std::byte>( index % 251U) ;
	}

	return payload ;
}

/**
 * @brief 一つのframeが期待値と完全一致するか確認する。
 * @param[in] frame decode済みframe。
 * @param[in] expected_payload 期待するbinary payload。
 * @return 全header fieldとpayloadが一致すればtrue。
 */
[[nodiscard]] bool IsExpectedPing( const zrdp::protocol::Frame	  &frame,
								  const std::span<const std::byte> expected_payload)
{
	return frame.type == zrdp::protocol::MessageType::Ping && frame.stream_id == 0U &&
		   frame.sequence == 0x01020304U &&
		   std::equal( frame.payload.begin(), frame.payload.end(), expected_payload.begin(),
					  expected_payload.end()) ;
}

} // namespace

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief frame wire形式、fragment再構成、malformed入力拒否を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain()
{
	using zrdp::protocol::EncodeFrame ;
	using zrdp::protocol::Frame ;
	using zrdp::protocol::FrameDecoder ;
	using zrdp::protocol::FrameError ;
	using zrdp::protocol::MessageType ;

	const std::vector<std::byte> payload = MakePayload( 4097U) ;
	Frame						 frame{} ;
	frame.type	   = MessageType::Ping ;
	frame.sequence = 0x01020304U ;
	frame.payload  = payload ;

	std::vector<std::byte> encoded ;
	// 有効frameを固定headerとpayloadへencodeできることを確認する
	if( EncodeFrame( frame, encoded) != FrameError::None) {
		return 1 ;
	}

	// magic、version、header size、type、sequence、payload lengthをbyte単位で固定する
	constexpr std::array<std::byte, 24> expected_header{
		std::byte{0x45U}, std::byte{0x53U}, std::byte{0x56U}, std::byte{0x4EU}, std::byte{0x00U},
		std::byte{0x01U}, std::byte{0x00U}, std::byte{0x18U}, std::byte{0x00U}, std::byte{0x0AU},
		std::byte{0x00U}, std::byte{0x00U}, std::byte{0x00U}, std::byte{0x00U}, std::byte{0x00U},
		std::byte{0x00U}, std::byte{0x01U}, std::byte{0x02U}, std::byte{0x03U}, std::byte{0x04U},
		std::byte{0x00U}, std::byte{0x00U}, std::byte{0x10U}, std::byte{0x01U},
	} ;
	// wire headerのbyte orderと固定field配置が期待値と一致することを確認する
	if( !std::equal( expected_header.begin(), expected_header.end(), encoded.begin())) {
		return 2 ;
	}

	// 全ての二分割位置で同じ一frameへ再構成できることを確認する
	for( std::size_t split = 0U; split <= encoded.size(); ++split) {
		FrameDecoder	   decoder ;
		std::vector<Frame> decoded ;
		// 各二分割位置の前半fragmentをdecoderが受理することを確認する
		if( decoder.Push( std::span<const std::byte>( encoded).first( split), decoded) !=
		   FrameError::None) {
			return 3 ;
		}

		// 完全長を一回で渡した場合は、その時点で同じframeを返す
		if( split == encoded.size()) {
			// 全byteを前半で受信した場合だけ一件の完全frameとなることを確認する
			if( decoded.size() != 1U || !IsExpectedPing( decoded.front(), payload)) {
				return 4 ;
			}

			continue ;
		}

		// 完全長未満の一回目でframeを早期公開しない
		if( !decoded.empty()) {
			return 5 ;
		}

		// 後半fragmentで同じ一件のframeを完全再構成できることを確認する
		if( decoder.Push( std::span<const std::byte>( encoded).subspan( split), decoded) !=
			   FrameError::None ||
		   decoded.size() != 1U || !IsExpectedPing( decoded.front(), payload)) {
			return 6 ;
		}
	}

	FrameDecoder	   byte_decoder ;
	std::vector<Frame> decoded ;
	// transportが一byteずつ分割しても順序を維持して再構成する
	for( std::size_t index = 0U; index < encoded.size(); ++index) {
		// 一byteずつのfragmentもdecoderが順に受理することを確認する
		if( byte_decoder.Push( std::span<const std::byte>( encoded).subspan( index, 1U), decoded) !=
		   FrameError::None) {
			return 7 ;
		}

		// 最終byteより前に不完全frameを公開しない
		if( index + 1U < encoded.size() && !decoded.empty()) {
			return 8 ;
		}
	}

	// 一byte分割の完了時にpayloadを含む一件だけのframeとなることを確認する
	if( decoded.size() != 1U || !IsExpectedPing( decoded.front(), payload)) {
		return 9 ;
	}

	std::vector<std::byte> two_frames = encoded ;
	two_frames.insert( two_frames.end(), encoded.begin(), encoded.end()) ;
	FrameDecoder multi_decoder ;
	// 一回の受信に連結された複数frameも同じ順序で返す
	if( multi_decoder.Push( two_frames, decoded) != FrameError::None || decoded.size() != 2U ||
	   !IsExpectedPing( decoded[0], payload) || !IsExpectedPing( decoded[1], payload)) {
		return 10 ;
	}

	/**
	 * @cond LOCAL_CALLBACK_CONTRACT
	 * @brief 一header byteを破損させ、分類とdecoderのfail-closedを検証する。
	 * @param[in] offset 既知のheader内で置換するbyte位置。
	 * @param[in] replacement fixtureの破損値。元のencoded dataは変更しない。
	 * @param[in] expected 期待する固定decoder error分類。
	 * @return 期待分類で拒否し、frame未出力とfailed状態が成立すればtrue。
	 * @endcond
	 */
	auto expect_decode_error = [&encoded]( const std::size_t offset, const std::byte replacement,
										  const FrameError expected) {
		std::vector<std::byte> malformed = encoded ;
		malformed[offset]				 = replacement ;
		FrameDecoder	   decoder ;
		std::vector<Frame> frames ;
		return decoder.Push( malformed, frames) == expected && frames.empty() && decoder.failed() ;
	} ;

	// magic不一致後に別境界を探索せずBridgeを停止する
	if( !expect_decode_error( 0U, std::byte{0x00U}, FrameError::InvalidMagic)) {
		return 11 ;
	}

	// version不一致を暗黙downgradeしない
	if( !expect_decode_error( 5U, std::byte{0x02U}, FrameError::UnsupportedVersion)) {
		return 12 ;
	}

	// 未知header sizeを現layoutとして解釈しない
	if( !expect_decode_error( 7U, std::byte{0x19U}, FrameError::InvalidHeaderSize)) {
		return 13 ;
	}

	// 未知typeを任意extensionとして通過させない
	if( !expect_decode_error( 9U, std::byte{0x7FU}, FrameError::UnknownMessageType)) {
		return 14 ;
	}

	// 現versionで定義していないflagsを拒否する
	if( !expect_decode_error( 11U, std::byte{0x01U}, FrameError::UnsupportedFlags)) {
		return 15 ;
	}

	// connection controlへ非0 streamIdを指定できない
	if( !expect_decode_error( 15U, std::byte{0x01U}, FrameError::InvalidStreamId)) {
		return 16 ;
	}

	std::vector<std::byte> oversized_header( zrdp::protocol::kFrameHeaderBytes, std::byte{0U}) ;
	std::copy( encoded.begin(),
			  encoded.begin() + static_cast<std::ptrdiff_t>( zrdp::protocol::kFrameHeaderBytes),
			  oversized_header.begin()) ;
	constexpr std::uint32_t oversized_length = zrdp::protocol::kMaximumFramePayloadBytes + 1U ;
	oversized_header[20] = static_cast<std::byte>( (oversized_length >> 24U) & 0xFFU) ;
	oversized_header[21] = static_cast<std::byte>( (oversized_length >> 16U) & 0xFFU) ;
	oversized_header[22] = static_cast<std::byte>( (oversized_length >> 8U) & 0xFFU) ;
	oversized_header[23] = static_cast<std::byte>( oversized_length & 0xFFU) ;
	FrameDecoder oversized_decoder ;
	// 宣言長だけでも上限超過を検出し、payload到着を待たない
	if( oversized_decoder.Push( oversized_header, decoded) != FrameError::OversizedPayload) {
		return 17 ;
	}

	Frame oversized_frame{} ;
	oversized_frame.type = MessageType::Ping ;
	oversized_frame.payload.resize(
		static_cast<std::size_t>( zrdp::protocol::kMaximumFramePayloadBytes) + 1U) ;
	// 送信側も上限超過frameを生成しない
	if( EncodeFrame( oversized_frame, encoded) != FrameError::OversizedPayload || !encoded.empty()) {
		return 18 ;
	}

	Frame invalid_stream{} ;
	invalid_stream.type		 = MessageType::Data ;
	invalid_stream.stream_id = 0U ;
	// 将来stream messageにも予約streamId 0を使用させない
	if( EncodeFrame( invalid_stream, encoded) != FrameError::InvalidStreamId) {
		return 19 ;
	}

	std::vector<std::byte> excessive_input( zrdp::protocol::kMaximumDecoderInputBytes + 1U,
										   std::byte{0U}) ;
	FrameDecoder		   input_limit_decoder ;
	// 一回のcallbackによる無制限buffer確保を入力前に拒否する
	if( input_limit_decoder.Push( excessive_input, decoded) != FrameError::InputLimitExceeded) {
		return 20 ;
	}

	Frame connection_close{} ;
	connection_close.type = MessageType::Close ;
	// CLOSEはAgent終了通知用streamId 0と将来stream用非0の両方をencode可能にする
	if( EncodeFrame( connection_close, encoded) != FrameError::None) {
		return 21 ;
	}

	FrameDecoder close_decoder ;
	// connection-level CLOSEの予約stream ID・sequence・空payloadを確認する
	if( close_decoder.Push( encoded, decoded) != FrameError::None || decoded.size() != 1U ||
	   decoded.front().type != MessageType::Close || decoded.front().stream_id != 0U ||
	   decoded.front().sequence != 0U || !decoded.front().payload.empty()) {
		return 22 ;
	}

	connection_close.stream_id = 7U ;
	// 有効connection CLOSEを再encodeできなければ固定frame試験を失敗にする
	if( EncodeFrame( connection_close, encoded) != FrameError::None) {
		return 23 ;
	}

	Frame connection_window_update{} ;
	connection_window_update.type	   = MessageType::WindowUpdate ;
	connection_window_update.stream_id = 0U ;
	connection_window_update.payload   = {
		  std::byte{0x00U},
		  std::byte{0x00U},
		  std::byte{0x04U},
		  std::byte{0x00U},
	  } ;
	// Phase 2ではstreamId 0のWINDOW_UPDATEをconnection全体creditへ使用する
	if( EncodeFrame( connection_window_update, encoded) != FrameError::None) {
		return 24 ;
	}

	return 0 ;
}
