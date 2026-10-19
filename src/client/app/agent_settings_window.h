//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_settings_window.h
 * @brief 接続専用Agent設定を登録・削除して自動保存する画面を定義する。
 * @version 01.00.06
 * @date 2026/10/08
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  接続先設定と非同期INI入出力のUIを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/08  確認付きAgent配置の境界と操作を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  接続別設定と自動Agent管理・診断削除を反映。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  登録・削除だけの自動保存画面へ簡素化。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  接続変更と登録済み宛先だけの表示・起動を統一。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "client/agent_settings/agent_settings_service.h"

#include <Windows.h>

namespace zrdp::client::app {

/** @brief manager STA上で画面だけを所有し、I/Oを設定workerへ渡すcontroller。 */
class AgentSettingsWindow final {
public:
	/**
	 * @brief workerを所有せず、applicationより短い画面controllerを構築する。
	 * @param[in] instance resourceを所有するmodule。
	 * @param[in,out] service applicationが単独所有し、controllerより長く生存するworker。
	 */
	AgentSettingsWindow( HINSTANCE instance, agent_settings::AgentSettingsService &service) ;
	/** @brief 生存画面だけをUI threadで破棄する。workerのjoinは行わない。 */
	~AgentSettingsWindow() ;
	/** @brief UI所有資源の複製を禁止する。 */
	AgentSettingsWindow( const AgentSettingsWindow &)			= delete ;
	/**
	 * @brief UI所有資源の複製を禁止する。
	 * @return 呼出し禁止のため値を生成しない。
	 */
	AgentSettingsWindow &operator=( const AgentSettingsWindow &) = delete ;
	/**
	 * @brief 一つだけのmodeless画面を生成し、workerへLoadを予約する。
	 * @param[in] owner 接続管理Window。testではnullptr。
	 * @param[in] preferred_name 開く設定名。空ならresourceの既定名。
	 * @param[in] connection_key managerで選んだ接続組。空は接続未選択で編集不可。
	 * @return 必須controlを生成できた場合はtrue。
	 */
	[[nodiscard]] bool	 Show( HWND owner, std::wstring preferred_name,
							  std::wstring connection_key = {}) ;
	/**
	 * @brief 開いている画面を別接続へ再読込みし、保存中は最後の変更だけを保留する。
	 * @param[in] preferred_name 現在の接続表示名。
	 * @param[in] connection_key managerで選んだ接続組。空は未選択で編集不可。
	 * @note Windowの新規生成やfocusの変更は行わない。保存失敗では元入力を保持する。
	 */
	void				 ChangeConnection( std::wstring preferred_name, std::wstring connection_key) ;
	/**
	 * @brief 所有するdialog HWNDを返す。
	 * @return 生成前／破棄後はnullptr。
	 */
	[[nodiscard]] HWND	 window() const noexcept ;
	/** @brief 保存結果を反映する接続組だけを返す。 @return 生存画面へ固定したkey。 */
	[[nodiscard]] std::wstring_view connection_key() const noexcept
	{
		return connection_key_ ;
	}

