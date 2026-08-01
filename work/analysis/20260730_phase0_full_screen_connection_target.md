# Phase 0 全画面接続先表示の確認

## 対象

- version: `0.1.9.0`
- executable: `out/ZRdpSvnBridge.exe`
- 対象機能: 全画面connection barとwindow titleでの接続先識別

## 実装結果

- `RdpConnectionSettings::server`の検証済み値から
  `ZRdp SVN Bridge - <接続先>`を構築する。
- 接続前に`IMsTscAx::FullScreenTitle`と
  `IMsRdpClientNonScriptable3::ConnectionBarText`へ同じBSTRを設定する。
- `ConnectionBarText`をreadbackし、設定した有限長BSTRとの完全一致を確認する。
- `FullScreenTitle`の`MAX_PATH - 1`文字制約を超える場合は、実際の接続先設定を
  変更せず、表示末尾だけを`...`として省略する。
- 接続先を診断logへ出力せず、ユーザー名、password、credentialを表示へ
  含めない。
- hostname入力時もDNS解決や接続先探索を追加せず、入力値をそのまま使用する。

Microsoft仕様:

- [IMsTscAx::FullScreenTitle property](https://learn.microsoft.com/en-us/windows/win32/termserv/imstscax-fullscreentitle)
- [IMsRdpClientNonScriptable3::ConnectionBarText property](https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpclientnonscriptable3-connectionbartext)

## ローカル検証

- x64 Debug build: 成功、warning 0
- x64 Debug CTest: 5/5成功
- x64 Release build: 成功、warning 0
- x64 Release CTest: 5/5成功
- 文書用IPv4 literalで接続前設定と`ConnectionBarText` readbackに成功
- 空、256文字、埋め込みNULの接続先拒否に成功
- Release EXEは非表示起動後1.2秒間message loopを維持し、検証processを終了
- PE: x64、High Entropy VA、ASLR、DEP、CFG有効
- 依存DLL: Windows system DLLのみ

## 配布物

- file version: `0.1.9.0`
- product version: `0.1.9.0`
- size: `440320` bytes
- SHA-256:
  `A787456A6FF841C4D1C22F19BB8D8773534D9DABCE5C54DA1781052595D68F25`
- Authenticode: 未署名

## 実機確認待ち

- 実RDP接続後、全画面connection barに
  `ZRdp SVN Bridge - <入力した接続先>`が表示されること。
- 全画面からwindow modeへ戻った後もRDP接続と選択中の表示modeを維持すること。

この確認はPhase 0のRDP UI確認であり、`ESVNBRG`、Agent、localhost HTTP Proxy、
SVN操作の成立確認ではない。
