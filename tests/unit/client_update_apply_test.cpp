//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file client_update_apply_test.cpp
 * @brief 差替えの事前検証、部分失敗、rollback、生存新版への非介入を確認する。
 * @version 01.00.00
 * @date 2026/10/07
 * @author Shunji Ogawa
 *
 * @par 更新履歴
 * <pre>
 * VerNo.         author        update      comment
 * Ver.01.00.00   Shunji Ogawa  2026/10/07  更新transactionの失敗注入testを追加。
 * </pre>
 */
//------------------------------------------------------------------------------
#include "client/update/update_apply.h"

#include <array>

namespace {

/** @brief 更新transaction境界を短く参照するtest用namespace。 */
namespace update = zrdp::client::update ;

/** @brief fileやprocessを操作せずtransactionの順序だけを記録する。 */
class FakeApply final : public update::IClientUpdateApplyOperations {
public:
	/**
	 * @brief 事前検証結果を返し、呼出順を記録する。
	 * @return 注入した固定結果。
	 */
	[[nodiscard]] update::UpdateError ValidateBeforeReplacement() noexcept override
	{
		Record( 1U) ;
		return before ;
	}

	/**
	 * @brief 部分失敗を含む差替え状態を仮想的に変更する。
	 * @return 注入した差替え結果。
	 */
	[[nodiscard]] update::UpdateError Replace() noexcept override
	{
		Record( 2U) ;
		original = !partial_replacement && replaced != update::UpdateError::None ;
		return replaced ;
	}

	/**
	 * @brief 差替え後の検証結果を返す。
	 * @return 注入した検証結果。
	 */
	[[nodiscard]] update::UpdateError ValidateInstalledCandidate() noexcept override
	{
		Record( 3U) ;
		return installed ;
	}

	/**
	 * @brief 新版の初期化と生存状態を返す。
	 * @return 注入した再起動結果。
	 */
	[[nodiscard]] update::UpdateRestartResult RestartAndConfirm() noexcept override
	{
		Record( 4U) ;
		return restarted ;
	}

	/**
	 * @brief 旧版が不変か確認する。
	 * @return 仮想配置が旧版ならtrue。
	 */
	[[nodiscard]] bool OriginalIsInstalled() noexcept override
	{
		Record( 5U) ;
		return original ;
	}

	/**
	 * @brief 成功時だけ旧版状態を復元する。
	 * @return 注入した復元結果。
	 */
	[[nodiscard]] update::UpdateError RestoreOriginal() noexcept override
	{
		Record( 6U) ;
		// 復元が成功した場合だけ原配置を旧版へ戻す
		if( restored == update::UpdateError::None) {
			original = restore_writes_original ;
		}

		return restored ;
	}

	/**
	 * @brief 呼出し順を有限arrayへ記録する。
	 * @param[in] value 検証段階の固定ID。
	 */
	void Record( const unsigned int value) noexcept
	{
		// test自身もbounded recorderをoverflowさせない
		if( count < calls.size()) {
			calls[count++] = value ;
		}
	}

	/// 署名・hash等の事前検証結果。
	update::UpdateError			  before{update::UpdateError::None} ;
	/// Windows差替え相当の結果。
	update::UpdateError			  replaced{update::UpdateError::None} ;
	/// 差替え後の検証結果。
	update::UpdateError			  installed{update::UpdateError::None} ;
	/// 旧版backupの復元結果。
	update::UpdateError			  restored{update::UpdateError::None} ;
	/// ready／停止済み／生存中の起動結果。
	update::UpdateRestartResult	  restarted{update::UpdateRestartState::Ready,
											update::UpdateError::None} ;
	/// 原配置の仮想旧版状態。
	bool						  original{true} ;
	/// 失敗した差替えが原配置を変化させる場合はtrue。
	bool						  partial_replacement{false} ;
	/// 復元APIが成功を返しても旧版を残さない注入状態。
	bool						  restore_writes_original{true} ;
	/// 操作IDのbounded記録。
	std::array<unsigned int, 12U> calls{} ;
	/// 完了した記録数。
	std::size_t					  count{0U} ;
} ;

} // namespace

/**
 * @brief 正常更新と全rollback判断をin-memoryで確認する。
 * @return 成功は0、失敗段階は非0 code。
 */
int main()
{
	FakeApply good ;
	// 事前検証→差替え→再検証→新版readyの順序だけを正常扱いにする
	if( update::ApplyClientUpdate( good) != update::UpdateError::None || good.count != 4U ||
	   good.calls[0] != 1U || good.calls[1] != 2U || good.calls[2] != 3U || good.calls[3] != 4U) {
		return 1 ;
	}

	FakeApply invalid ;
	invalid.before = update::UpdateError::InvalidSignature ;
	// 事前検証失敗ではfileを一度も変更しない
	if( update::ApplyClientUpdate( invalid) != update::UpdateError::InvalidSignature ||
	   invalid.count != 1U) {
		return 2 ;
	}

	FakeApply no_change ;
	no_change.replaced = update::UpdateError::ReplacementFailure ;
	// 失敗しても旧版が不変なら不要な復元を書込まない
	if( update::ApplyClientUpdate( no_change) != update::UpdateError::ReplacementFailure ||
	   !no_change.original || no_change.count != 3U || no_change.calls[2] != 5U) {
		return 3 ;
	}

	FakeApply partial ;
	partial.replaced			= update::UpdateError::ReplacementFailure ;
	partial.partial_replacement = true ;
	// Windowsの部分置換失敗はbackup復元と再検証を必須にする
	if( update::ApplyClientUpdate( partial) != update::UpdateError::ReplacementFailure ||
	   !partial.original || partial.count != 5U || partial.calls[3] != 6U ||
	   partial.calls[4] != 5U) {
		return 4 ;
	}

	FakeApply wrong_image ;
	wrong_image.installed = update::UpdateError::HashMismatch ;
	// 差替え後のhash不一致では新版を一度も実行しない
	if( update::ApplyClientUpdate( wrong_image) != update::UpdateError::HashMismatch ||
	   !wrong_image.original || wrong_image.count != 6U || wrong_image.calls[3] != 5U) {
		return 5 ;
	}

	FakeApply start_failed ;
	start_failed.restarted = {} ;
	// 起動失敗／停止済み新版だけは旧版へ戻せる
	if( update::ApplyClientUpdate( start_failed) != update::UpdateError::RestartFailure ||
	   !start_failed.original) {
		return 6 ;
	}

	FakeApply alive ;
	alive.restarted = {update::UpdateRestartState::Running, update::UpdateError::RestartFailure} ;
	// 生存新版への強制停止や復元を行わず、backupを保持する
	if( update::ApplyClientUpdate( alive) != update::UpdateError::RestartFailure || alive.original ||
	   alive.count != 4U) {
		return 7 ;
	}

	FakeApply failed_restore ;
	failed_restore.installed = update::UpdateError::MetadataMismatch ;
	failed_restore.restored	 = update::UpdateError::RollbackFailure ;
	// 復元失敗を元のerrorだけで隠さない
	if( update::ApplyClientUpdate( failed_restore) != update::UpdateError::RollbackFailure) {
		return 8 ;
	}

	FakeApply lying_restore ;
	lying_restore.installed				  = update::UpdateError::MetadataMismatch ;
	lying_restore.restore_writes_original = false ;
	// APIが成功を返しても旧版再検証に失敗した場合は復旧完了にしない
	if( update::ApplyClientUpdate( lying_restore) != update::UpdateError::RollbackFailure) {
		return 9 ;
	}

	return 0 ;
}
