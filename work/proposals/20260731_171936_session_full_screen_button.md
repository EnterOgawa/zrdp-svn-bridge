# セッションWindow全画面ボタン追加提案

## 要望

接続後のRDPセッションWindowで、表示モード欄と同じ一行から全画面へ
切り替えられるボタンを表示する。

## 現状

`MainWindow`には`IDC_FULL_SCREEN`、`OnFullScreen()`、ActiveXの
`SetFullScreen()`、全画面状態に応じた「全画面」／「ウィンドウ」表示切替が
既に実装されている。ただし複数セッション向けUIへ整理した際、
`full_screen_button_`が表示対象と配置処理から外されているため画面に現れない。

## 変更

1. セッションWindowの表示操作欄で`full_screen_button_`を表示する。
2. 動的解像度radioの右側へDPI対応寸法で配置する。
3. 接続成立前は無効、接続成立後は有効という既存状態制御を維持する。
4. 押下時は既存`OnFullScreen()`を使用し、Windows標準互換の
   `Ctrl+Alt+Break`経路も維持する。
5. ActiveXの全画面eventに合わせ、ボタン文言を「全画面」／「ウィンドウ」へ
   切り替える。
6. Client／Agentの配布versionを`0.7.11.0`へ揃える。

## 影響範囲

- ActiveX／COM: 既存`SetFullScreen()`呼出し経路を再公開するだけで、
  lifecycle、STA、event購読は変更しない。
- RDP表示mode: 等倍、Smart Sizing、動的解像度の意味と設定を変更しない。
- Virtual Channel／Proxy／Agent／allowlist: 変更しない。
- Protocol compatibility: wire version、frame、stream stateを変更しない。
- Security: 接続先、credential、payloadを新規表示・記録しない。

## 検証

- Debug／Releaseを`/W4 /WX`でbuildする。
- Debug／Releaseの全CTestを実行する。
- resource契約で「全画面」／「ウィンドウ」文字列を検証する。
- Release Clientのversion、icon、portable binary契約を確認する。
- 実機では接続後のボタン表示、有効化、全画面移行、Window復帰、
  `Ctrl+Alt+Break`回帰を確認する。

## 失敗時動作

ActiveXが全画面切替を拒否した場合は、既存どおりRDP接続を維持し、
エラーを表示してボタン文言を先行変更しない。

## Rollback

`MainWindow`の表示対象と配置、resource version、契約test、関連記録だけを
作業前archiveから戻す。ActiveX、Bridge、Proxy、Agent network処理は
変更対象に含めない。

## ローカル実施結果

- [x] `full_screen_button_`をセッションWindowの表示対象へ追加した。
- [x] 動的解像度radioの右側へ100 logical pixel幅で配置した。
- [x] 既存`OnFullScreen()`、接続状態による有効化、eventによる文言切替を
      変更せず再利用した。
- [x] resource契約へ「全画面」／「ウィンドウ」を追加した。
- [x] Client／Agentのversionを`0.7.11.0`へ揃えた。
- [x] Debug buildをwarningなしで完了し、30/30 CTestを成功させた。
- [x] Release Client／Agentを一時出力へlinkし、28件の回帰CTestと
      2件のRelease成果物契約をすべて成功させた。
- [x] 実行中Client終了後に標準`out`へRelease成果物を生成した。
- [x] 標準`out`を対象にRelease全30 CTestを成功させた。
- [ ] ユーザー実機でボタン表示、全画面、Window復帰を確認する。

作業前archiveは
`work/tmp/20260731_171936_before_session_full_screen_button.zip`、
詳細結果は
`work/analysis/20260731_phase9_i010_session_full_screen_button_local_validation.md`
である。
