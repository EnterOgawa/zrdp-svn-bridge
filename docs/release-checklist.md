# ZRdp SVN Bridge 社内配布・署名確認書

- 文書ID: `ZRDP-REL-001`
- 対象Version: `0.7.14.0`
- 更新日: 2026-08-01

## 1. 配布候補

| File | Version | 未署名size | 未署名SHA-256 |
|---|---|---:|---|
| `ZRdpSvnBridge.exe` | `0.7.14.0` | 676,352 byte | `B8CF2929567715E8AB5794564392A26328E911636EDD351FC8CAD42317A19118` |
| `ZRdpSvnAgent.exe` | `0.7.14.0` | 414,208 byte | `7F51EA61B71211DA4491B1EA0A90EF079C0BA3A4EC40C5F25A2DC0323B35A427` |

このhashは署名前のsource-derived成果物を識別するための値である。Authenticode署名後は
file内容とSHA-256が変化するため、署名済み配布物のhashを別途記録する。

署名前の正本はGit管理外の
`work/tmp/release-unsigned-final-0.7.14/`へ同じSHA-256で保全済みである。この
directoryはrollbackと署名前比較だけに使用し、社内配布物へ含めない。

## 2. 署名前Gate

- [x] Client／AgentがVersion `0.7.14.0`で一致する。
- [x] Debug buildがwarningなしで完了する。
- [x] Debug全30 CTestが成功する。
- [x] Release buildがwarningなしで完了する。
- [x] Release全30 CTestが成功する。
- [x] 未署名成果物のsizeとSHA-256を記録する。
- [x] PDB、log、dump、capture、実`agent.ini`を配布候補へ含めない。
- [x] 仕様書、利用手順書、リリースノートをVersion `0.7.14.0`へ合わせる。
- [x] 文書に実IP、実URL、repository名、ユーザー名、credential、token情報を含めない。
- [x] 実RDP接続中にSession Windowを閉じ、WindowとClient processが残留しない。
  2026-07-31のユーザー実機確認と固定診断logで確認済み。

## 3. 署名責任と取扱い

Authenticode署名は、組織からコード署名権限を付与されたユーザーが、組織承認済みの
USB token、証明書、timestamp手順を使用して実施する。

- Token PIN、PUK、certificate秘密鍵をcommand line、script、chat、logへ記録しない。
- PINはtoken middlewareの保護されたUIにだけ入力する。
- Token serial、certificate subject、thumbprint、provider固有情報を本repositoryへ
  記録しない。
- 署名自動化script、署名用batch、秘密鍵file、certificate exportを作成しない。
- ClientとAgentの両方を同じ配布Gateで署名する。
- 署名後にEXEを再build、resource更新、binary patchしない。

CodexはUSB token、certificate、PINを操作せず、ユーザーから署名完了の連絡を受けた
後に公開情報だけで署名状態、Version、hashを検証する。

## 4. ユーザー署名作業

- [x] 組織承認済みの手順で`out\ZRdpSvnBridge.exe`へ署名する。
- [x] 組織承認済みの手順で`out\ZRdpSvnAgent.exe`へ署名する。
- [x] 両方で署名処理とRFC 3161 timestampが成功したことを確認する。
- [ ] Token middlewareを終了し、PINを保存していないことを確認する。
- [x] Codexへ両EXEの署名完了を連絡する。

## 5. 署名後検証

署名完了後に次を確認する。

- [x] `Get-AuthenticodeSignature`がClient／Agentとも`Valid`を返す。
- [x] Windows trust policyによる署名検証が成功する。
- [x] Signature digestがSHA-256で、RFC 3161 timestampを持つことを確認する。
- [ ] Signatureとtimestampが組織固有の署名規定を満たすことを承認者が確認する。
- [x] File Version／Product Versionが両方とも`0.7.14.0`である。
- [x] Client／Agentの署名済みSHA-256を記録する。
- [x] 署名後のClient起動と一件のRDP接続を確認する。
- [x] 署名後のAgent起動とBridge Readyを確認する。
- [x] Proxy利用可と一件の安全なread-only SVN／Trac操作を確認する。

2026-08-01、上記3項目をユーザー環境で確認した。固定診断logでもRDPログイン完了、
Bridge Ready、Proxy有効化、process正常終了への到達を確認した。

### 署名済み成果物記録

| File | Version | 署名状態 | 署名済みsize | 署名済みSHA-256 |
|---|---|---|---:|---|
| `ZRdpSvnBridge.exe` | `0.7.14.0` | `Valid`、SHA-256、RFC 3161 | 688,712 byte | `15613A4846BB33FC3C33605C2B6F6601F92DF4C84CB42B7C3B3A187AF91B0ABC` |
| `ZRdpSvnAgent.exe` | `0.7.14.0` | `Valid`、SHA-256、RFC 3161 | 426,568 byte | `E8E245EAC803D16FDE8EBA2040A205F808169981B096834F97836F5DA2DB61A2` |

Certificate identityやtoken識別情報はこの表へ記載しない。

## 6. 社内配布物

必須配布物:

```text
Client/
└─ ZRdpSvnBridge.exe

Agent/
├─ ZRdpSvnAgent.exe
└─ 組織別に管理されたagent.ini

Documents/
├─ ZRdp_SVN_Bridge_利用方法_0.7.14.docx
├─ ZRdp_SVN_Bridge_設計書_0.7.14.docx
├─ release-notes-0.7.14.md
└─ known-limitations.md
```

`agent.ini`は共通binary packageへ混在させず、許可された対象PC／利用者へだけ別管理で
提供する。`client.ini`はbase port変更が必要な環境だけへ提供する。

配布禁止:

- PDB、OBJ、LIB、build directory
- Log、dump、packet capture、screen capture
- 実環境URL一覧、credential、RDP file
- USB token情報、certificate秘密情報、PIN／PUK
- 未署名または署名検証に失敗したEXE

## 7. Release Gate

- [ ] Documentation Gateのユーザー受入を得る。
- [x] Authenticode署名の技術検証と署名後smoke testを完了する。
- [ ] 組織固有の署名規定について承認者の最終確認を得る。
- [ ] 社内配布packageのfile一覧とhashを確認する。
- [ ] 社内試行利用者へ既知の制約と報告方法を案内する。
- [ ] Private／社内限定の配布先だけを使用する。
- [ ] Public Release、公開Package、公開download pageを作成しない。

Git tag、GitHub Release、asset uploadは、本書のGate完了とユーザーの別途明示指示が
あるまで実施しない。
