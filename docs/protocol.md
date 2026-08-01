# ESVNBRG Application Protocol

更新日: 2026-07-30

## 1. 基本規則

- Static Virtual Channel名は7文字の`ESVNBRG`とする。
- 現在のprotocol versionは`1`である。
- wire上の複数byte整数はすべてbig-endianとする。
- C++構造体のcastや`#pragma pack`でwire dataを解釈しない。
- 一回のSVC read／writeは最大1600 byteであり、frame境界とはみなさない。
- decoderはfragmented frameと一回の入力に連結された複数frameを扱う。
- malformed data後にbyte境界を探索して再同期せず、Bridge sessionを停止する。

## 2. Frame header

固定headerは24 byteである。

| Offset | Size | Field | 規則 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `ESVN` |
| 4 | 2 | version | `1` |
| 6 | 2 | header size | `24` |
| 8 | 2 | message type | 下表の固定値 |
| 10 | 2 | flags | version 1では`0` |
| 12 | 4 | streamId | connection controlでは`0` |
| 16 | 4 | sequence | messageごとの順序／request-ack値 |
| 20 | 4 | payload length | 最大65,536 byte |

一つのdecoderが保持できる入力は最大1 MiBである。magic、version、header size、
type、flags、streamId、payload lengthのいずれかが不正な場合はfail-closedとする。

## 3. Message type

| Value | Type | 用途 |
|---:|---|---|
| 1 | `HELLO` | connection capability開始 |
| 2 | `HELLO_ACK` | connection capability応答 |
| 3 | `OPEN` | Clientからのstream開始 |
| 4 | `OPEN_OK` | Agentのstream受理 |
| 5 | `OPEN_ERROR` | Agentのstream拒否 |
| 6 | `DATA` | stream binary data |
| 7 | `WINDOW_UPDATE` | stream／connection credit回復 |
| 8 | `HALF_CLOSE` | 送信方向終了 |
| 9 | `CLOSE` | stream終了またはAgent session終了 |
| 10 | `PING` | Phase 1 EchoまたはAgent死活確認要求 |
| 11 | `PONG` | Echoまたは死活確認応答 |
| 12 | `PROTOCOL_ERROR` | connection-level protocol停止通知 |

connection-level `CLOSE`は`streamId=0`、`sequence=0`、空payloadに固定する。
stream-level `CLOSE`は非0 `streamId`を使用する。`WINDOW_UPDATE`も非0
`streamId`では一stream、`streamId=0`ではconnection全体を表す。それ以外の
stream messageは非0 `streamId`、その他のconnection controlは`streamId=0`に
固定する。Clientだけが`OPEN`を開始し、Agentから開発PCへの逆方向接続開始は
できない。

## 4. HELLO payload

`HELLO`と`HELLO_ACK`は同じ20-byte payloadを使う。

| Offset | Size | Field | Phase 1 | Protocol core上限 | Phase 5 production |
|---:|---:|---|---:|---:|---:|
| 0 | 4 | feature bits | Echo=`0x00000001` | Echo + Streams=`0x00000003` | Echo + Streams=`0x00000003` |
| 4 | 4 | maximum frame payload | 65,536 | 65,536 | 65,536 |
| 8 | 4 | maximum streams | 0 | 32 | 8 |
| 12 | 4 | initial window bytes | 0 | 262,144 | 262,144 |
| 16 | 4 | transport chunk bytes | 1,600 | 1,600 | 1,600 |

両端は共通featureを取り、各上限には小さい側を採用する。Streams featureを
共通に持たない場合は、maximum streamsとinitial windowを必ず0へ戻す。
Streams featureがあるのにいずれかが0、またはfeatureがないのに非0である
capabilityは拒否する。Echo featureがない、不正な絶対上限、異なるprotocol
versionの場合も暗黙に近似解釈せず非互換として停止する。

## 5. Phase 1 state

```text
Client                              Agent
WaitingForAgent                     WaitingForHello
       HELLO ---------------------------->
             <------------------ HELLO_ACK
Ready                               Ready
       PING(sequence=N, bytes) ---------->
             <----- PONG(sequence=N, bytes)
EchoPassed                          Ready
       <------------------------ CLOSE
WaitingForAgent
       HELLO ---------------------------->
             <------------------ HELLO_ACK
Ready                               Ready
```

- HELLO系の`streamId`と`sequence`は0である。
- PING／PONGの`streamId`は0、sequenceは1から単調増加する。
- Clientは一度に一つのPINGだけを未応答として保持する。
- Ready／EchoPassed中は5秒間隔で固定8-byte payloadのhealth PINGを送る。
- health PINGが15秒無応答なら旧stream、decoder、queue、sequenceを破棄し、
  同じRDP channel上で新しいHELLOを開始する。
- Agentは直前より大きい連続sequenceだけを受理する。
- HELLOはPhase 1 Echo sequenceの新しい境界とし、Agentは直前sequenceを0へ戻す。
- PONGはPINGと同じsequence、payload長、全byteを持たなければならない。
- 15秒以内に完全なPONGを確認できない場合、ClientはBridgeだけを停止する。
- ClientはAgent待機中だけ1秒間隔でHELLOを再送する。handshake成立後のHELLOは
  Agentのstream stateをresetするため周期送信しない。
- AgentはCtrl+Cによる正常終了前にconnection-level CLOSEを送信し、Clientは
  Echo stateと旧sequenceを破棄して直ちにHELLO待機へ戻る。
- timeout直前の旧PONGは新HELLO_ACKまで破棄し、新sessionの応答へ再利用しない。
- 手動Echo中はhealth PINGを重ねず、Echo用HELLOでsequence境界を初期化する。
- RDP再接続後はdecoder、sequence、queue、Echo counterをすべて初期化する。

