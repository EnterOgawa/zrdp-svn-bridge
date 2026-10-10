//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file hello.h
 * @brief ESVNBRG HELLO capability payloadとPhase別local上限を定義する。
 * @version 01.00.01
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details ClientとAgentが互いの有限上限を交換し、Echoと多重化機能を
 *          feature bit単位で交渉する。stream機能を共有しないpeerとは
 *          maximum streamsとinitial windowを0へ固定する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  限定Agent controlと交渉featureを追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "protocol/frame.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace zrdp::protocol {

// Phase 1のbinary PING／PONG検証に対応するfeature bit
inline constexpr std::uint32_t kFeatureEcho = 0x00000001U ;

// Phase 2の複数streamとcredit-based flow controlに対応するfeature bit
inline constexpr std::uint32_t kFeatureMultiplexedStreams = 0x00000002U ;

// 限定INI同期・協調停止・再起動に対応するoptional feature bit
inline constexpr std::uint32_t kFeatureAgentLifecycle = 0x00000004U ;

// HELLO capability payloadの固定byte数
inline constexpr std::size_t   kHelloPayloadBytes = 20U ;

// stream数の交渉で受け付ける製品絶対上限
inline constexpr std::uint32_t kMaximumNegotiableStreams = 1024U ;

// flow-control windowで受け付ける製品絶対上限
inline constexpr std::uint32_t kMaximumNegotiableWindowBytes = 64U * 1024U * 1024U ;

// Phase 2でlocal processが提示する同時stream数
inline constexpr std::uint32_t kDefaultMultiplexedStreams = 32U ;

// Phase 2で各受信方向へ提示する初期credit
inline constexpr std::uint32_t kDefaultInitialWindowBytes = 256U * 1024U ;

/**
 * @brief HELLO／HELLO_ACKで交換する有限capability。
 */
struct HelloCapabilities {
	/// 対応機能のbit集合。
	std::uint32_t feature_bits{kFeatureEcho} ;

	/// peerから受信可能な一frame payload上限。
	std::uint32_t maximum_frame_payload{kMaximumFramePayloadBytes} ;

	/// 同時利用できるstream上限。stream featureなしでは0。
	std::uint32_t maximum_streams{0U} ;

	/// 一stream当たりの初期受信window。stream featureなしでは0。
	std::uint32_t initial_window_bytes{0U} ;

	/// 一回のStatic Virtual Channel write chunk上限。
	std::uint32_t transport_chunk_bytes{
		static_cast<std::uint32_t>( kStaticVirtualChannelChunkBytes)} ;
} ;

/**
 * @brief Phase 2 stream多重化に対応するlocal capabilityを作成する。
 * @return Echoと多重化を有効化した有限な既定値。
 */
[[nodiscard]] constexpr HelloCapabilities MakePhase2HelloCapabilities() noexcept
{
	HelloCapabilities capabilities{} ;
	capabilities.feature_bits		  = kFeatureEcho | kFeatureMultiplexedStreams ;
	capabilities.maximum_streams	  = kDefaultMultiplexedStreams ;
	capabilities.initial_window_bytes = kDefaultInitialWindowBytes ;
	return capabilities ;
}

/**
 * @brief HELLO capabilityを20-byte fixed payloadへencodeする。
 * @param[in] capabilities 検証対象のlocal capability。
 * @param[out] payload 成功時の所有binary payload。
 * @return 成功時は`FrameError::None`、不正値または資源不足時は分類結果。
 */
[[nodiscard]] FrameError EncodeHelloCapabilities( const HelloCapabilities &capabilities,
												 std::vector<std::byte>	 &payload) noexcept ;

/**
 * @brief 20-byte HELLO payloadを検証して所有値へdecodeする。
 * @param[in] payload peerから受け取ったbinary payload。
 * @param[out] capabilities 成功時の検証済みcapability。
 * @return 成功時は`FrameError::None`、長さまたは値が不正なら分類結果。
 */
[[nodiscard]] FrameError DecodeHelloCapabilities( std::span<const std::byte> payload,
												 HelloCapabilities		   &capabilities) noexcept ;

/**
 * @brief localとpeerの共通かつ小さい上限をBridge sessionへ採用する。
 * @param[in] local local processのcapability。
 * @param[in] peer peer processから検証済みのcapability。
 * @param[out] negotiated 成功時の共通capability。
 * @return Echoを含む安全な共通範囲があれば`FrameError::None`。
 */
[[nodiscard]] FrameError NegotiateHelloCapabilities( const HelloCapabilities &local,
													const HelloCapabilities &peer,
													HelloCapabilities		&negotiated) noexcept ;

} // namespace zrdp::protocol
