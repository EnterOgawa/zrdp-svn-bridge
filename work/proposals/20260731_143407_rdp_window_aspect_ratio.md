# Phase 9改善 I-005: RDP session Windowの手動アスペクト比制約

## 1. 目的

RDP session Windowを利用者が手動で変更する際、title barや表示mode欄を除いた
実際のRDP表示領域が、接続開始時にZRdpから要求したremote desktopと同じ
アスペクト比を維持するようにする。

接続先applicationのlayoutとWindows標準のWindow操作を維持するため、最大化、
Windowsスナップ、ActiveX全画面には制約を適用しない。

## 2. 変更対象

- `src/client/app/main_window.{h,cpp}`
  - 接続を受理したsessionごとに初期desktop幅／高さを保持する。
  - `WM_ENTERSIZEMOVE`、`WM_SIZING`、`WM_EXITSIZEMOVE`を処理する。
  - 非client領域と表示mode欄を除いたRDP領域の比率だけを固定する。
- `src/client/app/rdp_window_sizing.{h,cpp}`
  - UIやActiveXに依存しない、8方向のWindow矩形補正を実装する。
- `tests/unit/rdp_window_sizing_test.cpp`
  - 辺、角、anchor、最小寸法、不正値を検証する。
- `AGENTS.md`、RDP設計、テスト計画、ロードマップ
  - 適用範囲とWindows標準操作との境界を記録する。

Bridge、Proxy、Agent、wire protocol、allowlist、接続履歴形式は変更しない。

## 3. 設計

接続時に`RdpConnectionSettings::desktop_width`と`desktop_height`として
ActiveXへ渡した値を、対象sessionの固定比率とする。

```text
RDP表示幅
---------------- = desktop_width / desktop_height
RDP表示高さ

top-level Window幅
  = RDP表示幅 + 左右の非client frame

top-level Window高さ
  = RDP表示高さ + 上下の非client frame + 表示mode欄
```

`WM_SIZING`の左、右、上、下、4角を区別し、利用者が動かしていない反対辺を
anchorする。角操作では`WM_ENTERSIZEMOVE`時の寸法から変化率が大きい軸を主操作軸
として扱い、一回のdrag途中で補正結果を基準にし直さない。

pixelへ変換する際の丸め差は最大1 pixelとする。DPI適用済み最小Window寸法を
下回る場合は、比率を維持したまま両軸を拡大する。乗算、加算、RECT座標範囲を
検証し、不正値やoverflow時は候補を変更せずUSER32の既定処理へ戻す。

## 4. 表示modeごとの動作

- `NativeScroll`
  - remote desktop寸法を変更しない。
  - 比率を維持したviewport内でActiveX標準scroll barを使用する。
- `SmartSizing`
  - remote desktop寸法を変更せず、比率固定済みのRDP領域へClient側だけで縮小する。
- `DynamicResolution`
  - 比率固定済みのRDP領域を、既存の250 ms debounce後に
    `UpdateSessionDisplaySettings`へ渡す。

Window変更を理由に`Reconnect`を呼び出さない。

## 5. Windows標準操作との境界

- 通常Windowの辺／角dragだけを`WM_SIZING`で補正する。
- 最大化はmonitor work areaを使用するWindows標準動作へ委譲する。
- WindowsスナップはOSが決めた矩形を変更しない。
- ActiveX全画面中はMicrosoft Controlへ表示を委譲する。
- Window移動だけでは寸法を変更しない。

## 6. Security boundaryと失敗時動作

- 接続先、ユーザー名、desktop寸法を新しい設定fileやlogへ出力しない。
- 接続成立前に有効なdesktop比率がない場合は制約を無効にする。
- 矩形計算失敗でRDP、Bridge、Proxy、他sessionを終了しない。
- sessionごとに比率と手動変更状態を所有し、複数session間で共有しない。

## 7. Test観点

- 左右辺を基準に高さを補正する。
- 上下辺を基準に幅を補正する。
- 4角で反対角をanchorする。
- 角dragの主操作軸を一回の変更開始寸法から判定する。
- title bar、frame、表示mode欄をRDP比率計算から除外する。
- DPI適用済み最小Window寸法を比率維持したまま満たす。
- 不正比率、不正辺、反転RECT、overflow候補を適用しない。
- Debug／Release全CTestとportable versionを確認する。

## 8. Rollback

`WM_SIZING`処理、session内の比率保持、独立計算module、対応testと文書を一括して
戻す。ActiveX設定、履歴、Agent、Proxy、allowlist、wire protocolに移行作業は
発生しない。旧Client／Agentへ戻す場合も設定fileの削除は不要である。
