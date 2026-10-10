# localhost HTTP Proxy

更新日: 2026-10-07

## 1. 現在の状態

ZRdp `0.7.14.0`は、Clientにloopback限定HTTP Forward ProxyとHTTP/SVN互換の
複数connection中継を実装済みである。Phase 9改善I-001では、同じHTTP境界が
Mylyn TracのWeb／XML-RPCにも適合することを確認した。改善I-002ではwire
protocolを変更せず、RDP sessionごとに独立したProxy workerとloopback portを
所有する。RDP接続と対応AgentのHELLOが成立すると、allowlist一致済みHTTP
SVN／承認済みTrac endpointへ中継する。

```text
local HTTP client
  -> 127.0.0.1:<session専用port>
  -> session専用ClientProxyWorker
  -> ClientBridge
  -> ESVNBRG
  -> AgentBridge
  -> AgentStreamRelay
  -> allowlist一致済みMock HTTP Server
```

Proxy worker、HTTP parser、Socket I/O、`StreamMultiplexer`はUI／COM thread外で
動作する。workerからActiveXへの送信は有限Frame queueとWindow Messageで
UI／COM threadへ戻し、RDP描画や入力をblocking I/Oで停止させない。

## 2. Listenと接続受付

- production bind先はcompile時に`127.0.0.1`へ固定する。
- 最初のsessionの既定portは`18080`である。EXE隣接の固定`client.ini`に
  `[proxy] port=<1..65535>`がある場合、その値をbase portとする。追加sessionは
  `base port + 未使用slot`を使用する。port `0`はloopback bind自動testだけで
  使用する。
- 各sessionがIPv4 `AF_INET` listenerを一つだけ作り、IPv6、`0.0.0.0`、LAN、Wi-Fi、
  VPN NIC、hostnameではlistenしない。
- `SO_EXCLUSIVEADDRUSE`を設定し、意図しないlistener共有を許可しない。
- 同時RDP sessionは16件に制限する。session終了時に対応port slotだけを解放し、
  他sessionのlistenerとstreamを維持する。
- 別processとのport競合では対象sessionだけを`BindFailed`とし、未使用portの探索や
  自動fallbackを行わない。
- Agentとのcompatible HELLOが未成立の場合、local接続へ固定
  `503 Service Unavailable`を返して直ちに閉じる。
- 一sessionは最大8 local connectionと8 Bridge streamを受け付ける。
  9件目以降は固定`503`で拒否して数だけを計上する。
- RDP切断、Agentのconnection-level `CLOSE`、Bridge失敗、Client終了では、
  local Socket、stream、queueを破棄する。RDP画面自体は可能な限り維持する。
- 最初の有効HTTP headerは接続受付から30秒以内に完成させる。部分受信では
  期限を延長せず、Slowloris相当の接続を対象Socketだけで終了する。
- OPEN開始後は送受信進捗が30分ないlocal connectionを
  `CLOSE(Timeout)`として終了する。

## 3. HTTP request境界

- 対象schemeはIPv4 literalをauthorityに持つ`http://`だけとする。
- port省略時はHTTP既定port `80`、明示時は`1..65535`だけを受け付ける。
- absolute-form request-targetをorigin-formへ変換する。
- origin-formと`OPTIONS *`等のasterisk-formも、`Host`から同じ固定宛先を
  確定できる場合だけ受け付ける。
- `Host`は一件必須とし、URL authorityとのIPv4とportの一致を検証する。
- 一つのincoming connectionは、最初の正常requestの`IPv4:port`へ固定する。
  Keep-Alive中の宛先変更は拒否する。
- `Proxy-Connection`、`Proxy-Authorization`、標準hop-by-hop header、および
  `Connection`で指定されたheaderを、後述の検証済みchunked framingを除いて
  upstreamへ送らない。
- `Connection`が`Host`または`Content-Length`を指名した要求は、header／bodyの
  転送開始前に固定`400`で拒否する。大小文字、comma、複数Connection行、受信分割で
  結果を変えず、指名したheaderの欠落やbody長0でも受理しない。
