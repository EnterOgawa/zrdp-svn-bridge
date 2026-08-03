# ログイン前接続再試行二段階化の検証

日時: 2026-08-03

## 対象

- 回帰修正: R-005
- 変更対象: `ZRdpSvnBridge.exe`のみ
- 非変更対象: Agent、Virtual Channel、Proxy、application protocol、allowlist
- retry条件: R-003の`discReason=263`、`ExtendedDisconnectReason=0`完全一致を維持
- retry間隔: 2,000 ms、5,000 ms
- 最大試行数: 初回を含む3回

## ローカル検証

- Debug `unit.session_disconnect_policy`: 1/1成功
- Debug CTest: 30/30成功
- Release CTest: 30/30成功
- `git diff --check`: 成功
- Client file version: `0.7.14.0`
- Client ProductVersion: `0.7.14.0`
- 署名前Client Authenticode状態: `NotSigned`
- 署名前Client SHA-256:
  `1EF4BB651ACC7B3D451FBBB6801A74DE17D4C01A7B563850B3F95E78A8450850`

## Authenticode署名

2026-08-03、ユーザーの明示指示によりClientだけを署名した。PIN、token識別情報、
certificate識別情報はrepositoryへ記録していない。

- Windows trust: `Valid`
- SignTool検証: 成功、warning 0、error 0
- File digest: SHA-256
- Timestamp: RFC 3161、検証成功
- 既存Agentと同一署名者: 確認済み
- 署名後size: 688,712 byte
- 署名後SHA-256:
  `C9C342418A0E321FAD01DDC329F3AA09094DAF9CC844FECC8C8AE4D534927663`
- 署名前正本:
  `work/tmp/release-unsigned-r005-0.7.14/ZRdpSvnBridge.exe`

## Agent非変更確認

作業前とClient限定Release build後の`out/ZRdpSvnAgent.exe` SHA-256は、次の値で
一致した。

`E8E245EAC803D16FDE8EBA2040A205F808169981B096834F97836F5DA2DB61A2`

Release Agent targetはbuildしていない。従って今回の実機確認は、既存Agentを
変更せず、`out/ZRdpSvnBridge.exe`だけを差し替えて実施できる。

## 実機Gate

- 通常RDP接続
- 自然再発時の有限自動復旧
- 認証UI取消時に再試行しないこと
- Agent／Proxy／SVN／Trac／複数session／表示modeの回帰なし

実機Gateを通過するまで本Clientは署名済み試験候補として扱い、社内配布Gateを
完了扱いにしない。
