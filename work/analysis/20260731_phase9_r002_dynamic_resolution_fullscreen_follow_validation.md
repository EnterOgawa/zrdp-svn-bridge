# Phase 9回帰修正R-002 動的解像度の全画面追従ローカル検証

実施日: 2026-07-31

## 1. 現象

Version `0.7.11.0`のセッションWindowで全画面buttonを押すと、Microsoft Remote
Desktop ActiveXの全画面Windowには移行できた。しかし動的解像度を選択中でも、
接続先remote desktop領域は通常Window時の寸法から変化しなかった。

## 2. 原因

全画面移行eventで動的解像度timerを取消し、予約処理と適用処理の両方が
`full_screen()`状態を対象外にしていた。そのためActiveX全画面Windowだけが拡大し、
`UpdateSessionDisplaySettings`へmonitor寸法を送る経路が存在しなかった。

## 3. 修正

- 全画面状態から`WindowDisplayArea`または`FullScreenMonitor`を選ぶ純粋policyを
  追加した。
- 動的解像度では全画面移行とWindow復帰の両event後に250 ms debounceを予約する。
- 全画面ではセッションWindowが属するmonitor全体、WindowではRDP childの
  client領域を`UpdateSessionDisplaySettings`へ渡す。
- timer実行時点の状態を使い、短時間の往復で古いtargetを適用しない。
- 既存の`E_UNEXPECTED`限定有限retryと恒久失敗時の等倍縮退を維持した。
- 等倍表示とSmart Sizingではremote desktop寸法を変更しない。
- Client／Agentの配布versionを`0.7.12.0`へ揃えた。

## 4. 影響境界

- Remote Desktop ActiveXの生成、認証、event購読、全画面APIは変更していない。
- Virtual Channel、Application Protocol、Proxy、Agent、allowlist、履歴形式は
  変更していない。
- 新しいport、network経路、credential処理、payload logは追加していない。
- monitor寸法や接続先を診断logへ追加していない。

## 5. ローカル検証

| 検証 | 結果 |
|---|---|
| CMake configure `vs2022-x64` | 成功 |
| Debug build `/W4 /WX` | 成功、warningなし |
| Debug CTest | 30/30成功 |
| Release build `/W4 /WX` | 成功、warningなし |
| Release CTest | 30/30成功 |
| 動的解像度target policy | Window／全画面の両分岐成功 |
| source comment契約 | 成功 |
| Client／Agent version一致 | `0.7.12.0`で一致 |
| Authenticode | 両方とも未署名 |

標準成果物:

| 成果物 | byte | SHA-256 |
|---|---:|---|
| `out/ZRdpSvnBridge.exe` | 673,792 | `3F5C4FA8011560765FB5DF084E756A6FE4841809A0CFC91A5B7ADD57B73D1286` |
| `out/ZRdpSvnAgent.exe` | 413,696 | `2D2CF19D4109CA56A3DC3578DB7C3C003DB21542A585587BA75538171679919A` |

## 6. 実機Gate

次はユーザー環境で確認する。

1. 動的解像度でRDP接続する。
2. 全画面buttonを押し、remote desktop自体がmonitor全体へ追従することを確認する。
3. connection barまたは`Ctrl+Alt+Break`でWindowへ戻し、remote desktopがRDP表示
   領域へ再追従することを確認する。
4. `Ctrl+Alt+Break`から全画面へ入る経路でも同じ結果になることを確認する。
5. 等倍表示ではdesktop解像度維持とscroll、Smart Sizingではdesktop解像度維持と
   local縮小が継続することを確認する。
6. Agent、Proxy、SVN／Tracに回帰がないことを確認する。

実IP、接続名、domain、ユーザー名、画面内容は検証記録へ残さない。

## 7. Rollback

作業前状態は次へ保存した。

`work/tmp/20260731_174207_before_dynamic_resolution_fullscreen_follow.zip`

問題発生時は全画面target policy、event後の予約、Version、test、関連記録だけを
戻し、全画面buttonと受入済みBridge機能は維持する。
