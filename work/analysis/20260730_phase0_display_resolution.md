# Phase 0 接続解像度選択の更新

記録日: 2026-07-30（Asia/Tokyo）

## 要求

- 接続解像度の既定値をFull HDとする。
- WQHDとFullを選択可能にする。

## 実装

接続解像度の選択肢を次の3件に整理した。

1. `Full HD（1920 × 1080）` — 既定
2. `WQHD（2560 × 1440）`
3. `Full（現在のモニター解像度）`

`Full`は接続buttonを押した時点でZRdp main windowが存在するモニターを取得し、そのモニター全体のpixel幅と高さをRDP desktop sizeへ設定する。全画面表示への切替機能とは独立しており、接続後もwindow／全画面を切り替えられる。

モニターを取得できない場合や空のmonitor rectangleが返った場合は、推測した解像度へfallbackせず接続開始を中止する。

表示文字列はWin32 resourceへ格納した。file／product／manifest versionは`0.1.3.0`へ更新した。

## ローカル検証

- x64 Debug build: 成功、warning 0
- x64 Release build: 成功、warning 0
- Debug CTest: 3/3成功
- Release CTest: 3/3成功
- Release UI probe:
  - item count: `3`
  - default index: `0`
  - default label: `Full HD（1920 × 1080）`
  - WQHD label: `WQHD（2560 × 1440）`
  - Full label: `Full（現在のモニター解像度）`
- Release EXE version: `0.1.3.0`
- Release EXE path: `out/ZRdpSvnBridge.exe`
- Release EXE size: `400896` bytes
- Release EXE SHA-256: `44D025EBBF6F2F4D77F8866FEA91F983AC6F62C04B9C8852BF096516286D6DFE`
- x64、High Entropy VA、ASLR、DEP、CFG: 有効
- runtime dependency: Windows標準DLLのみ
