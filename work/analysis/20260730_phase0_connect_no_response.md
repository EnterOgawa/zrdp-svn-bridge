# Phase 0 接続ボタン無反応の調査と修正

記録日: 2026-07-30（Asia/Tokyo）

## 事象

ユーザー実機で`ZRdpSvnBridge.exe`の「接続」を押しても、見た目上の反応が確認できなかった。

実環境の接続先やcredentialは開発環境へ取り込まず、接続処理の段階だけを識別できるように修正した。実機での原因確定とPhase 0 Gate判定は、更新版の再確認結果を待つ。

## 確認した実装上の問題

1. 状態欄の「接続中」表示と入力欄の無効化が、ActiveX `Connect()`呼出しから戻った後に行われていた。
2. Microsoft RDP ActiveX Controlが表示するcredential／warning dialogの親windowを明示していなかった。
3. 接続クリック、設定適用、`Connect()`戻り値、ActiveX eventを区別する診断logがなかった。

Microsoftの仕様上、`Connect()`は非同期接続を開始し、接続完了は`IMsTscAxEvents`で判定する。したがって、呼出し前のUI反映と、呼出し結果からeventまでの診断が必要である。

## 修正

- `Connect()`前に「接続要求を送信中」を表示し、接続buttonと入力欄を無効化して即時描画する。
- `IMsRdpClientNonScriptable2::UIParentWindowHandle`へZRdpのmain windowを設定する。
- 接続先やユーザー名を受け取れない固定event／数値field方式の診断logを追加する。
- log file I/Oをworker threadへ分離し、queueを512 recordに制限する。
- current logを1 MiB、backupを3世代に制限する。
- EXE横へ書けない場合だけ`%LOCALAPPDATA%`へfallbackする。
- file／product versionを`0.1.1.0`へ更新する。
- 実機確認用Release EXEをworkspace直下の`out/ZRdpSvnBridge.exe`へ出力する。

## ローカル検証

- x64 Debug build: 成功、warning 0
- x64 Release build: 成功、warning 0
- Debug CTest: 3/3成功
- Release CTest: 3/3成功
- ActiveX dialog親window設定: smoke test成功
- log rotation／固定field／禁止語非出力: unit test成功
- Release EXE起動、空接続先のbutton click、入力拒否dialog、clean終了: 成功
- 上記操作のlog sequenceとexit code `0`: 確認済み
- Release EXE version: `0.1.1.0`
- Release EXE path: `out/ZRdpSvnBridge.exe`
- Release EXE size: `395776` bytes
- Release EXE SHA-256: `50A0B7706C62262A7D40153807B078F916AFBD70646FBE7785AC0B07B1A05BF4`
- x64、High Entropy VA、ASLR、DEP、CFG: 有効
- runtime dependency: Windows標準DLLのみ

`Connect()`を伴うnetwork試験は実施していない。Zscaler ONの支給PC接続はユーザー実機で再確認する。

その後の実機ログで`Connect()`と`OnConnecting`までは到達し、直後に`disconnect reason=2825`で終了していたことが判明した。後続調査と修正は`20260730_phase0_disconnect_2825.md`へ記録する。
