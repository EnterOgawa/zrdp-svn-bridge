# Phase 3 allowlist限定TCP接続 実装提案

## 1. 目的

Phase 2で実装した`StreamMultiplexer`をAgentのI/O ownerへ接続し、Clientから
受信した`OPEN`の宛先が設定済みallowlistと完全一致した場合だけ、Agentから
Mock HTTP Serverへの非同期TCP接続を開始する。

このPhaseでは実取引先SVN、DNS、HTTP Proxy、任意TCP Forwardを使用しない。
実環境値を含む設定もrepositoryへ追加しない。

## 2. 変更対象

### 新規ファイル

- `src/agent/network/allowlist.h/.cpp`
  - UTF-8の`agent.ini`を有限長で解析する。
  - `endpoint.<匿名数値ID>=IPv4:port`だけを受け付ける。
  - IPv4 literalとTCP portの完全一致検索を公開する。
- `src/agent/network/upstream_connection.h/.cpp`
  - RAIIで`SOCKET`と`WSAStartup`を管理する。
  - DNSを使用せず、`OpenTarget`の4 octetを直接`AF_INET`へ変換する。
  - non-blocking connect／send／receive／half-closeを公開する。
- `src/agent/bridge/agent_stream_relay.h/.cpp`
  - Agent roleの`StreamMultiplexer`、単一upstream Socket、有限bufferを所有する。
  - `OPEN`、`DATA`、`WINDOW_UPDATE`、`HALF_CLOSE`、`CLOSE`をSocket状態へ対応させる。
- `config/examples/agent.ini.example`
  - RFC 5737の文書用IPv4 addressだけを使用する設定例。
- `tests/agent/allowlist_test.cpp`
  - parser、完全一致、全拒否条件を検証する。
- `tests/agent/agent_stream_relay_test.cpp`
  - fake Socketでpolicy拒否、timeout、refused、reset、session cleanupを検証する。
- `tests/integration/phase3_agent_tcp_test.cpp`
  - loopback Mock Server一台との単一stream往復、half-close、closeを検証する。

### 変更ファイル

- `src/agent/bridge/agent_bridge.h/.cpp`
  - 有効なrelayが設定された場合だけPhase 2 stream capabilityを広告する。
  - stream frameをrelayへ渡し、生成frameを既存serialized WTS writer queueへ戻す。
- `src/agent/app/main.cpp`
  - EXEと同じdirectoryの`agent.ini`を起動時に一度だけ読み込む。
  - 有効設定とWinsock初期化が揃った場合だけrelayを有効化する。
  - WTS read timeout中もnon-blocking Socketをpumpする。
- `src/agent/CMakeLists.txt`、`tests/CMakeLists.txt`
  - 新規library、Winsock依存、test targetを追加する。
- `docs/configuration.md`、`docs/agent.md`、`docs/security.md`、
  `docs/test-plan.md`、`docs/roadmap.md`
  - Phase 3の設定契約、security boundary、検証結果を反映する。

## 3. 設定形式

portable配布を維持するため、Agentは自身と同じdirectoryの`agent.ini`だけを読む。
初期版の形式は次に限定する。

```ini
[allowlist]
endpoint.1001=192.0.2.10:80
```

- fileはUTF-8 ASCII subset、最大64 KiB、最大128 endpointとする。
- sectionは`[allowlist]`だけ、keyは`endpoint.<1以上のuint32>`だけとする。
- IPv4は曖昧表現を避けるstrict dotted decimal、portは`1..65535`とする。
- hostname、CIDR、wildcard、port range、IPv6、OS Proxy設定は受け付けない。
- 匿名数値IDと`IPv4:port`の両方について重複を拒否する。
- 空設定、不明section、不明key、行長超過、余分な文字を拒否する。

実環境`agent.ini`は既存`.gitignore`対象であり、template以外をGit管理しない。

## 4. Agent Socket境界

