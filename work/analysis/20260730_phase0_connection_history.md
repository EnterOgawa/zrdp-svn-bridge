# Phase 0 接続履歴 実装記録

日付: 2026-07-30

## 目的

RDP接続先とユーザー名を編集可能なコンボボックスとし、過去に認証完了した値を再利用できるようにする。同時に、候補の重複と平文保存を避ける。

## 実装

- 認証完了時だけ「接続先＋ユーザー名」の組を記録する。
- 直近10組をmost-recent-firstで保持する。
- 同じ組は大小文字を区別せず削除してから先頭へ追加する。
- 接続先候補とユーザー名候補も、それぞれ大小文字を区別せず重複排除する。
- 接続先を選ぶと、その接続先に対応する最も新しいユーザー名を補完する。
- `%LOCALAPPDATA%\ZRdpSvnBridge\connection-history.bin`へcurrent-user DPAPIで暗号化保存する。
- file I/OとDPAPI処理は最大16要求の専用workerで直列化し、完了結果を`WM_APP` messageでUI threadへ返す。
- passwordはdata modelに含めず、履歴内容は診断ログへ出力しない。
- 読込／保存結果は`HRESULT`とrecord件数だけを固定eventとして記録する。

## ローカル検証

- Debug構成を`/W4`、`/WX`のままbuildした。
- 未作成fileの扱い、DPAPI暗号化と復号、再起動相当の再読込を確認した。
- 大小文字だけ異なる同一組が1件へ統合され、再利用時に先頭へ移動することを確認した。
- 10件上限、平文UTF-8／UTF-16が暗号化fileに現れないこと、破損fileの拒否を確認した。
- workerの非同期load／save、result snapshot、終了時drainをhidden test windowで確認した。

実際のドロップダウン操作と再起動後の候補表示は、ユーザー環境で`0.1.5.0`を実行して確認する。

## 配布build

- Debug CTest: 4/4成功
- Release CTest: 4/4成功
- Release version: `0.1.5.0`
- Release size: `436736` bytes
- SHA-256: `FF4D7125E0CE6A63BE113E7DAB53C1016CB54019685805B818E4965AC07281C9`
- x64、High Entropy VA、ASLR、DEP、Control Flow Guardを確認した。
- dependencyはWindows system DLLだけであり、外部runtime DLLを追加していない。
- hidden起動したReleaseで、接続先とユーザー名が編集可能なdrop-down ComboBoxとして作成されることを確認した。
- 認証未完了のUI probeでは履歴fileが作成されず、入力したprobe値も診断logへ出力されないことを確認した。
