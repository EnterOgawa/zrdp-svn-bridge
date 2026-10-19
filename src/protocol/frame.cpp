//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file frame.cpp
 * @brief ESVNBRG frameの明示的なbyte encodeとincremental decodeを実装する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details wire長、offset、type、streamIdを使用前に検証し、allocation failureを
 *          protocol境界内で分類する。受信payloadの内容はlogへ渡さない。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  限定Agent controlと交渉featureを追加。
 * </pre>
 */
//------------------------------------------------------------------------------

#include "protocol/frame.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace zrdp::protocol {
namespace {

// wire上でframe先頭を識別するASCII magic
constexpr std::array<std::byte, 4> kFrameMagic{
	std::byte{0x45U},
	std::byte{0x53U},
	std::byte{0x56U},
	std::byte{0x4EU},
} ;

/**
 * @brief raw type値を定義済みMessageTypeへ検証変換する。
 * @param[in] raw_type wireから読み取った符号なし値。
 * @param[out] type 既知値の場合に対応する列挙値。
 * @return 定義済みtypeならtrue。
 */
[[nodiscard]] constexpr bool TryParseMessageType( const std::uint16_t raw_type,
												 MessageType		&type) noexcept
{
	// 未知typeを将来typeとして黙って通過させず、現versionではfail-closedにする
	if( raw_type < static_cast<std::uint16_t>( MessageType::Hello) ||
	   raw_type > static_cast<std::uint16_t>( MessageType::AgentStop)) {
		return false ;
	}

	type = static_cast<MessageType>( raw_type) ;
	return true ;
}

/**
 * @brief wire bufferの指定位置からbig-endian 16-bit値を読む。
 * @param[in] data header全体を含む受信span。
 * @param[in] offset 読取り開始位置。
 * @return host byte orderへ変換した値。
 */
[[nodiscard]] constexpr std::uint16_t ReadUint16( const std::span<const std::byte> data,
												 const std::size_t				  offset) noexcept
{
	return static_cast<std::uint16_t>( (std::to_integer<std::uint16_t>( data[offset]) << 8U) |
									  std::to_integer<std::uint16_t>( data[offset + 1U])) ;
}

/**
 * @brief wire bufferの指定位置からbig-endian 32-bit値を読む。
 * @param[in] data header全体を含む受信span。
 * @param[in] offset 読取り開始位置。
 * @return host byte orderへ変換した値。
 */
[[nodiscard]] constexpr std::uint32_t ReadUint32( const std::span<const std::byte> data,
												 const std::size_t				  offset) noexcept
{
	return (std::to_integer<std::uint32_t>( data[offset]) << 24U) |
		   (std::to_integer<std::uint32_t>( data[offset + 1U]) << 16U) |
		   (std::to_integer<std::uint32_t>( data[offset + 2U]) << 8U) |
		   std::to_integer<std::uint32_t>( data[offset + 3U]) ;
}

/**
 * @brief host 16-bit値をbig-endianで既確保bufferへ書く。
 * @param[in,out] output header領域を確保済みの出力buffer。
 * @param[in] offset 書込み開始位置。
 * @param[in] value host byte orderの値。
 */
constexpr void WriteUint16( const std::span<std::byte> output, const std::size_t offset,
						   const std::uint16_t value) noexcept
{
	output[offset]		= static_cast<std::byte>( (value >> 8U) & 0xFFU) ;
	output[offset + 1U] = static_cast<std::byte>( value & 0xFFU) ;
}

/**
 * @brief host 32-bit値をbig-endianで既確保bufferへ書く。
 * @param[in,out] output header領域を確保済みの出力buffer。
 * @param[in] offset 書込み開始位置。
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
 * @brief message typeとstreamIdのconnection／stream境界を検証する。
 * @param[in] type 検証対象type。
 * @param[in] stream_id wireのstreamId。
 * @return 現versionで有効な組合せならtrue。
 */
[[nodiscard]] constexpr bool IsValidStreamId( const MessageType	 type,
											 const std::uint32_t stream_id) noexcept
{
	// CLOSEとWINDOW_UPDATEはconnection全体と個別streamの両scopeを持つ
	if( type == MessageType::Close || type == MessageType::WindowUpdate) {
		return true ;
	}

	// connection controlを任意streamへ偽装できないよう0へ固定する
	if( IsConnectionControlMessage( type)) {
		return stream_id == 0U ;
	}

	// Phase 2のstream messageは予約値0を使用できない
	return stream_id != 0U ;
}

/**
 * @brief decoderをerror状態へ移し、保持dataと出力を破棄する。
 * @param[in,out] buffer 破棄する未完成受信data。
 * @param[in,out] decoded 部分的に完成していた今回の出力。
 * @param[in,out] failed decoderのfail-closed flag。
 * @param[in] error 呼出し側へ返す分類。
 * @return 指定したerror。
 */
[[nodiscard]] FrameError FailDecode( std::vector<std::byte> &buffer, std::vector<Frame> &decoded,
									bool &failed, const FrameError error) noexcept
{
	buffer.clear() ;
	decoded.clear() ;
	failed = true ;
	return error ;
}

} // namespace

