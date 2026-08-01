# Phase 1 Static Virtual Channel Echo 実装提案

作成日時: 2026-07-30 17:28:27（Asia/Tokyo）

## 1. 目的

Phase 0のRDP成立Gate通過を受け、同じRDPセッション内にStatic Virtual Channel
`ESVNBRG`を常設する。ClientとAgentの双方向binary transportを確立し、
`HELLO`／`HELLO_ACK`および`PING`／`PONG`で欠落、重複、順序、byte一致を
検証できる状態までをPhase 1の範囲とする。

この段階ではlocalhost HTTP Proxy、SVN HTTP解析、upstream Socket、allowlist、
任意宛先への転送を実装しない。RDP画面だけが成立した状態をSVN利用可能とは
表示しない。

## 2. 変更対象

| 対象 | 主なclass／function | 役割 |
|---|---|---|
| `src/protocol/frame.h/.cpp` | `FrameDecoder`、`EncodeFrame` | 固定byte orderのframe encode、incremental decode、有限長検証 |
| `src/protocol/hello.h/.cpp` | `HelloCapabilities` | HELLO capabilityの明示encode／decode |
| `src/client/bridge/client_bridge.h/.cpp` | `ClientBridge` | handshake、bounded送信queue、Echo検証状態 |
| `src/client/rdp/rdp_control_host.h/.cpp` | `RdpControlHost` | Connect前の`ESVNBRG`登録、binary BSTR送信 |
| `src/client/app/main_window.h/.cpp` | `MainWindow` | COM callback外への送信marshal、Agent状態、Echo実行UI |
| `src/agent/app/main.cpp` | Agent entry point | 診断log、session検証、lifecycle |
| `src/agent/channel/wts_virtual_channel.h/.cpp` | `WtsVirtualChannel` | SVC open、bounded read、serialized write、RAII close |
| `src/agent/bridge/agent_bridge.h/.cpp` | `AgentBridge` | HELLO応答とPINGのbinary echo |
| `tests/protocol/*` | protocol test | fragmentation、複数frame、invalid input、埋込みNUL |
| `tests/agent/*` | Agent session／bridge test | state、queue上限、HELLO／PING応答 |
| `docs/protocol.md`、`docs/agent.md` | 設計文書 | wire format、session条件、失敗時動作 |
| `docs/roadmap.md`、`docs/test-plan.md` | 進捗／手順 | Phase 0受入結果とPhase 1実機Gate |

## 3. Application frame

Phase 2で拡張する最終frame headerの基礎をPhase 1から使用する。C++構造体の
castや`#pragma pack`は使用せず、全整数をnetwork byte orderで個別に
encode／decodeする。

```text
offset  size  field
0       4     magic = "ESVN"
4       2     protocol version
6       2     header size
8       2     message type
10      2     flags（Phase 1は0）
12      4     streamId（control messageは0）
16      4     sequence
20      4     payload length
```

- header sizeは24 byteとする。
- Phase 1で送受信するtypeは`HELLO`、`HELLO_ACK`、`PING`、`PONG`、
  `PROTOCOL_ERROR`に限定する。
- 最大frame payloadは64 KiB、decoderの保持量と送信queueを有限値にする。
- HELLOにはfeature bit、最大frame payload、最大stream数、initial window、
  transport chunk上限を含める。
- Echoでは一度に一つのPINGだけを未応答状態とし、PONGのsequenceとpayloadを
  完全一致で検証する。
- 1 byte、1 KiB、64 KiB、1 MiB、10 MiBは複数PINGへ分割し、決定的なbinary
  patternを使用する。patternには埋込みNULを含めるが、内容をlogへ出力しない。

## 4. ActiveX／COMへの影響

- `IMsTscAx::CreateVirtualChannels(L"ESVNBRG")`を、ActiveX instanceごとに
  最初の`Connect`より前に呼ぶ。再接続時も登録済みchannelを必須条件として
  `Connect`を行う。
- `IMsTscAx::SendOnVirtualChannel`はRDP ActiveXを所有するCOM STAのUI thread
  だけで呼ぶ。
- workerまたはCOM callback内から直接送信せず、bounded queueへ所有copyを
  入れ、専用`WM_APP` messageでcallback復帰後のUI threadへmarshalする。
- binary payload BSTRは`SysAllocStringByteLen`で作成し、`wcslen`、UTF-16変換、
  Base64変換を使用しない。
- `OnChannelReceivedData`は既存どおり`SysStringByteLen`で所有copy化し、
  channel名が`ESVNBRG`と完全一致するdataだけをBridgeへ渡す。
- Bridge登録、送信、decodeの失敗はBridgeだけを停止し、RDP画面接続は可能な
  限り維持する。

## 5. AgentとVirtual Channelへの影響

- `ZRdpSvnAgent.exe`はinstaller、service、driver、管理者権限を要求しない
  portable console EXEとする。
- Agentは自身のprocess Session IDを取得し、Session 0、非active session、
  `WTSClientProtocolType != 2`の非RDP sessionではchannelを開かず終了する。
