//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file agent_start_window.h
 * @brief 接続先Explorerの選択を利用者確認して起動キーを要求する。
 * @version 01.00.01
 * @date 2026/10/09
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/09  session所有STAの確認付き起動画面を追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once
#include <Windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include "client/rdp/rdp_fixed_input_sequence.h"

namespace zrdp::client::app {
/** @brief Agentなしの接続先へEnterだけを送るmodeless確認画面。
 * @note 全methodとcallbackは同じsession所有STAで実行する。
 */
class AgentStartWindow final {
public:
	/** @brief resource moduleを非所有参照する。
	 * @param[in] instance resource module。
	 * @param[in] clock test用単調ms境界。不在ではGetTickCount64を使用する。
	 */
	explicit AgentStartWindow( HINSTANCE instance, std::function<ULONGLONG()> clock = {}) ;
	/** @brief 所有dialogを同じSTA上で解放する。 */
	~AgentStartWindow() ;
	/** @brief dialog所有権を複製しない。 */
	AgentStartWindow( const AgentStartWindow &)			   = delete ;
	/** @brief dialog所有権を複製しない。 @return 呼出し禁止。 */
	AgentStartWindow  &operator=( const AgentStartWindow &) = delete ;
	/** @brief 対象世代を固定して確認画面を開く。
	 * @param[in] target runtimeだけに表示する接続先。
	 * @param[in] epoch 確認開始時の認証世代。0は拒否。
	 * @param[in] send 世代と認証を再確認する固定Enter境界。
	 * @param[in] prepare 確認後の初回focus準備。不在は非I/O fixture用のS_OK。
	 * @return 確認画面を表示できた場合だけtrue。
	 */
	[[nodiscard]] bool Show( std::wstring target, std::uint64_t epoch, std::function<HRESULT()> send,
							std::function<HRESULT()> prepare = {}) ;
	/** @brief 他Window切替時に未送信Enterを失効し、戻っても再開しない。 */
	void			   CancelPendingInput() noexcept ;
	/** @brief 同じ確認済みEnterが未送信または要求中かを返す。 @return 存在時true。 */
	[[nodiscard]] bool pending_input() const noexcept ;
	/** @brief 接続失効と実Agent応答を反映する。
	 * @param[in] epoch 現在の認証世代。未認証では0。
	 * @param[in] ready HELLO_ACKで確認したBridge ready状態。
	 */
	void			   Update( std::uint64_t epoch, bool ready) noexcept ;
	/** @brief modeless dialog navigationを処理する。
	 * @param[in,out] message 所有STAの一message。
	 * @return この画面が処理した場合はtrue。
	 */
	[[nodiscard]] bool ProcessDialogMessage( MSG &message) const noexcept ;
	/** @brief 確認画面のHWNDを返す。 @return 不在ならnullptr。 */
	[[nodiscard]] HWND window() const noexcept ;

private:
	/** @brief Win32境界から例外を漏らさず確認操作をrouteする。
	 * @param[in] window 所有dialog。
	 * @param[in] message Win32 message。
	 * @param[in] wparam command値。
	 * @param[in] lparam 初期化時の非所有controller。
	 * @return 処理済みならTRUE。
	 */
	static INT_PTR CALLBACK	   DialogProcedure( HWND window, UINT message, WPARAM wparam,
											   LPARAM lparam) noexcept ;
	/** @brief 固定状態文字列を表示する。 @param[in] id resource ID。 */
	void					   Status( UINT id) const noexcept ;
	/** @brief 遅延Enterだけを一回要求する。例外は停止表示へ変換する。 */
	void					   AdvanceInput() noexcept ;
	/** @brief 単調msを取得する。 @return nativeまたはfakeのtick。 */
	[[nodiscard]] ULONGLONG	   Tick() const ;
	/// resourceの非所有module。
	HINSTANCE				   instance_ ;
	/// 同じsession STAが所有するdialog。
	HWND					   window_{nullptr} ;
	/// runtimeだけの対象表示。
	std::wstring			   target_ ;
	/// 画面表示時に固定する認証世代。
	std::uint64_t			   epoch_{0U} ;
	/// 所有STAから固定キーだけを送る境界。
	std::function<HRESULT()>   send_ ;
	/// 確認後の初回focus準備。入力列は受け付けない。
	std::function<HRESULT()>   prepare_ ;
	/// 有限待機・再入拒否と取消を所有する固定Enter sequence。
	rdp::FixedInputSequence	   sequence_{} ;
	/// 仮想時刻を使用できる、非所有外部I/Oなしのclock。
	std::function<ULONGLONG()> clock_ ;
	/// 二重要求と失効後の再試行を拒否する状態。
	bool					   stopped_{false} ;
	/// 切断・再認証後に旧画面の確認を復活させない状態。
	bool					   invalidated_{false} ;
	/// 実Agent応答を現在まで確認できている状態。
	bool					   ready_{false} ;
} ;
} // namespace zrdp::client::app
