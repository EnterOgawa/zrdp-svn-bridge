# Phase 5 HTTP／SVN互換性 実装提案

- 作成日: 2026-07-31
- 対象version: `0.5.0.0`

## 1. 目的

Phase 4で成立した単一HTTP connectionの経路を、HTTP SVNで必要となる
request framing、認証challenge、Keep-Alive、複数repositoryへ拡張する。
実取引先SVNは使用せず、Mock HTTP／WebDAV Serverと必要に応じたローカルtest
SVNだけをGate対象とする。

```text
複数local HTTP client
  -> 127.0.0.1:18080
  -> Client connection table
  -> StreamMultiplexer(Client)
  -> ESVNBRG
  -> StreamMultiplexer(Agent)
  -> Agent stream table
  -> allowlist一致済み複数Mock Server
```

## 2. 変更しない境界

- Static Virtual Channel名、protocol version、24-byte frame headerを変更しない。
- `OPEN`はClientだけが開始し、IPv4 4 byteとportだけを運ぶ。
- Agentはallowlist完全一致後だけoutbound Socketを作成する。
- Client listenerはIPv4 loopback `127.0.0.1`へ固定する。
- `CONNECT`、HTTPS tunnel、SOCKS、任意TCP forward、DNSを追加しない。
- HTTP／SVN payload、URL、header、credentialをUIまたはlogへ渡さない。
- ActiveX／COM呼出しはUI STA threadに限定する。

## 3. HTTP request framing

`HttpRequestTransformer`へ次の有限状態を追加する。

```text
Header
  -> Content-Length Body -> Header
  -> Chunk Size -> Chunk Data -> Chunk CRLF
     -> Chunk Size ...
     -> zero chunk -> Trailer -> Header
```

- `Transfer-Encoding`は単一の最終codingが`chunked`の場合だけ受理する。
- `Content-Length`と`Transfer-Encoding`の同時指定を拒否する。
- chunk sizeは16進数、optional extension、CRLFをincrementalに検証する。
- chunk size lineは8 KiB、trailer blockは64 KiB、decoded body合計は64 GiBを
  上限とする。
- chunk wire dataは検証後に同じchunked codingでupstreamへ中継する。
- `Transfer-Encoding: chunked`と`Trailer`は新しいupstream connectionにも
  framing情報として再送する。
- `Connection`、`Proxy-Connection`、`Proxy-Authorization`、`TE`、
  Connection列挙headerは引き続き除去する。
- `Expect: 100-continue`、`Authorization`、Cookie、WebDAV headerは
  end-to-end headerとして内容を解釈せず維持する。
- responseはopaque byte streamとして同一local connectionへ戻す。
  `100 Continue`、401／407 challenge、chunked responseを改変しない。

## 4. 複数connectionとSocket継続性

productionの同時stream上限を8とする。

- protocol coreの絶対上限32は変更しない。
- Clientはlocal connection IDごとにHTTP parser、stream ID、未送信queue、
  half-close状態を所有する。
- 一local connectionは最初の`IPv4:port`と一Agent Socketへ固定する。
- Keep-Alive中の複数request、NTLM等のconnection-oriented認証は同じstreamと
  upstream Socketを継続利用する。
- 別local connectionは別stream IDを割り当て、同時に別server／portを利用できる。
- Agentはstream IDごとにallowlist結果、Socket、connect期限、送受信queueを
  所有する。
- Client／Agentとも一owner threadで全Socketを公平にround-robin処理し、
  thread-per-connectionを作成しない。

## 5. 有限資源

| 対象 | 上限 |
|---|---:|
| production同時stream | 8 |
| 一streamのClient送信待ち | 256 KiB |
| 一streamのClient受信待ち | 256 KiB |
| Client各方向の全stream合計 | 2 MiB |
| 一streamのAgent Socket送信待ち | 256 KiB |
| Agent全stream Socket送信待ち | 2 MiB |
| 一streamのAgent read保留 | 16 KiB |
| multiplexer connection window | 2 MiB（256 KiB × 8） |
| UI／worker Frame queue | 各4,096件／4 MiB |
| 一worker iterationのSocket operation | 有限32回 |

上限到達時は新規接続を固定`503`で拒否するか、既存streamへbackpressureを返す。
一streamのHTTP構文違反やlocal Socket resetはそのstreamだけを閉じる。
wire protocol違反、global queue破損、credit違反はBridge session全体をfail-closedに
する。

## 6. Proxy port設定

bind addressは変更不能なまま、optional `client.ini`の
`[proxy] port=<1..65535>`だけをworker threadで読み込めるようにする。

- EXEと同じdirectoryの固定名だけを読む。
- file欠落時は既定`18080`を使う。
- 不正fileは別portへfallbackせずProxyをFailedにする。
- section、key、file size、line lengthを有限化する。
- hostname、bind address、upstream endpoint、credentialを表現できない。
- 実`client.ini`はGit管理対象外、文書用templateだけをrepositoryへ置く。

## 7. 検証

### HTTP parser

- fragmented chunk size／data／CRLF／trailer
- chunk extensionと複数chunk
- invalid hex、overflow、巨大line、欠落CRLF
- Content-LengthとTransfer-Encoding競合
- `Expect: 100-continue` header維持
- Authorization、Cookie、Depth、Destination、Lock-Token等のbyte維持
- Keep-Aliveとpipelined request

### 複数stream

- 8 local connectionの同時OPEN
- 9件目の固定拒否
- 異なるserver／port／repositoryへのinterleaved DATA
- 一streamのresetが他streamへ影響しない
- connection window exhaustion／recovery
- RDP切断相当で全local／upstream Socketを終了

### Mock WebDAV／SVN sequence

- `OPTIONS`、`GET`、`HEAD`、`POST`
- `PROPFIND`、`REPORT`
- `MKACTIVITY`、`CHECKOUT`、`PUT`
- `DELETE`、`COPY`、`MOVE`、`MERGE`
- `LOCK`、`UNLOCK`
- 401 challenge後の同一Socket再request
- `Expect: 100-continue`
- chunked request／response
- 複数repositoryの並行sequence

## 8. Gateとrollback

Phase 5完了条件は、Mock／test環境で上記sequenceと複数connectionが成立し、
Debug／Release全CTestおよび反復結合testが成功することである。
Repository browseだけでは完了としない。

実SVNへのread-only接続はPhase 7、write操作はPhase 8まで行わない。

問題時はVersion `0.4.0.0`の単一connection実装へ戻せるよう、wire protocolを
変更せず、HTTP parser、Client connection table、Agent stream tableを独立target内に
閉じ込める。DVC、外部runtime、OS設定変更へfallbackしない。
