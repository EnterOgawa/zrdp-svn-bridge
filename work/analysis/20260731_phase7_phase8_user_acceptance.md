# Phase 7／8 実環境ユーザー受入記録

- 確認日: 2026-07-31
- 対象version: `0.6.0.0`
- 対象platform: Windows x64

## 1. 判定

ユーザーが許可した実環境でZRdp Client、RDP Static Virtual Channel、Agent、
Zscaler経由のHTTP SVN接続が成立した。ユーザーから、read-onlyおよびCommit等を
含む全対象操作を問題なしとして扱う明示確認を得た。

- Phase 6 実機回帰: Gate通過
- Phase 7 Read-only: Gate通過
- Phase 8 Write: Gate通過
- Release Gate: 未署名のため未通過

問題が後日判明した場合は、該当操作を改善項目として再オープンする。

## 2. ユーザー受入対象

### Read-only

- Repository Explorer
- Log
- Diff／Compare
- Checkout
- Update

### Write

- Commit
- Add
- Delete
- Move
- conflict
- Lock
- Unlock

実endpoint、URL、repository、path、ユーザー名、source、diff、credentialは本記録へ
記載しない。

## 3. 診断log確認

確認対象:

- `out/logs/ZRdpSvnBridge.log`
- `out/logs/ZRdpSvnAgent.log`

確認結果:

- Client 41 record、Agent 15 record
- Client／AgentともERROR event 0件
- 未知event、未知field、不正数値、不正session ID 0件
- URL、IP、Authorization、Cookie、password、SVN pathの検出 0件
- Agent allowlist採用、Winsock初期化、stream relay有効化を確認
- Client ProxyのBridge有効化を確認
- Agent強制終了を`bridge.peer_timeout`で検出
- timeout時にRDP接続を維持し、Proxyだけを無効化
- Agent再起動後にBridge ReadyとProxy有効へ復帰
- 最終RDP切断とClient／Agent正常終了を確認

通常logはHTTP methodや操作対象を記録しない。このため個別SVN操作の成否は
ユーザー受入結果、通信経路と復旧の健全性は固定診断logを根拠とする。

## 4. Security boundary

- Zscaler、Firewall、EDR、GPO、routing、OS全体ProxyをZRdpから変更していない。
- Client listenerはloopback限定である。
- Agentは検証済みallowlist完全一致後のoutbound Socketだけを作成する。
- RDP以外の新しい開発PC・支給PC間listenerを作成していない。
- credential、HTTP／SVN payload、実接続情報を通常logへ記録していない。

## 5. 次段階

Version `0.6.0.0`を改善フェーズのbaselineとする。以後はユーザー要望を一件ずつ
実装、検証、受入し、すべての改善完了後に最終仕様書と利用手順書を作成する。
