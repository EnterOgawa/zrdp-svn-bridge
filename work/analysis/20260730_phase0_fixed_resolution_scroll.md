# Phase 0 固定解像度とウィンドウ追従

日付: 2026-07-30

> **Superseded:** `0.1.6.0`の実機確認で、選択した初期解像度が接続先application layoutを変更することを確認した。本方式は`docs/adr/0003-fixed-initial-rdp-desktop-and-local-scaling.md`および`work/analysis/20260730_phase0_remote_desktop_preservation.md`の方式へ置き換えた。

## 背景

ZRdpの初期ウィンドウが小さい状態で、認証直後にリモート解像度を表示領域へ変更すると、接続先アプリケーションのpanel配置やlayoutが意図せず変化する。

Windows標準RDPに近い既定動作として、選択したリモート解像度を維持し、狭いウィンドウではスクロールして操作する。利用者が明示的に選択した場合だけ、リモート解像度をウィンドウへ追従させる。

## 表示policy

| `ウィンドウサイズに合わせる` | 初期リモート解像度 | Smart Sizing | 接続後の動的解像度変更 | 狭いウィンドウ |
|---|---|---|---|---|
| OFF（既定） | Full HD／WQHD／Fullの選択値 | OFF | 行わない | ActiveX標準スクロールバー |
| ON | 接続時のRDP表示領域 | OFF | 250 ms debounce後に行う | 解像度自体を表示領域へ合わせる |

ONの場合は固定解像度の選択欄を無効化し、競合する2つの指定を同時に受け付けない。チェック状態は保存せず、起動時は常にOFFとする。

## Microsoft ActiveX仕様との対応

- `IMsRdpClientAdvancedSettings::SmartSizing`は画面をcontrol領域へscaleする機能であり、有効時はスクロールバーが表示されない。このため固定解像度modeでは明示的に無効化する。
- `IMsRdpClient9::UpdateSessionDisplaySettings`は、利用者が追従を有効にした場合だけ使用する。

参照:

- https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpclientadvancedsettings-smartsizing
- https://learn.microsoft.com/en-us/previous-versions/windows/desktop/legacy/mt703457(v=vs.85)

## 実装

- 既存の`Smart Sizing`チェック欄を`ウィンドウサイズに合わせる`へ置き換えた。
- 既定checkをOFFへ変更した。
- OFFでは`RdpConnectionSettings::smart_sizing = false`とし、`WM_SIZE`からセッション解像度更新をscheduleしない。
- ONでは接続前に現在のRDP表示領域を初期desktop寸法へ使用し、接続後も既存のdebounce付き表示更新を有効化する。
- ON中は画面サイズcomboを無効化する。
- 診断logには接続先や寸法を出さず、`rdp.connection_settings_apply_started enabled=0|1`だけを記録する。

## 検証境界

ローカルでは、既定check、control有効状態、build、ActiveX設定適用、動的更新APIの未接続時拒否を確認する。

スクロールバーの表示、リモート解像度の維持、接続先アプリケーションlayoutの維持は実RDPセッションに依存するため、`0.1.6.0`でユーザー実機確認を行う。

## ローカル検証結果

- Debug CTest: 4/4成功
- Release CTest: 4/4成功
- ActiveX smoke testで`RdpConnectionSettings`のSmart Sizing既定値がOFFであることを確認した。
- hidden UI probeで、`ウィンドウサイズに合わせる`が既定OFF、解像度comboが有効であることを確認した。
- 同probeで、チェック時に解像度comboが無効化され、チェック解除時に再度有効化されることを確認した。
- Release version: `0.1.6.0`
- Release size: `436736` bytes
- SHA-256: `9DE4BFB58FA9FC2C53956574869F0E740D0D30321091CAC08EE490EC0946BF4D`
- x64、High Entropy VA、ASLR、DEP、Control Flow Guardを確認した。
- dependencyはWindows system DLLだけであり、外部runtime DLLを追加していない。