`WinsockUpstreamConnector`はClient由来targetを直接受け取らない。
`AgentStreamRelay`が先に`Allowlist::Find`を実行し、一致した場合だけconnectorを
呼び出す。拒否時は`OPEN_ERROR(RejectedByPolicy)`を返し、Socket作成、DNS、
別port、別proxy、fallbackを一切行わない。

production adapterは次を保証する。

- outbound `AF_INET/SOCK_STREAM/IPPROTO_TCP`だけを作成する。
- `bind`、`listen`、`accept`、DNS API、OS Proxy APIを呼ばない。
- Socketは最初からnon-blockingとし、WTS owner threadを長時間停止させない。
- connect timeoutは5秒、I/Oは100 ms以下のWTS poll周期で進める。
- 一Pumpのsend／receive回数を有限にし、Virtual Channel処理をstarvationさせない。
- 単一stream、Socket書込み待ち最大256 KiB、Socket読込み待ち最大64 KiBとする。
- RDP切断、Agent終了、protocol error、timeout、peer resetでSocketを閉じる。

Phase 3のHELLOでは`maximum_streams=1`を広告する。複数streamのprotocol自体は
Phase 2で維持し、複数Socket化はPhase 5で実測とresource上限を確認して有効化する。

## 5. 失敗時動作

- `agent.ini`欠落／不正:
  - AgentはEcho-only capabilityで起動を継続する。
  - SVN streamは広告せず、RDP画面接続を維持する。
  - logには固定error分類と行番号だけを記録し、pathや入力値を記録しない。
- Winsock初期化失敗:
  - Echo-onlyへ縮退し、OS error codeだけを記録する。
- allowlist不一致:
  - `OPEN_ERROR(RejectedByPolicy)`を返し、接続処理を呼ばない。
- connect timeout／refused:
  - それぞれ`Timeout`／`ConnectFailed`を返し、別経路へfallbackしない。
- established Socketのreset／I/O error:
  - 固定`CLOSE` reasonへ変換し、streamとSocketを終了する。
- RDP切断／Agent終了:
  - relayの`StopSession`で全stream、buffer、Socketを破棄する。
- protocol／queue異常:
  - Bridgeをfail-closedへ移し、Socketを閉じる。RDP ActiveX処理には波及させない。

## 6. protocol互換性

wire type、header、byte order、payload形式はPhase 2から変更しない。
Echo-only Clientとは共通featureだけを交渉し、従来のPhase 1 Echoを維持する。
Phase 2対応Clientが接続した場合だけstream capabilityが成立する。

Agentは接続完了後にだけ`OPEN_OK`を返す。これによりClientはupstream Socketが
確立する前に`DATA`を送らない。timeoutと接続拒否は固定`OPEN_ERROR`で返す。

## 7. security boundaryへの影響

- ActiveX／COM: 変更なし。
- RDP Virtual Channel: 既存`ESVNBRG`とserialized writer queueを継続する。
- Client HTTP Proxy: Phase 4まで未実装。
- Agent Socket: allowlist完全一致後のoutbound TCPだけを新設する。
- log: 実IP、port、URL、HTTP data、credentialを渡せるAPIを追加しない。
  endpointは設定の匿名数値IDだけを診断可能値とする。

## 8. test

- allowlist正常系、大小の境界、重複、空、IPv4／port不正、wildcard等を網羅する。
- fake connectorの呼出し回数を確認し、policy拒否で0回であることを保証する。
- fake pending connectionと単調時刻でconnect timeoutを再現する。
- refused／reset／partial send／would-block／half-close／StopSessionを検証する。
- loopback Mock Serverでbinary byte一致、flow control、half-close、normal closeを確認する。
- Debug／Releaseの全CTestを`/W4 /WX`で実行する。
- production sourceに`listen`、`accept`、DNS APIがないことを静的確認する。

## 9. rollback

新規`zrdp_agent_network`と`AgentStreamRelay`、`AgentBridge`のrelay接続、mainの
設定読込みを外せば、既存のEcho-only Agentへ戻せる。protocol wire format、
RDP Client、履歴、ActiveX設定を変更しないため、設定fileや移行処理のrollbackは
不要である。