- end-to-end header、method、path、query、HTTP versionは必要以上に変更しない。
- `Content-Length` bodyは入力chunk単位で中継し、request全体を保持しない。
- `Transfer-Encoding`は単一の`chunked`だけを受け付け、size、extension、data
  CRLF、zero chunk、trailerをincrementalに検証して同じwire framingで中継する。
- 検証済みchunkedの`Transfer-Encoding`／`Trailer`は、Connectionで指名されていても
  次hopのframingとして維持する。長さheaderだけを落として元のbody処理を続けない。
- `Content-Length`と`Transfer-Encoding`の併用、複数coding、未知codingを
  request smuggling境界で拒否する。
- `Expect: 100-continue`、Authorization、Cookie、Depth、Destination、
  Lock-Token等のend-to-end headerは内容解析や保存をせず維持する。
- responseはopaque byte streamとして扱い、`100 Continue`、401 challenge、
  redirect、Set-Cookie、Content-Encoding、chunked responseを変更しない。
- `CONNECT`、HTTPS、Upgrade、hostname、IPv6、userinfo、fragment、
  Host欠落／重複／不一致をfail-closedで拒否する。

現在の有限上限は次のとおりである。

| 対象 | 上限 |
|---|---:|
| 一request header | 64 KiB |
| parser一回の入力 | 64 KiB |
| 一requestのdecoded body | 64 GiB |
| chunk size line | 8 KiB |
| chunk trailer block | 64 KiB |
| 一回の入力で処理するheader | 128件 |
| 最初のHTTP header完成期限 | 30秒 |
| OPEN開始後の完全無通信 | 30分 |
| 同時connection／stream | 8 |
| 一stream local→Agent未送信queue | 256 KiB |
| 一stream Agent→local未送信queue | 256 KiB |
| 各方向の全stream合計 | 2 MiB |
| UI／worker Frame queue | 各4,096件／4 MiB |

parser拒否は内容を含まない固定`400`、`405`、`431`、`501`へ変換する。
Agentのallowlist拒否、接続失敗、資源不足、timeoutはそれぞれ固定
`403`、`502`、`503`、`504`へ変換する。error responseへURL、endpoint、
header、body、OS error文字列を含めない。

## 4. Agent側の前提

Proxyから指定された宛先をAgentが無条件に接続することはない。Phase 3で実装した
`IPv4 address + TCP port`完全一致allowlistを通過した場合だけupstream Socketを
作成する。CIDR wildcard、port range、任意hostname、別経路へのfallbackは
初期版で許可しない。

## 5. Phase 5／6検証結果

- production listenerがWindows TCP table上で`127.0.0.1`だけに存在することを
  自動testで確認した。
- Bridge未ready時の固定`503`と接続拒否counterを確認した。
- fragmented header、複数header、absolute-form変換、Host不一致、宛先変更、
  禁止scheme／method／header、body分割を単体testで確認した。
- production local Socket、Client／Agent Bridge encode／decode、production
  Agent Winsock、loopback Mock HTTP Serverを一本につないだ。
- 512 KiB超requestと384 KiB超responseを256 KiB window越しに送り、
  変換後headerとbinary bodyのbyte完全一致、credit回復、half-close、
  normal close、active stream解放を確認した。
- fragmented chunked request、extension、trailer、`100 Continue`、chunked
  response、認証header透過を確認した。
- production capabilityを8 streamへ揃え、8 Socketの独立route、9件目拒否、
  一Socket reset後の残stream継続、全session cleanupを確認した。
- 二local connectionから異なるserver／port／repositoryへ同時接続し、401 NTLM
  challenge後も同じupstream Socketを使用することを確認した。
- `OPTIONS`、`GET`、`HEAD`、`POST`、`PROPFIND`、`REPORT`、
  `MKACTIVITY`、`CHECKOUT`、`PUT`、`DELETE`、`COPY`、`MOVE`、`MERGE`、
  `LOCK`、`UNLOCK`相当sequenceをproduction Proxy／SVC経路で確認した。
