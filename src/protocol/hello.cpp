//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file hello.cpp
 * @brief HELLO capabilityの明示的なnetwork byte order変換と交渉を実装する。
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

#include "protocol/hello.h"

#include <algorithm>
#include <new>

namespace zrdp::protocol {
namespace {

/**
 * @brief capability値が製品の絶対上限内か確認する。
 * @param[in] capabilities 検証対象。
 * @return allocation、queue、transport上限として安全ならtrue。
 */
[[nodiscard]] constexpr bool IsValidCapabilities( const HelloCapabilities &capabilities) noexcept
{
	// EchoなしのpeerとはPhase 1 sessionを成立させない
	if( (capabilities.feature_bits & kFeatureEcho) == 0U) {
		return false ;
	}

	// peer宣言値をframe allocationへ使う前に製品上限内へ制限する
	if( capabilities.maximum_frame_payload == 0U ||
	   capabilities.maximum_frame_payload > kMaximumFramePayloadBytes) {
		return false ;
	}

	// 将来stream上限もHELLO段階から無制限値を受け付けない
	if( capabilities.maximum_streams > kMaximumNegotiableStreams) {
		return false ;
	}

	// 将来window値を32-bitであっても有限な製品上限へ制限する
	if( capabilities.initial_window_bytes > kMaximumNegotiableWindowBytes) {
		return false ;
	}

	const bool streams_enabled = (capabilities.feature_bits & kFeatureMultiplexedStreams) != 0U ;
	// stream featureと上限値を一体で検証し、0 windowのready状態を作らない
	if( streams_enabled &&
	   (capabilities.maximum_streams == 0U || capabilities.initial_window_bytes == 0U)) {
		return false ;
	}

	// stream非対応peerの非0値を暗黙に機能有効化せず、capabilityを一意にする
	if( !streams_enabled &&
	   (capabilities.maximum_streams != 0U || capabilities.initial_window_bytes != 0U)) {
		return false ;
	}

	// SVC writeをMicrosoftのdocumented chunk上限より大きく交渉しない
	return capabilities.transport_chunk_bytes > 0U &&
		   capabilities.transport_chunk_bytes <= kStaticVirtualChannelChunkBytes ;
}

/**
 * @brief big-endian 32-bit値を既確保payloadへ書く。
 * @param[in,out] output 20 byte以上の出力span。
 * @param[in] offset 書込み位置。
 * @param[in] value host byte orderの値。
 */
constexpr void WriteUint32( const std::span<std::byte> output, const std::size_t offset,
						   const std::uint32_t value) noexcept
{
	output[offset]		= static_cast<std::byte>( (value >> 24U) & 0xFFU) ;
	output[offset + 1U] = static_cast<std::byte>( (value >> 16U) & 0xFFU) ;
	output[offset + 2U] = static_cast<std::byte>( (value >> 8U) & 0xFFU) ;
	output[offset + 3U] = static_cast<std::byte>( value & 0xFFU) ;
}

/**
 * @brief big-endian 32-bit値をHELLO payloadから読む。
 * @param[in] input 20 byteの検証済みspan。
 * @param[in] offset 読取り位置。
 * @return host byte orderの値。
 */
[[nodiscard]] constexpr std::uint32_t ReadUint32( const std::span<const std::byte> input,
												 const std::size_t				  offset) noexcept
{
	return (std::to_integer<std::uint32_t>( input[offset]) << 24U) |
		   (std::to_integer<std::uint32_t>( input[offset + 1U]) << 16U) |
		   (std::to_integer<std::uint32_t>( input[offset + 2U]) << 8U) |
		   std::to_integer<std::uint32_t>( input[offset + 3U]) ;
}

} // namespace

FrameError EncodeHelloCapabilities( const HelloCapabilities &capabilities,
								   std::vector<std::byte>  &payload) noexcept
{
	payload.clear() ;

	// local設定でも絶対上限を迂回できないようencode前に同じ検証を適用する
	if( !IsValidCapabilities( capabilities)) {
		return FrameError::OversizedPayload ;
	}

	try {
		payload.resize( kHelloPayloadBytes) ;
		const std::span<std::byte> output( payload) ;
		WriteUint32( output, 0U, capabilities.feature_bits) ;
		WriteUint32( output, 4U, capabilities.maximum_frame_payload) ;
		WriteUint32( output, 8U, capabilities.maximum_streams) ;
		WriteUint32( output, 12U, capabilities.initial_window_bytes) ;
		WriteUint32( output, 16U, capabilities.transport_chunk_bytes) ;
	} catch( const std::bad_alloc &) {
		payload.clear() ;
		return FrameError::ResourceExhausted ;
	} catch( ...) {
		payload.clear() ;
		return FrameError::ResourceExhausted ;
	}

	return FrameError::None ;
}

FrameError DecodeHelloCapabilities( const std::span<const std::byte> payload,
								   HelloCapabilities			   &capabilities) noexcept
{
	// field欠落やtrailing dataを別versionとして近似解釈しない
	if( payload.size() != kHelloPayloadBytes) {
		return FrameError::InvalidHeaderSize ;
	}

	HelloCapabilities candidate{} ;
	candidate.feature_bits			= ReadUint32( payload, 0U) ;
	candidate.maximum_frame_payload = ReadUint32( payload, 4U) ;
	candidate.maximum_streams		= ReadUint32( payload, 8U) ;
	candidate.initial_window_bytes	= ReadUint32( payload, 12U) ;
	candidate.transport_chunk_bytes = ReadUint32( payload, 16U) ;

	// peer宣言値をsession stateへ公開する前に全fieldを検証する
	if( !IsValidCapabilities( candidate)) {
		return FrameError::OversizedPayload ;
	}

	capabilities = candidate ;
	return FrameError::None ;
}

FrameError NegotiateHelloCapabilities( const HelloCapabilities &local, const HelloCapabilities &peer,
									  HelloCapabilities &negotiated) noexcept
{
	// encode済みlocal値を前提にせず、両端を同じ絶対境界で再検証する
	if( !IsValidCapabilities( local) || !IsValidCapabilities( peer)) {
		return FrameError::OversizedPayload ;
	}

	HelloCapabilities result{} ;
	result.feature_bits = local.feature_bits & peer.feature_bits ;
	// 共通Echo機能を持たないsessionをreadyとして公開しない
	if( (result.feature_bits & kFeatureEcho) == 0U) {
		return FrameError::UnknownMessageType ;
	}

	result.maximum_frame_payload =
		std::min( local.maximum_frame_payload, peer.maximum_frame_payload) ;
	result.maximum_streams		= std::min( local.maximum_streams, peer.maximum_streams) ;
	result.initial_window_bytes = std::min( local.initial_window_bytes, peer.initial_window_bytes) ;
	// 共通stream featureがなければ、片側の非0上限をsessionへ公開しない
	if( (result.feature_bits & kFeatureMultiplexedStreams) == 0U) {
		result.maximum_streams		= 0U ;
		result.initial_window_bytes = 0U ;
	}

	result.transport_chunk_bytes =
		std::min( local.transport_chunk_bytes, peer.transport_chunk_bytes) ;
	// min計算後もstream featureに必要な下限が残ることを確認する
	if( !IsValidCapabilities( result)) {
		return FrameError::OversizedPayload ;
	}

	negotiated = result ;
	return FrameError::None ;
}

} // namespace zrdp::protocol
