# ZRdp SVN Bridge テスト計画

更新日: 2026-08-03

## 1. 現在の対象

Phase 0の基本RDP機能は、2026-07-30にユーザーから期待どおりとの受入結果を得た。
Phase 1は実RDP Echo、AgentのCtrl+C終了、同じRDP session内でのAgent再起動、
再Echoの成功logを確認し、Gateを通過した。Phase 2 Application Protocol／多重化も
local Gateを通過し、Phase 3 Agent allowlist限定TCP接続、Phase 4 Client
localhost HTTP Proxy、Phase 5 HTTP／SVN互換性もlocal Gateを通過した。
Phase 6 Hardeningはlocal Gateとユーザー実機回帰を通過した。2026-07-31には
JavaHLから実SVNへ接続し、Phase 7 read-onlyとPhase 8 writeの全対象操作について
ユーザーから問題なしとの受入結果を得た。Phase 9改善I-001のMylyn Trac疎通も
ユーザー環境で成立した。改善I-002の独立session Windowを維持しつつ、
改善I-003の接続管理Window／名前付き接続はユーザーから期待どおりとの結果を
得た。改善I-004／I-005の実機受入待ちを維持しつつ、改善I-006の
初回動的解像度は別接続でログイン完了直後の`E_UNEXPECTED`が再発したため、
Version `0.7.10.0`で有限retryを追加して実機Gateを再通過した。改善I-007として、
成立済みRDP sessionの最終外部切断後に対象Windowだけを無通知終了する動作を
検証する。改善I-008では、濃紺tileを廃止した枠なしpopカラーiconを
Client／Agentへ共通適用し、実機feedbackに基づいて占有率を拡大し黄色を除く。
Version `0.7.10.0`では動的解像度の限定再試行と、実機で相互停止が判明した
複数RDP sessionをsession専用COM STAへ分離する回帰修正を実施した。
2026-07-31、動的解像度、異なる二台の同時RDP、Agent接続済みsession終了後の
別RDP／manager継続、表示mode変更、Proxy port再利用についてユーザー実機Gateを
通過した。改善I-009では`AplMain.java`の構成を参考に全88 sourceのコメントを
整備し、継続監査用CTestを追加した。runtime変更がないため追加実機試験は行わない。
改善I-010では、接続後のセッションWindowへ既存ActiveX経路を使用する
全画面buttonを追加した。ユーザー実機で全画面移行自体は成立したが、
動的解像度でもremote desktop寸法が通常Window時のまま残ることが判明した。
回帰修正R-002では、動的解像度だけが通常WindowのRDP領域と全画面monitorへ
追従するよう変更し、Version `0.7.12.0`をローカル検証した。
回帰修正R-003では、まれなログイン前切断をClient／Windows event logで特定し、
観測済みの`discReason=263`、`ExtendedDisconnectReason=0`だけを2秒後に一度
再試行するVersion `0.7.14.0`候補をローカル検証した。
回帰修正R-005では、同じ接続先についてZRdpと`mstsc.exe`の双方で二回続けて
同じSSL handshake失敗が発生し、`mstsc.exe`の三回目で成立した観測結果を受け、
対象条件を広げずClientの有限再試行だけを2秒後、5秒後の二段階へ拡張した。
Agent、Virtual Channel、Proxy、protocol、認証条件は変更していない。

- Client: `out/ZRdpSvnBridge.exe`
- Agent: `out/ZRdpSvnAgent.exe`
- 標準`out` version: 両方とも`0.7.14.0`
- Platform: Windows x64
- 配布: installerなしのportable EXE
- Static Virtual Channel: `ESVNBRG`
- localhost HTTP Proxy: sessionごとの`127.0.0.1:<割当port>`限定、
  各最大8 connection
- RDP session: 一つのClient processで最大16 Window
- Agent upstream Socket／allowlist: 完全一致後の最大8 stream
- HTTP基本中継: Mock HTTP／WebDAV Serverまで実装済み
- HTTP／SVN互換性: chunked、認証継続、複数connection、対象method実装済み
- HTTP／Trac互換性: Web GET、XML-RPC POST、Cookie、redirectをMock確認済み
- Hardening: Agent health、slow peer timeout、restart／reconnect実装済み

Phase 5ではproduction local Socket、Client／Agent BridgeとMock Agent Socketを
一本につなぎ、二server／port／repositoryのWebDAV sequenceを確認した。
実取引先SVNのRepository参照、Checkout、Update、Commit等は、Phase 7／8の
ユーザー受入結果により成立を確認済みである。

## 2. ローカル自動テスト

