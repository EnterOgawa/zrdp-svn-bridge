# Phase 0 ローカル検証結果

検証日: 2026-07-30（Asia/Tokyo）

## 判定

- 開発基盤は完了した。
- Phase 0の`ZRdpSvnBridge.exe`はローカル実装と接続なし検証を完了した。
- Phase 0 Gateは未通過である。Zscaler ONのユーザー環境で実RDP接続を確認する必要がある。
- Agent、Static Virtual Channel Echo、HTTP Proxy、SVN Bridgeは未実装である。

## 実装した範囲

- COM STA上のWin32 UI
- ATL `CAxWindow`によるRemote Desktop ActiveX host
- 最新登録Control優先と、未登録時だけのversion fallback
- `IMsTscAxEvents` Connection PointのRAII購読／解除
- 接続先、ユーザー名、画面サイズ、Smart Sizing、接続、切断
- UIボタンおよびMicrosoft Control標準動作による全画面切替
- 接続前の`RedirectClipboard = VARIANT_TRUE`
- Microsoft標準のclient-side credential prompt
- Microsoft標準credential／warning dialogの親window指定
- credential保存禁止
- server authentication失敗時の標準warning表示方針
- warning、fatal error、disconnect reason、自動再接続、全画面eventの状態表示
- COM event callbackからの例外遮断
- Virtual Channel BSTRをbyte長でbounded copyするevent境界
- 接続クリックからActiveX eventまでを追跡する固定field診断log
- UI threadからfile I/Oを分離したbounded log queue
- 1 MiB・3世代のlog rotation

## Build結果

| 構成 | 結果 | Warning |
|---|---|---|
| x64 Debug | 成功 | 0 |
| x64 Release | 成功 | 0 |

使用条件:

- C++20
- Unicode
- `/W4`
- `/WX`
- `/permissive-`
- `/Zc:__cplusplus`
- Debug `/MTd`
- Release `/MT`

## CTest結果

Debug／Releaseの両方で次の3件が成功した。

1. `unit.build_contract`
   - x64
   - C++20
   - Unicode
   - static CRT
2. `unit.rdp_host_smoke`
   - COM STAとATL host初期化
   - RDP ActiveX生成
   - 空接続先の拒否
   - 文書例示用IPv4を使用した接続前設定
   - Smart Sizing
   - clipboard有効化と読戻し
   - credential prompt／保存禁止設定
   - event購読解除とwindow破棄
   - ActiveX dialog親window設定
3. `unit.diagnostic_log`
   - 1 MiB到達時のrotation
   - 固定event、`HRESULT`、codeの出力
   - bounded logのclean shutdown
   - 接続先、Authorization、passwordが出力されないこと

テストは`Connect()`を呼ばず、RDPまたは外部network通信を発生させていない。

## 実行確認

- Debug／Releaseともにメインwindowを起動できた。
- titleは`ZRdp SVN Bridge - Phase 0 (0.1.1)`である。
- ローカル環境では`MsRdpClient11`が選択された。
- 空の接続先で「接続」を押すと入力警告を表示し、通信を開始しない。
- 空の接続先でのクリック受理と入力拒否が固定eventとして診断logへ記録された。
- Release EXEのclean終了時にqueued logがflushされ、exit code `0`で終了した。
- 標準`WM_CLOSE`で強制終了せず、exit code `0`で終了した。
- 画面captureで接続欄、ユーザー名、画面サイズ、Smart Sizing、接続／切断、全画面、RDP表示領域、状態欄の配置を確認した。

初回起動確認ではATL executable module未初期化によるActiveX host crashを検出した。global `CAtlExeModuleT`を追加して修正し、その後のDebug／Release起動と自動smoke testで再発しないことを確認した。

## Release binary確認

`dumpbin`で次を確認した。

- x64 PE
- High Entropy VA
- Dynamic Base
- NX compatible
- Control Flow Guard
- import先はWindows標準DLLだけ
- `VCRUNTIME*.dll`、`MSVCP*.dll`等の追加Visual C++ runtime DLL dependencyなし
- ATL runtime DLL dependencyなし

## 未確認

- 実支給PCへの接続
- Zscaler ONでのRDP経路
- NLA／実credential prompt
- 実keyboard／mouse／画面更新
- 実全画面切替
- 双方向clipboard
- 接続中切断／再接続
- 実証明書／認証warning

実機手順は`docs/test-plan.md`に記載した。
