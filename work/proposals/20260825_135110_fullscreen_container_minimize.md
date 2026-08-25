# R-010 全画面connection barからのSession Window最小化

日時: 2026-08-25

## 現象

ActiveX全画面中にconnection barの最小化を選ぶと、remote desktop表示だけが退避し、
最大化された白いZRdp Session Windowが画面上に残る。

## 原因

Microsoft RDP ActiveXは全画面connection barの最小化操作を
`IMsTscAxEvents::OnRequestContainerMinimize`（DISPID 14）でcontainerへ通知する。
Version `0.7.15.0`はこのeventをdispatchしていないため、Session Window自身を
最小化できていない。

## 修正方針

1. DISPID 14を引数なしの所有`RdpEvent`へ変換する。
2. 接続前に`ContainerHandledFullScreen=1`を設定してreadbackし、container eventを
   確実に有効化する。
3. `MainWindow::OnRdpEvent`は固定診断eventを記録する。
4. COM callbackのreentrancyを避け、Session Windowへ`WM_SYSCOMMAND`／
   `SC_MINIMIZE`を非同期送信する。
5. `WM_SIZE`の`SIZE_MINIMIZED`ではActiveXをresizeせず、動的解像度timerを解除する。
6. 復元時は既存の`LayoutControls`とdebounce経路だけを使用する。

## 非変更境界

- RDP接続、NLA、CredSSP、certificate検証を変更しない。
- remote desktop data、clipboard、音声、入力を独自処理しない。
- Virtual Channel、Application Protocol、Agent、allowlist、Proxyを変更しない。
- 最小化を切断、stream close、再接続の契機にしない。

## 検証

- Debug／Release全CTest
- Version `0.7.16.0`のresource／portable binary契約
- 全画面最小化、taskbar復元、動的解像度再追従のユーザー実機確認
