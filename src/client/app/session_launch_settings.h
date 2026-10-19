//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file session_launch_settings.h
 * @brief 接続管理Windowから独立RDP session Windowへ渡す設定境界を定義する。
 * @version 01.00.08
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details passwordや認証tokenを含めず、COM STA上で所有文字列として受け渡す。
 *          Proxy portはClientApplicationが有限slotから割り当てる。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Clientの同意付き更新と設定の分離を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/08  Agent設定画面の操作要求を分離。
 * Ver.01.00.03   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/09  session画面のAgent配置・確認付き起動を追加。
 * Ver.01.00.05   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  接続別設定と自動Agent管理・診断削除を反映。
 * Ver.01.00.07   Shunji Ogawa  2026/10/10  managerメニューを撤廃し自動更新確認へ統一。
 * Ver.01.00.08   Shunji Ogawa  2026/10/10  接続変更と登録済み宛先だけの表示・起動を統一。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "client/proxy/client_proxy_config.h"
#include "client/rdp/rdp_control_host.h"
#include "common/agent_setup.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace zrdp::client::app {

/**
 * @brief 利用者が接続管理Windowで確定した一件のRDP接続要求。
 */
struct SessionConnectionRequest {
	/// 利用者が任意指定する接続プロファイル名。ActiveXへは渡さない。
	std::wstring					 connection_name ;

	/// RDP接続先PC名またはIPアドレス。
	std::wstring					 server ;

	/// 任意のRDPドメイン。
	std::wstring					 domain ;

	/// passwordを含まないRDPユーザー名。
	std::wstring					 user_name ;

	/// session生成時に適用する排他的表示mode。
	rdp::RdpDisplayMode				 display_mode{rdp::RdpDisplayMode::NativeScroll} ;

	/// sessionのConnect前に一度だけ適用する音声リダイレクト設定。
	rdp::RdpAudioRedirectionSettings audio ;
} ;

/**
 * @brief 一つの独立session Windowを生成する検証済みstartup設定。
 */
struct SessionLaunchSettings {
	/// managerで検証済みのRDP接続要求。
	SessionConnectionRequest			 connection ;

	/// 1..65535のsession専用loopback Proxy port。設定異常時だけ0。
	std::uint16_t						 proxy_port{0U} ;

	/// port slotの解放に使用する0起点の匿名番号。
	std::size_t							 proxy_slot{0U} ;

	/// `client.ini`異常時にRDPを維持しProxyだけ停止する固定分類。
	proxy::ClientProxyConfigurationError proxy_configuration_error{
		proxy::ClientProxyConfigurationError::None} ;
	/// 配置入口の対象固定に使う、再利用しないprocess内session ID。
	std::uint64_t										  session_id{0U} ;
	/// このsessionだけに転送する配布用drive。生成失敗時はnullptr。
	std::shared_ptr<common::agent_setup::DeploymentDrive> deployment_drive ;
} ;

/**
 * @brief 新規session生成を拒否した固定理由。
 */
enum class SessionOpenError : std::uint32_t {
	/// 異常は検出されておらず、要求の処理を継続できる。
	None				 = 0U,
	/// 同時RDP sessionの製品上限へ到達している。
	SessionLimitReached	 = 1U,
	/// session専用Proxy portのslotを割り当てられない。
	ProxyPortUnavailable = 2U,
	/// 独立session Windowを生成できなかった。
	WindowCreationFailed = 3U,
	/// memory等の有限資源を確保できず、部分状態を公開しない。
	ResourceExhausted	 = 4U,
	/// 同意済み更新の適用中であり、新規sessionを開始しない。
	UpdateInProgress	 = 5U,
	/// 配置用clipboardの準備・公開中は追加RDPへの共有を避ける。
	DeploymentInProgress = 6U,
} ;

/**
 * @brief 接続管理Windowへ返す非機密のsession生成結果。
 */
struct SessionOpenResult {
	/// 接続管理Windowへ返すpayloadなしの生成結果。
	SessionOpenError   error{SessionOpenError::None} ;

	/// 成功時のsession専用port。Proxy設定異常時は0。
	std::uint16_t	   proxy_port{0U} ;

	/**
	 * @brief RDP session Windowを生成できたか返す。
	 * @return Window所有権をapplicationへ移せた場合はtrue。
	 */
	[[nodiscard]] bool succeeded() const noexcept
	{
		return error == SessionOpenError::None ;
	}
} ;

/**
 * @brief 接続管理Windowからapplication ownerへsession生成を要求する境界。
 */
class IConnectionManagerObserver {
public:
	/**
	 * @brief 派生instanceの破棄をvirtual境界で完了する。
	 */
	virtual ~IConnectionManagerObserver() = default ;

	/**
	 * @brief 一件のRDP／Bridge session Windowを生成する。
	 * @param[in] request passwordを含まない検証済み接続設定。
	 * @return Window生成とProxy port割当の固定結果。
	 */
	[[nodiscard]] virtual SessionOpenResult
				 OpenSession( SessionConnectionRequest request) noexcept	 = 0 ;
	/**
	 * @brief Agentの完全一致HTTP接続先だけを編集する画面を開く。
	 * @param[in] preferred_name 現在の接続名。空なら既定の設定名。
	 */
	virtual void ShowAgentSettings( std::wstring preferred_name) noexcept = 0 ;
	/** @brief managerで確定した接続組のAgent設定だけを編集する。
	 * @param[in] request passwordを含まない接続入力。
	 */
	virtual void ShowConnectionAgentSettings( SessionConnectionRequest request) noexcept
	{
		ShowAgentSettings( std::move( request.connection_name)) ;
	}

	/** @brief 現在の接続入力へ開いているAgent設定画面だけを追従させる。
	 * @param[in] request passwordなしの入力。Window生成やRDP開始は要求しない。
	 * @note 追従しないobserverでは既定で無操作とする。
	 */
	virtual void UpdateOpenAgentSettings( SessionConnectionRequest request) noexcept
	{
		static_cast<void>( request) ;
	}
} ;

/**
 * @brief session Windowからapplication ownerへ成功接続を通知する境界。
 */
class ISessionWindowObserver {
public:
	/**
	 * @brief 派生instanceの破棄をvirtual境界で完了する。
	 */
	virtual ~ISessionWindowObserver()													 = default ;
	/** @brief session操作の対象を一件queueでmanagerへ渡す。
	 * @param[in] session_id 再利用しない対象ID。
	 * @param[in] epoch 認証済み接続世代。
	 * @return manager通知を受付けた場合だけtrue。
	 */
	[[nodiscard]] virtual bool RequestSessionAgentSettings( std::uint64_t session_id,
														   std::uint64_t epoch) noexcept = 0 ;

	/**
	 * @brief Microsoft認証完了後の非機密接続組を履歴ownerへ渡す。
	 * @param[in] connection_name 任意の接続プロファイル名。
	 * @param[in] server RDP接続先。
	 * @param[in] domain 任意domain。
	 * @param[in] user_name passwordを含まないユーザー名。
	 * @param[in] audio 接続時に適用した音声リダイレクト設定。
	 */
	virtual void RememberSuccessfulConnection( std::wstring connection_name, std::wstring server,
											  std::wstring domain, std::wstring user_name,
											  rdp::RdpAudioRedirectionSettings audio) noexcept = 0 ;
} ;

} // namespace zrdp::client::app
