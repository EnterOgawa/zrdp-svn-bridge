//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file update_apply.h
 * @brief 更新helperの検証、差替え、再起動、rollbackの順序を定義する。
 * @version 01.00.01
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  更新適用のfail-closed transactionを追加。
 * Ver.01.00.01   Shunji Ogawa  2026/10/10  transactionを固定二EXEセットと部分復元へ適用。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once

#include "client/update/update_manifest.h"

namespace zrdp::client::update {

/** @brief 新版の生存確認に応じたrollback可否。 */
enum class UpdateRestartState {
	/// 新版が接続管理Windowの初期化完了を通知した。
	Ready,
	/// 新版を開始できなかったか、開始後に終了済みである。
	Stopped,
	/// 新版は生存しているが、有限待機内に初期化完了を通知しなかった。
	Running,
} ;

/** @brief 再起動の固定結果。processやpathを公開しない。 */
struct UpdateRestartResult {
	/// 生存している新版へ介入しないための確認状態。
	UpdateRestartState state{UpdateRestartState::Stopped} ;
	/// 起動または初期化の固定失敗分類。
	UpdateError		   error{UpdateError::RestartFailure} ;
} ;

/** @brief 固定配置と検証済み候補だけを扱うhelper側file境界。 */
class IClientUpdateApplyOperations {
public:
	/** @brief transaction資源を解放する。 */
	virtual ~IClientUpdateApplyOperations()											= default ;
	/**
	 * @brief 親終了後の旧版二EXE・両候補・署名・hashを再検証する。
	 * @return 完全に不変で有効な場合だけNone。
	 */
	[[nodiscard]] virtual UpdateError		  ValidateBeforeReplacement() noexcept	= 0 ;
	/**
	 * @brief 二候補へ順次差替え、旧版二fileをbackupへ残す。
	 * @return 成功はNone。失敗時も部分置換の可能性をcallerが扱う。
	 */
	[[nodiscard]] virtual UpdateError		  Replace() noexcept					= 0 ;
	/**
	 * @brief 差替え後の二実行fileを同意セットの完全値と照合する。
	 * @return 完全一致はNone。
	 */
	[[nodiscard]] virtual UpdateError		  ValidateInstalledCandidate() noexcept = 0 ;
	/**
	 * @brief 検証済み新版を開始し、初期化結果を有限時間だけ待つ。
	 * @return 新版の生存状態と固定結果。
	 * @note 生存processを強制終了しない。
	 */
	[[nodiscard]] virtual UpdateRestartResult RestartAndConfirm() noexcept			= 0 ;
	/**
	 * @brief 元の二EXEが同じ配置で完全に維持されているか確認する。
	 * @return 旧版の署名・hash等が一致した場合だけtrue。
	 */
	[[nodiscard]] virtual bool				  OriginalIsInstalled() noexcept		= 0 ;
	/**
	 * @brief 変更されたfileだけを有効な旧版backupから固定配置へ復元する。
	 * @return 復元できた場合だけNone。
	 */
	[[nodiscard]] virtual UpdateError		  RestoreOriginal() noexcept			= 0 ;
} ;

/**
 * @brief 親終了確認済みhelperで差替えとrollbackを直列実行する。
 * @param[in,out] operations 固定配置のWindows実装または隔離test実装。
 * @return 完全に起動確認できた場合はNone。復元失敗はRollbackFailure。
 * @note 生存中の新版をrollbackや強制終了の対象にしない。
 */
[[nodiscard]] UpdateError ApplyClientUpdate( IClientUpdateApplyOperations &operations) noexcept ;

} // namespace zrdp::client::update
