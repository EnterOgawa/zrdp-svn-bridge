# Phase 2 ローカル検証記録

最終更新: 2026-07-30 21:27:13（Asia/Tokyo）

## 1. 対象

Phase 1のversion 1 frame headerを維持し、transport非依存の
`zrdp::protocol::StreamMultiplexer`を追加した。Socket、DNS、HTTP Proxy、
実SVN接続は使用していない。

- Client-only OPEN
- IPv4 4 byte + TCP port 2 byteの固定OPEN payload
- 最大32 stream
- stream初期window 256 KiB
- connection window最大4 MiB
- outbound 4,096 frame／4 MiB
- inbound 4,096 event／4 MiB
- DATA方向別sequence
- WINDOW_UPDATEによるstream／connection credit回復
- HALF_CLOSE、normal close、timeout、peer reset、crossed close
- 固定codeだけのOPEN_ERROR／CLOSE／PROTOCOL_ERROR

## 2. Build／CTest

Visual Studio 2022、Windows SDK 10.0.26100.0、x64、C++20、`/W4`、`/WX`で
Debug／Releaseの全targetをbuildし、warning 0で完了した。

Debug／ReleaseともCTestは14/14成功した。追加testは次の2件である。

- `protocol.multiplexer`
  - capability、OPEN state、duplicate ID／sequence、invalid transition
  - stream／connection window exhaustionとrecovery
  - credit inflation、outbound／inbound queue上限
  - OPEN_ERROR、HALF_CLOSE、timeout、peer reset、crossed close
  - session resetとAgent起点OPEN拒否
- `integration.phase2_multiplexer`
  - 文書用IPv4 addressだけを使った3本のfake Socket
  - Client -> Agent合計1,290,335 byte
  - Agent -> Client合計1,126,515 byte
  - 全streamの双方向byte完全一致
  - 双方向HALF_CLOSEとnormal CLOSE

## 3. Security結果

- endpointはOpenTarget eventへ所有値として渡すだけでSocketを作成しない。
- queue／credit不足時に無制限bufferへfallbackしない。
- peerのmalformed state、credit水増し、上限超過はfail-closedにする。
- protocol error payloadへendpoint、OS error文字列、DATA内容を含めない。
- Phase 1 Echo、Agent再起動、RDP ActiveXの既存testを含む全CTestが回帰成功した。

## 4. Release成果物

Phase 2 protocol libraryと同じsource stateから生成した最新Release buildは
次のとおりである。runtime UIはPhase 1 Echo範囲を維持し、multiplexerのSocket
adapterとupstream Socketはまだ使用しない。

| File | Size | Version | SHA-256 | 署名 |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 537,600 byte | `0.2.1.0` | `BB56D2038D5DA8E186173E3EB64318FD8C15268DE8FF432E26B1574ED74A6A06` | 未署名 |
| `out/ZRdpSvnAgent.exe` | 371,200 byte | `0.2.1.0` | `208F8FB7A1277509B9F50775B97D773B63A75DBA7B2F2B8920156F16E0CFAB21` | 未署名 |

installer、remote upload、Release assetは作成していない。

## 5. 残作業

- Phase 3でallowlist完全一致parserとMock Server向けSocket adapterを実装

Phase 2はローカルGateであり、追加の実機RDP testは不要である。Phase 3でも
実取引先SVNへは接続しない。