実RDP、Zscaler、支給PC、実SVN／Tracへ接続せず、DebugとReleaseの両方で
次を実行する。

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug --parallel
ctest --preset debug
cmake --build --preset release --parallel
ctest --preset release
```

確認対象:

- x64、C++20、`/W4`、`/WX`、`/MT`
- Client／Agentのversion resource一致
- frame encode／decodeとbig-endian
- 全header／payload境界のfragmentation
- 一回の入力に連結された複数frame
- invalid magic／version／type／flags／streamId／length
- HELLO capability検証と非互換拒否
- Client／Agent bounded queue
- duplicate／out-of-order sequence拒否
- 埋込みNULを含むpayloadの完全一致
- Agent session判定
- fake SVC上の合計11,600,897-byte Echo
- Agent不在、Echo timeout、RDP disconnect相当reset
- 5秒間隔health PING、15秒deadline、timeout後の新HELLO
- timeout前sessionの遅延PONG／DATA破棄とAgent再起動
- Phase 2 capabilityとPhase 1 peerへの安全なdowngrade
- Client-only OPEN、IPv4+port固定payload、最大32 stream
- streamごとの双方向DATA sequence
- stream／connection window exhaustionとWINDOW_UPDATE recovery
- outbound 4,096 frame／4 MiB、inbound 4,096 event／4 MiB上限
- OPEN拒否、HALF_CLOSE、normal close、timeout、peer reset、crossed close
- fake Socket上の3 stream双方向byte完全一致
- allowlist strict parser、完全一致、全拒否条件、atomic reload
- allowlist拒否時にconnector呼出し0回
- fake Socketのconnect timeout／refused／reset／partial send／cleanup
- 接続済みAgent upstream Socketの30分無通信timeout
- production Winsockとloopback Mock Serverの512 KiB超双方向byte完全一致
- TCP half-close、normal close、RDP切断相当の全Socket cleanup
- IPv4 loopbackだけのProxy listenerとBridge未ready時の固定503
- 初回HTTP headerの絶対30秒期限と、OPEN後local無通信30分期限
- incremental HTTP request header解析とabsolute-form／origin-form変換
- Host一致、宛先固定、CONNECT／Upgrade／未知Transfer-Encoding拒否
- hop-by-hop header除去と`Content-Length`／chunked body streaming
- production Client ProxyからAgent relayまでの512 KiB超request／384 KiB超response
- Phase 4経路のFIN、CLOSE、flow-control recovery、active stream cleanup
- 8 local／upstream streamの独立route、9件目拒否、reset分離
- `Expect: 100-continue`、認証header、Cookie、WebDAV header透過
- 全対象WebDAV／DeltaV methodのMock sequence
- optional `client.ini`のport-only strict parser
- base portからのsession別順次割当、解放、再利用、上限、overflow
- 同一COM STAで二つのRDP ActiveX instanceを同時生成し、所有権を共有しない
- 異なる二つのCOM STAでActiveXを生成し、一方の破棄後も他方のSTAが応答する
- 動的解像度の`E_UNEXPECTED`だけを500／1,000／1,500 msで有限再試行し、
  ほかのHRESULTと上限後を再試行しない
- 動的解像度targetを通常WindowではRDP child領域、全画面ではmonitor全体へ
  選び分ける
- 接続管理Window、次のProxy port、状態・統計を含むsession titleのresource契約
- セッションWindowの「全画面」／「ウィンドウ」button resource契約
- 名前付き履歴Version 3の音声設定保存／復元、同名・同一接続組の重複排除、
  Version 1／2履歴の安全な既定値による後方互換読込
- マイクON／OFF、3種類の音声再生先、未知値拒否、ActiveX readback
- 接続時desktop比率による4辺／4角のWindow矩形補正、反対辺anchor、
  最小寸法、不正値拒否
- ログイン完了有無、local切断、credential取消による外部切断Window終了policy
- ログイン前`263/0`完全一致だけの2秒後、5秒後・最大二回接続再試行policyと
  全除外条件
- 接続再試行の予約、開始、成功、上限到達を示す固定診断event
- 接続名、接続先、domain、userが暗号化fileへ平文で残らないこと
- portable EXEのx64種別とversion
- Agent crash／restart、Client restart、RDP reconnect、allowlist変更の反復

## 3. 実機テストの前提

- Zscaler Client Connectorを通常どおりONにする。
- 現在ZRdpから接続できる支給PCだけを対象にする。
- R-003確認時はClientとAgentを標準`out`へ反映した同じbuildの`0.7.14.0`で揃える。
- Agentは接続中のRDPユーザーセッション内で通常ユーザーとして起動する。
- Agent EXEの配置には、所属組織で許可された方法だけを使用する。
- 実SVN URL、repository、credential、Commit操作は使用しない。
- 証明書／認証warningは通常手順で利用者が判断し、自動承認しない。
- 実IP、ドメイン、ユーザー名、画面captureをGit管理ファイルへ記録しない。

## 4. Phase 1／Bridge health実機回帰手順

1. 開発PCで`ZRdpSvnBridge.exe`を起動する。
2. 接続管理Windowから通常どおり支給PCへ新しい接続を開始し、ログイン完了後も
   画面操作できることを確認する。
3. session titleがRDP接続済み、Agent待機中、Proxy待受中を示すことを確認する。
4. 接続中のRDP画面内で`ZRdpSvnAgent.exe`を起動する。
5. 有効な`agent.ini`を読んだAgentがClient接続済みを表示し、session titleが
   Agent接続済み、Proxy利用可へ遷移することを確認する。
6. Agent consoleでCtrl+Cを押して停止し、RDP画面接続が維持されることを確認する。
7. 通常のhealth checkにより最大約20秒でtitleがAgent待機中へ戻ることを確認する。
8. 同じRDPセッション内でAgentを再起動し、再びAgent接続済み、Proxy利用可へ
   戻ることを確認する。
9. session Windowを閉じ、確認後にRDPが切断され、Agentもchannel read終了後に
   停止することを確認する。

11,600,897 byteの実RDP Echoと同一session内のAgent再起動／再Echoは、
2026-07-30のPhase 1 Gateで確認済みである。Version `0.7.10.0`の通常session
Windowは表示mode以外の操作欄を持たないため、Echo buttonを再公開せず自動testで
回帰する。

AgentをRDP外の開発PCまたはconsole sessionで起動した場合は、activeなRDP
ユーザーセッションではない旨を表示して終了code `2`となるのが仕様である。
実機Gateでは意図的に誤配置する必要はない。

## 5. Phase 1結果チェックリスト

### ローカル

- [x] Client／Agent／protocolをx64 Debugでwarningなしにbuildできる
- [x] Debug CTest全件が成功する
- [x] frameのfragmentationと複数frame再構成が成功する
- [x] malformed frameと非互換versionをfail-closedで拒否する
- [x] Client／Agent queueに件数・byte上限がある
- [x] 埋込みNULを含むbinary payloadを保持する
- [x] fake SVCで1 byte、1 KiB、64 KiB、1 MiB、10 MiBが完全一致する
- [x] fake SVCの合計確認byte数が11,600,897である
- [x] connection-level CLOSE後に同じfake RDP sessionでAgentを再起動できる
- [x] handshake成立後にidle HELLOを送らずactive streamをresetしない
- [x] Echo再実行時にsequenceを新しいHELLO境界から開始できる
- [x] Agent不在相当でRDP stateを変更しない
- [x] 未応答PINGをtimeout分類で停止できる
- [x] Release buildとCTestが成功する
- [x] `out`に同versionのx64 Client／Agentをportable成果物として出力できる
- [x] Client／Agentの両EXEに共通ZRdp icon resourceが埋め込まれている

### ユーザー実機

- [x] Zscalerを停止・変更せずRDP接続を維持できる
- [x] RDPログイン完了後にClientがAgent待機を表示する
- [x] 同じRDP session内でAgentが`ESVNBRG`を開ける
- [x] ClientとAgentがReadyになる
- [x] 実RDP上で合計11,600,897 byteのEchoが成功する
- [x] Echo中もRDP画面、keyboard、mouseが応答する
- [x] Agent異常終了をhealth checkが最大約20秒で自動検出する
- [x] Ctrl+C後にRDPを切断せずAgentを再起動できる
- [x] 再起動Agentで再度Echoが成功する
- [x] health timeout後もRDP画面操作を継続できる
- [x] health timeout後に同じRDP内のAgent再起動でReadyへ戻る
- [x] RDP再接続とAgent再起動後に新しいEchoが成功する
- [x] RDP切断時にClientがBridge stateを破棄する
- [x] RDP切断後にAgentがchannel errorを検出して終了する
- [x] credential、payload、実接続情報がClient／Agent logにない

Agentがconnection-level CLOSEを送れない異常終了では、active streamをresetしない
health PINGで検出し、旧streamを破棄して同一RDP session内で新HELLOを開始する。
途中requestは自動再送しない。

## 6. Phase 2結果チェックリスト

### Protocol

- [x] version 1 headerとbig-endian規則を維持する
- [x] Phase 1 peerとはEcho-onlyへdowngradeする
- [x] OPEN／OPEN_OK／OPEN_ERRORの固定payloadとstateを検証する
- [x] duplicate stream IDとduplicate DATA sequenceをfail-closedで拒否する
- [x] Agent roleからOPENを開始できない
- [x] stream ID、sequence、credit、queueをsession resetで破棄する

### Flow control／resource

- [x] stream windowの枯渇と回復を確認する
- [x] connection 4 MiB windowの枯渇と回復を確認する
- [x] 未送信byteを超えるcredit水増しを拒否する
- [x] outbound 4,096 frame上限で非fatal backpressureを返す
- [x] inbound 4,096 event上限超過をfail-closedで拒否する
- [x] payload、endpoint、OS error文字列をprotocol errorへ含めない

### 多重化

- [x] fake Socketで3 streamを同時に確立する
- [x] Client->Agentの全streamでbyte完全一致する
- [x] Agent->Clientの全streamでbyte完全一致する
- [x] HALF_CLOSE後も反対方向DATAを継続できる
- [x] normal／timeout／peer reset／crossed closeでrecordを解放する
- [x] Debug全14 CTestが成功する
- [x] Release全14 CTestが成功する

Phase 2はローカルGateであり、追加の実機RDP操作は要求しない。Phase 3では
文書用allowlistとloopback Mock Serverだけを使用し、実取引先SVNには接続しない。

## 7. Phase 3結果チェックリスト

### Allowlist

- [x] templateをproduction file loaderで解析できる
- [x] IPv4とportの完全一致だけを許可する
- [x] hostname、CIDR、wildcard、IPv6、port rangeを拒否する
- [x] 不正IPv4／port、重複ID／endpoint、空設定、未知keyを拒否する
- [x] file／line／entry上限を適用する
- [x] 失敗reloadで以前の検証済み設定を置換しない

### Socket relay

- [x] policy拒否時にconnectorを呼ばない
- [x] allowlist一致後だけnon-blocking connectを開始する
- [x] connect timeout、refused、peer resetを固定wire codeへ変換する
- [x] partial send／WouldBlock後に未送信dataを保持する
- [x] Socketへ処理済みのDATAだけWINDOW_UPDATEする
- [x] 双方向half-close後にnormal CLOSE／ackでstreamを解放する
- [x] StopSessionでactive Socketとbufferを破棄する
- [x] production Agentにinbound listen／DNS経路がない

### Mock Server integration

- [x] production Winsock adapterで127.0.0.1のOS割当portだけへ接続する
- [x] 単一streamで512 KiB超のrequest／responseがbyte完全一致する
- [x] 複数windowのcredit exhaustion／recoveryが成立する
- [x] Client FIN、server FIN、normal CLOSEが成立する
- [x] Debug全17 CTestが成功する
- [x] Release全17 CTestが成功する
- [x] `out`のversion、hash、依存DLL、署名状態を確認する

Phase 3はlocal Gateである。実支給PCへ`agent.ini`を配置する確認や、実SVNへの
connectはPhase 7の明示指示まで行わない。

## 8. Phase 4結果チェックリスト

### Loopback listener

- [x] production workerが`127.0.0.1`だけへbindする
- [x] Windows TCP tableにloopback以外のlistenerが存在しない
- [x] `SO_EXCLUSIVEADDRUSE`を適用する
- [x] Bridge未ready時に固定`503`を返して直ちに閉じる
- [x] 一local connection／一stream上限を適用する
- [x] Stop時にlistener、local Socket、stream、queueを解放する

### HTTP request境界

- [x] fragmented headerと一入力内の複数requestを解析する
- [x] absolute-formをorigin-formへ変換する
- [x] IPv4 literalとport省略時の80だけを受理する
- [x] Host欠落、重複、不一致、不正IPv4／portを拒否する
- [x] Keep-Alive中の宛先変更を拒否する
- [x] `CONNECT`、HTTPS、Upgrade、Transfer-Encoding／chunkedを拒否する
- [x] proxy固有headerとhop-by-hop headerをupstreamから除去する
- [x] `Content-Length` bodyを全量保持せずchunk単位で中継する
- [x] header、body、queue、処理回数へ有限上限を適用する

### Client／Agent結合経路

- [x] production local TCP clientから`ClientProxyWorker`へ接続する
- [x] `ClientBridge`／`AgentBridge`の実encode、fragment、decodeを通過する
- [x] Agent allowlist完全一致後だけproduction Winsockを開始する
- [x] loopback Mock HTTP Serverへ変換済みrequestをbyte完全一致で送る
- [x] 512 KiB超requestを256 KiB window越しに送る
- [x] 384 KiB超responseを256 KiB window越しに受ける
- [x] local FIN、server FIN、normal CLOSE、ack、active stream解放を確認する
- [x] RDP／Bridge停止相当でlocal／upstream両Socketを解放する

### Build／回帰

- [x] Debugを`/W4`／`/WX`でwarningなしにbuildする
- [x] Debug全21 CTestが成功する
- [x] Releaseを`/W4`／`/WX`でwarningなしにbuildする
- [x] Release全21 CTestが成功する
- [x] Phase 4結合testがDebug 20/20、Release 20/20で成功する
- [x] `out`へ同versionのx64 portable Client／Agentを生成する
- [x] chunked、`100 Continue`、認証、複数connectionをPhase 5で確認する
- [x] 実SVN操作をPhase 7／8の明示承認後に確認する

Phase 4は2026-07-30にlocal Gateを通過した。この時点で追加の実機操作は要求しない。
Phase 5はMock／test SVNだけを使用し、実取引先SVNへ自動接続しない。

## 9. Phase 5結果チェックリスト

### HTTP framing／認証

- [x] fragmented chunk size／data／terminator／trailerを再構成する
- [x] chunk extensionと複数chunkを同じwire形式で中継する
- [x] invalid hex、overflow、巨大line、欠落CRLFを拒否する
- [x] `Content-Length`と`Transfer-Encoding`の併用を拒否する
- [x] `Expect: 100-continue`を維持し、`100 Continue` responseを変更しない
- [x] Authorization、Cookie、Depth、Destination、Lock-Tokenを変更しない
- [x] 401 NTLM challenge後も同じupstream Socketを使用する

### 複数connection／資源

- [x] Client／Agentが同じmaximum 8 streamをHELLOで提示する
- [x] 8 local connection／upstream Socketを独立routeできる
- [x] 9件目を既存streamへ影響させず拒否する
- [x] 一streamのHTTP拒否／peer reset後も残streamを継続する
- [x] RDP切断相当のStopで全local／upstream Socketを閉じる
- [x] 一stream256 KiB、全stream2 MiB、1 pump 32 operation上限を持つ

### Mock WebDAV／SVN相当sequence

- [x] 二つのlocal TCP connectionを異なるserver／port／repositoryへ接続する
- [x] production Proxy、ClientBridge、SVC分割、AgentBridgeを通過する
- [x] `OPTIONS`、`GET`、`HEAD`、`POST`、`PROPFIND`、`REPORT`を中継する
- [x] `MKACTIVITY`、`CHECKOUT`、`PUT`、`DELETE`、`COPY`、`MOVE`、
  `MERGE`、`LOCK`、`UNLOCK`を中継する
- [x] chunked request／responseと`100 Continue`を同じ結合経路で確認する
- [x] URL、header、body、credential、endpointを診断logへ追加しない

### Build／回帰

- [x] Debugを`/W4`／`/WX`でwarningなしにbuildする
- [x] Debug全23 CTestが成功する
- [x] Releaseを`/W4`／`/WX`でwarningなしにbuildする
- [x] Release全23 CTestが成功する
- [x] Phase 4／5結合testがDebug／Release各20/20で成功する
- [x] `out`へversion `0.5.0.0`のx64 Client／Agentを生成する

Phase 5は2026-07-31に架空endpointとMock WebDAVだけでlocal Gateを通過した。
追加の常駐SVN serverや製品runtime dependencyは導入していない。実取引先SVNへの
read-only操作はPhase 7、write操作はPhase 8の明示指示まで行わない。

## 10. Phase 6結果チェックリスト

### Protocol／session recovery

- [x] Ready中のhealth PING／PONGを64回連続して処理する
- [x] 15秒無応答相当で旧stream／queueを破棄し、新HELLOを生成する
- [x] timeout前sessionの遅延PONG／DATAを新HELLO_ACKまで破棄する
- [x] Agent crash／restartとClient restartをfake SVCで再現する
- [x] RDP disconnect／reconnect相当を32回反復して旧stateを再利用しない
- [x] 停止と交差したProxy workerの旧Frameで新HELLO待機をFailedへ戻さない

### Timeout／security boundary

- [x] 一byteのslow headerも接続受付からの絶対期限で閉じる
- [x] OPEN後のlocal idleを`CLOSE(Timeout)`へ変換してSocketを閉じる
- [x] 接続済みupstream idleを`CLOSE(Timeout)`へ変換してSocketを閉じる
- [x] allowlist不正reloadで旧検証済み設定を維持し、新Agentだけへ新設定を適用する
- [x] production listenerがWindows TCP table上でloopback一件だけである
- [x] production Agent sourceにinbound listen／DNS経路がない
- [x] HTTP `CONNECT`、hostname、任意port、allowlist外接続を拒否する
- [x] 診断logが固定event／数値fieldだけで、health payloadやendpointを含まない
- [x] Release EXEに認証header、Cookie、password、文書用endpoint文字列がない

### Build／反復

- [x] Debugを`/W4`／`/WX`でwarningなしにbuildし、全24 CTestが成功する
- [x] Releaseを`/W4`／`/WX`でwarningなしにbuildし、全24 CTestが成功する
- [x] Phase 4／5／6結合testがDebug／Release各20/20で成功する
- [x] malformed／resource／allowlist／timeout主要6 testがDebug／Release各20/20で成功する
- [x] `out`へversion `0.6.0.0`のx64 portable Client／Agentを生成する
- [x] ASLR、DEP、Control Flow Guardとsystem DLLだけの依存を確認する
- [x] 未署名であることを確認し、Release Gate未通過として扱う

### ユーザー実機回帰

- [x] Version `0.6.0.0`で通常のRDP画面、入力、全画面、clipboardを回帰確認する
- [x] Agent強制終了後、最大約20秒でClientがAgent待機へ戻る
- [x] health timeout後もRDP画面操作を継続できる
- [x] 同じRDP内でAgentを再起動し、ReadyとEcho成功へ戻る
- [x] RDP切断／再接続後にAgentを起動し直し、新しいEchoが成功する

実機回帰自体は実SVNへ接続せず実施した。その結果とユーザーの明示承認を受けて、
後続のPhase 7 Repository Explorer／Checkout等を実施した。

## 11. Phase 7／8実環境受入結果

実操作のmethod、URL、repository、path、source、diffは通常logへ記録しない。
各操作の成否はユーザー受入結果、通信経路と復旧の健全性は固定診断logを
それぞれ正本として判定した。

### Phase 7 Read-only

- [x] JavaHLの既存repository URLを変更せずlocalhost Proxyへ接続する
- [x] Repository Explorer
- [x] Log
- [x] Diff／Compare
- [x] Checkout
- [x] Update
- [x] 許可済みserver上の複数repository

### Phase 8 Write

- [x] Commit
- [x] Add
- [x] Delete
- [x] Move
- [x] conflict
- [x] Lock
- [x] Unlock

### 実機log

- [x] Agent強制終了を`bridge.peer_timeout`で検出する
- [x] timeout中にRDP接続を維持する
- [x] Agent再起動後にBridge ReadyとProxy有効へ戻る
- [x] Client／AgentともERROR eventがない
- [x] URL、IP、credential、HTTP／SVN payloadが通常logへ混入しない
- [x] 未知event、未知field、不正session IDがない

Phase 7／8は2026-07-31のユーザー明示確認をもってGate通過とする。操作methodを
通常logへ追加して証跡化する変更は、機密情報最小化の方針に反するため行わない。
問題が後日判明した場合は、該当操作だけを改善項目として再オープンする。

## 12. Phase 9改善I-001 Trac互換性

### ローカル

- [x] 導入済みMylyn Trac `3.26.0`のrepository固有Proxy実装を確認する
- [x] Web fallbackがGET、XML-RPCがHTTP/1.1 POSTを使用することを確認する
- [x] absolute-form GET／POSTをorigin-formへ変換する
- [x] Authorization、Cookie、Content-Type、Content-Length、XML bodyを維持する
- [x] 302、Set-Cookie、Content-Encoding、response bodyをopaqueに維持する
- [x] production Proxy、ClientBridge、SVC、AgentBridge、Agent relayを通す
- [x] allowlist外endpoint、hostname、HTTPS、HTTP `CONNECT`を引き続き拒否する
- [x] production codeと`ESVNBRG` wire protocolを変更しない
- [x] Debug／Releaseの全25 CTestを成功させる

### ユーザー実機

- [x] Mylyn Trac repository固有Proxyを`127.0.0.1:<port>`へ設定する
- [x] 既存Trac URLを変更せず「設定の検証」が成功する
- [x] Mylyn同期とticket参照の疎通が成功する
- [x] Trac疎通中もRDP、SVN、Agent healthに回帰がない
- [ ] 必要になった場合だけticket／comment更新を個別確認する
- [ ] 必要になった場合だけattachment download／uploadを個別確認する

実Tracの更新／attachment操作は、ユーザーが必要とする安全な対象だけで行う。
実URL、ticket ID、credential、comment、attachment内容は報告またはGit管理文書へ
含めない。

## 13. Phase 9改善I-002 複数RDP session Window

### ローカル

- [x] 接続管理Windowとsession Windowの所有権を`ClientApplication`へ集約する
- [x] manager COM STAとsession専用COM STAの所有境界を分離する
- [x] 各sessionが独立したActiveX、Bridge、Proxy workerを所有する
- [x] 最初のsessionへbase port、後続へ`base port + slot`を重複なく割り当てる
- [x] session終了時に対応slotだけを解放し、最小空きportを再利用する
- [x] 最大16 session、port 65535超過、slot枯渇を拒否する
- [x] session Window内は表示mode一行だけを表示し、他の操作欄とstatus barを
      表示しない
- [x] 接続管理Windowの初期modeを引き継ぎ、接続中も3 modeを切り替えられる
- [x] session titleへ接続先、RDP／Agent／Proxy状態、割当port、stream数、
      方向別転送量を表示する
- [x] 標準の`Ctrl+Alt+Break`要求で全画面を切り替える
- [x] 閉じる操作で確認後に対象sessionだけを切断する
- [x] 接続管理Windowへ生存session数と次の割当portを表示する
- [x] 異なるSTAで二つのRDP ActiveXを生成し、一方の破棄後も他方へmessageを
      dispatchできる
- [x] `RequestClose`後のActiveX破棄で重複`Disconnect`を呼ばない
- [x] sessionからの履歴通知をmanager STAへmarshalし、manager objectへ直接触れない
- [x] Debugの全29 CTestを成功させる
- [x] Releaseの全29 CTestを成功させる
- [x] Version `0.7.10.0`のportable Client／Agentを`out`へ生成する

### ユーザー実機

- [x] 起動時に接続管理Windowだけが表示される
- [x] 最初の「新しい接続」で別session Windowが開き、自動的にRDP認証へ進む
- [x] session Windowに表示mode一行だけがあり、他の操作欄やstatus barがない
- [x] 接続中に等倍表示、Smart Sizing、動的解像度を切り替えられる
- [x] 最初のsession titleにRDP／Agent／Proxy状態と`127.0.0.1:18080`が表示される
- [x] Agent起動後、titleがAgent接続済み／Proxy利用可へ遷移する
- [x] `Ctrl+Alt+Break`で全画面とWindow表示を往復できる
- [x] 閉じる操作で対象sessionだけを切断・終了できる
- [x] 二つ目の接続を開始でき、別Windowに`127.0.0.1:18081`が表示される
- [ ] 各RDP session内でAgentを起動し、両方が独立してBridge Readyになる
- [ ] 各Proxy portが対応するRDP sessionの許可済みHTTP endpointへ到達する
- [x] Agent接続済みsessionを閉じても、もう片方のRDPとmanagerが継続する
- [x] 閉じたport slotが次の新規sessionで再利用される
- [x] 通常利用の一件だけを最初に起動した場合、従来の`18080`設定で利用できる

実機では`ZRdpSvnBridge.exe`を複数process起動せず、一つの接続管理Windowから
二件を開始する。実接続先、domain、user、Proxy経由のURLは報告へ含めない。

2026-07-31の実機Gateでは、二件のうちAgent接続済みsessionを閉じた後、
別sessionが応答不能となり強制終了が必要だったため一度不合格とした。Version
`0.7.10.0`の専用STA修正後、別RDP／managerの継続、残存sessionの表示mode変更、
Proxy port再利用をユーザーが確認した。両sessionでAgent／Proxyを同時成立させる
追加Gateだけは未確認として残す。

## 14. Phase 9改善I-003 名前付き接続プロファイル

### ローカル

- [x] 接続管理Windowの親背景を`COLOR_BTNFACE`へ統一する
- [x] 64文字上限の任意接続名コンボボックスを追加する
- [x] 名前選択時に接続先、domain、userを一組で復元する
- [x] 同一接続組と同じ非空接続名を大小文字無視で重複排除する
- [x] current-user DPAPI履歴をVersion 2へ更新する
- [x] Version 1履歴を名前なしとして後方互換読込する
- [x] 接続名、接続先、domain、userの平文が暗号化fileにないことを確認する
- [x] 通常session titleへ`接続名 (接続先)`を表示する
- [x] Debugの全26 CTestを成功させる
- [x] Releaseの全26 CTestを成功させる
- [x] Version `0.7.3.0`のportable Client／Agentを`out`へ生成する

### ユーザー実機

- [x] ラベル／ラジオボタンの背景矩形が親Windowから浮いて見えない
- [x] 接続名を入力して認証完了後、再起動しても候補から選べる
- [x] 接続名の選択で対応する接続先、domain、userが復元される
- [x] 同じ接続名または同じ接続組が候補へ重複表示されない
- [x] 名前付きsession titleに接続名と実接続先の両方が表示される
- [x] 名前なし履歴と名前なし新規接続も従来どおり利用できる
- [x] RDP、Agent、Proxy、SVN／Trac操作に回帰がない

実機結果には接続名、実接続先、domain、userを記載せず、各項目の成否だけを
報告する。

## 15. Phase 9改善I-004 RDP音声リダイレクト

### ローカル

- [x] 影響範囲、security boundary、失敗時動作、rollbackを提案書へ記録する
- [x] 接続管理Windowへマイク転送check boxを追加し、既定OFFとする
- [x] リモート音声の再生先3種類をdrop-downへ追加する
- [x] `RdpConnectionSettings`を介してConnect前のActiveXへ適用する
- [x] setter後のマイクON／OFFと音声再生先をreadbackする
- [x] DPAPI履歴をVersion 3へ更新し、profile選択時に音声設定を復元する
- [x] Version 1／2履歴をマイクOFF／このPCで再生として読み込む
- [x] Debugの全26 CTestを成功させる
- [x] Releaseの全26 CTestを成功させる
- [x] Version `0.7.4.0`のportable Client／Agentを`out`へ生成する

### ユーザー実機

- [ ] 接続管理Windowにマイク転送とリモート音声の選択欄が表示される
- [ ] マイクOFFで従来どおり接続できる
- [ ] マイクONでクライアントPCの既定マイクを接続先sessionから利用できる
- [ ] `このPCで再生する`で接続先の音声がクライアントPCから再生される
- [ ] 名前付きprofileを再選択すると保存した音声設定が復元される
- [ ] 設定変更が新しいsessionまたは再接続後に反映される
- [ ] RDP画面、Agent、Proxy、SVN／Trac操作に回帰がない

GPOまたはWindows privacy設定で拒否された場合はpolicyを変更せず、利用できない
設定名と成否だけを報告する。音声、device名、接続先、ユーザー名をlogや
Git管理資料へ記録しない。

## 16. Phase 9改善I-005 手動Windowアスペクト比制約

### ローカル

- [x] 影響範囲、Windows標準操作との境界、失敗時動作、rollbackを提案書へ記録する
- [x] 接続を受理したsessionごとに初期desktop幅／高さを保持する
- [x] 非client領域と表示mode欄をRDP比率計算から除外する
- [x] 左右辺を幅基準、上下辺を高さ基準として補正する
- [x] 4角で反対角をanchorし、drag開始寸法から主操作軸を判定する
- [x] 最小Window寸法を比率維持したまま適用する
- [x] 不正比率、不正辺、反転矩形、overflow候補を適用しない
- [x] Debugの全27 CTestを成功させる
- [x] Releaseの全27 CTestを成功させる
- [x] Version `0.7.5.0`のportable Client／Agentを`out`へ生成する

### ユーザー実機

- [ ] 接続後に左右辺をdragし、RDP表示領域の比率が維持される
- [ ] 接続後に上下辺をdragし、RDP表示領域の比率が維持される
- [ ] 4角を縦横それぞれ主方向にdragし、不自然な跳躍なく追従する
- [ ] 等倍表示ではremote desktop寸法を変えず、必要箇所をscrollできる
- [ ] Smart Sizingではremote desktop寸法を変えず、比率を維持して縮小表示される
- [ ] 動的解像度では比率固定後の表示領域へ接続先desktopが追従する
- [ ] 最大化、Windowsスナップ、`Ctrl+Alt+Break`全画面は従来どおり動作する
- [ ] 複数sessionがそれぞれの接続時比率を独立して維持する
- [ ] RDP画面、Agent、Proxy、SVN／Trac操作に回帰がない

実機結果には接続時解像度、実接続先、ユーザー名、画面captureを記録せず、
各操作の成否だけを報告する。

## 17. Phase 9改善I-006 初回動的解像度の安定化

### ローカル

- [x] `OnConnected`では動的解像度timerを予約しない
- [x] `OnLoginComplete`後だけ表示modeの実行時変更を許可する
- [x] `UpdateSessionDisplaySettings`をログイン完了済み状態だけに限定する
- [x] 自動再接続完了後はログイン済み状態を復元して再適用する
- [x] 接続状態policyを単体テストする
- [x] 別実接続では`OnLoginComplete`の約0.24秒後にも`E_UNEXPECTED`が返ることを
      診断logで特定する
- [x] `E_UNEXPECTED`だけを500 ms、1,000 ms、1,500 ms後に有限再試行する
- [x] interface不在、不正引数、成功値、再試行上限後を再試行しない
- [x] Debug／Releaseの全29 CTestを成功させる
- [x] Version `0.7.10.0`のportable Client／Agentを`out`へ生成する

### ユーザー実機

- [x] 接続管理Windowで動的解像度を選択して新しい接続を開始する
- [x] 接続途中に表示mode変更エラーが表示されない
- [x] ログイン完了後、remote desktopがsession Windowの表示領域へ追従する
- [x] 等倍表示とSmart Sizingへ切り替え、再度動的解像度へ戻せる
- [x] Window resize後も動的解像度が継続する
- [ ] RDP画面、Agent、Proxy、SVN／Trac操作に回帰がない
- [x] Version `0.7.10.0`で再発した接続先へ動的解像度で接続し、途中の
      `E_UNEXPECTED`でerror dialogを表示せず自動回復する

## 18. Phase 9改善I-007 成立済みsessionの外部切断時終了

### ローカル

- [x] 現在の接続試行で`OnLoginComplete`を受信したか保持する
- [x] `OnAutoReconnected`をログイン済みsessionへの復帰として保持する
- [x] 成立済みsessionの非local切断だけを無通知終了対象にする
- [x] ログイン前接続失敗、local切断、credential取消を終了対象から除外する
- [x] 外部切断時は既存Bridge stream／Proxy経路を停止してからWindowを閉じる
- [x] 切断reasonをpayloadなしの数値診断logへ残す
- [x] 終了policyの単体テストを追加する
- [x] Debugの全28 CTestを成功させる
- [x] Releaseの全28 CTestを成功させる
- [x] Version `0.7.7.0`のportable Client／Agentを`out`へ生成する

### ユーザー実機

- [ ] RDPログイン完了後に接続先を再起動または安全な方法で切断する
- [ ] Microsoft Controlの自動再接続中はsession Windowが維持される
- [ ] 最終切断後にRDP接続エラーdialogが表示されない
- [ ] 切断された対象session Windowだけが自動的に閉じる
- [ ] 接続管理Windowと他のRDP sessionは維持される
- [ ] 新しいsessionを同じ接続profileから再度開始できる
- [ ] Agent、Proxy、SVN／Trac操作に回帰がない

初回認証失敗を再現する必要はない。問題発生時だけ、実IPやユーザー情報を除いた
切断reason、extended reason、Client診断logの固定eventを報告する。

## 19. Phase 9改善I-008 枠なしpopカラーicon

### ローカル

- [x] 外周の濃紺角丸tile、badge、containerを持たない
- [x] black、charcoal、navyの背景を含まない
- [x] PNG masterがRGBAで四隅のalphaが0である
- [x] chroma key由来のgreen不透明pixelが0である
- [x] subject占有率がmaster幅約96%／高さ約92%である
- [x] 黄色、gold、amber、limeのendpointを使用しない
- [x] 16、24、32、48pxを明背景と暗背景で確認する
- [x] ICOが16、20、24、32、40、48、64、128、256pxを持つ
- [x] Debug Client／Agentに共通icon resourceが存在する
- [x] Debug EXEから抽出した32px iconが新意匠である
- [x] Release 32px iconのalpha 128以上pixelが旧185から新339へ増加する
- [x] Release 32px iconの黄色系pixelが0である
- [x] Debugの全28 CTestを成功させる
- [x] Releaseの全28 CTestを成功させる
- [x] Version `0.7.9.0`のportable Client／Agentを`out`へ生成する

### ユーザー実機

- [ ] ExplorerでClient／Agentが枠なしの明るいiconとして表示される
- [ ] 接続管理Windowとsession Windowのtitle barに同じiconが表示される
- [ ] taskbarのsmall iconでもZ型Bridgeと二つのendpointを識別できる
- [ ] 明るいthemeと暗いthemeで黒いtile状に見えない
- [ ] RDP、Agent、Proxy、SVN／Trac操作に回帰がない

Windows icon cacheに旧iconが残った場合はsystem cacheを自動削除しない。
新しいEXEのtitle bar表示と、必要に応じて別folderへコピーしたExplorer表示で
確認する。

## 20. Version 0.7.10 複数session／動的解像度回帰

### ローカル

- [x] session WindowとRDP ActiveXをsession専用COM STAで生成・破棄する
- [x] manager STAは接続管理Window、port slot、完了通知だけを所有する
- [x] 一方の非接続ActiveXを破棄した後も他方のSTAが5秒以内に応答する
- [x] 動的解像度の一時失敗を有限回だけ再試行する単体testを追加する
- [x] close、disconnect、動的解像度logへsession識別用portを付与する
- [x] Debug／Releaseの全29 CTestを成功させる
- [x] Version `0.7.10.0`のportable Client／Agentを`out`へ生成する

### ユーザー実機

- [x] 動的解像度を選択した新規接続がerror dialogなしで追従する
- [x] 異なる二台へ同時接続し、一方のRDP session内だけでAgentを起動する
- [x] Agent接続済みsessionを閉じても、もう一方のRDP画面が操作できる
- [x] 同じ操作中も接続管理Windowが応答する
- [x] 残存sessionの表示modeを切り替えられる
- [x] 新規sessionを開始でき、解放済みProxy portが再利用される

Gate結果: 2026-07-31、ユーザーから全項目が期待どおり動作したとの受入結果を
得た。実接続先、ユーザー情報、URLは記録していない。

## 21. Phase 9改善I-009 全ソースコメント契約

### ローカル

- [x] `src`、`tests`、`assets/icons`の`.h`／`.cpp`／`.rc`全88 fileを走査する
- [x] 全fileで冒頭30行以内の`@file`／`@brief`を確認する
- [x] Doxygen監査で未説明compound／function／variableが0件である
- [x] 全loopの直前に目的コメントがある
- [x] 全test entryに検証目的と終了code契約がある
- [x] sourceへ作成者、手書きversion、手書き変更履歴を持ち込まない
- [x] Debug buildを`/W4 /WX`で成功させる
- [x] Debug CTestを30/30成功させる
- [x] Release Clientを一時出力先へlinkし、成果物契約を成功させる
- [x] Release CTestを30/30成功させる

標準Release出力のClientは実行中だったため上書きせず、同じRelease projectを
`work/tmp/release-comment-verify`へ出力してlinkを確認した。production動作、
wire protocol、Proxy、Agent、allowlist、設定形式には変更がないため、
実RDP／実SVN／実Tracの追加操作は要求しない。

## 22. Phase 9改善I-010 セッション全画面ボタン

### ローカル

- [x] 既存の全画面ActiveX APIとevent経路を再利用する
- [x] 表示mode一行へ全画面buttonを表示する
- [x] 動的解像度radioの右側へDPI対応配置する
- [x] 接続前は無効、接続成立後は有効とする
- [x] 全画面中の状態通知で表示を「ウィンドウ」へ変更する
- [x] Debug buildをwarningなしで成功させる
- [x] Debug CTestを30/30成功させる
- [x] Release Client／Agent候補をwarningなしでlinkする
- [x] Release回帰28件と成果物契約2件をすべて成功させる
- [x] Client／Agent候補がVersion `0.7.11.0`で一致する
- [x] 標準`out`へVersion `0.7.11.0`を生成する
- [x] 標準`out`を対象にRelease CTestを30/30成功させる

### ユーザー実機

- [x] 接続後の表示操作欄に「全画面」buttonが表示される
- [x] button押下でMicrosoft全画面へ移行する
- [ ] 全画面からWindow表示へ復帰できる
- [ ] `Ctrl+Alt+Break`でも従来どおり往復できる
- [ ] 復帰後も選択中の表示modeが維持される
- [ ] Agent、Proxy、SVN／Trac操作に回帰がない

実IP、接続名、domain、ユーザー名、接続先画面は結果へ記録しない。

## 23. 回帰修正R-002 動的解像度の全画面monitor追従

### ローカル

- [x] 全画面状態からtarget種別を選択する単体testを追加する
- [x] Window表示をRDP child領域へ対応させる
- [x] 全画面表示をセッションWindowのmonitor全体へ対応させる
- [x] 全画面移行とWindow復帰の両eventで250 ms debounceを予約する
- [x] 等倍表示とSmart Sizingを更新対象外に保つ
- [x] Debug buildとCTestを30/30成功させる
- [x] Release buildとCTestを30/30成功させる
- [x] Client／AgentをVersion `0.7.12.0`で一致させる
- [x] 標準`out`のportable EXEを更新する

### ユーザー実機

- [ ] 動的解像度で接続し、通常Windowのremote desktopが表示領域へ追従する
- [ ] 全画面buttonでremote desktop自体がmonitor全体へ拡大する
- [ ] connection barまたは`Ctrl+Alt+Break`でWindowへ戻り、remote desktopが
      RDP表示領域へ再追従する
- [ ] `Ctrl+Alt+Break`から全画面へ入る経路でも同じ結果になる
- [ ] 等倍表示ではremote desktop解像度を維持して必要時にscrollする
- [ ] Smart Sizingではremote desktop解像度を維持してlocal縮小する
- [ ] 表示mode error dialogが出ず、Agent／Proxy／SVN／Tracに回帰がない

## 24. 回帰修正R-003 ログイン前SSL handshake一時失敗の限定再試行

### ローカル

- [x] 失敗試行が設定適用、CredSSP、Virtual Channel登録、`Connect()`成功後、
      `OnConnected`前の`263/0`で終了したことを確認する
- [x] Windows RDP Client event logの同時刻にSSL handshake継続失敗があることを
      確認する
- [x] login未完了、local切断でない、credential取消でない、終了中でない、
      `263/0`完全一致、未再試行の全条件を単体testする
- [x] 一条件でも異なる場合と、一回予約済みの場合を再試行しない
- [x] retry delayを2,000 ms、上限を一回に固定する
- [x] COM callback内では`Connect()`せず、Window timerから設定再適用後に開始する
- [x] 予約取消、stale timer、同期的失敗、二度目失敗で停止する
- [x] 固定診断eventへ自由形式、接続先、credential、passwordを追加しない
- [x] Debug buildとCTestを30/30成功させる
- [x] 一時出力先のRelease buildとCTestを30/30成功させる
- [x] Client／Agent候補がVersion `0.7.14.0`で一致する
- [x] 候補のSHA-256と未署名状態を確認する
- [x] 実行中Client停止後に標準`out`のportable EXEを更新する
- [x] 標準`out`を対象にRelease CTestを30/30成功させる

### ユーザー実機

- [x] 通常の初回接続が従来どおり成立する
- [ ] 自然再発した場合、error dialogや`mstsc.exe`の事前接続なしで一回だけ
      自動復旧する
- [ ] 自然再発時のlogが予約、開始、成功の固定eventを同じsession portで示す
- [ ] 二度続けて失敗した場合は追加再試行せず、従来のerrorを表示する
- [ ] Microsoft認証UIの取消では再試行も追加errorも表示しない
- [ ] 成立済みsessionの外部切断時終了、表示mode、Agent、Proxy、SVN／Tracに
      回帰がない

自然再発を強制するためのserver再起動、security設定変更、証明書操作は行わない。
通常接続を確認し、再発した場合だけ固定eventで限定再試行を判定する。

2026-07-31、ユーザーからVersion `0.7.13.0`の起動とRDP接続成功の報告を得た。
Version `0.7.14.0`では接続中Window終了経路の実機再確認を追加Gateとする。

### 接続中Session終了の残留process回帰

- [x] 未接続では直ちにWindow破棄を選ぶ純粋policy test
- [x] 接続中では`Disconnect()`要求を選ぶ純粋policy test
- [x] 既に切断中では二重要求せず完了待ちを選ぶ純粋policy test
- [x] Debug／Release各30 CTest
- [ ] 実RDP接続中に閉じるbuttonを押し、`OnDisconnected`後にWindowが閉じる
- [ ] 接続管理Windowも閉じ、`ZRdpSvnBridge.exe` processが残留しない
- [ ] 固定診断logが`rdp.disconnect_call_returned`、`rdp.event.disconnected`、
      `process.stopping`へ到達する
Client固定診断logでも同Versionの`process.started`後に
`rdp.event.login_complete`へ到達し、通常成功経路にretry eventがないことを
確認した。自然再発は未発生のため、限定再試行経路は継続監視とする。

## 25. 回帰修正R-005 ログイン前再試行の二段階化

### ローカル

- [x] 同一事象で`mstsc.exe`も二回失敗し、三回目の通常handshakeで成立したことを
      Client／Windows固定診断logから確認する
- [x] 永続的なRDP接続履歴または保存済み`TERMSRV`資格情報の有無を原因と断定しない
- [x] R-003の対象条件と除外条件を変更せず、delayだけを2,000 ms、5,000 msの
      二段階へ拡張する
- [x] 初回を含む三回目の失敗後は追加再試行せず、従来のerrorを表示する
- [x] 二段階のdelay、上限到達、全除外条件を純粋policy testで確認する
- [x] Debug／Release各30 CTestを成功させる
- [x] Releaseでは`ZRdpSvnBridge.exe`だけをbuildする
- [x] 作業前後の`ZRdpSvnAgent.exe` SHA-256が一致することを確認する
- [x] Client候補がVersion `0.7.14.0`であることと、署名前正本を保全する
- [x] Clientだけを署名し、Windows trust `Valid`、SHA-256、RFC 3161 timestamp、
      既存Agentと同一署名者であることを確認する

### ユーザー実機

- [ ] 通常接続が従来どおり成立する
- [ ] 自然再発時、初回または有限再試行内でRDP接続が成立する
- [ ] 初回を含む三回とも同じ条件で失敗した場合だけ従来のerrorを表示する
- [ ] Microsoft認証UIを取り消した場合は再試行しない
- [ ] 接続成立後のAgent、Proxy、SVN／Trac、複数session、表示modeに回帰がない

自然再発を強制するための接続先再起動、security設定変更、証明書操作は行わない。
今回の候補はClientだけを差し替え、既存Agentを継続使用する。

2026-08-03、新しい接続先で初回と二回のretryがすべて`263/0`となり、password入力を
三回要求した。Windows標準Clientとの比較で、同Clientはcredential入力後にserver
certificate警告を表示し、利用者の明示継続後にdesktopへ到達した。R-005の回数不足
ではなく、ActiveX security dialog表示の互換性をR-006として修正する。

## 26. 回帰修正R-006 certificate警告ダイアログ互換性

### 解析／設計

- [x] ZRdpの認証dialog eventと`263/0`を時系列で確認する
- [x] Windows標準Clientでcredential UI後にcertificate警告が表示され、明示継続後に
      desktopへ到達する差分を確認する
- [x] `AuthenticationLevel=2`を維持し、値`0`／`1`、自動承認、証明書登録を採用しない
- [x] Microsoft公開の`RedirectionWarningDialogVersion=1`を互換候補として採用する
- [x] ADR 0010と修正提案へActiveX、security、protocol非影響、rollbackを記録する

### ローカル

- [x] `IMsRdpExtendedSettings::Property`へ`VT_UI4`の値`1`を接続前に設定する
- [x] `ShowRedirectionWarningDialog=TRUE`候補が既存接続を壊すことを実機確認し撤回する
- [x] `AuthenticationLevel=2`をsetter後にreadbackし、不一致をfail-closedにする
- [x] 認証dialog表示中は親WindowからActiveX childへfocusを移し直さない
- [x] Microsoft認証dialog表示後の`263/0`を自動retryから除外する
- [x] 既存の2秒／5秒retryが認証UI表示前にだけ残ることを純粋policy testで確認する
- [x] ActiveX smoke、retry policy、固定診断logの対象testを成功させる
- [x] Debug／Release全CTestを成功させる
- [x] Release Clientだけを更新し、AgentのSHA-256が変化しないことを確認する
- [x] ClientのAuthenticode状態とSHA-256を確認する

初回実機候補ではVersion 1とAuthenticationLevelの設定は成功したが、二回目のwarning
eventが約1 msでdismissされて`263/0`となった。password要求を一回で止めるretry除外は
成功した。`ShowRedirectionWarningDialog=TRUE`候補は既存接続先でも`Connect()`が
`S_FALSE`、`disconnect code=1`となったため撤回した。

### ユーザー実機

- [ ] credential入力後にWindows標準certificate警告が表示される
- [ ] 警告で「はい」を選ぶとdesktopへ到達する
- [ ] 警告で「いいえ」を選ぶとpassword再入力や追加retryなしに未接続へ戻る
- [x] R-006回帰撤回版で通常の信頼済み接続先へ従来どおり接続できる
- [ ] 接続後のAgent、Proxy、SVN／Trac、複数session、表示modeに回帰がない
- [ ] 固定logで`dialog_version=1 enabled=1`、`authentication_level=2`を確認する

実機確認ではcertificateを登録せず、「今後確認しない」等の永続化optionも選ばない。
ZRdpが警告を自動承認せず、利用者の今回の明示判断だけで接続できることを確認する。

## 27. 回帰修正R-007 ActiveX既定の認証ダイアログ親Window

### 解析／設計

- [x] 回帰撤回版で従来接続先が復旧し、問題接続先だけが`263/0`で残ることを確認する
- [x] Windows固定診断で`TsSslEventHandshakeContinueFailed (0x80004005)`を確認する
- [x] `UIParentWindowHandle`の未設定時はActiveX Control Windowが既定親となる
      Microsoft公開契約を確認する
- [x] 修正提案へActiveX、security、protocol非影響、rollbackを記録する

### ローカル

- [x] session top-level Windowへの`UIParentWindowHandle`明示setterを削除する
- [x] `rdp.activex_default_dialog_parent_selected`を固定eventとして追加する
- [x] 対象のActiveX smoke、diagnostic log、source comment testを成功させる
- [x] Debug／Release全30 CTestを成功させる
- [x] Release Clientだけを更新し、AgentのSHA-256が変化しないことを確認する
- [x] ClientのVersion、Authenticode状態、size、SHA-256を記録する

### ユーザー実機

- [x] R-007候補でも従来接続先へ回帰なく接続できる
- [x] credential入力後にcertificate警告が操作可能な状態で表示される
- [x] 警告で「はい」を選ぶとdesktopへ到達する
- [x] 警告で「いいえ」を選ぶと`263/0`後に重複したRDP接続errorが表示される問題を再現する
- [ ] 接続後のAgent、Proxy、SVN／Trac、複数session、表示modeに回帰がない
- [x] 固定logに`rdp.activex_default_dialog_parent_selected`、`dialog_version=1`、
      `authentication_level=2`が記録される

2026-08-03の実機logでは、credential UIに対応する最初のwarning eventが約6秒、
certificate UIに対応する二回目が約4秒表示された。二回目のdismiss後に自動retryを
挟まず`OnConnected`、`OnLoginComplete`へ到達し、利用者操作による切断まで正常に
維持された。

NLA、CredSSP、server authentication、certificate検証は変更しない。警告の自動承認、
certificate store／registry／GPO／RDP cache操作、`mstsc.exe` fallbackは行わない。

## 28. 回帰修正R-008 certificate警告拒否後の重複error抑止

### 解析／設計

- [x] ActiveXが警告buttonの選択値を直接通知しない制約を確認する
- [x] warning二回以上、login未完了、非local、credential取消以外、`263/0`完全一致へ
      分類を限定する
- [x] 修正提案へ誤分類可能性、security／protocol非影響、rollbackを記録する

### ローカル

- [x] warning表示回数を接続試行開始時にresetし、二回で飽和させる
- [x] 完全一致時にretryと追加errorを行わず、対象session Windowだけを閉じる
- [x] 固定field `security_warning_cancelled`を追加し、秘密情報を記録しない
- [x] true条件とwarning一回、login済み、local、credential取消、reason違い、
      extended reason違いの全除外条件を自動testする
- [x] Debug／隔離Release各30 CTestを成功させる
- [x] 標準`out`のresource／portable binary contractを成功させる
- [x] AgentのSHA-256が作業前後で不変であることを確認する

### ユーザー実機

- [x] 従来接続先へ回帰なく接続できる
- [x] certificate警告で「いいえ」を選ぶと追加errorなしで対象session Windowだけが閉じる
- [x] 接続管理Windowが維持される
- [ ] 同時接続中の他sessionが影響を受けない
- [ ] certificate警告で「はい」を選ぶ接続とAgent／Proxyに回帰がない
- [ ] 固定logに`security_warning_cancelled=1`と`263/0`が記録される

certificateの登録、警告の永続承認、server設定変更は行わない。通常の利用者操作だけで
確認し、接続先、certificate名、domain、ユーザー名を報告へ含めない。

2026-08-03、ユーザー実機で「いいえ」後に追加errorなしで対象session Windowだけが
閉じる期待動作を確認した。受入後のClientは既存Agentと同じ署名者で署名し、Windows
trust `Valid`、SHA-256 signature、RFC 3161 timestamp、Version維持を静的確認した。
署名後Clientの起動、RDP、Bridge、Proxyは別Gateとして残す。

## 29. 報告してほしい情報

成功時はチェック項目と概算所要時間だけでよい。失敗時は次の
固定情報を報告する。

- Clientのsession titleに表示されたRDP／Agent／Proxy状態
- Agent consoleの固定message
- Agentの終了code
- Client log `ZRdpSvnBridge.log`の`bridge.*` eventと数値field
- Agent log `ZRdpSvnAgent.log`の`agent.*`／`bridge.*` eventと数値field
- RDP自体も切れた場合だけ`Disconnect reason`と`Extended reason`

次は報告へ含めない。

- RDP／SVN／Trac password
- 実接続先名またはIP
- ドメイン、ユーザー名
- 支給PCまたは取引先画面のcapture
- Virtual Channel payload
- 実SVN／Trac URL、repository名、ticket、comment、attachment、source、diff

Phase 1 Gateは、実RDP上のEchoと同一session内のAgent再起動／再Echoを
2026-07-30のClient／Agent logで確認して通過した。Phase 4はproduction
Client／Agent経路からloopback Mock Serverまでのlocal Gateを通過した。Phase 5は
2026-07-31にMock WebDAVの複数connection／read-write相当method sequenceを確認した。
Phase 6は同日にAgent health、slow peer、restart／reconnect、security boundaryを
local完了し、実機のAgent強制終了／再起動でも回帰成功した。Phase 7／8は同日の
ユーザー実操作と受入結果によりGateを通過した。
