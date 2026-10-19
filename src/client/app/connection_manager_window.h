//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file connection_manager_window.h
 * @brief 複数RDP sessionを開始する接続管理Windowを定義する。
 * @version 01.00.04
 * @date 2026/08/01
 * @author Shunji Ogawa
 *
 * @details RDP ActiveX、Virtual Channel、Proxyを所有せず、接続入力と
 *          DPAPI保護履歴だけをCOM STA UI thread上で管理する。
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  指定C/C++規約へコメントと書式を統一。
 * Ver.01.00.01   Shunji Ogawa  2026/10/07  Clientの同意付き更新と設定の分離を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  接続別設定と自動Agent管理・診断削除を反映。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  接続変更と登録済み宛先だけの表示・起動を統一。
 * Ver.01.00.04   Shunji Ogawa  2026/10/11  コメント監査の不足と実装不一致を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "client/app/connection_history.h"
#include "client/app/session_launch_settings.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zrdp::client::app {

/**
 * @brief 接続入力、履歴候補、次のProxy portを表示するトップレベルWindow。
 */
class ConnectionManagerWindow final {
public:
	/**
	 * @brief 未生成の接続管理Window controllerを構築する。
	 * @param[in] instance resourceとwindow classを所有するmodule instance。
	 * @param[in] observer session Windowのapplication owner。
	 */
	ConnectionManagerWindow( HINSTANCE instance, IConnectionManagerObserver &observer) ;

	/**
	 * @brief 履歴workerを停止し、生存中WindowをUI thread上で破棄する。
	 */
	~ConnectionManagerWindow() ;

	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 */
	ConnectionManagerWindow( const ConnectionManagerWindow &)			= delete ;
	/**
	 * @brief 所有資源とlifecycleの複製を禁止する。
	 * @return 呼出し禁止のため戻り値は生成しない。
	 */
	ConnectionManagerWindow &operator=( const ConnectionManagerWindow &) = delete ;

	/**
	 * @brief 接続管理Windowと全子controlを生成して表示する。
	 * @param[in] show_command WinMain由来の初期表示command。
	 * @return 必須UIを公開できた場合はtrue。
	 */
	[[nodiscard]] bool		 Create( int show_command) ;

	/**
	 * @brief Tab移動等のdialog互換messageを処理する。
	 * @param[in] message application message loopが取得した一件。
	 * @return このWindow階層で処理した場合はtrue。
	 */
	[[nodiscard]] bool		 ProcessDialogMessage( MSG &message) const noexcept ;

	/**
	 * @brief 現在のトップレベルHWNDを返す。
	 * @return 生成前または破棄後はnullptr。
	 */
	[[nodiscard]] HWND		 window() const noexcept ;
	/**
	 * @brief 固定resourceの更新状態をsession／Proxy状態と並べて表示する。
	 * @param[in] status 更新内容やURLを含まない短い状態表示。
	 */
	void					 UpdateClientUpdateStatus( std::wstring status) ;

	/**
	 * @brief session数、次のport、設定異常を専用状態欄へ反映する。
	 * @param[in] active_sessions 生存中session Window数。
	 * @param[in] next_port 次回割当可能port。枯渇時はnullopt。
	 * @param[in] configuration_error `client.ini`の固定検証結果。
	 */
	void UpdateRuntimeStatus( std::size_t active_sessions, std::optional<std::uint16_t> next_port,
							 proxy::ClientProxyConfigurationError configuration_error) ;

	/**
	 * @brief 認証成功したsessionの接続profileを共通履歴workerへ登録する。
	 * @param[in] connection_name 任意の接続名。
	 * @param[in] server RDP接続先。
	 * @param[in] domain 任意domain。
	 * @param[in] user_name passwordを含まないユーザー名。
	 * @param[in] audio 接続時に適用した音声リダイレクト設定。
	 */
	void RememberSuccessfulConnection( std::wstring connection_name, std::wstring server,
									  std::wstring domain, std::wstring user_name,
									  rdp::RdpAudioRedirectionSettings audio) noexcept ;

private:
	/**
	 * @brief Win32 messageを対応するinstanceへdispatchする。
	 * @param[in] window messageを受信したHWND。
	 * @param[in] message Win32 message ID。
	 * @param[in] wparam message固有のWPARAM。
	 * @param[in] lparam message固有のLPARAM。
	 * @return instance処理またはDefWindowProcWの結果。
	 */
	static LRESULT CALLBACK WindowProcedure( HWND window, UINT message, WPARAM wparam,
											LPARAM lparam) noexcept ;

	/**
	 * @brief instanceへrouting済みmessageを処理する。
	 * @param[in] message Win32 message ID。
	 * @param[in] wparam message固有のWPARAM。
	 * @param[in] lparam message固有のLPARAM。
	 * @return 処理済みmessageの結果またはDefWindowProcWの結果。
	 */
	[[nodiscard]] LRESULT	HandleMessage( UINT message, WPARAM wparam, LPARAM lparam) ;

	/**
	 * @brief process内で共有するmanager window classを登録する。
	 * @return 登録済みまたは登録成功時はtrue。
	 */
	[[nodiscard]] bool		RegisterWindowClass() const noexcept ;

	/**
	 * @brief 接続入力と状態表示に必要な子controlを生成する。
	 * @return 全必須controlを生成できた場合はtrue。
	 */
	[[nodiscard]] bool		CreateControls() ;

	/**
	 * @brief 現在client領域とDPIへ子controlを配置する。
	 * @param[in] client_width manager client領域の幅。
	 * @param[in] client_height manager client領域の高さ。
	 */
	void					LayoutControls( int client_width, int client_height) ;

