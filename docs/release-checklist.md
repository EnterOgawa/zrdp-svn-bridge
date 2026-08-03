# ZRdp SVN Bridge 社内配布・署名確認書

- 文書ID: `ZRDP-REL-001`
- 対象Version: `0.7.14.0`
- 更新日: 2026-08-03

## 1. 配布候補

| File | Version | 未署名size | 未署名SHA-256 |
|---|---|---:|---|
| `ZRdpSvnBridge.exe` | `0.7.14.0` | 676,352 byte | `1EF4BB651ACC7B3D451FBBB6801A74DE17D4C01A7B563850B3F95E78A8450850` |
| `ZRdpSvnAgent.exe` | `0.7.14.0` | 414,208 byte | `7F51EA61B71211DA4491B1EA0A90EF079C0BA3A4EC40C5F25A2DC0323B35A427` |

このhashは署名前のsource-derived成果物を識別するための値である。Authenticode署名後は
file内容とSHA-256が変化するため、署名済み配布物のhashを別途記録する。

R-005 Clientの署名前正本はGit管理外の
`work/tmp/release-unsigned-r005-0.7.14/`へ同じSHA-256で保全済みである。既存Agentの
署名前正本は`work/tmp/release-unsigned-final-0.7.14/`に維持する。これらのdirectoryは
rollbackと署名前比較だけに使用し、社内配布物へ含めない。

### R-008署名前受入成果物

R-006回帰撤回版で従来接続先が回復したことを確認した後、ActiveX既定の認証dialog
親Windowを使用するR-007 Clientで問題接続先と従来接続先の接続を確認した。続いて、
certificate警告拒否後の重複errorを抑止するR-008 Clientを標準`out`へ生成した。
回帰撤回版ClientはGit管理外の
`work/tmp/release-r006-rollback-confirmed-0.7.14/ZRdpSvnBridge.exe`へ退避し、
rollback可能な状態を維持している。

| File | Version | 状態 | size | SHA-256 |
|---|---|---|---:|---|
| `ZRdpSvnBridge.exe` | `0.7.14.0` | R-008・未署名実機確認候補 | 677,888 byte | `6DEFEA345BEE01379E0F3FA3546FC78586F8C59FEAC6CD4ED8EB433317E1259D` |
| `ZRdpSvnAgent.exe` | `0.7.14.0` | 既存署名済み、R-008で非変更 | 426,568 byte | `E8E245EAC803D16FDE8EBA2040A205F808169981B096834F97836F5DA2DB61A2` |

R-006初回候補は設定成功後もwarningが即時dismissされた。追加したwarning dialog表示
propertyは既存接続を壊したため撤回し、撤回後Clientで既存接続の回復を確認した。
R-007はsession top-level Windowへの`UIParentWindowHandle`設定を外し、credential UIと
certificate UIをActiveX Control Windowの既定親へ委譲する。R-008は二回目のwarning後、
login前、非local、credential取消以外、`263/0`完全一致だけで追加errorを抑止する。
Debug／隔離Release各30 CTestと標準`out`のresource／portable binary contractを通過した。
「いいえ」後の実機受入を完了し、Clientは既存Agentと同じ署名者で再署名した。
署名後smoke testを完了するまでは社内配布物として扱わない。

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

Codexが署名を実行する場合は、その都度ユーザーの明示指示を必要とする。PINを取得、
保存、command lineへ指定せず、必要時はtoken middlewareの保護されたUIだけを
利用する。署名後は公開情報だけで署名状態、Version、hashを検証する。

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
- [x] 署名済みClient／Agentでresource／portable binary contractを再確認する。
- [ ] R-005署名後Clientの起動と一件のRDP接続を確認する。
- [x] 署名後のAgent起動とBridge Readyを確認する。
- [ ] R-005署名後ClientでProxy利用可と一件の安全なread-only SVN／Trac操作を
      確認する。

2026-08-01、上記3項目をユーザー環境で確認した。固定診断logでもRDPログイン完了、
Bridge Ready、Proxy有効化、process正常終了への到達を確認した。
2026-08-03、R-005 Clientの署名後にR-008まで改善を継続した。R-008の実機受入後、
Clientだけを既存Agentと同じ署名者で再署名し、Windows trust `Valid`、SHA-256 signature、
RFC 3161 timestamp、Version維持を静的検証した。R-008署名後Clientの起動、RDP、
Bridge、Proxyの実機確認は未実施である。

### 署名済み成果物記録

| File | Version | 署名状態 | 署名済みsize | 署名済みSHA-256 |
|---|---|---|---:|---|
| `ZRdpSvnBridge.exe` | `0.7.14.0` | `Valid`、SHA-256、RFC 3161 | 688,592 byte | `6743D2B9F4200F36D12B3D2891A9B92842747C48D4ECEA46F170A8C2ACC37165` |
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
- [x] Authenticode署名の技術検証を完了する。
- [ ] R-005署名後ClientのRDP／Bridge／Proxy smoke testを完了する。
- [x] R-007未署名Clientで従来接続先への回帰がないことを実機確認する。
- [x] R-007未署名Clientでcertificate警告の「はい」とLoginCompleteを実機確認する。
- [x] R-007未署名Clientでcertificate警告の「いいえ」と重複errorを実機確認する。
- [x] R-008未署名Clientで「いいえ」後に追加errorなしで対象sessionだけが閉じることを確認する。
- [x] R-008受入後にClientを再署名し、署名後hashとtimestampを記録する。
- [ ] R-008署名後Clientの起動、RDP、Bridge、Proxy smoke testを完了する。
- [ ] 組織固有の署名規定について承認者の最終確認を得る。
- [ ] 社内配布packageのfile一覧とhashを確認する。
- [ ] 社内試行利用者へ既知の制約と報告方法を案内する。
- [ ] Private／社内限定の配布先だけを使用する。
- [ ] Public Release、公開Package、公開download pageを作成しない。

Git tag、GitHub Release、asset uploadは、本書のGate完了とユーザーの別途明示指示が
あるまで実施しない。
