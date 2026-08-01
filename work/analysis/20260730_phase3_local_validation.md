# Phase 3 Agent allowlist限定TCP ローカル検証

- 検証日時: 2026-07-30
- 対象version: `0.3.0.0`
- 対象platform: Windows x64

## 1. 検証範囲

実RDP、支給PC、Zscaler、実取引先IP、実SVN、credentialを使用せず、次だけを
ローカルで確認した。

- strict `agent.ini` parserと完全一致`Allowlist`
- fake connectorによるpolicy／Socket異常系
- `AgentBridge`から`AgentStreamRelay`へのPhase 3 frame routing
- production Winsock adapterから127.0.0.1 Mock Serverへの単一stream
- Debug／Release build、CTest、portable EXE metadata
- production Agentのinbound API／DNS非依存

## 2. Allowlist

- `config/examples/agent.ini.example`をproduction file loaderで解析した。
- IPv4とportの完全一致だけを許可した。
- addressまたはport片方だけの一致を拒否した。
- hostname、CIDR、wildcard、IPv6、port rangeを拒否した。
- strict dotted decimal、非0 port、非0匿名IDを検証した。
- duplicate ID、duplicate endpoint、不正section／key、空設定を拒否した。
- file 64 KiB、line 256 byte、entry 128件の上限を確認した。
- parser失敗時に以前の検証済みallowlistを置換しないことを確認した。
- allowlist拒否時のfake connector呼出し回数が0であることを確認した。

## 3. Socket relay

fake connectionで次を確認した。

- immediate connect failureを`OPEN_ERROR(ConnectFailed)`へ変換
- pending connectを5秒で`OPEN_ERROR(Timeout)`へ変換
- partial sendと一度の`WouldBlock`後にbinary dataを完全送信
- Socketへ全DATAを書き終えた時点だけでWINDOW_UPDATEを返却
- upstream peer resetを`CLOSE(PeerReset)`へ変換
- Client／upstreamの双方向HALF_CLOSE後にnormal CLOSE／ack
- `StopSession`でactive Socketを一度closeし、IDとbufferを破棄

## 4. Mock Server integration

integration test process内で127.0.0.1のOS割当portだけをlistenし、production
`WinsockUpstreamConnector`から接続した。Agent production sourceはlistenしない。

- 単一streamを確立
- request: 524,425 byte
- response: 524,425 byte
- request／response／Mock Server受信dataがbyte完全一致
- 256 KiB initial windowを跨ぐWINDOW_UPDATE回復
- Client FIN、server FIN、normal CLOSE、ackを確認
- test終了時にactive streamが0

## 5. Build／CTest

| Configuration | Build | CTest |
|---|---|---|
| Debug | `/W4 /WX`で成功 | 17 / 17成功 |
| Release | `/W4 /WX`で成功 | 17 / 17成功 |

Releaseの最終CTestでは全17件が成功した。
loopback Mock Server integrationはDebug／Releaseそれぞれ20回連続実行し、
合計40 / 40成功した。

## 6. Production Agent network監査

`src/agent`を静的検索し、production sourceに次の呼出しがないことを確認した。

- `bind`
- `listen`
- `accept`
- `getaddrinfo`
- `gethostbyname`
- DNS／WinHTTP／WinINet API

Release `ZRdpSvnAgent.exe`の`WS2_32.dll` import ordinalは次だけである。

```text
3   closesocket
4   connect
7   getsockopt
8   htonl
9   htons
10  ioctlsocket
16  recv
18  select
19  send
22  shutdown
23  socket
111 WSAGetLastError
115 WSAStartup
116 WSACleanup
```

inbound `accept=1`、`bind=2`、`listen=13`と名前解決APIはimportされていない。

## 7. Portable成果物

| File | Size | Version | SHA-256 | Authenticode |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 538,112 byte | `0.3.0.0` | `FC6DBFD190D469BF3DA9E12AEB9E7B9EA0E6896C02C630F6D427A407CD2AE747` | 未署名 |
| `out/ZRdpSvnAgent.exe` | 421,376 byte | `0.3.0.0` | `64FDF2E7982C11F00755A78D4203B13D0E9CA2DC6D91212958F81A14E49F7ED9` | 未署名 |

両EXEはx64、High Entropy VA、ASLR、DEP、Control Flow Guardを有効としている。
依存先はWindows system DLLだけであり、VC runtime DLLや追加製品runtimeを必要と
しない。Agentへ`WS2_32.dll`依存が追加されたが、Windows system DLLであり同梱
しない。

## 8. Gate判定

Phase 3のローカル終了条件を満たした。

- Mock Serverに限った単一streamの往復通信が成立した。
- allowlist拒否時にSocket作成や別経路fallbackを行わない。
- Agent／RDP停止相当でSocketを閉じる。
- 実取引先SVNへ接続していない。

Client runtimeはlocalhost HTTP ProxyとClient stream workerをまだ持たないため、
通常実行時はEcho-onlyへ交渉される。次はPhase 4の設計と実装であり、現時点を
「SVN透過機能完成」とは判定しない。
