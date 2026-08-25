# 全画面フォールバックボタン削除検証

## 対象

- Version: `0.7.20.0`
- 変更: 全画面中のZRdp独自`最小化`／`ウィンドウ`overlayを削除する。
- 維持: Microsoft connection bar、通常Windowの`全画面`button、
  `Ctrl+Alt+Break`、container／ActiveX全画面遷移順序。

## 静的確認

- `IDS_MINIMIZE`、`IDC_FULL_SCREEN_MINIMIZE`、`full_screen_minimize_button_`、
  `FullScreenEscapeControlBounds`、`MakeFullScreenEscapeControlBounds`の残存なし。
- `CMakeLists.txt`、Client／Agent manifest、version resource、binary contractを
  `0.7.20.0`へ同期した。
- `git diff --check`成功。

## Build／CTest

- `cmake --preset vs2022-x64`: 成功。
- `cmake --build --preset debug --parallel`: 成功。
- `ctest --preset debug`: 30/30成功。
- `cmake --build --preset release --parallel`: 成功。
- `ctest --preset release`: 30/30成功。

## 標準portable出力

| Binary | FileVersion | Size | Authenticode | SHA-256 |
|---|---:|---:|---|---|
| `ZRdpSvnBridge.exe` | `0.7.20.0` | 682,496 bytes | `NotSigned` | `C704C582B7F93A04040B27BC638A01BF2B2F0A0B137F1DD564CE211F2BA85F21` |
| `ZRdpSvnAgent.exe` | `0.7.20.0` | 417,280 bytes | `NotSigned` | `D3ACDC5F1F195325991E2AD181103134AAD762C78F65B5924F189874B401FA38` |

## 実機gate

- 全画面中にZRdp独自buttonが表示されないこと。
- Microsoft connection barの最小化／Window復帰が機能すること。
- taskbarから復元後も全画面状態を維持すること。
- `Ctrl+Alt+Break`、三表示mode、動的解像度、RDP／Bridge／Proxy／別sessionへ
  回帰がないこと。

## Authenticode署名

ユーザーの明示指示により、Client／Agentを組織コード署名証明書で署名した。PINは
token middlewareの保護UIだけで入力され、PIN、token識別情報、certificate識別情報を
repositoryへ記録していない。署名前成果物はGit管理外の
`work/tmp/unsigned-0.7.20.0-before-signing-20260825_160010/`へ保全した。

- File digest: SHA-256
- Timestamp: GlobalSign RFC 3161
- SignTool verify: 両EXE成功、warning 0、error 0
- Windows trust: 両EXE `Valid`
- 署名後resource／portable binary contract: 2/2成功
- 署名後の再build／binary変更: なし

| Binary | FileVersion | Size | Authenticode | SHA-256 |
|---|---:|---:|---|---|
| `ZRdpSvnBridge.exe` | `0.7.20.0` | 694,856 bytes | `Valid`、SHA-256、RFC 3161 | `B5321D67BC7FB3127611A98BCF223856F59DB22B226D683DC446CE320BB19E2F` |
| `ZRdpSvnAgent.exe` | `0.7.20.0` | 429,640 bytes | `Valid`、SHA-256、RFC 3161 | `D52EA278A9C9508F272CD5B4519B00BC154D8C781B61B30CB3D17E0F8A98EA31` |

## ユーザー実機受入

2026-08-25、ユーザー実機で署名後Version `0.7.20.0`の動作確認を完了した。全画面中に
ZRdp独自buttonを表示せず、Microsoft connection barから最小化／Window復帰できること、
taskbar復元、`Ctrl+Alt+Break`、三表示mode、動的解像度、RDP／Bridge／Proxy／別sessionに
回帰がないことを受入済みとする。
