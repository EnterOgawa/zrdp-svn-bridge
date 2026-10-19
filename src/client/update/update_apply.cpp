//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file update_apply.cpp
 * @brief 差替えの部分失敗を扱い、生存中processへ介入しない適用順序を実装する。
 * @version 01.00.00
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  検証後適用と失敗時復元の有限順序を追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/update/update_apply.h"

namespace zrdp::client::update {
namespace {

/**
 * @brief 差替えの部分失敗または新版の停止後だけ旧版を復元する。
 * @param[in,out] operations 固定配置のfile境界。
 * @param[in] failure 元の失敗分類。
 * @return 旧版が正常なら元のerror、復元できない場合はRollbackFailure。
 */
[[nodiscard]] UpdateError RollBack( IClientUpdateApplyOperations &operations,
								   const UpdateError			 failure) noexcept
{
	// ReplaceFileの失敗後も旧版が不変なら、不要な書込みを行わない
	if( operations.OriginalIsInstalled()) {
		return failure ;
	}

	// backupの完全検証と復元後の再検証が揃わなければ復旧完了と報告しない
	if( operations.RestoreOriginal() != UpdateError::None || !operations.OriginalIsInstalled()) {
		return UpdateError::RollbackFailure ;
	}

	return failure ;
}

} // namespace

UpdateError ApplyClientUpdate( IClientUpdateApplyOperations &operations) noexcept
{
	const UpdateError verified = operations.ValidateBeforeReplacement() ;
	// 旧版、候補、署名、hashの不変を確認するまで配置へ書込まない
	if( verified != UpdateError::None) {
		return verified ;
	}

	const UpdateError replaced = operations.Replace() ;
	// Windowsの部分置換失敗を「旧版が必ず残る」と仮定せず復元を確認する
	if( replaced != UpdateError::None) {
		return RollBack( operations, replaced) ;
	}

	const UpdateError installed = operations.ValidateInstalledCandidate() ;
	// 未確認のfileを一度も実行せず、停止状態で旧版へ戻す
	if( installed != UpdateError::None) {
		return RollBack( operations, installed) ;
	}

	const UpdateRestartResult restarted = operations.RestartAndConfirm() ;
	// 初期化済み新版だけを正常更新として扱う
	if( restarted.state == UpdateRestartState::Ready && restarted.error == UpdateError::None) {
		return UpdateError::None ;
	}

	// 生存中の新版にRDP sessionがあり得るため、強制停止やfile復元を行わない
	if( restarted.state != UpdateRestartState::Stopped) {
		return UpdateError::RestartFailure ;
	}

	return RollBack( operations, UpdateError::RestartFailure) ;
}

} // namespace zrdp::client::update
