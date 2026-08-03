# R-007 ActiveX既定の認証ダイアログ親Window ローカル検証

- 実施日時: 2026-08-03 16:58 JST
- 対象Version: `0.7.14.0`
- 変更対象: Clientのみ

## 1. 原因境界

R-006回帰撤回版で従来接続先が回復した一方、certificate確認が必要な接続先は
credential UI後に二回目の認証warning eventを即時dismissし、`263/0`で終了した。
Windows固定診断では`TsSslEventHandshakeContinueFailed (0x80004005)`に対応する。

Windows標準Remote Desktop Clientは利用者がcertificate警告を明示承認した後に
次のSSL handshakeへ進むため、R-007はActiveX container内のdialog親Window境界だけを
変更対象とした。

## 2. 実装結果

- `IMsRdpClientNonScriptable2::UIParentWindowHandle`へsession top-level Windowを
  明示設定する処理を削除した。
- credential UIと後続certificate UIのmodal ownershipをActiveX Control Windowの
  既定処理へ一貫して委譲した。
- 固定event`rdp.activex_default_dialog_parent_selected`を一ActiveX生成につき一回
  記録する。
- `RedirectionWarningDialogVersion=1`、`AuthenticationLevel=2`、CredSSP、NLA、
  focus保護、認証UI表示後のretry禁止を維持した。
- Agent、Virtual Channel、Protocol、Proxy、allowlist、接続履歴を変更していない。

## 3. 自動検証

| 構成 | 結果 |
|---|---|
| 対象test: ActiveX smoke／diagnostic log／source comment | 3/3成功 |
| Debug build | warningなしで成功 |
| Debug CTest | 30/30成功 |
| 隔離Release build | warningなしで成功 |
| 隔離Release CTest | 30/30成功 |
| 標準`out` resource contract | 成功 |
| 標準`out` portable binary contract | 成功 |

実RDP接続、certificate警告の操作、certificate登録、registry／GPO変更は自動testで
行っていない。

## 4. 成果物

| File | Version | Authenticode | size | SHA-256 |
|---|---|---|---:|---|
| `out/ZRdpSvnBridge.exe` | `0.7.14.0` | `NotSigned` | 677,376 byte | `FA7222A43D46E6C74B1C3A5BA476A265C4F8D163370F3708A20EA9AE676CCE7E` |
| `out/ZRdpSvnAgent.exe` | `0.7.14.0` | `Valid`、既存 | 426,568 byte | `E8E245EAC803D16FDE8EBA2040A205F808169981B096834F97836F5DA2DB61A2` |

Agent SHA-256は作業前後で一致した。R-006回帰撤回版Clientは
`work/tmp/release-r006-rollback-confirmed-0.7.14/ZRdpSvnBridge.exe`へ退避し、
SHA-256 `DD6871B0D2186C4BF56A6027A58F72B02F6187D2FD209AE9AF2F82DA740A3929`
を確認した。

## 5. Security確認

- `AuthenticationLevel=2`、NLA、CredSSP、certificate検証を維持した。
- certificate警告を抑止、自動承認、記憶しない。
- certificate store、registry、GPO、credential manager、RDP cacheを変更していない。
- certificate名、接続先、domain、ユーザー名、credential、画面内容をlog／文書へ
  追加していない。
- 問題接続先で成立しない場合も`mstsc.exe`やsecurity緩和へfallbackしない。

## 6. 未完了Gate

- R-007候補でも従来接続先へ回帰なく接続できること。
- 「いいえ」で追加password promptなしに未接続へ戻ること。
- 接続後のAgent、Proxy、SVN／Trac、複数session、表示modeに回帰がないこと。

## 7. 実機結果

2026-08-03、問題接続先でcertificate警告が操作可能な状態となり、利用者が明示継続後、
desktopへ到達した。固定logでは次の順序を確認した。

1. `rdp.activex_default_dialog_parent_selected`
2. `rdp.security_dialog_configuration_returned`の`dialog_version=1 enabled=1`
3. `rdp.authentication_level_configuration_returned`の`authentication_level=2`
4. credential UIに対応する約6秒間のauthentication warning
5. certificate UIに対応する約4秒間のauthentication warning
6. `rdp.event.connected`
7. `rdp.event.login_complete`
8. 利用者操作による`rdp.event.disconnected locally_requested=1`

二回目のwarningは従来の約1 ms即時dismissから利用者操作可能な時間へ変わり、
自動retry、追加password prompt、`263/0`を挟まず接続した。certificate名、接続先、
domain、ユーザー名、credentialはlogおよび本記録へ含めていない。
