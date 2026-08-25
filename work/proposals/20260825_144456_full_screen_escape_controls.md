# R-012 全画面復帰経路の保証

日時: 2026-08-25

## 現象

Version `0.7.17.0`でZRdpのtop-level frameと表示mode欄を全画面中に隠した結果、
Microsoft connection barも表示されない環境では、mouse操作でWindow modeまたはlocal
desktopへ戻る経路が画面上から失われた。診断logでは`request_go_full_screen`と
全画面API成功を確認できる一方、復帰要求eventは記録されていない。

## 変更対象

- `src/client/rdp/rdp_control_host.cpp`
  - `RdpControlHost::Impl::ApplyConnectionSettings`
- `src/client/app/main_window.cpp`
  - `MainWindow::OnCreate`、`LayoutControls`、`SetDisplayControlsVisible`、`WM_COMMAND`
- `src/client/app/main_window.h`
  - 全画面専用最小化button handle
- `src/client/app/resource.h`、`src/client/app/ZRdpSvnBridge.rc`
  - 最小化buttonのresource
- version resource、manifest、contract test、RDP文書

## 修正

1. 接続前に`DisplayConnectionBar=VARIANT_TRUE`と`PinConnectionBar=VARIANT_FALSE`を
   設定し、Microsoft connection barを表示可能かつ展開状態にする。
2. `IMsRdpClientAdvancedSettings3`でconnection barの最小化buttonと復帰buttonを
   明示的に表示する。
3. 全画面中は表示mode radioを隠す一方、ZRdpの「最小化」「ウィンドウ」buttonだけを
   RDP child領域へoverlayする。Microsoft barが表示されない場合の有限fallbackとする。
4. 「最小化」は既存`WM_SYSCOMMAND/SC_MINIMIZE`経路を使い、taskbarから復元後も
   全画面状態を維持する。「ウィンドウ」は既存のcontainer rollback経路を使う。

## 影響

- ActiveX／COM: 接続前のconnection bar property設定を追加する。
- Virtual Channel／HTTP Proxy／Agent Socket／security boundary: 変更しない。
- protocol compatibility: wire format、version、stream処理を変更しない。
- RDP認証、証明書警告、credential処理を変更しない。

## 失敗時動作

- connection bar必須propertyを設定またはreadbackできないControlでは接続を開始しない。
- overlay buttonはActiveXと同じUI threadで操作し、失敗してもRDP／Bridge／Proxyを
  切断しない。

## Test

- resource／source comment／portable binary contract
- Debug／Release全CTest
- Microsoft connection bar設定のsource contract
- 実RDPでconnection bar、ZRdp fallback button、最小化、Window復帰、
  `Ctrl+Alt+Break`、三表示mode、別session非影響を確認する。

## Rollback

本変更のconnection bar property設定、全画面専用button、resource、version更新を
一組で戻す。Virtual Channel、Proxy、Agent成果物は機能差分を持たない。