- Debug／Release全CTestとPhase 4／5結合testの反復結果を
  `docs/test-plan.md`へ記録した。
- 1 byteだけ送るslow headerが絶対期限で閉じることと、OPEN後のidle connectionが
  `CLOSE(Timeout)`を生成してlocal Socketを閉じることを確認した。
- Agent異常終了後のhealth timeout、同一RDP sessionでのAgent再起動、Client再起動、
  RDP再接続相当のstate再生成をfake SVCで反復した。

### Connection指名の拒否と要求境界

2026-10-07の公開前ソース検証では、ConnectionがHost／Content-Lengthを指名する
要求の拒否と、正常要求の変換前後の境界を回帰testへ追加した。

- 大小文字混在、comma、複数Connection行、header欠落、body長0を、すべての
  二分割位置とbyte単位入力で拒否し、その要求の出力を生成しない。
- 初回の不正要求と同一callback内のpipeline違反では、固定400を返し、
  Agent向けOPEN／DATAも上流送信queueも生成しない。
- OPEN済みKeep-Aliveでの違反は対象streamのCLOSEだけを生成し、別streamの
  正常要求を継続できる。
- 正常な固定長body、chunked、Trailer、既存hop-by-hop除去、複数要求は、
  wire byteと再解析後の要求数・body境界を維持する。
- Windows x64のDebug／Release buildと、各構成の全30 CTestが成功した。

このGateはin-memoryとloopback Mockでの検証である。署名済み配布EXEは変更せず、
修正を隔離buildで確認した。実SVN／Tracの回帰は、配布候補を用いてユーザーが
承認した環境・操作で別途確認する。

## 6. Mylyn Trac互換性

開発環境に導入済みのMylyn Trac Connector `3.26.0`をread-onlyで監査した。
Trac repository設定にはrepository固有のProxy host／portがあり、既定のsystem
Proxyを使わずZRdpだけを選択できる。

- Web fallbackはHTTP GETを使用する。
- XML-RPCはHTTP/1.1 POSTを使用し、repository URL配下の`/xmlrpc`へXML bodyを
  送る。
- repository認証、HTTP challenge、Cookie、Content-Type、Content-Length、
  Accept-Encoding／Content-Encodingはend-to-end情報として中継する。
- attachmentもXML-RPC payloadとして同じHTTP POST経路を使用する。
- responseはZRdpで解釈せず、redirect、Cookie、XML／gzip bodyをMylynへ返す。

`proxy.trac_http_compatibility`ではabsolute-form GET／POSTのorigin-form変換と
header／body透過を確認した。`integration.phase5_http_svn_compatibility`には
Mock TracのWeb GET、302／Set-Cookie、認証付きXML-RPC POST、gzip responseを追加し、
production Proxy、ClientBridge、SVC、AgentBridge、Agent relayを通してbyte一致を
確認した。

Trac対応は汎用HTTP Proxy化を意味しない。対象はユーザー承認済みの
`IPv4:port` allowlist entryに限定し、別endpoint、hostname、HTTPS、
HTTP `CONNECT`へのredirectまたは要求は従来どおり拒否する。

## 7. 実環境確認

Phase 6までは架空endpointとMock WebDAVだけを使用した。2026-07-31にユーザーが
許可した実環境でJavaHLをlocalhost Proxyへ設定し、既存repository URLを変更せず、
Phase 7 read-onlyとPhase 8 writeの全対象操作が問題ないとの受入結果を得た。

診断logはHTTP method、URL、repository、path、header、bodyを記録しない。このため
操作結果はユーザー受入を根拠とし、logではProxy有効化、Bridge Ready、
Agent強制終了検出、再起動後の復旧、error不在、機密情報不在を確認した。
許可済みendpoint以外へ接続範囲を拡大したことを意味しない。

実環境のMylyn設定検証、ticket query／参照、定期同期はユーザー環境で成立した。
Ticket更新やattachmentは必要になった許可済み対象だけを個別確認する。実TracのURL、
ticket、credential、attachment内容はlogまたはGit管理文書へ記録しない。
