# Phase 9改善I-005 RDP Windowアスペクト比制約 ローカル検証

検証日: 2026-07-31

## 1. 対象

- Client／Agent version: `0.7.5.0`
- Platform: Windows x64
- Build: Visual Studio 2022、C++20、`/W4`、`/WX`、Release `/MT`
- wire protocol: Version `1`のまま
- Agent／Proxy／allowlist／接続履歴形式: 変更なし

## 2. 実装結果

- 接続を受理した各sessionで、ActiveXへ要求した初期desktop幅／高さを保持する。
- `WM_ENTERSIZEMOVE`で一回のdrag開始寸法を固定し、`WM_EXITSIZEMOVE`で破棄する。
- `WM_SIZING`の左、右、上、下、4角を接続時desktop比率へ補正する。
- title bar、非client frame、表示mode欄をRDP表示領域の比率計算から除外する。
- 左／上操作では反対辺をanchorし、角操作ではdrag開始時からの変化率が大きい軸を
  主操作軸として扱う。
- DPI適用済み最小Window寸法を、アスペクト比を維持したまま満たす。
- 最大化、Windowsスナップ、ActiveX全画面は補正せず標準動作へ委譲する。
- 等倍表示とSmart Sizingではremote desktop寸法を変更しない。動的解像度だけが
  補正後RDP領域を既存の250 ms debounce経路で接続先へ適用する。
- Window変更を理由として`Reconnect`を追加していない。
- desktop寸法、接続先、ユーザー名を設定fileまたは診断logへ追加していない。

## 3. 自動テスト

```text
Debug:   27 / 27 passed
Release: 27 / 27 passed
```

新規`unit.rdp_window_sizing`の重点確認:

- 左右辺の幅基準補正
- 上下辺の高さ基準補正
- 4角の反対角anchor
- 角操作の主軸判定
- 固定非RDP領域を除いた16:9計算
- 比率を維持した最小Window寸法
- 不正desktop比率と不正`WMSZ_*`の無変更拒否

既存Protocol、Bridge、Proxy、Agent、Phase 1～6結合testもすべて成功した。
実RDP、支給PC、Zscaler、実SVN／Tracへの自動接続は行っていない。

## 4. Portable成果物

| File | Size | Version | SHA-256 | Authenticode |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 676,864 byte | 0.7.5.0 | `F7C19CB5AA0649C4613C9575F45B5C4810E4CC121875ED46FDB95B6F6D24CE6F` | NotSigned |
| `out/ZRdpSvnAgent.exe` | 423,424 byte | 0.7.5.0 | `42E029B50DD9FE25FF22F5003FCFF3375EBC2CB2331662055AF520A5A0806FBC` | NotSigned |

未署名はRelease Gate前の想定状態である。署名処理はユーザーが実施する。

## 5. 実機確認待ち

- 接続後、左右辺をdragするとRDP表示領域の縦寸法も比率に従って変化する。
- 接続後、上下辺をdragするとRDP表示領域の横寸法も比率に従って変化する。
- 4角を横主方向／縦主方向にdragしても、途中で主軸が反転せず追従する。
- 等倍表示ではremote desktop layoutが変化せず、必要部分をscrollできる。
- Smart Sizingではremote desktop layoutが変化せず、比率を維持して縮小表示される。
- 動的解像度では補正後の比率を維持してremote desktopが追従する。
- 最大化、Windowsスナップ、`Ctrl+Alt+Break`全画面が従来どおり動作する。
- 複数sessionがそれぞれの接続時desktop比率を独立して維持する。
- Agent、Proxy、SVN／Trac操作に回帰がない。

実機結果には実解像度、接続先、domain、ユーザー名、画面captureを記録せず、
各項目の成否だけを報告する。
