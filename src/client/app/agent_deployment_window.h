//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_deployment_window.h
 * @brief manager STA上の配置確認とOLE公開の所有者を定義する。
 * @version 01.00.06
 * @date 2026/10/08
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/08  手動folder確認付き配置画面を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/09  初回配置と固定Agent起動の自動化を追加。
 * Ver.01.00.02   Shunji Ogawa  2026/10/09  初期配置の停止段階とHRESULT診断を追加。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  Agent起動の統合と明示未署名配置テストを追加。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  起動と設定の分離、tray常駐、backup付き更新を追加。
 * Ver.01.00.05   Shunji Ogawa  2026/10/10  トレイ終了後の同じRDP内Agent再起動を修正。
 * Ver.01.00.06   Shunji Ogawa  2026/10/10  Agent配置・起動の署名要件と未署名別経路を撤廃。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once
#include "client/agent_deployment/agent_deployment.h"
#include "common/agent_setup.h"
#include "client/agent_deployment/automatic_deployment_status.h"
#include "client/agent_settings/agent_settings_model.h"
#include <functional>
#include <optional>

namespace zrdp::client::app {

/** @brief testで実clipboardを変更しないための非所有操作境界。 */
struct DeploymentClipboardOps {
	/// 一参照をcallerに残したままclipboardへ公開する。
	std::function<HRESULT( IDataObject *)> publish ;
	/// S_OKは自分、S_FALSEは他所有、失敗は判定不能として区別する。
	std::function<HRESULT( IDataObject *)> current ;
	/// 所有とclipboard世代を排他取得前後で再確認して自分のobjectだけを消す。
	std::function<HRESULT( IDataObject *)> clear ;
} ;
/** @brief 固定resourceの確認をfakeに差替えるtest境界。 */
using DeploymentConfirmation = std::function<bool( HWND, UINT, std::wstring_view)> ;
/** @brief 貼付けキーを所有session STAへ予約する非blocking callback。 */
using DeploymentPaste		 = std::function<bool( DWORD)> ;

/** @brief 設定編集とは独立した今回だけの起動選択。 */
struct AgentLaunchChoice {
	/// 起動を明示承認した保存済みprofile index。取消／設定表示では空。
	std::optional<std::size_t> profile_index ;
	/// 編集画面を利用者が要求した場合だけtrue。
	bool					   open_settings{false} ;
} ;

/** @brief 保存読込みと確認後に同じ認証済み一sessionか判定する。
 * @param[in] expected_id 明示起動した対象ID。
 * @param[in] expected_epoch 明示起動時の認証世代。
 * @param[in] live_id 現在生存する対象ID。
 * @param[in] live_epoch 現在の認証世代。
 * @param[in] session_count 現在の生存session数。
 * @return 非0の同一ID・世代かつ一sessionの場合だけtrue。
 */
[[nodiscard]] inline bool CanUseSavedAgentLaunch( std::uint64_t expected_id,
												 std::uint64_t expected_epoch,
												 std::uint64_t live_id, std::uint64_t live_epoch,
												 std::size_t session_count) noexcept
{
	return expected_id != 0U && expected_epoch != 0U && expected_id == live_id &&
		   expected_epoch == live_epoch && session_count == 1U ;
}

/** @brief 保存済み一件を起動へ使用できるか確認する。
 * @param[in] profile 保存済み設定。用途と利用承認を推定しない。
 * @return 空でなく全件が検証・承認済みの場合だけtrue。
 */
[[nodiscard]] bool
IsAgentLaunchProfileUsable( const agent_settings::AgentSettingsProfile &profile) noexcept ;

/** @brief 二ファイル公開中もRDPを操作できるmodeless配置controller。 */
class AgentDeploymentWindow final {
public:
	/** @brief OLEと固定resourceをmanager STAに所有する。
	 * @param[in] instance resource所有module。
	 * @param[in] clipboard testだけのclipboard境界。空ならWindows OLE。
	 * @param[in] confirm testだけの確認境界。空なら既定「いいえ」のdialog。
	 * @param[in] clock testだけの単調clock。空ならGetTickCount64。
	 */
	explicit AgentDeploymentWindow( HINSTANCE instance, DeploymentClipboardOps clipboard = {},
								   DeploymentConfirmation	  confirm = {},
								   std::function<ULONGLONG()> clock	  = {}) ;
	/** @brief 自分のclipboardだけを消し、COM STA終了前にOLEを解放する。 */
	~AgentDeploymentWindow() ;
	/** @brief controllerの所有状態を複製しない。 */
	AgentDeploymentWindow( const AgentDeploymentWindow &)			= delete ;
	/** @brief controllerの所有状態を複製しない。 @return 呼出し禁止。 */
	AgentDeploymentWindow &operator=( const AgentDeploymentWindow &) = delete ;
	/** @brief 保存済み設定と今回の警告だけを独立画面で確認する。
	 * @param[in] target 固定したsessionのruntime表示名。
	 * @param[in] profiles 全承認済み保存設定だけの有限一覧。
	 * @param[in] initialized 自作画面だけを操作するtest境界。製品では空。
	 * @return 起動承認／設定表示／取消の選択。起動承認時だけ準備状態へ入る。
	 */
	[[nodiscard]] AgentLaunchChoice
	ConfirmLaunch( std::wstring target, const agent_settings::AgentSettingsProfiles &profiles,
				  std::function<void( HWND)> initialized = {}) ;
	/** @brief 共有clipboardへの公開を事前確認する。
	 * @param[in] target 固定したsessionのruntime表示名。
	 * @param[in] automatic 初期配置と起動の確認を使用する場合true。
	 * @return 明示承認して準備状態へ入った場合だけtrue。
	 */
	[[nodiscard]] bool ConfirmPreparation( std::wstring target, bool automatic = false) ;
	/** @brief 準備失敗ではclipboardを変更せず準備状態を解除する。 */
	void			   CancelPreparation() noexcept ;
	/** @brief 二ファイルだけを公開して配置確認画面を生成する。
	 * @param[in] package 完全なworker結果。
	 * @param[in] paste 固定session STAへの非blocking要求。
	 * @return clipboardと画面を両方公開できた場合だけtrue。
	 */
	[[nodiscard]] bool Show( std::shared_ptr<const agent_deployment::Package> package,
							DeploymentPaste									 paste) ;
	/** @brief readonly公開済みdriveの固定Agentを一回だけ起動する。
	 * @param[in] drive 接続前に固定した同じsessionの配布drive。
	 * @param[in] launch 所有STAへ予約する一件の固定入力。
	 * @param[in] status 所有STAのatomic状態だけを確認する非blocking callback。
	 * @return 画面と起動要求を受付けた場合だけtrue。
	 */
	[[nodiscard]] bool
					   ShowAutomatic( std::shared_ptr<common::agent_setup::DeploymentDrive>			drive,
									 DeploymentPaste												launch,
									 std::function<agent_deployment::AutomaticDeploymentSnapshot()> status) ;
	/** @brief 標準dialog navigationを処理する。
	 * @param[in,out] message managerの一message。
	 * @return 所有画面が処理した場合だけtrue。
	 */
	[[nodiscard]] bool ProcessDialogMessage( MSG &message) const noexcept ;
	/** @brief 準備または公開中か返す。 @return 追加接続を禁止する場合はtrue。 */
	[[nodiscard]] bool active() const noexcept ;
	/** @brief 次の明示起動のため応答確認済み成功画面だけを閉じる。
	 * @return 成功画面を閉じた場合だけtrue。進行中と失敗画面は保全する。
	 */
	[[nodiscard]] bool CloseCompleted() noexcept ;
	/** @brief 所有dialog HWNDを返す。 @return 画面不在ではnullptr。 */
	[[nodiscard]] HWND window() const noexcept ;
	/** @brief 所有sessionからのキー送信結果を反映する。
	 * @param[in] result Microsoftのキー送信結果。copy完了ではない。
	 */
	void			   OnPasteResult( HRESULT result) noexcept ;
	/** @brief 対象切断・再接続で以降の貼付けを停止する。 */
	void			   Invalidate() noexcept ;

private:
	/** @brief dialog boundaryから例外を漏らさずrouteする。
	 * @param[in] window 所有dialog。
	 * @param[in] message Win32 message。
	 * @param[in] wparam command固有値。
	 * @param[in] lparam 初期化時の非所有controller。
	 * @return 処理済みならTRUE。
	 */
	static INT_PTR CALLBACK	   DialogProcedure( HWND window, UINT message, WPARAM wparam,
											   LPARAM lparam) noexcept ;
	/** @brief resourceを有限copyする。 @param[in] id 固定resource ID。 @return 所有文字列。 */
	[[nodiscard]] std::wstring Text( UINT id) const ;
	/** @brief 既定「いいえ」で確認する。 @param[in] id 固定resource ID。 @return 明示承認ならtrue。 */
	[[nodiscard]] bool		   Confirm( UINT id) const ;
	/** @brief 自分のclipboardのみを停止し、dialogを通常破棄する。 */
	void					   Finish() noexcept ;
	/** @brief 自動方式の最初の停止だけを画面と固定logへ反映する。
	 * @param[in] failure owner結果またはUIが分類した数値結果。
	 */
	void ShowAutomaticFailure( agent_deployment::AutomaticDeploymentSnapshot failure) noexcept ;
	/** @brief 一回の単調clock値を読む。 @return test値またはWindows tick。 */
	[[nodiscard]] ULONGLONG										   AutomaticTick() const ;
	/// resourceの非所有module。
	HINSTANCE													   instance_ ;
	/// manager STAの所有dialog。
	HWND														   window_{nullptr} ;
	/// current/publish/clearのplatform境界。
	DeploymentClipboardOps										   clipboard_ ;
	/// test用の確認callback。
	DeploymentConfirmation										   confirm_ ;
	/// owner STAへ固定キーを予約するcallback。
	DeploymentPaste												   paste_ ;
	/// caller所有の一参照。clipboard自身にも別参照がある。
	IDataObject													  *object_{nullptr} ;
	/// runtimeのみの接続先表示。logへ出さない。
	std::wstring												   target_ ;
	/// OleInitializeの成功に対応する解放責任。
	bool														   ole_initialized_{false} ;
	/// worker準備の承認済み状態。
	bool														   preparing_{false} ;
	/// owner STAの結果を待つ唯一の要求。
	bool														   pending_{false} ;
	/// キー要求が少なくとも一回成功した状態。copy成功ではない。
	bool														   sent_{false} ;
	/// 今回のAgent応答を実際に観測済みの成功画面だけを区別する。
	bool														   response_confirmed_{false} ;
	/// 切断・clipboard変更で再貼付けを禁止した状態。
	bool														   invalidated_{false} ;
	/// 手動clipboard配置とは別の、自動初期配置の確認状態。
	bool														   automatic_{false} ;
	/// 起動要求が実際にowner STAへ受付けられた時刻。
	ULONGLONG													   automatic_started_{0U} ;
	/// 公開済みdriveの非UI所有状態。終了時に未送信入力を失効する。
	std::shared_ptr<common::agent_setup::DeploymentDrive>		   drive_ ;
	/// 非blockingな入力・応答状態の取得。
	std::function<agent_deployment::AutomaticDeploymentSnapshot()> automatic_status_ ;
	/// timeoutを待たずに失敗注入するtest境界。実runtimeはWindows clock。
	std::function<ULONGLONG()>									   clock_ ;
	/// 後続取消で失敗詳細を上書きしない最初の結果。
	agent_deployment::AutomaticDeploymentSnapshot				   automatic_failure_ ;
} ;
} // namespace zrdp::client::app
