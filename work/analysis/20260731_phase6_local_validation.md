# Phase 6 Hardening ローカル検証結果

- 検証日: 2026-07-31
- 対象version: `0.6.0.0`
- 対象platform: Windows x64

## 1. 検証範囲

実RDP、支給PC、Zscaler、実SVNへ接続せず、架空endpoint、fake Static Virtual
Channel、loopback Mock HTTP／WebDAV Serverだけを使用した。

主な追加境界:

- Ready中5秒間隔のAgent health PINGと15秒応答期限
- Agent無応答後の旧Proxy stream破棄と同一RDP内の新HELLO
- timeout前sessionの遅延PONG／DATAを新HELLO_ACKまで破棄
- 初回HTTP headerの30秒絶対期限
- local／upstream connectionの30分完全無通信期限
- Agent crash／restart、Client restart、RDP reconnect相当の反復
- allowlist変更の全体検証と新Agentへの切替

wire protocol version、frame layout、feature bit、Static Virtual Channel名は変更して
いない。

## 2. BuildとCTest

実行command:

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug --parallel
ctest --preset debug --output-on-failure
cmake --build --preset release --parallel
ctest --preset release --output-on-failure
```

結果:

- Debug: warning 0、24/24成功
- Release: warning 0、24/24成功
- `integration.phase6_hardening`を新規登録
- x64、C++20、Unicode、`/MT`、version resource、icon resource契約を含む

反復結果:

- Phase 4／5／6 integration: Debug各20/20、Release各20/20
- `protocol.client_bridge`: Debug 20/20、Release 20/20
- malformed／resource／timeout主要6 test:
  - `protocol.frame`
  - `protocol.multiplexer`
  - `proxy.http_request_parser`
  - `proxy.client_worker`
  - `agent.allowlist`
  - `agent.stream_relay`
  - Debug各20/20、Release各20/20

## 3. 異常系結果

- health PING／PONGを一sessionで64回連続処理した。
- Agent crash相当の未応答後に新HELLOを生成し、新AgentとReadyへ戻った。
- timeout前に生成された旧PONGと旧stream DATAをHELLO_ACK前に破棄した。
- Client process再起動相当の新HELLOでAgent側sequenceを初期化した。
- RDP disconnect／reconnect相当を32回反復し、decoder、sequence、queueを
  新sessionへ再利用しなかった。
- 一byteだけ送信したslow headerを絶対期限で閉じた。
- OPEN後のlocal idleで`CLOSE(Timeout)`を生成し、local Socketを閉じた。
- 接続済みupstream idleで`CLOSE(Timeout)`を生成し、Socketとstreamを解放した。
- 不正allowlist reloadは旧検証済み設定を維持し、新Agentだけへ新設定を適用した。

## 4. Security boundary監査

自動testおよびsource監査結果:

- production Agentのinbound Socket／DNS API呼出し: 0
- production Client Proxyの`bind`呼出し: 1
- bind address: `htonl(INADDR_LOOPBACK)`固定
- Windows TCP owner table上の対象listener: IPv4 loopback一件
- HTTP `CONNECT`拒否境界: 1
- 必須`.gitignore` pattern欠落: 0
- source／test／文書／設定例の予期しない有効IPv4 endpoint token: 0
- Release EXE内のAuthorization、Proxy-Authorization、Cookie、password、
  文書用endpoint、SVN path audit文字列: 0
- 診断log testで`bridge.peer_timeout`を確認し、任意endpoint／認証文字列の
  混入がないことを確認した。

workspaceはGit repositoryとして未初期化のため、Git index／history監査は対象外で
ある。`logs/`、dump、capture、実設定の除外規則は`.gitignore`に存在する。

## 5. Portable成果物

| File | Size | Version | SHA-256 | Authenticode |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 644,096 byte | `0.6.0.0` | `1B30666A2DB96720C0A4BFAFE00D14BC13B0F50572A280B2BFB7F4D09A762C6A` | NotSigned |
| `out/ZRdpSvnAgent.exe` | 423,424 byte | `0.6.0.0` | `430580277B60822498178568CB39EBFD0F03CC86546AF16489274B474C413412` | NotSigned |

両EXE:

- PE x64
- Dynamic Base（ASLR）
- NX compatible（DEP）
- Control Flow Guard
- 静的CRTで、製品固有DLLまたは外部runtime dependencyなし

Client依存:

- `COMCTL32.dll`
- `GDI32.dll`
- `ole32.dll`
- `SHELL32.dll`
- `USER32.dll`
- `WS2_32.dll`
- `CRYPT32.dll`
- `OLEAUT32.dll`
- `KERNEL32.dll`

Agent依存:

- `WTSAPI32.dll`
- `USER32.dll`
- `WS2_32.dll`
- `ole32.dll`
- `KERNEL32.dll`

未署名のため署名済みRelease Gateは通過していない。

## 6. 残存Gate

ローカルPhase 6は完了した。次はVersion `0.6.0.0`を実RDPで使用し、実SVNへ
接続せず次を回帰確認する。

1. 通常のRDP画面、入力、全画面、clipboard
2. Agent強制終了後、最大約20秒でClientがAgent待機へ戻る
3. timeout中もRDP画面を操作できる
4. 同じRDP内でAgentを再起動し、ReadyとEcho成功へ戻る
5. RDP切断／再接続後にAgentを起動し直し、新しいEchoが成功する

Phase 7の実SVN read-only操作は、この回帰結果とユーザーの明示承認を受けるまで
開始しない。既知制約と残存riskは`docs/known-limitations.md`に記録した。