FrameError EncodeFrame( const Frame &frame, std::vector<std::byte> &encoded) noexcept
{
	encoded.clear() ;

	// payload長を32-bit wire fieldと製品上限の両方へ収める
	if( frame.payload.size() > static_cast<std::size_t>( kMaximumFramePayloadBytes)) {
		return FrameError::OversizedPayload ;
	}

	// connection controlとstream dataの予約streamId規則をencode側でも強制する
	if( !IsValidStreamId( frame.type, frame.stream_id)) {
		return FrameError::InvalidStreamId ;
	}

	const auto	raw_type	   = static_cast<std::uint16_t>( frame.type) ;
	MessageType validated_type = MessageType::Hello ;
	// 列挙型へ不正なcastを行った呼出し元もwireへ出さない
	if( !TryParseMessageType( raw_type, validated_type)) {
		return FrameError::UnknownMessageType ;
	}

	try {
		const std::size_t encoded_size = kFrameHeaderBytes + frame.payload.size() ;
		encoded.resize( encoded_size) ;
		const std::span<std::byte> output( encoded) ;

		std::copy( kFrameMagic.begin(), kFrameMagic.end(), output.begin()) ;
		WriteUint16( output, 4U, kProtocolVersion) ;
		WriteUint16( output, 6U, static_cast<std::uint16_t>( kFrameHeaderBytes)) ;
		WriteUint16( output, 8U, raw_type) ;
		WriteUint16( output, 10U, 0U) ;
		WriteUint32( output, 12U, frame.stream_id) ;
		WriteUint32( output, 16U, frame.sequence) ;
		WriteUint32( output, 20U, static_cast<std::uint32_t>( frame.payload.size())) ;

		// payloadは文字列変換せず、明示長のままheader直後へcopyする
		std::copy( frame.payload.begin(), frame.payload.end(),
				  output.begin() + static_cast<std::ptrdiff_t>( kFrameHeaderBytes)) ;
	} catch( const std::bad_alloc &) {
		encoded.clear() ;
		return FrameError::ResourceExhausted ;
	} catch( ...) {
		// C++ library境界の予期しない例外も部分frameを公開せず分類する
		encoded.clear() ;
		return FrameError::ResourceExhausted ;
	}

	return FrameError::None ;
}

