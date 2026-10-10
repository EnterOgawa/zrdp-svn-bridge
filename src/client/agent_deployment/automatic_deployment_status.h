//------------------------------------------------------------------------------
/**
 * Copyright (c) 2026 EnterSystem CO.,LTD.
 * SPDX-License-Identifier: Apache-2.0
 * @file automatic_deployment_status.h
 * @brief 初回配置の停止分類と機密を含まないatomic snapshotを定義する。
 * @version 01.00.04
 * @date 2026/10/09
 * @author Shunji Ogawa
 * @par 更新履歴
 * <pre>
 * Ver.01.00.00   Shunji Ogawa  2026/10/09  停止段階、理由、HRESULTを一括公開する。
 * Ver.01.00.01   Shunji Ogawa  2026/10/09  自動入力のfocusとWindowsキー転送を確認。
 * Ver.01.00.02   Shunji Ogawa  2026/10/10  Agent固定入力をSendInputへ切替え停止境界を維持。
 * Ver.01.00.03   Shunji Ogawa  2026/10/10  トレイ終了後の同じRDP内Agent再起動を修正。
 * Ver.01.00.04   Shunji Ogawa  2026/10/10  完了入力sequenceの限定初期化と二回目起動を修正。
 * </pre>
 */
//------------------------------------------------------------------------------
#pragma once
#include <Windows.h>
#include <cstdint>
#include "client/rdp/rdp_fixed_input_sequence.h"

