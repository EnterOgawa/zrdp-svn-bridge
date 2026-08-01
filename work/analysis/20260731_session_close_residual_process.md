# Version 0.7.13 Session終了後の残留process分析

## 1. 事象

2026-07-31、約2時間の実RDP接続後にSession Windowを閉じたところ、画面は消えたが
`C:\develop\zrdp\out\ZRdpSvnBridge.exe` processが残留した。対象PIDと実行pathを
確認し、診断後に当該processだけを停止した。

## 2. 証拠

固定診断logは次へ到達した。

- `ui.close_requested`
- `rdp.event.confirm_close`
- `proxy.client_state_changed proxy_state=0`

一方、`process.stopping`は記録されなかった。Wait Chain Traversalではapplication
thread間のcycleは検出されず、Proxy停止後のActiveX／ATL OLE破棄境界が残った。
Crash dumpは機密payloadを含み得るため、作成していない。

## 3. 原因範囲

Windowの閉じる操作だけが`RequestClose()`、`OnConfirmClose`、即時`DestroyWindow`を
使用し、通常の「切断」buttonは`Disconnect()`、`OnDisconnected`を使用していた。
後者は実運用で終了できているため、接続中Controlを破棄するタイミング差を原因範囲と
判断した。Microsoft公開手順への違反とは断定せず、実測されたControl実装差へ対処する。

## 4. 対策

Version `0.7.14.0`で、接続中Windowの終了を`Disconnect()`、`OnDisconnected`、
Window破棄の順へ統一した。既切断、切断中、接続中を純粋policyで分離し、二重COM
呼出しと接続中ActiveXの即時破棄を避ける。

詳細判断は`docs/adr/0008-disconnect-before-session-window-destroy.md`へ記録した。

## 5. ローカル検証

- Debug build: 成功、warning 0
- Debug CTest: 30／30成功
- Release build: 成功、warning 0
- Release CTest: 30／30成功
- Client／Agent: `0.7.14.0`
- 両EXE: 未署名

## 6. 実機Gate結果

2026-07-31、Version `0.7.14.0`で実RDP接続中のSession Windowと接続管理Windowを
閉じた後にClient processが残留しないことをユーザーが確認した。固定診断logでも
`rdp.event.disconnected`と`process.stopping`への到達を確認した。R-004を完了し、
Authenticode署名Gateへ進む。

## 7. 署名後確認

2026-08-01、Client／Agent両EXEのWindows trust `Valid`、SHA-256 signature digest、
RFC 3161 timestamp、Version `0.7.14.0`維持、署名後SHA-256を確認した。署名後の
runtime smoke testとRelease Gateは別途実施する。