FrameError FrameDecoder::Push( const std::span<const std::byte> fragment,
							  std::vector<Frame>			  &decoded) noexcept
{
	decoded.clear() ;

	// malformed input後に別位置から暗黙再同期しない
	if( failed_) {
		return FrameError::InputLimitExceeded ;
	}

	// 一回のcallbackが無制限な所有copyを要求しないよう入力自体を制限する
	if( fragment.size() > kMaximumDecoderInputBytes ||
	   buffer_.size() > kMaximumDecoderInputBytes - fragment.size()) {
		return FailDecode( buffer_, decoded, failed_, FrameError::InputLimitExceeded) ;
	}

	try {
		buffer_.insert( buffer_.end(), fragment.begin(), fragment.end()) ;

		std::size_t consumed = 0U ;
		// 一つのtransport inputに連結された全frameを順番どおり取り出す
		while( buffer_.size() - consumed >= kFrameHeaderBytes) {
			const std::span<const std::byte> available( buffer_.data() + consumed,
													   buffer_.size() - consumed) ;

			// 境界不明のdataをmagic探索で読み飛ばさずfail-closedにする
			if( !std::equal( kFrameMagic.begin(), kFrameMagic.end(), available.begin())) {
				return FailDecode( buffer_, decoded, failed_, FrameError::InvalidMagic) ;
			}

			const std::uint16_t version = ReadUint16( available, 4U) ;
			// protocol downgradeや近似互換を行わず、完全一致だけを受け付ける
			if( version != kProtocolVersion) {
				return FailDecode( buffer_, decoded, failed_, FrameError::UnsupportedVersion) ;
			}

			const std::uint16_t header_size = ReadUint16( available, 6U) ;
			// 未知拡張headerを現versionのfield位置で誤解釈しない
			if( header_size != kFrameHeaderBytes) {
				return FailDecode( buffer_, decoded, failed_, FrameError::InvalidHeaderSize) ;
			}

			MessageType type = MessageType::Hello ;
			// 未知typeは必須か任意か判断できないため処理を継続しない
			if( !TryParseMessageType( ReadUint16( available, 8U), type)) {
				return FailDecode( buffer_, decoded, failed_, FrameError::UnknownMessageType) ;
			}

			// version 1ではflags拡張を定義していないため0以外を拒否する
			if( ReadUint16( available, 10U) != 0U) {
				return FailDecode( buffer_, decoded, failed_, FrameError::UnsupportedFlags) ;
			}

			const std::uint32_t stream_id = ReadUint32( available, 12U) ;
			// control messageとstream messageのrouting境界を曖昧にしない
			if( !IsValidStreamId( type, stream_id)) {
				return FailDecode( buffer_, decoded, failed_, FrameError::InvalidStreamId) ;
			}

			const std::uint32_t payload_length = ReadUint32( available, 20U) ;
			// 宣言長をallocationやoffset加算に使う前に有限上限を検証する
			if( payload_length > kMaximumFramePayloadBytes) {
				return FailDecode( buffer_, decoded, failed_, FrameError::OversizedPayload) ;
			}

			const std::size_t frame_size =
				kFrameHeaderBytes + static_cast<std::size_t>( payload_length) ;
			// 未完成payloadは次のfragmentまでbufferに保持する
			if( available.size() < frame_size) {
				break ;
			}

			Frame frame{} ;
			frame.type		= type ;
			frame.stream_id = stream_id ;
			frame.sequence	= ReadUint32( available, 16U) ;
			frame.payload.assign( available.begin() + static_cast<std::ptrdiff_t>( kFrameHeaderBytes),
								 available.begin() + static_cast<std::ptrdiff_t>( frame_size)) ;
			decoded.push_back( std::move( frame)) ;
			consumed += frame_size ;
		}

		// 完全frameだけを一度に除去し、未完成末尾を次回へ残す
		if( consumed > 0U) {
			buffer_.erase( buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>( consumed)) ;
		}
	} catch( const std::bad_alloc &) {
		return FailDecode( buffer_, decoded, failed_, FrameError::ResourceExhausted) ;
	} catch( ...) {
		// COM／thread callback側へC++例外を漏らさずBridge errorへ変換する
		return FailDecode( buffer_, decoded, failed_, FrameError::ResourceExhausted) ;
	}

	return FrameError::None ;
}

void FrameDecoder::Reset() noexcept
{
	buffer_.clear() ;
	failed_ = false ;
}

bool FrameDecoder::failed() const noexcept
{
	return failed_ ;
}

} // namespace zrdp::protocol
