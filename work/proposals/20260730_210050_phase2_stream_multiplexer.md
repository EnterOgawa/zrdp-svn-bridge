# Phase 2 Application Protocol／多重化 実装提案

作成日時: 2026-07-30 21:00:50（Asia/Tokyo）

## 1. 目的と範囲

Phase 1の実RDP Static Virtual Channel Echo Gate通過を受け、既存のversion 1
frame headerを変更せず、複数のHTTP接続を一つの`ESVNBRG`上で識別・制御する
transport非依存の`StreamMultiplexer`を実装する。

Phase 2はin-memory／fake Socketで完結させる。Winsock、allowlist、localhost
listen、HTTP解析、実SVN接続は追加しない。Phase 1のRDP画面、Agent再起動、
binary Echo経路は回帰対象として維持する。

## 2. 変更対象

| 対象 | 主な型／機能 | 役割 |
|---|---|---|
| `src/protocol/frame.h/.cpp` | `WINDOW_UPDATE` routing規則 | stream単位とconnection全体のcredit更新を識別する |
| `src/protocol/hello.h/.cpp` | stream featureとPhase 2 capability | 最大stream数、initial window、frame上限を交渉する |
| `src/protocol/multiplexer.h/.cpp` | `StreamMultiplexer` | stream lifecycle、flow control、bounded queue、eventを所有する |
| `tests/protocol/multiplexer_test.cpp` | 状態／異常系test | duplicate、invalid transition、window、queue上限を検証する |
| `tests/integration/phase2_multiplexer_test.cpp` | fake Socket結合test | 複数streamの双方向dataとcloseをin-memoryで検証する |
| `docs/protocol.md` | wire payloadと状態遷移 | Phase 2 protocolを正本化する |
| `docs/architecture.md`等 | Phase境界 | network未実装であることと次Phaseへの接続点を明記する |

## 3. Protocol設計

- HELLOへ`MultiplexedStreams` feature bitを追加する。
- Phase 2のlocal上限は最大32 stream、stream初期window 256 KiB、
  frame payload最大64 KiBとする。
- connection初期windowは
  `initial window × maximum streams`を4 MiBで上限化して導出する。
- `OPEN` payloadはIPv4 4 byteとTCP port 2 byteの固定6 byteとする。
  host name、DNS名、CIDR、port rangeは表現しない。
- stream IDはClientだけが非0値を単調増加で割り当て、session内で再利用しない。
- `DATA` sequenceは方向ごとに1から単調増加させ、欠落、重複、順序違反を拒否する。
- `WINDOW_UPDATE` payloadは非0の32-bit credit差分とする。非0 stream IDは
  stream credit、stream ID 0はconnection全体creditを更新する。
- `HALF_CLOSE`は送信方向だけを終了する。
- stream `CLOSE`は理由codeを持つrequestと空payloadのackを区別し、
  crossed closeも有限状態で処理する。
- `OPEN_ERROR`、stream `CLOSE`、`PROTOCOL_ERROR`は固定数値codeだけを送り、
  error文字列、endpoint、payloadを診断logへ渡さない。

## 4. 並行処理と所有権

`StreamMultiplexer`自身は一つのowner threadから呼ぶpure state machineとする。
受信`Frame`、送信`Frame`、公開eventは所有bufferで受け渡す。SocketやCOM methodを
class内から呼ばないため、Phase 3／4でAgent I/O workerまたはClient Bridge workerへ
配置できる。

受信dataはeventとしてcallerへ渡した後も、callerが
`ConsumeReceivedData`を呼ぶまでcreditを返さない。これによりSocket書込みが
詰まった場合にRDP側の送信も停止する。

## 5. Security boundary

- Agent起点の`OPEN`を禁止し、Clientだけがstreamを開始する。
- stream数、frame payload、stream window、connection window、送信frame queue、
  受信event queueをすべて有限値にする。
- peerが未送信byteを超える`WINDOW_UPDATE`を送ってcreditを水増しした場合は
  fail-closedにする。
- duplicate stream ID、未知stream、invalid state、sequence違反、window超過、
  不正payload長は該当multiplexer sessionを停止する。
- endpointはallowlist判定前の不可信入力としてeventへ渡すだけとし、Phase 2では
  Socketを作成しない。
- password、HTTP header/body、SVN data、実endpointをsource、test、docs、logへ
  記録しない。testでは文書用IPv4 addressだけを使用する。

## 6. ActiveX／Agent／互換性への影響

- RDP ActiveX、COM STA、BSTR変換、WTS channel APIは変更しない。
- version 1 headerと既存message type番号は変更しない。
- Phase 1だけのpeerとは共通featureがEchoだけになり、stream上限とwindowを0として
  downgradeする。互換性のない値をstream対応として扱わない。
- Phase 2 engineはtransport非依存であり、実行EXEのnetwork surfaceを増やさない。
- DVC、追加DLL、外部library、registry、Firewall、Zscaler設定は変更しない。

## 7. 失敗時動作

- local queueまたはcredit不足は`WouldBlock`として返し、無制限bufferへfallbackしない。
- peer由来のprotocol違反は既存data／streamを破棄し、可能な場合だけ固定codeの
  `PROTOCOL_ERROR`を一件queueする。
- allocation失敗はpartial stateを公開せずmultiplexerを停止する。
- RDP再接続相当の`Reset`では全stream ID、sequence、credit、queue、eventを破棄する。
- Phase 2の失敗からRDP画面を切断する要求は生成しない。

## 8. Test

- capability有無と上限交渉
- OPEN encode／decode、Client-only開始、duplicate ID、最大stream数
- OPEN_OK／OPEN_ERRORとinvalid transition
- 双方向DATA sequence、unknown stream、空／oversized payload
- stream／connection window exhaustionと`WINDOW_UPDATE`による回復
- credit水増し、整数overflow、送受信queue上限
- local／remote／双方HALF_CLOSE
- normal close、crossed close、timeout、peer reset
- `PROTOCOL_ERROR`送受信とfail-closed
- session reset後に旧streamを再利用しないこと
- fake Socketを用いた複数streamの独立した双方向byte一致
- Phase 1 Echo、Debug／Release全CTestの回帰

## 9. Rollback

新規`multiplexer` source／testとstream feature定義を除去し、
`WINDOW_UPDATE`のconnection-level許可を元へ戻す。既存Phase 1 Client、
Agent、RDP ActiveX、Echo、Agent再起動処理にはnetwork／OS設定変更がないため、
OS側rollbackは不要である。