## 6. Phase 2 stream protocol

### 6.1 OPEN

`OPEN` payloadは次の固定6 byteであり、host name、DNS名、CIDR、port rangeを
表現しない。

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | IPv4 octet |
| 4 | 2 | TCP port、big-endian、非0 |

Clientはstream IDを1から単調増加で割り当て、同じBridge session内で再利用しない。
Agentは重複または過去のID、交渉済み最大数を超えるOPENをfail-closedで拒否する。
`StreamMultiplexer`はAgent callerへ`OpenRequested` eventを渡す。Phase 3の
`AgentStreamRelay`はallowlist完全一致後にだけnon-blocking connectを開始し、
接続完了後に`OPEN_OK`、policy拒否／timeout／接続失敗時に`OPEN_ERROR`を返す。
Phase 3 Agentのlocal `maximum streams`は1である。

`OPEN`、`OPEN_OK`、`OPEN_ERROR`のsequenceは0である。`OPEN_OK`は空payload、
`OPEN_ERROR`は次の4-byte固定codeだけを持つ。

| Code | 意味 |
|---:|---|
| 1 | policy拒否 |
| 2 | connect失敗 |
| 3 | timeout |
| 4 | resource不足 |

OS error文字列、endpoint、HTTP dataはerror payloadへ含めない。

### 6.2 DATAとsequence

- `DATA`は非空かつ交渉済みmaximum frame payload以下とする。
- sequenceは方向ごとに1から単調増加し、0、欠落、重複、順序入替を拒否する。
- `OPEN_OK`受信前、local HALF_CLOSE後、peer HALF_CLOSE受信後の該当方向へ
  DATAを送らない。
- 受信payloadは所有eventへmoveし、文字列化、内容検査、log出力を行わない。

### 6.3 Flow control

各方向は一stream当たり`initial window bytes`のcreditを持つ。connection全体の
初期creditは次で導出し、64-bitで計算する。

```text
min(initial window bytes × maximum streams, 4 MiB)
```

DATA送信時はstreamとconnection両方のcreditを減算する。一方でも不足する場合は
queueを増やさず`WouldBlock`を返す。受信側callerがdownstreamへ実際に処理した
byte数だけ、次の2 frameをqueueする。

- `WINDOW_UPDATE(streamId=N, payload=delta)`
- `WINDOW_UPDATE(streamId=0, payload=delta)`

payloadはbig-endian 32-bitの非0差分、sequenceは0である。peerが実際の未確認
送信byteを超える差分を返した場合はcredit水増しとしてfail-closedにする。

### 6.4 HALF_CLOSEとCLOSE

`HALF_CLOSE`はsequence 0、空payloadで、送信方向だけを終了する。反対方向の
DATAは継続でき、両端のHALF_CLOSE後は`BothHalfClosed`となる。

stream `CLOSE` requestはsequence 0と4-byte固定reasonを持つ。ackはsequence 1、
空payloadである。

| Reason | 意味 |
|---:|---|
| 0 | normal |
| 1 | timeout |
| 2 | peer reset |
| 3 | local error |
| 4 | session stopping |

双方のrequestがcrossした場合も各requestへ一度だけackし、ack受信後にrecordを
除去する。terminal close時はそのstreamの未取得DATAを破棄し、connection creditを
他streamへ戻す。

### 6.5 PROTOCOL_ERRORと有限資源

semantic protocol違反時は既存stream、event、送信dataを破棄し、可能な場合だけ
`PROTOCOL_ERROR(streamId=0, sequence=0, payload=4-byte code)`を一件送る。
peerからPROTOCOL_ERRORを受信した場合は応答loopを作らず停止する。

- 同時stream: negotiated maximum、Phase 2 local既定32
- Phase 3 Agent runtimeの同時stream: 1
- outbound frame queue: 最大4,096件、header込み4 MiB
- inbound event queue: 最大4,096件、DATA payload 4 MiB
- stream初期window: Phase 2 local既定256 KiB
- connection window: 最大4 MiB

local queue／credit不足は非fatalな`WouldBlock`とし、peer由来の上限超過、
invalid state、unknown stream、sequence違反、credit水増しはmultiplexer sessionを
fail-closedにする。`StartSession`／`StopSession`は旧stream ID、sequence、credit、
queue、eventをすべて破棄する。

## 7. Echo検証

論理targetは次の順であり、大きいtargetは最大32 KiBの複数PINGへ分割する。

| Target | Byte |
|---|---:|
| 1 byte | 1 |
| 1 KiB | 1,024 |
| 64 KiB | 65,536 |
| 1 MiB | 1,048,576 |
| 10 MiB | 10,485,760 |
| 合計 | 11,600,897 |

payloadはoffsetから再生成可能なbinary patternであり、埋込みNULを含む。
Clientは各PONGを全byte比較するが、payload内容をUI、console、診断logへ出力しない。

## 8. Transport有限資源と失敗動作

- Client SVC送信queue: 最大4,096 chunk、合計5 MiB
- Agent送信queue: 最大256 chunk、合計256 KiB
- Client UI／Proxy worker Frame queue: 各最大4,096件、合計4 MiB
- Client local／upstream未送信queue: 方向ごとに256 KiB
- SVC chunk: 1～1600 byte
- frame payload: 最大64 KiB
- decoder入力: 最大1 MiB
- local HTTP初回header期限: 接続受付から30秒
- local／upstream完全無通信期限: 30分

encode、decode、allocation、queue、sequence、payload、timeout、transportの異常は
該当Bridge sessionを停止する。RDP画面接続は可能な限り維持し、旧queueや
旧sequenceを次のRDP sessionへ再利用しない。独自圧縮、暗号化、再送、途中再開は
実装しない。
