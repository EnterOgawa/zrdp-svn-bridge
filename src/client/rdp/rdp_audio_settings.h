/**
 * @file rdp_audio_settings.h
 * @brief RDP接続時に確定する音声リダイレクト設定modelを定義する。
 *
 * @details ActiveX固有の値をUI、接続履歴、session境界へ散在させず、
 *          passwordや音声dataを含まない有限の設定値だけを共有する。
 */
#pragma once

#include <cstdint>

namespace zrdp::client::rdp {

/**
 * @brief 接続先で再生される音声の出力先を表す。
 *
 * 値は`IMsRdpClientAdvancedSettings5::AudioRedirectionMode`と一致させ、
 * ActiveX境界で追加変換を必要としない。
 */
enum class RdpAudioPlaybackMode : std::uint32_t {
    PlayOnClient = 0U,          ///< 接続先の音声をZRdp実行PCで再生する。
    PlayOnRemoteComputer = 1U,  ///< 接続先PC側で音声を再生する。
    DoNotPlay = 2U,             ///< 両側で接続先の音声を再生しない。
};

/**
 * @brief 接続開始前にMicrosoft RDP ActiveXへ適用する音声設定。
 */
struct RdpAudioRedirectionSettings {
    /// ZRdp実行PCの既定マイクを接続先sessionへ転送する場合はtrue。
    bool redirect_microphone{false};

    /// 接続先で発生した音声の再生先。
    RdpAudioPlaybackMode playback_mode{
        RdpAudioPlaybackMode::PlayOnClient};
};

/**
 * @brief 永続化またはUIから得た音声再生先が既知値か検証する。
 * @param[in] mode 検証する音声再生先。
 * @return ActiveXへ安全に渡せる3値のいずれかならtrue。
 */
[[nodiscard]] constexpr bool IsValidAudioPlaybackMode(
    const RdpAudioPlaybackMode mode) noexcept {
    return mode == RdpAudioPlaybackMode::PlayOnClient ||
        mode == RdpAudioPlaybackMode::PlayOnRemoteComputer ||
        mode == RdpAudioPlaybackMode::DoNotPlay;
}

}  // namespace zrdp::client::rdp
