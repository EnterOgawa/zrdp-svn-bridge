# Phase 9改善I-010 セッション全画面ボタン ローカル検証

実施日: 2026-07-31

## 1. 結果

接続後の各RDPセッションWindowで、動的解像度radioの右側に
「全画面」ボタンを表示する変更を実装した。

既存実装には`IDC_FULL_SCREEN`、`OnFullScreen()`、
`RdpControlHost::SetFullScreen()`、ActiveX eventに追従する
「全画面」／「ウィンドウ」文言切替が存在していた。今回の変更は、
非表示だった既存buttonを表示対象とDPI対応配置へ戻すものである。

## 2. 実装

- セッションWindowで表示するcontrolを4件から5件へ拡張した。
- 全画面buttonを動的解像度radioの右側へ100 logical pixel幅で配置した。
- 接続前は無効、接続済み／ログイン済み／再接続中は有効という既存policyを
  維持した。
- button押下と`Ctrl+Alt+Break`は同じMicrosoft ActiveX全画面状態へ収束する。
- ActiveXが拒否した場合はRDP接続を維持し、button文言を先行変更しない。
- Client／Agentのversion resourceを`0.7.11.0`へ更新した。

## 3. Build／test

| 検証 | 結果 |
|---|---|
| CMake configure | 成功 |
| Debug build `/W4 /WX` | 成功、warning 0 |
| Debug CTest | 30/30成功 |
| Release Client link | 成功、warning 0 |
| Release Agent link | 成功、warning 0 |
| Release CTest | 30/30成功 |
| Release resource契約 | CTest内で成功 |
| Release portable binary契約 | CTest内で成功 |
| Client／Agent file version | 両方`0.7.11.0` |

初回ローカル検証では、動作中の標準`out/ZRdpSvnBridge.exe`を無断終了・
上書きせず、Release Client／Agentを
`work/tmp/release-fullscreen-0.7.11`へ出力した。その後ユーザーの終了連絡を
受け、標準`out`へ同じVersionをRelease buildし、全30 CTestを再実行した。

| 配布物 | SHA-256 | 署名 |
|---|---|---|
| `out/ZRdpSvnBridge.exe` | `487C4DD5DA786E860F795B5E67AFEF648A09C669F9235C1F2279E09E82008D7F` | 未署名 |
| `out/ZRdpSvnAgent.exe` | `D8CDC744C9556A2949D5B87792FD6D3DD6E36AA78BAD43482B9E2C7CD1A053F9` | 未署名 |

標準出力生成とローカルGateは完了した。残る項目はユーザー実機表示確認である。

## 4. 互換性・security

- RDP ActiveXの全画面API、STA、event購読、接続／切断処理は変更していない。
- 等倍表示、Smart Sizing、動的解像度の動作は変更していない。
- wire protocol、Static Virtual Channel、Proxy、Agent、allowlist、
  設定形式、履歴形式は変更していない。
- credential、payload、実接続先、実ユーザー情報をsource、resource、log、
  文書へ追加していない。

## 5. 実機確認項目

- [ ] 接続後の表示操作欄に「全画面」buttonが表示される。
- [ ] 接続成立後にbuttonが有効化される。
- [ ] button押下でMicrosoft全画面へ移行する。
- [ ] 全画面からWindow表示へ復帰できる。
- [ ] `Ctrl+Alt+Break`による往復も継続する。
- [ ] 復帰後も選択中の表示modeとRDP画面操作が維持される。
- [ ] Agent／Proxy／SVN／Tracに回帰がない。

## 6. Rollback

`work/tmp/20260731_171936_before_session_full_screen_button.zip`から、
今回のUI表示、version、契約test、記録だけを復元できる。
