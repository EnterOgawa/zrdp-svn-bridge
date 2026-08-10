# R-009 Agent channel自動回復 署名検証記録

日時: 2026-08-10

## 対象

- Version: `0.7.15.0`
- Client: `out/ZRdpSvnBridge.exe`
- Agent: `out/ZRdpSvnAgent.exe`
- 変更目的: 一時RDP transport断後、同じRDP user sessionが存続する間のAgent channel再確立

## 標準Release Gate

- Release build: 成功
- Release CTest: 30／30成功
- Client／Agent: Windows x64 portable binary契約成功
- File／Product Version: 両方とも`0.7.15.0`

### 署名前成果物

| File | size | Authenticode | SHA-256 |
|---|---:|---|---|
| `ZRdpSvnBridge.exe` | 677,888 byte | `NotSigned` | `712936231CC8A9A5756ABFBAE9CD558D8071DEB5AD899DCC8135E3BFDEB1E341` |
| `ZRdpSvnAgent.exe` | 416,768 byte | `NotSigned` | `FA58F97660E4002891D0B88505D7078F865B2010A9DF6C04BAA68D587D6471E0` |

署名前正本はGit管理外の
`work/tmp/release-unsigned-agent-recovery-0.7.15/`へ保全した。

## Authenticode署名

ユーザーの明示指示により、Client／Agentを既存Version `0.7.14.0`配布物と同一署名者で
署名した。PIN、token識別情報、certificate識別情報はrepositoryへ記録していない。

- Windows trust: 両方とも`Valid`
- SignTool verify: 両方とも成功
- File digest: SHA-256
- Timestamp: GlobalSign RFC 3161、検証成功
- 既存配布物と同一署名者: 確認済み
- 署名後resource／portable binary contract: 2／2成功

### 署名後成果物

| File | size | Version | SHA-256 |
|---|---:|---|---|
| `ZRdpSvnBridge.exe` | 690,248 byte | `0.7.15.0` | `090D937A0AB183AAC37BFF436AFCF44A7940497F2D505947212A69E1D2C1DBB9` |
| `ZRdpSvnAgent.exe` | 429,128 byte | `0.7.15.0` | `9512FA46928604D442DCFE6BCD9901C3CDFF940209D1616DF60BF702599B3A51` |

署名後にEXEの再build、resource更新、binary patchは行っていない。

## 残る実機Gate

- 一時network link断中にAgent processが終了しないこと
- Microsoft RDP Controlの自動再接続後、Agent再起動なしでBridge Readyへ戻ること
- 切断時の旧SVN／Trac操作を自動再送せず、新しい操作だけが成功すること
- 別RDP session Windowへ影響しないこと

この実機Gateを通過するまでは、Version `0.7.15.0`を署名済み試験候補として扱う。