	/**
	 * @brief Tab等の標準dialog messageを処理する。
	 * @param[in,out] message applicationのmessage。
	 * @return 画面が処理した場合はtrue。
	 */
	[[nodiscard]] bool ProcessDialogMessage( MSG &message) const noexcept ;
	/**
	 * @brief workerの所有結果を受付IDで照合して画面へ反映する。
	 * @param[in] notice applicationが一件だけdrainした結果。
	 */
	void			   OnNotice( agent_settings::AgentSettingsNotice notice) noexcept ;

private:
	/**
	 * @brief dialog messageをinstanceへrouteし、例外をWin32外へ漏らさない。
	 * @param[in] window 所有dialog。
	 * @param[in] message Win32 message ID。
	 * @param[in] wparam message固有値。
	 * @param[in] lparam 初期化時は非所有controller。
	 * @return 処理済みならTRUE、標準dialog処理へ渡す場合はFALSE。
	 */
	static INT_PTR CALLBACK DialogProcedure( HWND window, UINT message, WPARAM wparam,
											LPARAM lparam) noexcept ;
	/**
	 * @brief instanceへrouteしたWin32 messageを処理する。
	 * @param[in] message Win32 message ID。
	 * @param[in] wparam message固有値。
	 * @param[in] lparam message固有値。
	 * @return 処理済みならTRUE。
	 */
	[[nodiscard]] INT_PTR	HandleMessage( UINT message, WPARAM wparam, LPARAM lparam) ;
	/** @brief 有限一覧の列と入力上限を初期化する。 */
	void					InitializeControls() ;
	/** @brief 保存一覧のLoadを予約し、古い画面世代の処理と競合しない。 */
	void					RequestLoad() ;
	/**
	 * @brief 一件の登録・削除候補を非同期保存し、成功するまで旧一覧を保持する。
	 * @param[in] candidate 検証対象。接続keyと表示名はmanagerの固定値を使用する。
	 */
	void					SaveCandidate( agent_settings::AgentSettingsProfile candidate) ;
	/**
	 * @brief worker処理中の編集と競合操作を無効化する。
	 * @param[in] busy 画面の一件要求が処理中か。
	 */
	void					SetBusy( bool busy) ;
	/**
	 * @brief 選択接続の保存一覧を復元し、未関連付け設定は表示しない。
	 * @param[in] name 未保存接続で使う固定表示名。
	 */
	void					SelectProfile( std::wstring name) ;
	/**
	 * @brief 登録済み表示rowを元snapshotの承認済みentry indexへ検証して対応付ける。
	 * @param[in] row list viewの表示row。
	 * @return 範囲と登録状態が一致する場合の元index。不一致はnullopt。
	 */
	[[nodiscard]] std::optional<std::size_t> RegisteredEndpointIndex( int row) const noexcept ;
	/** @brief 登録済みentryだけを一覧へ表示し、元indexをitem dataへ保持する。 */
	void									 RenderEndpoints() ;
	/**
	 * @brief 選択した一entryを編集欄へ復元する。
	 * @param[in] index draft内の検証済みindex。
	 */
	void									 SelectEndpoint( std::size_t index) ;
	/** @brief 入力欄を空にし、portと用途を既定値へ戻す。 */
	void									 ClearEndpointFields() ;
	/**
	 * @brief 入力宛先の業務利用を明示承認し、登録・同一宛先更新を自動保存する。
	 */
	void									 RegisterEndpoint() ;
	/** @brief 選択entryだけを削除し、最後の削除も空設定として自動保存する。 */
	void									 RemoveEndpoint() ;
	/**
	 * @brief 固定resourceだけを非modalの状態欄へ表示する。
	 * @param[in] identifier 日本語resource ID。
	 */
	void									 ShowText( UINT identifier) const ;
	/**
	 * @brief 内容を含まない固定失敗分類だけを表示する。
	 * @param[in] result UI／workerの固定検証結果。
	 */
	void					   ShowError( agent_settings::AgentSettingsResult result) const noexcept ;
	/**
	 * @brief resource文字列を有限の所有copyへ読む。
	 * @param[in] identifier 文字列resource ID。
	 * @return 読めた文字列。欠落時は空。
	 */
	[[nodiscard]] std::wstring Text( UINT identifier) const ;
	/**
	 * @brief childの編集文字列を有限copyで読む。
	 * @param[in] identifier 編集control ID。
	 * @return 現在のUTF-16入力。上限超過時は空。
	 */
	[[nodiscard]] std::wstring Input( int identifier) const ;
	/// resourceを所有するmoduleの非所有参照。
	HINSTANCE				   instance_ ;
	/// applicationが所有するworkerの非所有参照。
	agent_settings::AgentSettingsService &service_ ;
	/// このcontrollerが所有するmodeless dialog。
	HWND								  window_{nullptr} ;
	/// 完全に読込み／保存済みのprofile一覧。
	agent_settings::AgentSettingsProfiles profiles_ ;
	/// 現在画面で編集する一件の所有draft。
	agent_settings::AgentSettingsProfile  draft_ ;
	/// Load完了時に復元する名前。logへ出さない。
	std::wstring						  preferred_name_ ;
	/** @brief 保存完了後に適用する最後の接続変更。 */
	struct PendingConnection {
		/// 切替後の表示名。
		std::wstring name ;
		/// 切替後の完全一致key。空は接続未選択。
		std::wstring key ;
	} ;
	/// 一件だけ保留する最新接続変更。保存対象のkeyを途中変更しない。
	std::optional<PendingConnection> pending_connection_ ;
	/// managerで選んだ接続組。空の場合は編集を禁止する。
	std::wstring					 connection_key_ ;
	/// 画面世代で待つ唯一の受付ID。
	std::uint64_t					 pending_{0U} ;
	/// 古い画面の結果をdrain後に初回Loadを再予約する状態。
	bool							 waiting_load_{false} ;
	/// 一覧再表示・入力復元中の選択通知を新操作と扱わない状態。
	bool							 restoring_{false} ;
	/// 保存結果が届くまで二重操作と画面終了を抑止する状態。
	bool							 saving_{false} ;
	/// 保存先読込みに失敗した場合の空一覧で上書き保存を禁止する状態。
	bool							 storage_loaded_{false} ;
} ;

} // namespace zrdp::client::app
