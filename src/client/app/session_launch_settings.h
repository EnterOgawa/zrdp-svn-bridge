/**
 * @file session_launch_settings.h
 * @brief 接続管理Windowから独立RDP session Windowへ渡す設定境界を定義する。
 *
 * @details passwordや認証tokenを含めず、COM STA上で所有文字列として受け渡す。
 *          Proxy portはClientApplicationが有限slotから割り当てる。
 */
#pragma once

#include "client/proxy/client_proxy_config.h"
#include "client/rdp/rdp_control_host.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace zrdp::client::app {

/**
 * @brief 利用者が接続管理Windowで確定した一件のRDP接続要求。
 */
struct SessionConnectionRequest {
    /// 利用者が任意指定する接続プロファイル名。ActiveXへは渡さない。
    std::wstring connection_name;

    /// RDP接続先PC名またはIPアドレス。
    std::wstring server;

    /// 任意のRDPドメイン。
    std::wstring domain;

    /// passwordを含まないRDPユーザー名。
    std::wstring user_name;

    /// session生成時に適用する排他的表示mode。
    rdp::RdpDisplayMode display_mode{
        rdp::RdpDisplayMode::NativeScroll};

    /// sessionのConnect前に一度だけ適用する音声リダイレクト設定。
    rdp::RdpAudioRedirectionSettings audio;
};

/**
 * @brief 一つの独立session Windowを生成する検証済みstartup設定。
 */
struct SessionLaunchSettings {
    /// managerで検証済みのRDP接続要求。
    SessionConnectionRequest connection;

    /// 1..65535のsession専用loopback Proxy port。設定異常時だけ0。
    std::uint16_t proxy_port{0U};

    /// port slotの解放に使用する0起点の匿名番号。
    std::size_t proxy_slot{0U};

    /// `client.ini`異常時にRDPを維持しProxyだけ停止する固定分類。
    proxy::ClientProxyConfigurationError proxy_configuration_error{
        proxy::ClientProxyConfigurationError::None};
};

/**
 * @brief 新規session生成を拒否した固定理由。
 */
enum class SessionOpenError : std::uint32_t {
    None = 0U,
    SessionLimitReached = 1U,
    ProxyPortUnavailable = 2U,
    WindowCreationFailed = 3U,
    ResourceExhausted = 4U,
};

/**
 * @brief 接続管理Windowへ返す非機密のsession生成結果。
 */
struct SessionOpenResult {
    /// 接続管理Windowへ返すpayloadなしの生成結果。
    SessionOpenError error{SessionOpenError::None};

    /// 成功時のsession専用port。Proxy設定異常時は0。
    std::uint16_t proxy_port{0U};

    /**
     * @brief RDP session Windowを生成できたか返す。
     * @return Window所有権をapplicationへ移せた場合はtrue。
     */
    [[nodiscard]] bool succeeded() const noexcept {
        return error == SessionOpenError::None;
    }
};

/**
 * @brief 接続管理Windowからapplication ownerへsession生成を要求する境界。
 */
class IConnectionManagerObserver {
public:
    virtual ~IConnectionManagerObserver() = default;

    /**
     * @brief 一件のRDP／Bridge session Windowを生成する。
     * @param[in] request passwordを含まない検証済み接続設定。
     * @return Window生成とProxy port割当の固定結果。
     */
    [[nodiscard]] virtual SessionOpenResult OpenSession(
        SessionConnectionRequest request) noexcept = 0;
};

/**
 * @brief session Windowからapplication ownerへ成功接続を通知する境界。
 */
class ISessionWindowObserver {
public:
    virtual ~ISessionWindowObserver() = default;

    /**
     * @brief Microsoft認証完了後の非機密接続組を履歴ownerへ渡す。
     * @param[in] connection_name 任意の接続プロファイル名。
     * @param[in] server RDP接続先。
     * @param[in] domain 任意domain。
     * @param[in] user_name passwordを含まないユーザー名。
     * @param[in] audio 接続時に適用した音声リダイレクト設定。
     */
    virtual void RememberSuccessfulConnection(
        std::wstring connection_name,
        std::wstring server,
        std::wstring domain,
        std::wstring user_name,
        rdp::RdpAudioRedirectionSettings audio) noexcept = 0;
};

}  // namespace zrdp::client::app
