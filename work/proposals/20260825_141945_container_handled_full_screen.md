# R-011 Container-handled全画面のWindow遷移修正提案

## 現象と原因

Version `0.7.16.0`は全画面connection barの最小化要求を受けるため、接続前に
`ContainerHandledFullScreen=1`を設定した。Microsoftの契約では、このmodeを有効に
するとControl自身は`Ctrl+Alt+Break`による全画面Window遷移を行わず、containerへ
`OnRequestGoFullScreen`／`OnRequestLeaveFullScreen`を通知する。

現実装は通知後に`IMsRdpClient::FullScreen`だけを変更し、ZRdpのtop-level Windowの
frame、monitor矩形、表示mode欄を変更していない。このためControl側の状態とbutton
文言だけが全画面となり、ZRdp Windowは設定行を残したWindow modeのままとなる。

参考にしたMicrosoft公式契約:

- `IMsTscAdvancedSettings::ContainerHandledFullScreen property`
- `IMsTscAxEvents::OnRequestGoFullScreen method`
- `IMsTscAxEvents::OnRequestContainerMinimize method`
- `IMsRdpClient::FullScreen property`

## 変更対象

- `src/client/app/main_window.h`
  - Window modeのstyleと`WINDOWPLACEMENT`をsession単位で保持する。
  - container全画面の適用／復元と表示mode欄の表示切替を専用methodへ閉じ込める。
- `src/client/app/main_window.cpp`
  - 全画面開始時に現在monitor全体へborderless top-level Windowを配置する。
  - 全画面中はZRdpのtitle bar、frame、表示mode欄を隠し、RDP表示をclient全体へ広げる。
  - Window復帰時に元styleと`WINDOWPLACEMENT`を復元する。
  - UI buttonとActiveX request eventを同じ非同期遷移へ収束させる。
- `src/client/app/rdp_container_full_screen.h`
  - full-screen styleとRDP表示領域top位置の純粋policyを定義する。
- `tests/unit/rdp_window_sizing_test.cpp`
  - frame除去、既存style保持、全画面時toolbar除外を検証する。
- Version resource、RDP設計、test plan、roadmap
  - 修正候補をVersion `0.7.17.0`として記録する。

## 失敗時動作

- monitor情報、Window style、配置の取得に失敗した場合は全画面へ遷移せず、成立中の
  RDP、Bridge、Proxyを維持して分類済みHRESULTを表示する。
- top-level Windowの全画面化に失敗した場合はActiveXの`FullScreen`を直前状態へ戻す。
- Window復帰失敗時は中途半端なframeを公開せず、保存済み全画面styleへrollbackする。
- 動的解像度は成功した表示状態に対してだけ既存debounce経路から更新する。

## 影響範囲

- ActiveX／COM: `ContainerHandledFullScreen`と既存event購読は維持する。COM操作は同じ
  STA UI threadに限定する。
- Virtual Channel／Protocol: frame、SVC、Bridge sessionに変更なし。
- HTTP Proxy／Agent Socket: listener、stream、allowlistに変更なし。
- Security boundary: 新しい接続、権限変更、credential保持、診断情報追加は行わない。
- 複数session: 保存するWindow状態は各`MainWindow` instance専有とする。

## テストとrollback

- full-screen style／RDP boundsの単体testを既存CTestへ追加する。
- Debug／隔離Releaseの全CTest、resource／portable binary契約を実行する。
- 実機ではbuttonと`Ctrl+Alt+Break`の往復、connection bar、最小化／復元、三表示mode、
  RDP／Agent／Proxy維持、別session非影響を確認する。
- rollbackはR-011のClient変更とVersion／文書更新を戻す。Agent runtimeとprotocolを
  戻す必要はない。