namespace zrdp::client::agent_deployment {
/** @brief 初回配置の固定処理段階。値は診断記録に使用する。 */
enum class AutomaticDeploymentStage : std::uint8_t {
	/// 入力要求前。
	None			 = 0,
	/// 所有STAへの要求受付。
	Request			 = 1,
	/// 接続、専用drive、Bridgeの条件確認。
	Validation		 = 2,
	/// 固定起動文字列のclipboard公開。
	Clipboard		 = 3,
	/// Win+R送信。
	Run				 = 4,
	/// 固定入力timerの開始。
	Timer			 = 5,
	/// Ctrl+V直前の確認と送信。
	Paste			 = 6,
	/// Enter直前の確認と送信。
	Enter			 = 7,
	/// Agent応答待ち。
	AgentResponse	 = 8,
	/// Windowsキー転送設定と対象Controlの入力focus準備。
	InputPreparation = 9,
} ;
/** @brief 停止原因の固定分類。文字列やclipboard内容は保持しない。 */
enum class AutomaticDeploymentReason : std::uint8_t {
	/// 失敗なし。
	None					= 0,
	/// 認証世代の失効。
	SessionChanged			= 1,
	/// 一回の入力処理が既に開始済み。
	AlreadyStarted			= 2,
	/// 専用drive未準備または取消済み。
	DriveUnavailable		= 3,
	/// BridgeがAgent待機状態でない。
	BridgeUnavailable		= 4,
	/// clipboard世代不一致または取得不能。
	ClipboardChanged		= 5,
	/// Win32処理が失敗した。
	WindowsFailure			= 6,
	/// Microsoftキー送信APIが失敗した。
	InputFailure			= 7,
	/// 所有STAへ要求を予約できない。
	QueueFailure			= 8,
	/// timer生成に失敗した。
	TimerFailure			= 9,
	/// 45秒でAgent応答を確認できない。
	AgentTimeout			= 10,
	/// 例外を安全に停止へ変換した。
	UnexpectedException		= 11,
	/// owner状態を取得するcallbackが不在。
	StatusUnavailable		= 12,
	/// 入力完了前に操作期限へ到達した。
	InputTimeout			= 13,
	/// キー転送設定または対象Controlへの入力focusを確認できない。
	InputPreparationFailure = 14,
	/// 固定列の一部だけを受付。追加解放・再送は行わない。
	PartialInput			= 15,
} ;
/** @brief UIとowner STA間で整合した一件の固定数値結果。 */
struct AutomaticDeploymentSnapshot {
	/// 0は進行／待機、1は応答確認、2は既存Agent、-1は停止。
	int						  status{0} ;
	/// 最後に実行または拒否した段階。
	AutomaticDeploymentStage  stage{AutomaticDeploymentStage::None} ;
	/// 停止時の固定分類。
	AutomaticDeploymentReason reason{AutomaticDeploymentReason::None} ;
	/// 失敗したAPIのHRESULTまたは固定分類に対応するHRESULT。
	HRESULT					  result{S_OK} ;
} ;
/** @brief 送信直前にownerが観測した安全条件だけのsnapshot。 */
struct AutomaticDeploymentGuard {
	/// 同じ非0認証世代である。
	bool session_matches ;
	/// 初回または予定された入力段階である。
	bool phase_allowed ;
	/// 同じ専用driveがsealedである。
	bool drive_ready ;
	/// BridgeがAgent待機状態である。
	bool bridge_waiting ;
	/// 必要な段階でclipboard世代が非0かつ一致する。
	bool clipboard_matches ;
} ;
/** @brief 既存の安全条件を固定の停止理由へ分類する。
 * @param[in] guard 内容を含まない観測値。
 * @return 最初の失敗分類。全条件成立ならNone。
 */
[[nodiscard]] constexpr AutomaticDeploymentReason
AutomaticDeploymentGuardReason( const AutomaticDeploymentGuard &guard) noexcept
{
	// 旧接続への入力を最優先で拒否する
	if( !guard.session_matches) {
		return AutomaticDeploymentReason::SessionChanged ;
	}

	// 二重要求または終了済み段階での追加入力を拒否する
	if( !guard.phase_allowed) {
		return AutomaticDeploymentReason::AlreadyStarted ;
	}

	// 取消済みまたは未準備packageを起動しない
	if( !guard.drive_ready) {
		return AutomaticDeploymentReason::DriveUnavailable ;
	}

	// Bridgeの状態変化後に初期配置を続けない
	if( !guard.bridge_waiting) {
		return AutomaticDeploymentReason::BridgeUnavailable ;
	}

	// 別clipboard内容を固定入力と誤認しない
	if( !guard.clipboard_matches) {
		return AutomaticDeploymentReason::ClipboardChanged ;
	}

	return AutomaticDeploymentReason::None ;
}

/** @brief 応答確認済みAgentの終了後だけ次の明示起動を許可する。
 * @param[in] phase 前回固定入力の段階。
 * @param[in] response_before 前回Bridgeが応答確認済みである。
 * @param[in] waiting_after 現在BridgeがAgent待機へ戻った。
 * @param[in] same_epoch 同じ非0認証世代である。
 * @param[in] succeeded 前回入力が失敗していない。
 * @return 完了段階からの安全な終了遷移だけtrue。自動再送は行わない。
 */
[[nodiscard]] constexpr bool CanResetCompletedAgentLaunch( int phase, bool response_before,
														  bool waiting_after, bool same_epoch,
														  bool succeeded) noexcept
{
	return phase == 4 && response_before && waiting_after && same_epoch && succeeded ;
}

/** @brief 同認証世代のAgent終了と入力完了の両方を確認して次の明示操作を準備する。
 * @param[in,out] sequence owner STAだけが操作する今回の固定入力状態。
 * @param[in] phase 前回の起動段階。
 * @param[in] response_before 前回Bridgeが応答確認済みである。
 * @param[in] waiting_after 現在BridgeがAgent待機へ戻った。
 * @param[in] same_epoch 同じ非0認証世代である。
 * @param[in] succeeded 前回入力が失敗していない。
 * @return guard成立かつsequenceがCompleteの場合だけ初期化してtrue。
 * @note 入力予約は行わず、停止したsequenceを復活させない。
 */
[[nodiscard]] inline bool ResetCompletedAgentLaunch( rdp::FixedInputSequence &sequence, int phase,
													bool response_before, bool waiting_after,
													bool same_epoch, bool succeeded) noexcept
{
	return CanResetCompletedAgentLaunch( phase, response_before, waiting_after, same_epoch,
										succeeded) &&
		   sequence.ResetCompleted() ;
}

/** @brief 数値結果を一つの64-bit atomic格納値へencodeする。
 * @param[in] value 内部契約の状態と分類。
 * @return HRESULTの全32-bitを保持した固定値。
 */
[[nodiscard]] constexpr std::uint64_t
EncodeAutomaticDeploymentSnapshot( const AutomaticDeploymentSnapshot &value) noexcept
{
	return static_cast<std::uint64_t>( value.status + 1) |
		   (static_cast<std::uint64_t>( value.stage) << 8U) |
		   (static_cast<std::uint64_t>( value.reason) << 16U) |
		   (static_cast<std::uint64_t>( static_cast<std::uint32_t>( value.result)) << 32U) ;
}

/** @brief 同じ一回のatomic loadから数値結果を復元する。
 * @param[in] value ownerがencodeした格納値。
 * @return 互いに異なる段階のfieldを混在させない結果。
 */
[[nodiscard]] constexpr AutomaticDeploymentSnapshot
DecodeAutomaticDeploymentSnapshot( std::uint64_t value) noexcept
{
	return {static_cast<int>( value & 0xFFU) - 1,
			static_cast<AutomaticDeploymentStage>( (value >> 8U) & 0xFFU),
			static_cast<AutomaticDeploymentReason>( (value >> 16U) & 0xFFU),
			static_cast<HRESULT>( static_cast<std::uint32_t>( value >> 32U))} ;
}

/** @brief Agent応答待ちの有限期限を判定する。
 * @param[in] started 操作開始時の単調tick。
 * @param[in] now 現在の単調tick。
 * @return 45秒以上経過した場合true。clock逆転は期限扱いしない。
 */
[[nodiscard]] constexpr bool AutomaticDeploymentTimedOut( ULONGLONG started, ULONGLONG now) noexcept
{
	return now >= started && now - started >= 45000U ;
}
} // namespace zrdp::client::agent_deployment
