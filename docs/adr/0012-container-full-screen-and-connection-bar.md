# ADR 0012: Container全画面とMicrosoft接続バーの同期

- Status: Accepted
- Date: 2026-08-25
- 記録整理日: 2026-10-07

## 背景

ActiveXの全画面状態だけを変更すると、ZRdpのframeと設定行が残り、通常Windowのまま
見える場合があった。逆にcontainerを全画面化した後で最前面へ移すと、別Windowとして
表示されるMicrosoft connection barを覆ってしまった。最小化時にもActiveXだけが
退避し、白いcontainer Windowが残る問題があった。

## 判断

- 接続前に`ContainerHandledFullScreen`、`DisplayConnectionBar`、接続バーの
  最小化／Window復帰操作を設定し、readbackを確認する。
- ActiveX eventからの要求はCOM callback内で直接Windowを変更せず、UI messageへ渡す。
- 全画面開始時はcontainerを現在monitor全体のborderless Windowへ先に配置し、
  top-level Z-orderを変更せず、その後にActiveXの`FullScreen`を有効化する。
- title bar、frame、表示mode欄を隠し、RDP領域をcontainerのclient全体へ広げる。
- Window復帰時はActiveX全画面を先に解除し、保存済みstyleと通常／最大化配置を
  session単位で復元する。片側失敗時は直前の表示状態へrollbackする。
- Microsoft接続バーの最小化要求はSession Window全体の標準最小化へ接続し、
  taskbarから復元した後も全画面状態を維持する。
- 全画面中の画面操作はMicrosoft接続バーと`Ctrl+Alt+Break`を使用する。
  ZRdp独自の最小化／復帰buttonをRDP領域へ重ねない。
- 最小化中は動的解像度timerを停止する。

## 影響と失敗時動作

Windowの表示切替でRDP、Virtual Channel、Proxy、既存HTTP streamを切断しない。
状態の所有はsession単位とし、別sessionの配置や接続へ影響させない。
security境界、認証UI、certificate判断、wire protocol、allowlistに変更はない。

接続バーの前面表示を回復できたため、検証中の独自overlayをVersion `0.7.20.0`で
撤去した。containerの後置最前面化と独自overlayの常設は採用しない。

## 検証

`rdp_window_sizing_test`とActiveX smoke testでstyle、RDP領域、接続前propertyを検証する。
ユーザー実機では、接続バー表示、最小化、Window復帰、taskbar復元、keyboard切替、
動的解像度を確認した。Debug／Release各30 CTestを通過し、署名後も受入済みである。

## 参照

- [RDP ActiveX設計](../rdp-activex.md)
- [Version 0.7.20リリースノート](../release-notes-0.7.20.md)
- `src/client/app/rdp_container_full_screen.h`
- `src/client/app/main_window.cpp`
