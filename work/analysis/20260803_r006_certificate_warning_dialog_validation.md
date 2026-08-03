# R-006 certificate警告ダイアログ互換性 ローカル検証

- 実施日時: 2026-08-03 16:19 JST
- 対象Version: `0.7.14.0`
- 変更対象: Clientのみ

## 1. 原因境界

ZRdp固定logでは、一回の接続試行で最初の認証dialogが数秒間表示された後、二回目の
`OnAuthenticationWarningDisplayed`と`Dismissed`がほぼ同時に発生し、`263/0`で
終了した。R-005は同じsequenceを初回を含む三回反復した。

Windows標準Clientではcredential UI後にserver certificate警告が表示され、利用者の
明示継続後にdesktopへ到達した。credentialやtransportを変更せず、ActiveX
connection security dialogとfocus所有へ修正範囲を限定した。

## 2. 実装結果

- `IMsRdpExtendedSettings::Property`の
  `RedirectionWarningDialogVersion`へ`VT_UI4`の`1`を接続前に設定した。
- `AuthenticationLevel=2`をsetter後にreadbackし、不一致をfail-closedにした。
- 認証dialog表示中は親Windowの`WM_SETFOCUS`からActiveX childへfocusを移さない。
- 現在試行でMicrosoft認証dialogを表示した場合、その後の`263/0`をR-005 retryから
  除外した。
- 固定診断eventへ`dialog_version`、`authentication_level`、`enabled`、`HRESULT`
  だけを追加した。
- Agent、Virtual Channel、Protocol、Proxy、allowlist、接続履歴を変更していない。

## 3. 自動検証

| 構成 | 結果 |
|---|---|
| 対象test: ActiveX smoke／retry policy／diagnostic log | 3/3成功 |
| Debug build | warningなしで成功 |
| Debug CTest | 30/30成功 |
| 独立Release build | warningなしで成功 |
| Release CTest | 30/30成功 |
| 標準`out` resource contract | 成功 |
| 標準`out` portable binary contract | 成功 |

Debug／Releaseの`unit.rdp_host_smoke`が現在のsystem ActiveXで
`RedirectionWarningDialogVersion`を含む接続前設定を受理した。実RDPへの接続、
certificateの登録、registry／GPO変更は自動testで行っていない。

## 4. 成果物

| File | Version | 状態 | size | SHA-256 |
|---|---|---|---:|---|
| `work/tmp/release-r006-rollback-confirmed-0.7.14/ZRdpSvnBridge.exe` | `0.7.14.0` | 未署名R-006回帰撤回版 | 677,888 byte | `DD6871B0D2186C4BF56A6027A58F72B02F6187D2FD209AE9AF2F82DA740A3929` |
| `out/ZRdpSvnAgent.exe` | `0.7.14.0` | 既存署名済み | 426,568 byte | `E8E245EAC803D16FDE8EBA2040A205F808169981B096834F97836F5DA2DB61A2` |

Agent SHA-256は作業前後で一致した。R-005署名済みClientは
`work/tmp/release-signed-r005-0.7.14/ZRdpSvnBridge.exe`へ退避した。`work/tmp`は
Git管理外であり、配布物へ含めない。

## 5. Security確認

- `AuthenticationLevel=2`、NLA、CredSSPを維持した。
- certificate警告を自動承認せず、利用者判断をMicrosoft UIへ委譲した。
- certificate store、registry、GPO、credential manager、RDP cacheを変更していない。
- certificate名、接続先、domain、ユーザー名、credential、画面内容をlog／文書へ
  追加していない。
- 認証UI表示後に接続が成立しない場合、password入力や警告判断を自動反復しない。

## 6. 未完了Gate

- credential UI後にcertificate警告が表示されること。
- 「はい」でdesktopへ到達すること。
- 「いいえ」で追加password promptなしに未接続へ戻ること。
- 通常の接続先、Agent、Proxy、SVN／Trac、複数session、表示modeに回帰がないこと。
- 固定logの`dialog_version=1 enabled=1`と`authentication_level=2`を確認すること。

## 7. 初回実機結果

2026-08-03の初回実機候補では、Version 1とAuthenticationLevel 2の設定は`S_OK`だった。
credential UIは約6秒表示された後、二回目のwarning eventが約1 msでdismissされ、
`263/0`で終了した。認証UI表示後の自動retryは禁止され、password要求は一回で停止した。

この結果を受け、`ShowRedirectionWarningDialog=TRUE`を追加候補として実装した。しかし、
既存接続先を含め`Connect()`が`S_FALSE`を返し、`disconnect code=1`で即時終了した。
certificate warningではなくRDP fileのredirection warning向けpropertyであるため不採用とし、
実装、固定event、文書上の採用記述を撤回した。

撤回後ClientではDebug／Release各30 CTestを再実施し、標準`out`のresource／portable binary
contractも成功した。Windows event logでは元の失敗を
`TsSslEventHandshakeContinueFailed (0x80004005)`として確認した。次の調査はdialog親Window
境界へ限定する。
- ユーザー受入後、必要な場合にClientを再署名すること。

## 8. 回帰撤回版の実機結果とR-007候補

回帰を起こした`ShowRedirectionWarningDialog=TRUE`を撤回したClientでは、従来接続先へ
再び接続できることをユーザーが確認した。問題接続先は引き続きcertificate警告を
操作できず、`263/0`で終了する。このため回帰撤回自体を受入済みとし、残件を
dialog親Window境界だけへ分離した。

R-007では`IMsRdpClientNonScriptable2::UIParentWindowHandle`へsession top-level Windowを
明示設定せず、ActiveX Control Windowの既定親を使用する。作業前Clientは
`work/tmp/release-r006-rollback-confirmed-0.7.14/ZRdpSvnBridge.exe`へ退避し、SHA-256
`DD6871B0D2186C4BF56A6027A58F72B02F6187D2FD209AE9AF2F82DA740A3929`を確認した。
