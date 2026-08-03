# R-008 certificate警告拒否後の重複error抑止 検証記録

## 1. 対象事象

R-007 ClientでMicrosoft標準certificate警告の「いいえ」を選ぶと、Controlが
`discReason=263`、`ExtendedDisconnectReason=0`を通知し、ZRdpが同じ失敗をRDP接続
error dialogとして重複表示した。従来接続先と警告の「はい」経路は実機で接続成功した。

## 2. 実装境界

- 一接続試行の`OnAuthenticationWarningDisplayed`回数を二回で飽和して保持する。
- warning二回以上、login未完了、非local、credential取消以外、`263/0`完全一致だけを
  certificate警告の取消候補とする。
- 一致時はretryと追加errorを行わず、対象session Windowだけを閉じる。
- 固定logへ`security_warning_cancelled=1`と数値切断reasonを記録する。
- Agent、Static Virtual Channel、Application Protocol、Proxy、allowlist、接続履歴、
  NLA、CredSSP、`AuthenticationLevel=2`、certificate検証は変更しない。

## 3. 自動検証

| Gate | 結果 |
|---|---|
| Debug CTest | 30／30成功 |
| 隔離Release CTest | 30／30成功 |
| 標準`out` resource／portable contract | 2／2成功 |
| Client Version | `0.7.14.0` |
| 署名前Client Authenticode | `NotSigned` |
| Agent SHA-256非変更 | 成功 |

pure policyは完全一致をtrueとし、warning一回、login済み、local切断、credential取消、
reason違い、extended reason違いをすべてfalseとすることを確認した。diagnostic testでは
固定fieldを出力し、接続先、certificate、credential、利用者選択文字列を追加していない。

## 4. 署名前成果物

- Client: `out/ZRdpSvnBridge.exe`
- size: 677,888 byte
- SHA-256: `6DEFEA345BEE01379E0F3FA3546FC78586F8C59FEAC6CD4ED8EB433317E1259D`
- Agent: `out/ZRdpSvnAgent.exe`
- Agent SHA-256: `E8E245EAC803D16FDE8EBA2040A205F808169981B096834F97836F5DA2DB61A2`

## 5. 実機結果

2026-08-03、ユーザーはcertificate警告で「いいえ」を選び、追加のRDP接続error
dialogなしで対象session Windowだけが閉じる期待動作を確認した。従来接続先はR-007で
回帰なく接続済みである。他の同時接続sessionと固定log fieldは未確認のため、通常の
回帰Gateとして残す。

## 6. 署名後成果物

- Client: `out/ZRdpSvnBridge.exe`
- size: 688,592 byte
- SHA-256: `6743D2B9F4200F36D12B3D2891A9B92842747C48D4ECEA46F170A8C2ACC37165`
- Authenticode: Windows trust `Valid`
- Signature digest: SHA-256
- RFC 3161 timestamp: あり
- 既存Agentと同じ署名者: 一致
- File／Product Version: `0.7.14.0`
- 署名後resource／portable binary contract: 2／2成功

未署名正本と署名済み正本はGit管理外の`work/tmp`へ別々に退避した。署名後にbuild、
resource更新、binary patchは行っていない。残るGateは署名後Clientの起動、RDP、Bridge、
Proxy smoke testである。

警告の「今後確認しない」等は選ばず、certificate store、server設定、registry、GPO、
RDP cacheを変更しない。
