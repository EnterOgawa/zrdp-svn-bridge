# R-013 Microsoft connection barのZ-order復旧

日時: 2026-08-25

## 現象

Version `0.7.16.0`ではMicrosoft connection barは表示されたが、ZRdp containerは
Window modeのままだった。Version `0.7.17.0`以降でcontainerをborderless全画面へ
変更したところ、connection barが表示されなくなった。

Version `0.7.18.0`の接続前設定は`DisplayConnectionBar`、raised状態、最小化／復帰
buttonのsetterとreadbackに成功している。したがって設定不足ではなく、ActiveXを
全画面化した後にZRdp containerを`HWND_TOP`へ移したことで、別Windowのconnection barを
ZRdpが覆うZ-order競合と判断する。

## 変更対象

- `src/client/app/main_window.cpp`
  - `MainWindow::ApplyFullScreenMode`
  - `MainWindow::SetContainerFullScreen`
- version resource、manifest、contract test、RDP文書

## 修正

1. 全画面へ入る際はZRdp containerを先にborderless monitor配置し、その後にActiveXの
   `FullScreen`を有効化する。connection barをcontainer配置後に生成／表示させる。
2. Windowへ戻る際はActiveXの全画面を先に解除し、その後にcontainerのstyleと配置を
   復元する。
3. container配置時の`HWND_TOP`を廃止し、現在Z-orderを維持する。
4. 片側失敗時は逆順で直前状態へrollbackする。
5. Version `0.7.18.0`で追加したZRdp fallback buttonは、実機Gate完了まで維持する。

## 影響

- ActiveX／COM: 全画面property呼出しとUSER32配置の順序だけを変更する。
- Virtual Channel／HTTP Proxy／Agent Socket／security boundary: 変更しない。
- protocol compatibility: wire format、version、stream処理を変更しない。

## Test

- Debug／Release全CTest
- source comment／resource／portable binary contract
- 実RDPでMicrosoft connection barがborderless全画面より前面に表示されること
- connection barの最小化／復帰、fallback、`Ctrl+Alt+Break`、動的解像度
- RDP、Agent、Proxy、HTTP stream、別session非影響

## Rollback

`ApplyFullScreenMode`の順序と`SetWindowPos`のZ-order flagをVersion `0.7.18.0`へ戻す。
Protocol、Proxy、Agent設定のrollbackは不要である。