- SVCは
  `WTSVirtualChannelOpenEx(WTS_CURRENT_SESSION, "ESVNBRG", 0)`だけで開く。
- `WTSVirtualChannelRead`と`WTSVirtualChannelWrite`はMicrosoft文書上
  thread-safeではないため、readとserialized write queueの責務を分けつつ、
  同一handleへのAPI呼出しはAgentの一つのI/O loopだけで順に行う。
- read／write chunkは`CHANNEL_CHUNK_LENGTH`以下とし、frame境界とはみなさない。
- write queueは件数と総byte数の両方に上限を持つ。
- Ctrl+C、RDP切断、channel error、protocol errorではI/O loopを停止し、
  queueを破棄してからchannel handleを閉じる。
- AgentはTCP listen、upstream Socket、DNS、Proxy、network探索を行わない。

## 6. Security boundary

- password、RDP credential、実接続先、実IP、domain、ユーザー名、HTTP data、
  SVN payloadをprotocol、Agent console、診断logへ出力しない。
- 診断logは固定event名、error code、message type、sequence、byte count、
  state transition等の有限な数値だけを記録する。
- malformed magic、version、header、type、flags、length、streamId、sequenceは
  fail-closedでBridge sessionを停止する。
- queue上限超過時に無制限bufferへfallbackしない。
- version不一致時にRDP設定やsecurity設定を弱めず、Bridgeだけを非互換状態に
  する。
- Zscaler、Windows Firewall、GPO、routing、OS Proxy、RDP security propertyを
  変更しない。

## 7. Protocol compatibility

Phase 1でversion 1のheaderとHELLO capability layoutを確定する。Phase 2では
同じheaderへstream messageとflow controlを追加し、Phase 1 type番号を変更しない。

互換versionでないpeerとはfail-closedにし、暗黙のdowngradeを行わない。
Static Virtual Channelの性能または安定性だけを理由にDVCへ変更せず、実測と
ユーザー承認が必要な既存ADR方針を維持する。

## 8. 失敗時動作

- Agent未起動: ClientはRDPを維持し、Agent待機を表示してHELLOを有限頻度で再送する。
- Agent終了／channel切断: 未応答PINGを15秒でtimeoutとしてEchoを中断し、
  Bridge停止を表示するがRDPは切断しない。
- encode／decode異常: 該当Bridge sessionをfail-closedにし、次のRDP sessionまで
  data planeを再利用しない。
- Client送信失敗: 残りqueueを破棄し、HRESULTだけを表示・記録する。
- Agent書込み失敗: writerを停止し、Win32 errorだけを表示・記録する。
- Echo不一致: sequenceまたはbyte不一致として停止し、payload値は表示・記録しない。

## 9. Test

### 自動test

- frame encode／decodeとnetwork byte order
- 1 byteずつのfragmentation
- header／payload境界の全分割位置
- 一回の受信bufferに複数frame
- invalid magic／version／type／flags／streamId
- oversized payload、length overflow、decoder buffer上限
- HELLO capability encode／decodeと不正値
- 埋込みNULを含むPING／PONG完全一致
- duplicate／out-of-order sequence拒否
- bounded送信queue上限
- Agent session判定のpure function
- Agent不在相当、version mismatch、RDP disconnect相当reset
- Debug／Release build、CTest、依存DLL、version resource、portable output

### ユーザー実機test

1. 開発PCで`ZRdpSvnBridge.exe`を起動して通常どおりRDP接続する。
2. 接続先RDP session内へ`ZRdpSvnAgent.exe`だけをコピーして起動する。
3. ClientがAgent readyを表示することを確認する。
4. ClientのEcho testを開始し、1 byte、1 KiB、64 KiB、1 MiB、10 MiBが
   すべて成功することを確認する。
5. Agent終了、再起動、RDP切断後もClientがcrashせず、RDP画面が継続することを
   確認する。

実機testでは実SVN、実URL、SVN credential、Commit、network設定を使用しない。

## 10. Rollback

- ClientのBridge controller、Agent target、protocol target、追加resourceとtestを
  削除し、`RdpControlHost`のchannel登録／送信methodを除去する。
- RDP表示、認証、履歴、clipboard、display modeのPhase 0実装は変更前のまま残す。
- installer、service、registry、Firewall、Zscaler、RDP設定を変更しないため、
  OS側のrollback作業は不要である。
- protocol version 1を外部配布後に変更する場合は、同じversion番号を再利用せず
  ADRとmigration方針を追加する。

## 11. 参照したMicrosoft仕様

- `IMsTscAx::CreateVirtualChannels`
  - channel名は1～7文字の英字で、`Connect`前に呼ぶ。
- `IMsTscAx::SendOnVirtualChannel`
  - data BSTRはNUL終端文字列である必要がない。
- `WTSVirtualChannelOpenEx`
  - flags 0でSVCを開き、`WTS_CURRENT_SESSION`を使用できる。
- `WTSVirtualChannelRead`
  - 一回に受信できる最大量は`CHANNEL_CHUNK_LENGTH`で、thread-safeではない。
- `WTSVirtualChannelWrite`
  - 同じhandleへの複数threadアクセスは直列化が必要である。
