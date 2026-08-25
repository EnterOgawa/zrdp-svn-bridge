# 全画面フォールバックボタン削除提案

## 変更理由

Version 0.7.19候補でMicrosoft Remote Desktop ActiveXのconnection barが実機表示
されたため、connection bar未表示時の退避経路として追加していたZRdp独自の
`最小化`／`ウィンドウ`buttonは不要となった。全画面UIをWindows標準RDP互換の
connection barへ統一し、RDP画面へ独自controlを重ねない構成へ戻す。

## 変更対象

- `src/client/app/main_window.cpp` / `MainWindow`
  - `IDC_FULL_SCREEN_MINIMIZE` command、control生成、全画面配置、z-order更新を削除する。
  - 全画面時は表示mode一行と通常Window用`全画面`buttonをすべて非表示にする。
- `src/client/app/main_window.h`
  - 全画面用最小化buttonのhandleを削除する。
- `src/client/app/resource.h`、`src/client/app/ZRdpSvnBridge.rc`
  - 専用control IDと`最小化`文字列resourceを削除する。
- `src/client/app/rdp_container_full_screen.h`
  - フォールバックbutton矩形の型と計算helperを削除する。
- `tests/unit/rdp_window_sizing_test.cpp`、`tests/unit/resource_contract_test.cpp`
  - 削除したhelper／resourceの契約testを除去する。
- version resource、README、`AGENTS.md`、`docs`一式
  - Versionを0.7.20へ更新し、全画面操作をMicrosoft connection barと
    `Ctrl+Alt+Break`へ統一した仕様へ同期する。

## 影響範囲

- ActiveX／COM: `DisplayConnectionBar`、`PinConnectionBar`、connection barの
  最小化／復帰button設定、containerとActiveXの全画面遷移順序は変更しない。
- Virtual Channel／HTTP Proxy／Agent Socket: 変更しない。
- security boundary: 接続先、credential、証明書、network境界、log内容を変更しない。
- protocol compatibility: `ESVNBRG`とapplication protocolに変更はない。
- 失敗時動作: connection barが環境要因で表示されない場合も
  `Ctrl+Alt+Break`は維持する。独自overlayへ自動fallbackしない。

## テスト観点

- Debug／Releaseの全CTestを成功させる。
- Client／Agentのversion resourceとportable binary契約を確認する。
- 全画面配置がRDP領域全体を使用し、ZRdp独自button resourceが残らないことを確認する。
- 実機でMicrosoft connection barだけが表示され、最小化、Window復帰、
  `Ctrl+Alt+Break`が機能することを確認する。

## rollback

Version 0.7.19候補のフォールバックcontrol生成、command、配置helper、resource、testを
一体で復元する。RDP／Bridge／Proxy protocolのrollbackは不要である。
