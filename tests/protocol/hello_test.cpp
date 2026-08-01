/**
 * @file hello_test.cpp
 * @brief HELLO capabilityの固定layout、検証、交渉をin-memoryで確認する。
 */

#include "protocol/hello.h"

#include <array>
#include <cstddef>
#include <vector>

// -----------------------------------------------------------------------------
// テスト実行
// -----------------------------------------------------------------------------

/**
 * @brief HELLO capabilityのencode、decode、互換性交渉を検証する。
 * @return 全検証成功時は0、失敗時は失敗段階を表す非0 code。
 */
int wmain() {
    using zrdp::protocol::DecodeHelloCapabilities;
    using zrdp::protocol::EncodeHelloCapabilities;
    using zrdp::protocol::FrameError;
    using zrdp::protocol::HelloCapabilities;
    using zrdp::protocol::NegotiateHelloCapabilities;

    HelloCapabilities local{};
    local.maximum_frame_payload = 65536U;
    local.maximum_streams = 0U;
    local.initial_window_bytes = 0U;
    local.transport_chunk_bytes = 1600U;

    std::vector<std::byte> encoded;
    // 既定Phase 1 capabilityを20 byteへencodeできることを確認する。
    if (EncodeHelloCapabilities(local, encoded) !=
            FrameError::None ||
        encoded.size() != zrdp::protocol::kHelloPayloadBytes) {
        return 1;
    }

    constexpr std::array<std::byte, 20> expected{
        std::byte{0x00U}, std::byte{0x00U},
        std::byte{0x00U}, std::byte{0x01U},
        std::byte{0x00U}, std::byte{0x01U},
        std::byte{0x00U}, std::byte{0x00U},
        std::byte{0x00U}, std::byte{0x00U},
        std::byte{0x00U}, std::byte{0x00U},
        std::byte{0x00U}, std::byte{0x00U},
        std::byte{0x00U}, std::byte{0x00U},
        std::byte{0x00U}, std::byte{0x00U},
        std::byte{0x06U}, std::byte{0x40U},
    };
    // byte orderとfield位置を完全一致で固定する。
    if (!std::equal(
            expected.begin(),
            expected.end(),
            encoded.begin())) {
        return 2;
    }

    HelloCapabilities decoded{};
    if (DecodeHelloCapabilities(encoded, decoded) !=
            FrameError::None ||
        decoded.feature_bits != local.feature_bits ||
        decoded.maximum_frame_payload !=
            local.maximum_frame_payload ||
        decoded.maximum_streams != local.maximum_streams ||
        decoded.initial_window_bytes !=
            local.initial_window_bytes ||
        decoded.transport_chunk_bytes !=
            local.transport_chunk_bytes) {
        return 3;
    }

    HelloCapabilities peer = local;
    peer.maximum_frame_payload = 32768U;
    peer.transport_chunk_bytes = 800U;
    HelloCapabilities negotiated{};
    // 各有限上限は両端の小さい値へ交渉する。
    if (NegotiateHelloCapabilities(
            local,
            peer,
            negotiated) != FrameError::None ||
        negotiated.maximum_frame_payload != 32768U ||
        negotiated.transport_chunk_bytes != 800U ||
        negotiated.feature_bits !=
            zrdp::protocol::kFeatureEcho) {
        return 4;
    }

    std::vector<std::byte> short_payload(
        zrdp::protocol::kHelloPayloadBytes - 1U,
        std::byte{0U});
    // field欠落payloadを別versionとして近似解釈しない。
    if (DecodeHelloCapabilities(
            short_payload,
            decoded) != FrameError::InvalidHeaderSize) {
        return 5;
    }

    HelloCapabilities no_echo = local;
    no_echo.feature_bits = 0U;
    // Phase 1必須Echo機能なしのlocal値をwireへ出さない。
    if (EncodeHelloCapabilities(no_echo, encoded) ==
            FrameError::None) {
        return 6;
    }

    HelloCapabilities oversized = local;
    oversized.maximum_frame_payload =
        zrdp::protocol::kMaximumFramePayloadBytes + 1U;
    // peer宣言上限を製品絶対上限より大きくできない。
    if (EncodeHelloCapabilities(oversized, encoded) !=
            FrameError::OversizedPayload) {
        return 7;
    }

    HelloCapabilities invalid_chunk = local;
    invalid_chunk.transport_chunk_bytes =
        static_cast<std::uint32_t>(
            zrdp::protocol::kStaticVirtualChannelChunkBytes + 1U);
    // Microsoft SVC chunk上限を超える交渉値を拒否する。
    if (EncodeHelloCapabilities(invalid_chunk, encoded) !=
            FrameError::OversizedPayload) {
        return 8;
    }

    const HelloCapabilities phase2 =
        zrdp::protocol::MakePhase2HelloCapabilities();
    // Phase 2既定値はstream featureと有限なstream／window上限を一体で持つ。
    if (EncodeHelloCapabilities(phase2, encoded) !=
            FrameError::None ||
        DecodeHelloCapabilities(encoded, decoded) !=
            FrameError::None ||
        (decoded.feature_bits &
         zrdp::protocol::kFeatureMultiplexedStreams) == 0U ||
        decoded.maximum_streams !=
            zrdp::protocol::kDefaultMultiplexedStreams ||
        decoded.initial_window_bytes !=
            zrdp::protocol::kDefaultInitialWindowBytes) {
        return 9;
    }

    // Phase 1 peerとの共通機能はEchoだけとし、stream上限を0へdowngradeする。
    if (NegotiateHelloCapabilities(
            phase2,
            local,
            negotiated) != FrameError::None ||
        negotiated.feature_bits !=
            zrdp::protocol::kFeatureEcho ||
        negotiated.maximum_streams != 0U ||
        negotiated.initial_window_bytes != 0U) {
        return 10;
    }

    HelloCapabilities inconsistent = phase2;
    inconsistent.initial_window_bytes = 0U;
    // stream featureだけを宣言した0-window capabilityを拒否する。
    if (EncodeHelloCapabilities(
            inconsistent,
            encoded) != FrameError::OversizedPayload) {
        return 11;
    }

    inconsistent = local;
    inconsistent.maximum_streams = 1U;
    // featureなしの非0 stream上限を暗黙に有効化しない。
    if (EncodeHelloCapabilities(
            inconsistent,
            encoded) != FrameError::OversizedPayload) {
        return 12;
    }

    return 0;
}