	/**
	 * @brief 現在入力を検証し、新しいsession Window生成を要求する。
	 */
	void					OnConnect() ;
	/** @brief 現在入力のAgent設定を開くか、生存する設定画面だけを追従させる。
	 * @param[in] open trueは明示表示、falseは入力変更による非表示の更新通知。
	 */
	void					OnAgentSettings( bool open = true) ;

	/**
	 * @brief background履歴workerを開始して初回Loadを要求する。
	 */
	void					LoadConnectionHistory() ;

	/**
	 * @brief workerの所有結果をUI候補へ反映する。
	 */
	void					OnConnectionHistoryReady() ;

	/**
	 * @brief 接続候補を再構築し、profile音声設定を維持する。
	 */
	void					PopulateConnectionHistoryCombos() ;

	/**
	 * @brief 選択した接続名に対応する一組のRDP識別情報を復元する。
	 */
	void					OnConnectionNameSelected() ;

	/**
	 * @brief 選択接続先に対応する最新接続名、domain、userを補完する。
	 */
	void					OnServerHistorySelected() ;

	/**
	 * @brief manager controlから接続時音声設定を検証して取得する。
	 * @return 再生先が選択済みなら設定値、異常状態ならnullopt。
	 */
	[[nodiscard]] std::optional<rdp::RdpAudioRedirectionSettings>
					  ReadAudioSettingsSelection() const noexcept ;

	/**
	 * @brief profileの音声設定をmanager controlへ復元する。
	 * @param[in] audio 検証済みまたは安全な既定値へ縮退する設定。
	 */
	void			  SetAudioSettingsSelection( rdp::RdpAudioRedirectionSettings audio) noexcept ;

	/**
	 * @brief system GUI fontを全子controlへ適用する。
	 */
	void			  ApplyControlFont() const noexcept ;

	/**
	 * @brief 指定値を96 DPI基準から現在Window DPIへ変換する。
	 * @param[in] value 96 DPI基準pixel値。
	 * @return 現在DPIへscaleしたpixel値。
	 */
	[[nodiscard]] int Scale( int value) const noexcept ;

	/**
	 * @brief UTF-16 string resourceを所有文字列として読み込む。
	 * @param[in] string_id 読込み対象resource ID。
	 * @return 読込み結果。存在しない場合は空文字列。
	 */
	[[nodiscard]] std::wstring			LoadStringResource( UINT string_id) const ;

	/**
	 * @brief control textを有限の所有文字列へcopyする。
	 * @param[in] control 読込み対象child control。
	 * @return 現在text。読込み不能時は空文字列。
	 */
	[[nodiscard]] std::wstring			ReadWindowText( HWND control) const ;

	// -------------------------------------------------------------------------
	// manager Windowと非所有Win32 control
	// -------------------------------------------------------------------------

	/// resourceとwindow classを所有するmodule instanceへの非所有参照。
	HINSTANCE							instance_ ;

	/// application lifetime中に生存するsession生成通知先。
	IConnectionManagerObserver		   &observer_ ;

	/// 接続入力を所有するtop-level Window。
	HWND								window_{nullptr} ;

	// 以下のchild HWNDはtop-level Windowが所有し、本classは参照だけを保持する
	/// 任意接続名label。
	HWND								connection_name_label_{nullptr} ;
	/// 接続名入力と履歴候補。
	HWND								connection_name_combo_{nullptr} ;
	/// RDP接続先label。
	HWND								server_label_{nullptr} ;
	/// 接続先入力と履歴候補。
	HWND								server_combo_{nullptr} ;
	/// 任意domain label。
	HWND								domain_label_{nullptr} ;
	/// domain入力と履歴候補。
	HWND								domain_combo_{nullptr} ;
	/// user name label。
	HWND								user_name_label_{nullptr} ;
	/// user name入力と履歴候補。
	HWND								user_name_combo_{nullptr} ;
	/// 新しいsessionを開始するbutton。
	HWND								connect_button_{nullptr} ;
	/// 現在入力した接続のAgent設定を編集するbutton。
	HWND								agent_settings_button_{nullptr} ;
	/// 初期表示mode群のlabel。
	HWND								display_mode_label_{nullptr} ;
	/// 等倍表示選択。
	HWND								native_mode_radio_{nullptr} ;
	/// Client側縮小表示選択。
	HWND								smart_sizing_mode_radio_{nullptr} ;
	/// remote寸法追従選択。
	HWND								dynamic_resolution_mode_radio_{nullptr} ;
	/// local resource設定群のlabel。
	HWND								local_resources_label_{nullptr} ;
	/// microphone redirect選択。
	HWND								redirect_microphone_check_{nullptr} ;
	/// remote audio再生先label。
	HWND								remote_audio_label_{nullptr} ;
	/// 三種類のaudio再生先選択。
	HWND								audio_playback_combo_{nullptr} ;
	/// session数と次のProxy port表示。
	HWND								status_text_{nullptr} ;

	// -------------------------------------------------------------------------
	// 接続profileとUI選択状態
	// -------------------------------------------------------------------------

	/// DPAPI file I/Oをmanager STA外で直列実行するworker。
	ConnectionHistoryWorker				connection_history_worker_ ;
	/// session／Proxy状態欄の基準文字列。更新状態を変更しても保持する。
	std::wstring						runtime_status_ ;
	/// applicationから受ける非機密の更新状態。
	std::wstring						client_update_status_ ;

	/// combo再構築とprofile復元に使用する最新履歴snapshot。
	std::vector<ConnectionHistoryEntry> connection_history_entries_ ;

	/// 次のsession開始時に適用する排他的表示mode。
	rdp::RdpDisplayMode					display_mode_{rdp::RdpDisplayMode::NativeScroll} ;
} ;

} // namespace zrdp::client::app
