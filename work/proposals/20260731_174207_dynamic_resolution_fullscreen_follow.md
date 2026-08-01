# 動的解像度の全画面追従修正提案

## 現象

セッションWindowの「全画面」buttonでMicrosoft ActiveX全画面へ移行できるが、
動的解像度を選択中でも接続先desktop領域がWindow時の寸法から変化しない。

## 原因

既存実装は全画面移行時に動的解像度timerを取消し、
`ScheduleDynamicResolutionUpdate()`と`ApplyDynamicResolutionUpdate()`も
`full_screen()`状態を更新不要として除外している。このため全画面Windowだけが
拡大され、`UpdateSessionDisplaySettings`へmonitor寸法が送られない。

## 変更方針

1. 動的解像度modeだけは全画面中も更新対象とする。
2. Window表示では従来どおりRDP childのclient領域を使用する。
3. ActiveX全画面では、セッションWindowが属するmonitor全体の寸法を使用する。
4. `OnEnterFullScreen`／`OnLeaveFullScreen`後に250 ms debounceで更新する。
5. button、`Ctrl+Alt+Break`、Microsoft connection barの全経路を同じpolicyへ
   収束させる。
6. 等倍表示とSmart Sizingでは接続先desktop寸法を変更しない。
7. 既存の`E_UNEXPECTED`限定有限retryと、恒久失敗時の等倍縮退を維持する。
8. Client／Agentの配布versionを`0.7.12.0`へ揃える。

## 影響範囲

- ActiveX／COM: 既存STA上の`UpdateSessionDisplaySettings`呼出し時点と寸法だけを
  変更する。Control生成、認証、全画面API、event購読は変更しない。
- 表示mode: 動的解像度だけに適用する。等倍／Smart Sizingは変更しない。
- Virtual Channel／Proxy／Agent／allowlist: 変更しない。
- Protocol compatibility: wire version、frame、stream stateを変更しない。
- Security: monitor寸法は診断logへ追加せず、接続先や画面内容も記録しない。

## 検証

- 全画面状態から選択すべき寸法種別を純粋policyとして単体テストする。
- Debug／Releaseを`/W4 /WX`でbuildする。
- Debug／Releaseの全CTestを実行する。
- Version `0.7.12.0`のClient／Agent成果物契約を確認する。
- 実機で動的解像度の全画面移行、Window復帰、button／shortcut両経路を確認する。

## 失敗時動作

一時的な`E_UNEXPECTED`は既存の有限delayで再試行する。恒久失敗または上限到達時は
RDP接続を維持し、等倍表示へ縮退して利用者へ通知する。

## Rollback

全画面時のtarget選択、event後の予約、version、test、関連記録だけを作業前archive
から戻す。全画面buttonそのものと、受入済みBridge／Proxy／Agent処理は維持する。

## 実施結果

- `DynamicResolutionTarget` policyを追加し、Window表示と全画面のtargetを
  COM非依存で選択できるようにした。
- `OnEnterFullScreen`、`OnLeaveFullScreen`、button要求の成功後に既存の
  250 ms debounceを予約するよう変更した。
- timer実行時に全画面ならmonitor全体、WindowならRDP childのclient領域を取得する。
- 等倍表示とSmart Sizingの更新条件は変更していない。
- Client／AgentをVersion `0.7.12.0`へ更新した。
- Debug／Release buildはwarningなしで成功した。
- Debug／Release CTestは各30/30成功した。
- 標準`out`のVersion、SHA-256、未署名状態を確認した。
- 実RDPでの全画面monitor追従とWindow復帰時の再追従はユーザー確認待ちである。

作業前archive:

`work/tmp/20260731_174207_before_dynamic_resolution_fullscreen_follow.zip`
