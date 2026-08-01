# Phase 5 HTTP／SVN互換性 ローカル検証結果

- 検証日: 2026-07-31
- 対象version: `0.5.0.0`
- 対象platform: Windows x64

## 1. Gate結果

- [x] HTTP chunked requestのincremental構文検証と透過中継
- [x] `Expect: 100-continue`と`100 Continue` response
- [x] Authorization、Cookie、WebDAV headerの非解析中継
- [x] Keep-Alive中の一local connection／一upstream Socket固定
- [x] production Client／Agentの同時8 stream
- [x] 9件目拒否と一stream障害時の残stream継続
- [x] 二server／port／repositoryの同時Mock sequence
- [x] 全対象WebDAV／DeltaV method
- [x] loopback固定のProxy port local設定
- [x] Debug／Release build、CTest、反復test
- [x] portable Client／Agent生成

Phase 5のローカルGateは通過した。実取引先SVN、実IP、実repository、credential、
Zscaler、RDP実機通信は使用していない。

## 2. 実装境界

- `HttpRequestTransformer`
  - fragmented chunk size／extension／data／CRLF／zero chunk／trailerを検証する。
  - `Content-Length`との競合、未知coding、複数Transfer-Encodingを拒否する。
  - responseはopaque byteとして変更しない。
- `ClientProxySession`／`ClientProxyWorker`
  - connection IDごとにparser、stream、queue、half-closeを所有する。
  - 最大8 local connection、一stream各方向256 KiB、全体各方向2 MiBとする。
- `AgentStreamRelay`
  - stream IDごとにallowlist結果、Socket、connect期限、queueを所有する。
  - 最大8 Socket、全Socket書込み待ち2 MiB、一Pump最大32 operationとする。
- `client.ini`
  - EXE隣接の固定fileだけをworker threadで読む。
  - `[proxy] port=<1..65535>`以外をfail-closedにする。
  - bind addressは常に`127.0.0.1`であり設定できない。

wire protocol version、24-byte frame header、Static Virtual Channel名`ESVNBRG`は
変更していない。

## 3. 自動test

Debug／Releaseの双方で次の23 CTestが成功した。

```text
100% tests passed, 0 tests failed out of 23
```

追加または拡張した主要test:

- `proxy.http_request_parser`
  - fragmented chunk、extension、trailer、framing競合、認証header
- `proxy.client_config`
  - port-only構文、未知key／section、重複、範囲、有限上限
- `proxy.client_session`
  - 二connectionの交差応答、認証header、stream分離、9件目拒否
- `agent.stream_relay`
  - 8 Socket独立route、partial I/O、peer reset分離、全cleanup
- `integration.phase5_http_svn_compatibility`
  - production local TCP、ClientBridge、SVC分割、AgentBridge、Mock upstream
  - 二server／port／repository
  - 401 NTLM challenge後の同一Socket再request
  - chunked request／response、`100 Continue`
  - `OPTIONS`、`GET`、`HEAD`、`POST`、`PROPFIND`、`REPORT`、
    `MKACTIVITY`、`CHECKOUT`、`PUT`、`DELETE`、`COPY`、`MOVE`、
    `MERGE`、`LOCK`、`UNLOCK`

反復結果:

| 構成 | Phase 4大容量経路 | Phase 5複数WebDAV経路 |
|---|---:|---:|
| Debug | 20/20 | 20/20 |
| Release | 20/20 | 20/20 |

## 4. Portable成果物

| File | Size | Version | SHA-256 | 署名 |
|---|---:|---:|---|---|
| `out/ZRdpSvnBridge.exe` | 641,024 byte | 0.5.0.0 | `8994DFA78086267434E931AEED2F17A47D6EB9A61001F2D1F4F02618103C7C56` | 未署名 |
| `out/ZRdpSvnAgent.exe` | 422,912 byte | 0.5.0.0 | `F483DA4EE1AE23D7AAE087FE54F433E9F743E09C87E1862192A9E35BF80CDE14` | 未署名 |

Clientのimport DLL:

```text
COMCTL32.dll
GDI32.dll
ole32.dll
SHELL32.dll
USER32.dll
WS2_32.dll
CRYPT32.dll
OLEAUT32.dll
KERNEL32.dll
```

Agentのimport DLL:

```text
WTSAPI32.dll
USER32.dll
WS2_32.dll
ole32.dll
KERNEL32.dll
```

VC runtime DLLはimportせず、既存の`/MT` portable方針を維持する。両EXEは
ユーザーの署名前であり、Release／asset uploadの対象ではない。

## 5. test用Subversion toolの判断

既存`C:\develop\tools`とPATHには`svn`、`svnadmin`、Apache HTTP Serverがなかった。
MSYS2 catalogではSubversion `1.14.5-3`、Apache-2.0、署名検証済みpackageを
確認したが、HTTP方式repositoryに必要なApache HTTP Server／`mod_dav_svn`は
同じ既存環境で利用可能と確認できなかった。

Apache Subversion本体はWindows binaryを直接保守せずthird-party package一覧を
案内している。VisualSVN ServerはHTTP SVN serverを提供するが、installer、
Windows service、追加listenerを伴う。Phase 5 Mock Gateに不要な常駐serviceと
system変更を増やさないため、自動導入しなかった。

製品runtime dependencyは追加していない。実SVN client／repositoryの確認は、
Phase 7／8でユーザーが明示した支給PC側の既存HTTP SVNだけを対象とする。

## 6. 次のGate

Phase 6ではmalformed frame／HTTP、資源枯渇、slow peer、timeout、Agent crash、
Client restart、RDP disconnect／reconnect、allowlist変更、log redactionを
重点的に検証する。Phase 6も実取引先SVNへは接続しない。
