# R-011 Container-handled全画面 ローカル検証

日時: 2026-08-25

## 原因

Version `0.7.16.0`は`ContainerHandledFullScreen=1`を有効化したが、ZRdp containerは
`OnRequestGoFullScreen`後にActiveXの`FullScreen` propertyだけを変更していた。
Microsoftの契約では、このmodeでtop-level Windowの全画面遷移はcontainerが所有する。
そのためControl状態とbutton文言だけが全画面となり、ZRdpのframeと表示mode欄が残った。

## 修正

- 全画面前のtop-level Window styleと`WINDOWPLACEMENT`をsessionごとに保存する。
- 現在monitor全体へ`WS_POPUP`のborderless Windowを配置する。
- 全画面中はtitle bar、frame、表示mode欄を隠し、RDP ActiveXをclient全体へ広げる。
- Window復帰時は元styleと通常／最大化配置を復元する。
- buttonと`OnRequestGoFullScreen`／`OnRequestLeaveFullScreen`を同じ非同期messageへ
  収束させる。
- ActiveXまたはUSER32の片側が失敗した場合は直前状態へrollbackする。
- Virtual Channel、Protocol、Proxy、Agent runtime、allowlistは変更しない。

## 自動検証

- Debug build: 成功、warning 0
- 隔離Debug CTest: 30／30成功
- 隔離Release build: 成功、warning 0
- 隔離Release CTest: 30／30成功
- 標準Debug CTest: 30／30成功
- 標準Release CTest: 30／30成功
- source comment contract: Debug／Releaseとも成功
- resource／portable binary contract: Debug／Releaseとも成功
- full-screen style／RDP content bounds policy: 成功

## 未署名隔離成果物

| File | size | Version | Authenticode | SHA-256 |
|---|---:|---|---|---|
| `ZRdpSvnBridge.exe` | 680,960 byte | `0.7.17.0` | `NotSigned` | `0F9C6FBEF64CC153991981B38BB26422C3DA3085F8542FDAE193C303FE4A4D9B` |
| `ZRdpSvnAgent.exe` | 417,280 byte | `0.7.17.0` | `NotSigned` | `B48887F2D6C53B3B62144074F9671534ABC4C10D2DCF7771EFFDC622AE00BDC1` |

隔離成果物はGit管理外の
`work/tmp/candidate-r011-container-full-screen`へ出力した。実行中Clientの終了確認後、
署名済みVersion `0.7.16.0`を
`work/tmp/signed-0.7.16.0-before-r011`へ保全し、標準`out`を未署名Version
`0.7.17.0`へ更新した。標準Release再ビルド後の成果物は次のとおりである。

| File | size | Version | Authenticode | SHA-256 |
|---|---:|---|---|---|
| `ZRdpSvnBridge.exe` | 680,960 byte | `0.7.17.0` | `NotSigned` | `F55ECD59F59329E755A2771B2AF9A6E01383738297FE075B11F106F5F72D05A4` |
| `ZRdpSvnAgent.exe` | 417,280 byte | `0.7.17.0` | `NotSigned` | `531D112ECB567CCB324B21842127ECA343DC803DA574D5176D769A1123FDDA1D` |

## 残る実機Gate

- buttonと`Ctrl+Alt+Break`の両方でRDPだけがmonitor全体へ表示されること
- connection barに接続先識別とWindow復帰操作が表示されること
- Window復帰時に通常配置または最大化状態が復元されること
- connection bar最小化とtaskbar復元後も全画面が維持されること
- 三表示mode、動的解像度、RDP、Agent、Proxy、HTTP stream、別sessionに回帰がないこと
