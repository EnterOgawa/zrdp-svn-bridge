# Phase 9 改善I-002 session表示mode一行 ローカル検証

更新日: 2026-07-31

## 1. 対象

- Client／Agent product version: `0.7.2.0`
- 対象改善: session Windowへ表示mode設定だけを再配置する
- wire protocol、Agent allowlist、HTTP Proxy境界: 変更なし

## 2. 実装結果

- session Windowはtitle bar、表示mode一行、RDP表示領域で構成する。
- 表示する操作controlは次の4個だけとする。
  - `表示モード:` label
  - `等倍表示（スクロール）`
  - `Smart Sizing（縮小表示）`
  - `動的解像度（Window追従）`
- 表示mode行は96 DPIで高さ31 pixel相当とする。
- 接続先、domain、ユーザー、接続／切断、全画面、Bridge、Echo、status barは
  session Windowへ表示しない。
- 接続管理Windowで選択した初期modeをsessionへ引き継ぎ、RDP接続成立後も
  既存の排他的切替処理を使用する。
- titleの接続先、RDP／Agent／Proxy状態、port、stream数、転送量表示を維持する。
- 閉じる操作による対象sessionの切断と`Ctrl+Alt+Break`全画面切替を維持する。

## 3. ローカル検証

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug --parallel
ctest --preset debug
cmake --build --preset release --parallel
ctest --preset release
```

- Debug build: 成功、`/W4`・`/WX`
- Debug CTest: 26/26成功
- Release build: 成功、`/W4`・`/WX`
- Release CTest: 26/26成功
- Client／Agent: x64、Version `0.7.2.0`

## 4. portable成果物

| file | size | version | SHA-256 | Authenticode |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 664,064 byte | `0.7.2.0` | `0D5E4769BD8ECFCC28FB702945726E6202AFDF0A4FB2F34A999BB447D5246F2A` | NotSigned |
| `out/ZRdpSvnAgent.exe` | 423,424 byte | `0.7.2.0` | `4FF11907A9A89D184109862B449220EDBCD160BAF6DF24F8789FB97FECEE6CF6` | NotSigned |

## 5. ユーザー実機確認待ち

- [ ] session Window上部に表示mode一行だけが表示される。
- [ ] 接続管理Windowで選択した初期modeがsessionへ反映される。
- [ ] 接続中に3 modeを切り替えられる。
- [ ] 等倍表示とSmart Sizingではremote desktop解像度を維持する。
- [ ] 動的解像度だけがWindow変更へremote desktop解像度を追従させる。
- [ ] mode切替後もRDP、Agent、Proxy通信が継続する。
- [ ] title状態表示、`Ctrl+Alt+Break`、閉じる操作に回帰がない。

実接続先、domain、ユーザー名、実URL、repository名、credential、画面captureは
本記録へ含めない。
