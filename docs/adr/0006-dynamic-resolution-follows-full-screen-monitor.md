# ADR 0006: 動的解像度の全画面monitor追従

- Status: Accepted
- Date: 2026-07-31
- Supersedes: ADR 0004の「全画面中は動的解像度更新を停止する」という判断

## Context

ADR 0004では、Microsoft Remote Desktop ActiveX Controlの全画面管理と競合しない
よう、全画面中の動的解像度timerを停止していた。この方式ではZRdpの全画面Window
自体はmonitor全体へ拡大する一方、接続先remote desktopは通常Window時の寸法に
残る。そのため、利用者が明示的に選択した「動的解像度（Window追従）」の意味と
全画面表示結果が一致しなかった。

等倍表示とSmart Sizingでは接続先application layoutを維持することが優先であり、
全画面切替を理由にremote desktop解像度を変えてはならない。動的解像度では反対に、
現在の表示対象へremote desktop自体を追従させることが選択済みの動作である。

## Decision

- 動的解像度だけはActiveX全画面中も
  `IMsRdpClient9::UpdateSessionDisplaySettings`の対象とする。
- 通常WindowではRDP child windowのclient領域をtarget寸法とする。
- ActiveX全画面では、セッションWindowが属するmonitor全体の寸法をtargetとする。
- `OnEnterFullScreen`と`OnLeaveFullScreen`の両方で、既存の250 ms debounceを
  使用して更新を予約する。
- button、`Ctrl+Alt+Break`、Microsoft connection barによる切替を同じActiveX
  event経路へ収束させる。
- timer実行時点の全画面状態からtargetを選ぶ。短時間に全画面とWindowを往復しても、
  古い遷移の寸法を適用しない。
- 既存の`E_UNEXPECTED`限定有限retry、恒久失敗時の等倍表示への縮退、
  `Reconnect`を使用しない方針を維持する。
- 等倍表示とSmart Sizingでは、全画面切替時も
  `UpdateSessionDisplaySettings`を呼ばない。

## Consequences

### Positive

- 動的解像度で全画面へ移行すると、remote desktop領域もmonitor全体へ追従する。
- Windowへ復帰すると、remote desktop領域も現在のRDP表示領域へ戻る。
- 等倍表示とSmart Sizingでは、接続先desktop解像度とapplication layoutを維持する。
- 全画面専用の再接続や独自Windowを追加せず、Microsoft ActiveXの標準経路を保てる。

### Negative

- 動的解像度では全画面移行と復帰のたびに接続先application layoutが変化し得る。
- RDP serverまたはMicrosoft Controlがsession display updateを拒否した場合、
  既存policyにより等倍表示へ縮退する。
- multi-monitorの全monitor結合表示は対象外であり、セッションWindowが属する
  一つのmonitorを使用する。

## Security and protocol impact

認証、NLA、CredSSP、server certificate、clipboard、Static Virtual Channel、
localhost Proxy、Agent、allowlist、Application Protocolは変更しない。monitor寸法、
接続先、画面内容を新たにlogへ記録しない。新しいnetwork経路や受信portも作らない。

## Validation

- 全画面状態からtarget種別を選択するpolicyを単体testで確認する。
- Debug／Release buildを`/W4 /WX`で成功させる。
- Debug／Releaseの全CTestを成功させる。
- 実RDPで動的解像度を選び、buttonと`Ctrl+Alt+Break`の両方について全画面移行と
  Window復帰を確認する。
- 等倍表示とSmart Sizingでは、同じ操作でremote desktop解像度が変化しないことを
  確認する。

## References

- Microsoft `IMsRdpClient9::UpdateSessionDisplaySettings`
- ADR 0003
- ADR 0004
