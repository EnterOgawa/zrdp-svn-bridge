/**
 * @file session_disconnect_policy.h
 * @brief RDP session Windowの終了開始と切断後の維持方針を判定する。
 */
#pragma once

namespace zrdp::client::app {

/**
 * @brief session Window終了要求を開始する時点の処理を表す。
 */
enum class SessionCloseStartAction {
    DestroyImmediately,  ///< 有効なRDP接続がないため直ちにWindowを破棄する。
    WaitForDisconnect,   ///< 進行中の切断完了eventを待ってからWindowを破棄する。
    RequestDisconnect,   ///< ActiveXへ明示切断を要求し、完了eventを待つ。
};

/**
 * @brief 現在のRDP状態からsession Window終了開始処理を選択する。
 * @param[in] connection_active RDP接続または接続遷移が有効な場合はtrue。
 * @param[in] disconnecting ActiveXが既に切断処理中の場合はtrue。
 * @return 二重切断と接続中ActiveXの即時破棄を避ける終了開始処理。
 *
 * 接続中のActiveXを直接破棄すると、Microsoft Control内部のOLE終了が長時間
 * 待機する場合がある。通常の切断buttonと同じDisconnect／OnDisconnected経路へ
 * 統一し、既に切断中なら同じ完了eventだけを待つ。
 */
[[nodiscard]] constexpr SessionCloseStartAction
SelectSessionCloseStartAction(
    const bool connection_active,
    const bool disconnecting) noexcept {
    // 有効な接続がなければ、非同期eventを待たずWindowを破棄できる。
    if (!connection_active) {
        return SessionCloseStartAction::DestroyImmediately;
    }

    // 進行中のDisconnectへ二重要求せず、最初のOnDisconnectedへ収束させる。
    if (disconnecting) {
        return SessionCloseStartAction::WaitForDisconnect;
    }

    return SessionCloseStartAction::RequestDisconnect;
}

/**
 * @brief 成立済みsessionの外部切断後にWindowを無通知で閉じるか判定する。
 * @param[in] login_completed 現在の接続試行でログイン完了済みならtrue。
 * @param[in] locally_requested_disconnect ZRdp自身が切断を要求した場合はtrue。
 * @param[in] credential_prompt_cancelled Microsoft認証画面の取消ならtrue。
 * @return 接続先再起動等の成立済みsession外部切断ならtrue。
 *
 * 初回接続失敗は原因確認のためWindowとerror表示を維持する。一方、成立済み
 * sessionが外部要因で最終切断された場合は、利用不能なsession Windowを残さない。
 */
[[nodiscard]] constexpr bool
ShouldCloseWindowAfterExternalDisconnect(
    const bool login_completed,
    const bool locally_requested_disconnect,
    const bool credential_prompt_cancelled) noexcept {
    return login_completed &&
        !locally_requested_disconnect &&
        !credential_prompt_cancelled;
}

}  // namespace zrdp::client::app
