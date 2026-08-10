# ZRdp SVN Bridge 開発ロードマップ

更新日: 2026-08-03

## 1. ロードマップの目的

本ロードマップは、開発 PC 上の SVN クライアントから、RDP セッション内の Static Virtual Channel と支給 PC 上の Agent を経由し、Zscaler を利用した支給 PC の既存 HTTP 通信で取引先 SVN サーバーへ到達するまでの開発順序と Gate を定義する。

最終的に成立させる経路は次のとおりである。

```text
開発 PC の SVN クライアント
  -> 127.0.0.1 HTTP Proxy
  -> ZRdpSvnBridge.exe
  -> 既存 RDP セッション内の ESVNBRG
  -> 支給 PC 上の ZRdpSvnAgent.exe
  -> 支給 PC の既存 HTTP 通信
  -> Zscaler
  -> allowlist 登録済み取引先 HTTP SVN サーバー
```

RDP ActiveX で画面接続できることは必要な前提条件だが、プロジェクトの完了ではない。Phase 0 を通過した時点では「RDP ActiveX 接続 Gate 通過、SVN Bridge 未完成」と明示する。

## 2. 開発原則

- Phase は原則として順番に進め、各 Gate の未解決事項を後続 Phase へ持ち込まない。
- 製品版の接続は常に **RDP + SVN Bridge** とし、RDP 専用モードは設けない。
- Bridge 障害時も RDP 画面接続は可能な限り維持する。
- Zscaler、Firewall、EDR、GPO、ルーティング、OS 全体 Proxy を変更または回避しない。
- Client は loopback 以外で HTTP Proxy を待ち受けない。
- Agent は `IPv4 address + TCP port` の完全一致 allowlist 以外へ接続しない。
- 実取引先環境へのアクセスは、該当 Phase でユーザーから明示指示を受けた場合だけ行う。
- Static Virtual Channel の不足は推測で判断せず、実測後に ADR とユーザー承認を経て DVC を検討する。
- 必要な開発ツールは既存環境を優先し、追加が必要な場合は原則 `C:\develop\tools` 配下へ配置して、用途、version、配布元、license を記録する。
- vcpkg、Conan、NuGet、製品の外部 runtime dependency は、開発ツール追加とは分離し、個別のユーザー承認なしに導入しない。

## 3. 現在地

| 項目 | 状態 | 備考 |
|---|---|---|
| 要件とセキュリティ境界 | 完了 | `AGENTS.md` に定義済み |
| 開発環境確認 | 完了 | `work/analysis/20260730_development_environment.md` |
| 標準フォルダ構成 | 完了 | `src`、`tests`、`docs`、`config`、`work` |
| `.gitignore` | 完了 | build、binary、log、capture、実環境設定等を除外 |
| Git repository 初期化 | 未着手 | ユーザー指示前に remote operation は行わない |
| CMake build scaffold | 完了 | x64 Debug／ReleaseとCTestを確認済み |
| ActiveX host／CRT判断 | 完了 | ADR 0001／0002 |
| Phase 0 RDP Gate | 通過 | 2026-07-30 ユーザー受入済み。Phase 6実機回帰も完了 |
| Phase 1 Gate | 通過 | 実RDP Echo、Ctrl+C後の同一session再接続、再Echoをlog確認済み |
| Phase 2 protocol core | ローカル完了 | transport非依存multiplexer、fake Socket、bounded flow control |
| Phase 3 Agent TCP | ローカル完了 | allowlist完全一致、単一Socket、loopback Mock Server |
| Phase 4 Client HTTP Proxy | ローカル完了 | loopback限定listener、HTTP変換、Client／Agent結合経路 |
| Phase 5 HTTP／SVN互換性 | ローカル完了 | chunked、認証、WebDAV sequence、8同時stream |
| Phase 6 Hardening | Gate通過 | health timeout、Agent強制終了／再起動、RDP継続を実機確認 |
| Phase 7 実環境Read-only | Gate通過 | 2026-07-31 ユーザー受入済み |
| Phase 8 実環境Write | Gate通過 | 2026-07-31 Commit等を含む全操作をユーザー受入済み |
| Phase 9 改善 | R-008 Gate通過 | certificate警告拒否後に重複errorなしで対象sessionだけが閉じることを実機確認 |
| Documentation Gate | 文書受入待ち | Version 0.7.14の仕様書、利用手順書、リリース文書へ同期済み |
| Release Gate | R-008署名後smoke待ち | ClientをAgentと同じ署名者で再署名し、Windows trustとRFC 3161 timestampを確認 |

## 4. 全体ロードマップ

```text
開発基盤
  -> Phase 0: RDP ActiveX Gate
  -> Phase 1: ESVNBRG Echo
  -> Phase 2: Protocol／多重化
  -> Phase 3: Agent制限付きTCP
  -> Phase 4: localhost HTTP Proxy
  -> Phase 5: HTTP／SVN互換性
  -> Phase 6: Hardening
  -> Phase 7: 実環境Read-only
  -> Phase 8: 実環境Write
  -> Phase 9: 改善項目を一件ずつ実施
  -> Documentation Gate
  -> Release Gate
```

| 段階 | 主成果 | Gate の性質 | 状態 |
|---|---|---|---|
| 開発基盤 | 再現可能なx64 C++20 build／test基盤 | ローカル | 完了 |
| Phase 0 | Window／全画面／clipboard対応RDP Client | ユーザー環境 | Gate通過（2026-07-30） |
| Phase 1 | `ESVNBRG`上の双方向Echo | ユーザー環境 | Gate通過（2026-07-30） |
| Phase 2 | 安全なframe protocolと複数stream | ローカル | 完了 |
| Phase 3 | allowlist限定Agent TCP接続 | Mock環境 | 完了 |
| Phase 4 | loopback限定HTTP Proxy | Mock環境 | 完了 |
| Phase 5 | SVN read／write相当sequence | Mock／Test環境 | 完了 |
| Phase 6 | 異常系、資源制限、切断耐性 | ローカル／Test／ユーザー環境 | Gate通過（2026-07-31） |
| Phase 7 | 実SVNのread-only操作 | 明示承認済み実環境 | Gate通過（2026-07-31） |
| Phase 8 | 指定test pathのwrite操作 | ユーザー確認済み実環境 | Gate通過（2026-07-31） |
| Phase 9 | ユーザー要望を一件ずつ改善 | 項目ごとの実装／検証／受入 | R-005実装済み、実機確認待ち |
| Documentation Gate | 最終仕様書と利用手順書 | 社内試行で残存項目を明示 | 文書作成済み、ユーザー受入待ち |
| Release Gate | 署名済みportable配布物 | 実機終了確認、ユーザー署名／承認 | 署名後smoke test通過、文書受入／配布確認待ち |

## 5. 準備段階: 開発基盤

### 実装・文書

- [x] ルート`CMakeLists.txt`と`CMakePresets.json`を作成する。
- [x] Visual Studio 2022、x64、Unicode、C++20を固定する。
- [x] project sourceへ`/W4`、`/WX`、`/permissive-`、`/Zc:__cplusplus`を適用する。
- [x] Debug／Releaseの中間生成物とtest binaryを`build/`以下へ生成する。
- [x] 実機確認用Release EXEをGit管理外の`out/`直下へ出力する共通規則を定義する。
- [x] 外部test frameworkを追加せずに開始できる最小test harnessとCTest登録を作成する。
- [x] Client、Agent、common、protocolのtarget境界を定義する。
- [x] ActiveX host方式を`work/proposals`へ記録し、ATL `CAxWindow`採用をADRへ記録する。
- [x] CRTを`/MT`とし、portable配布とsecurity servicing上の責任をADRへ記録する。
- [x] 現存する全C++ source／header／testへコメント規約を遡及適用し、監査結果を記録する。
- [x] ZRdp共通アプリアイコンを作成し、Client／AgentのEXEとwindowへ埋め込む。
- [x] Client／Agentの診断logを役割別の固定file名へ分離する。

### 終了条件

- [x] clean workspaceからconfigure、x64 Debug／Release build、CTestを実行できる。
- [x] warningが0件である。
- [x] build生成物が`build/`以下に限定される。
- [x] runtime dependencyと配布方針が文書化されている。

## 6. Phase 0: RDP ActiveX接続成立確認

### 実装

- [x] `ZRdpSvnBridge.exe`の最小Win32 UIを作成する。
- [x] COM STA UI thread上で、利用可能な最新のRemote Desktop ActiveX Controlを作成する。
- [x] 接続先PC名またはIP、ドメイン、ユーザー名、接続、切断を実装する。
- [x] 接続先、ドメイン、ユーザー名を編集可能な履歴コンボボックスとし、認証完了した直近10組をユーザー単位のDPAPIで暗号化保存する。
- [x] 同一の接続先＋ドメイン＋ユーザー名を大小文字を区別せず1組へ統合し、各コンボボックス内でも候補を重複させない。
- [x] ActiveXの`Domain`と`UserName`を接続前に別々に設定し、旧`DOMAIN\user`入力と履歴を分離して扱う。
- [x] ログイン前のMicrosoft標準credential UIキャンセルを限定分類し、追加のRDP接続error dialogを抑止する。
- [x] 接続履歴のfile I/OとDPAPI処理を有限queueのworkerへ分離し、結果だけをWindow MessageでUI threadへ返す。
- [x] 等倍表示とSmart Sizingの初期remote desktop寸法を、Windows標準RDPの全画面接続と同じ現在のmonitor全体とする。
- [x] ActiveXまたはWindows標準認証UIを使用し、passwordを保存しない。
- [x] CredSSPを接続前に明示的に有効化し、NLA対応状態を読み戻して検証する。
- [x] ウィンドウモードと全画面モードを実装する。
- [x] 既定では初期remote desktop寸法を維持し、Smart Sizingを無効にしてActiveX標準のscroll barを使用する。
- [x] 等倍表示、Smart Sizing、動的解像度を3つの排他的なラジオボタンとして実装する。
- [x] Smart Sizingではremote desktop寸法を維持し、Client側描画の拡縮だけを行う。
- [x] 動的解像度では250 ms debounce後に`UpdateSessionDisplaySettings`でremote desktop寸法をRDP表示領域へ追従させる。
- [x] 動的解像度では通常WindowのRDP領域と全画面monitorへ追従し、等倍表示とSmart Sizingではremote desktop寸法を維持する。
- [x] 接続先desktop領域を固定値へ変更するFull HD／WQHD／Full選択欄を廃止し、window変更による`Reconnect`を使用しない。
- [x] UI操作および標準`Ctrl+Alt+Break`で全画面を切り替えられる設定とevent反映を実装する。
- [x] 全画面connection barとwindow titleに製品名と現在の接続先を表示する。
- [x] `RedirectClipboard`を接続前に明示的に有効化する。
- [x] Client／接続状態、warning、fatal error、disconnect reasonを秘密情報なしで表示する。
- [x] 予期しない切断時にextended reasonとWindows由来の説明を取得し、COM callback復帰後にerror dialogを表示する。
- [x] 標準認証dialogの親windowを明示し、接続要求前に進行状態を描画する。
- [x] 固定event／数値fieldだけを扱うbounded queue式診断logと1 MiB・3世代rotationを実装する。
- [x] Connection Pointの購読解除、COM interface、window、BSTR等の所有権をRAIIで管理する。

### ローカル確認

- [x] ActiveX生成、設定適用、window破棄を自動smoke testで確認する。
- [x] 空の接続先をUIで拒否し、RDP通信を開始しないことを確認する。
- [x] COM callbackからC++例外を漏らさない境界を実装する。
- [x] 診断logへpassword、接続先、ドメイン、ユーザー名、clipboard内容、payloadを渡せないことを確認する。
- [x] 診断logのrotationと固定field出力を自動testで確認する。
- [x] 接続履歴の暗号化、再読込、重複排除、10件上限、破損data拒否、worker経由の非同期保存を自動testで確認する。
- [x] domain分離、旧`DOMAIN\user`互換、UPN維持、曖昧入力拒否、旧履歴formatへの再結合を自動testで確認する。
- [x] credential promptキャンセルreasonをログイン前だけ正常な利用者取消として分類する自動testを実装する。
- [x] 3つの表示mode resource、既定等倍radio、未接続時の動的解像度API拒否を自動testとUI probeで確認する。
- [x] 文書用IPv4 literalで全画面connection bar表示を接続前に設定し、同じ有限長文字列をreadbackできることを自動testで確認する。
- [x] 実RDP接続からの正常disconnectを確認する。
- [x] 接続中キャンセルと再接続を実RDP接続で確認する。

### ユーザー環境Gate

ZscalerをONのまま、現在`mstsc.exe`で接続可能な支給PCに対して次を確認する。

- [x] NLA／CredSSPによるRDP接続成立
- [x] Microsoft標準credential UI
- [x] 分離入力したドメインがMicrosoft標準credential UIのアカウントへ反映される
- [x] Microsoft標準credential UIをcloseしても追加のRDP接続error dialogが表示されない
- [x] keyboard、mouse、継続的な画面更新
- [x] ウィンドウモードでの画面表示
- [x] 全画面への移行と復帰
- [x] 既定の等倍表示でmonitor全体をbaselineとするremote desktop寸法が維持され、狭いwindowではscrollできる
- [x] 等倍表示でウィンドウ変更時に接続先アプリのレイアウトが変化しない
- [x] Smart SizingではClient側表示倍率だけがwindowへ追従し、remote desktop寸法は変化しない
- [x] 動的解像度ではremote desktop寸法がwindowへ追従し、他modeへ切り替えると追従が停止する
- [x] 3つの表示modeがラジオボタンとして排他的に選択できる
- [x] 全画面connection barに現在の接続先が製品名とともに表示される
- [x] 認証完了した接続先／ドメイン／ユーザー名を再起動後に履歴から選択でき、重複候補がない
- [x] 開発PCから支給PCへのプレーンテキストclipboard
- [x] 支給PCから開発PCへのプレーンテキストclipboard
- [x] 正常切断
- [x] 切断後の再接続
- [x] 認証warningが表示され、自動承認されないこと

### 終了条件

- 上記確認結果を記録し、ユーザーがPhase 0 Gate通過を明示する。
- ActiveX Clientだけ接続できない場合は回避を試みず、その結果を設計判断として返す。
- この時点ではSVN利用可能とは判定しない。

Gate結果: 2026-07-30、ユーザーから基本的なRemote Desktop機能が期待どおりとの
受入結果と、SVN透過機能へ進む指示を受領した。当時の個別未確認項目は後続の
Phase 6実機回帰と2026-07-31の全操作受入で確認済みとして閉じた。

## 7. Phase 1: Static Virtual Channel Echo

### 実装

- [x] すべての製品接続で`Connect`前に`CreateVirtualChannels("ESVNBRG")`を呼ぶ。
- [x] Clientからの送信をbounded queue経由でCOM UI threadへmarshalする。
- [x] `OnChannelReceivedData`内でBSTR byte長を取得し、callback中に所有bufferへコピーする。
- [x] `ZRdpSvnAgent.exe`をRDP対話ユーザーセッション内で起動するportable EXEとして作成する。
- [x] Agentが自身のSession IDを検証し、対象外セッションでは安全に終了する。
- [x] Agentが`WTSVirtualChannelOpenEx(WTS_CURRENT_SESSION, "ESVNBRG", 0)`でchannelを開く。
- [x] 一つのAgent I/O loopでreadとserialized writer queueを順に処理する。
- [x] `HELLO`／`HELLO_ACK`および`PING`／`PONG`を実装する。
- [x] Client／Agentのframe decoderと送信queueを有限化する。
- [x] 未応答PINGを15秒でBridge timeoutとし、RDP画面を維持する。
- [x] AgentのCtrl+C終了をCLOSEで通知し、同じRDP内で再起動可能にする。
- [x] Agent待機中だけHELLOを再送し、handshake後はactive streamをresetしない。
- [x] Protocol、Agent、architecture、security、実機手順を文書化する。

### 検証

- [x] fake transportでfragmentationと再構成を確認する。
- [x] fake SVCで1 byte、1 KiB、64 KiB、1 MiB、10 MiBを分割送信する。
- [x] 埋込みNUL、欠落相当、順序、byte完全一致、version不一致を自動確認する。
- [x] Agent未起動、Echo timeout、RDP切断相当resetを自動確認する。
- [x] Agent正常終了／異常終了相当からの同一RDP内再起動を自動確認する。
- [x] Debug buildとCTestをwarningなしで完了する。
- [x] Release buildとCTest、portable出力、依存DLLを確認する。
- [x] 実RDP sessionで合計11,600,897 byteのEchoを確認する。
- [x] Ctrl+C終了後、同じRDP sessionでAgentを再起動してReadyへ戻る。
- [x] 再起動Agentで合計11,600,897 byteのEchoを再確認する。
- [x] Agent終了とRDP切断を確認する。
- [x] Bridge障害後もRDP画面操作を継続できることを確認する。

### 終了条件

- `ESVNBRG`の双方向binary transportが成立する。
- BSTRを文字列として扱わず、埋め込みNULを含むpayloadを保持できる。
- この時点ではHTTP／SVN中継を実装しない。

## 8. Phase 2: Application Protocolと多重化

### 実装

- [x] Phase 1で確定したmagic、version、message type、streamId、payload lengthを持つ
  headerを維持する。
- [x] Phase 1のbyte orderと上限を維持し、最大32 streamと256 KiB initial windowを定義する。
- [x] `HELLO`、`HELLO_ACK`、`OPEN`、`OPEN_OK`、`OPEN_ERROR`、`DATA`、
  `WINDOW_UPDATE`、`HALF_CLOSE`、`CLOSE`、`PING`、`PONG`、
  `PROTOCOL_ERROR`を実装する。
- [x] incremental frame parser、fragment reassembly、stream state machineを実装する。
- [x] stream単位とconnection全体のflow control、bounded queue、backpressureを実装する。
- [x] Client-only OPEN、固定IPv4+port payload、単調増加stream IDを実装する。
- [x] normal／timeout／peer reset／crossed closeを有限状態で処理する。
- [x] RDP再接続相当のsession resetで旧stream ID、sequence、credit、queueを再利用しない。

### 検証

- [x] frame encode／decode
- [x] fragmented frame
- [x] 1回の受信に複数frame
- [x] invalid magic／version／type
- [x] oversized payload、integer overflow
- [x] duplicate streamId、duplicate DATA sequence、invalid transition
- [x] stream／connection window exhaustionとWINDOW_UPDATEによるrecovery
- [x] outbound 4,096 frame、inbound 4,096 event、各4 MiBのqueue upper bound
- [x] half-close、timeout、peer reset、crossed close
- [x] Agent absent、version mismatchのPhase 1回帰
- [x] fake Socketで3 stream、双方向約2.3 MiBのbyte完全一致
- [x] Debug全14 CTestをwarningなしで完了する
- [x] Release全14 CTestをwarningなしで完了する

### 終了条件

- in-memory transportだけで複数streamを安全に多重化できる。
- malformed inputとresource exhaustionをfail-closedで処理できる。

Phase 2の`StreamMultiplexer`はSocket、COM、WTS APIを呼ばない共有protocol層である。
実行EXEのPhase 1 Echo経路への接続は維持し、Phase 3でAgent I/O ownerと
allowlist限定Socket adapterへ組み込んだ。

## 9. Phase 3: Agentのallowlist限定TCP接続

### 実装

- [x] 実環境値を含まないallowlist設定templateを`config/examples`へ作成する。
- [x] `IPv4 address + TCP port`完全一致のparserとvalidationを実装する。
- [x] duplicate、不正IPv4、不正port、空設定を明示的に拒否する。
- [x] Agentが`OPEN`受信後、allowlist確認に成功した場合だけupstream Socketを作成する。
- [x] Agentからinbound TCP listenを行わない。
- [x] non-blocking connect、有限I/O、5秒timeout、単一stream上限を実装する。
- [x] Mock HTTP相当Server一台への接続、DATA中継、half-close、closeを実装する。
- [x] 設定不備／Winsock失敗時にEcho-onlyへ安全に縮退する。

### 検証

- [x] allowlist exact matchと全拒否条件
- [x] allowlist外ではconnector呼出しが0回であること
- [x] connect timeout、connection refused、peer reset
- [x] partial send、WouldBlock、flow-control credit回復
- [x] Agent終了／RDP切断相当の`StopSession`で全Socket終了
- [x] production Winsock adapterでloopback Mock Serverへ単一stream接続
- [x] 512 KiB超の双方向byte完全一致、half-close、normal close
- [x] production Agent sourceに`bind`／`listen`／`accept`／DNS APIがないこと
- [x] log APIが実IP、HTTP内容、credential、自由形式文字列を受け付けないこと
- [x] Debug全17 CTestをwarningなしで完了する
- [x] Release全17 CTestとportable成果物を確認する

### 終了条件

- Mock Serverに限った単一streamの往復通信が成立する。
- allowlist拒否時に別経路へfallbackしない。
- 実取引先SVNには接続しない。

Phase 3 Agentは完了し、Phase 4でClient runtimeから同じstream protocolへ接続した。
ただし、この時点ではMock HTTP Serverまでの単一connectionだけを完了条件とし、
通常のClient／Agent実行だけで実SVNを利用できるとは扱わない。

## 10. Phase 4: localhost HTTP Proxy

### 実装

- [x] Clientが既定で`127.0.0.1:18080`だけをlistenする。
- [x] loopback以外のbindを構成上も実装上も拒否する。
- [x] HTTP request lineとheaderのincremental parserを実装する。
- [x] absolute-formをorigin-formへ変換する。
- [x] IPv4 literal、明示portまたはHTTP既定port 80だけを受け付ける。
- [x] authorityと`Host` headerの一致を検証する。
- [x] `CONNECT`、`Upgrade`、宛先変更を拒否する。
- [x] hop-by-hop headerをHTTP規則に従って処理する。
- [x] bodyを全量保持せずstreamとしてBridgeへ渡す。
- [x] Client multiplexer、UI／worker有限queue、ActiveX送信marshalを接続する。
- [x] Bridge未ready時に固定`503`を返し、RDP画面を維持する。

### 検証

- [x] loopback-only bind
- [x] malformed request line
- [x] absolute-formからorigin-form
- [x] Host欠落／不一致
- [x] Keep-Alive中の宛先変更
- [x] `CONNECT`拒否
- [x] Agent未起動／Bridge未ready時の即時失敗
- [x] production Client Socketからfake SVC、Agent relay、Mock HTTP Serverまでの結合
- [x] 512 KiB超request／384 KiB超responseとflow-control回復
- [x] half-close、normal close、stream／Socket cleanup
- [x] Debug／Release全21 CTest
- [x] Phase 4結合testのDebug／Release各20回連続成功

### 終了条件

- [x] Client ProxyからSVC相当transport経由でMock HTTP Serverへ要求と応答を
  streamingできる。
- [x] LAN、Wi-Fi、VPN NICからProxyへ接続できない。

Phase 4は2026-07-30にローカルGateを通過した。chunked、`100 Continue`、
認証、複数connection／repository、SVN操作sequenceはPhase 5へ進める。

## 11. Phase 5: HTTP／SVN互換性

### 実装

- [x] HTTP/1.1 Keep-Alive、`Content-Length`、chunked transfer encoding、`Expect: 100-continue`を実装する。
- [x] request／response bodyを双方向にstreamingする。
- [x] Basic、Digest、NTLM等の認証headerを内容解析や保存をせず透過中継する。
- [x] connection-oriented authenticationのため、incoming connectionと同一upstream Socketの対応を維持する。
- [x] WebDAV／DeltaV methodを拒否しない。
- [x] 複数server、複数port、複数repositoryを同一RDP sessionで多重化する。
- [x] bind addressを`127.0.0.1`へ固定したまま、Proxy portだけを安全なlocal設定で
  変更可能にする。

### ローカル検証結果

- [x] fragmented chunk size／data／CRLF／trailerとchunk extensionを検証する。
- [x] `Content-Length`と`Transfer-Encoding`の競合、未知coding、巨大lineを拒否する。
- [x] `Expect`、Authorization、Cookie、WebDAV headerを内容解析せず維持する。
- [x] production Client／Agentの同時上限を8 streamへ揃え、9件目を拒否する。
- [x] 8 upstream Socketの独立routeと、一Socket reset時の残stream継続を確認する。
- [x] 二つのlocal TCP connectionから異なるserver／port／repositoryを同時利用する。
- [x] 401 NTLM challenge後の同一Socket再requestを確認する。
- [x] `OPTIONS`、`GET`、`HEAD`、`POST`、`PROPFIND`、`REPORT`、
  `MKACTIVITY`、`CHECKOUT`、`PUT`、`DELETE`、`COPY`、`MOVE`、`MERGE`、
  `LOCK`、`UNLOCK`相当sequenceをMock WebDAVで確認する。
- [x] chunked request／responseと`100 Continue` responseをSVC分割経路で確認する。
- [x] optional `client.ini`が`[proxy] port=<1..65535>`以外を
  fail-closedにすることを確認する。

2026-07-31にMock WebDAVを使用するローカルGateを通過した。実取引先SVNへは
接続していない。VisualSVN等の常駐serverを開発PCへ自動導入することは避け、
実SVN clientと実repositoryによる確認はPhase 7／8の明示承認済み環境で行う。

### 検証順序

1. Mock HTTP／WebDAV Server
2. 専用test SVN repository
3. Repository browse相当
4. Checkout、Update、Compare相当
5. Commit、Add、Delete、Move相当
6. conflict、Lock、Unlock相当
7. 複数repositoryと長時間Keep-Alive
8. large Checkout／Commit相当stream

必要なtest用SVN toolが既存環境にない場合は、licenseと配布元を確認して`C:\develop\tools`配下へ追加し、製品runtime dependencyにはしない。

### 終了条件

- Mock／test環境で主要SVN操作が既存URLを変更せず成立する。
- HTTP payload、Authorization、Cookie、path、queryをlogへ出力しない。
- Repository参照だけで完了としない。

## 12. Phase 6: Hardening

### 対象

- [x] malformed frame／HTTP
- [x] 巨大length、overflow、sequence error
- [x] queue／window／stream数上限
- [x] slow initial header、local／upstream idle timeout、half-close、reset
- [x] Agent crash／restart
- [x] Client restart
- [x] RDP disconnect／reconnect相当のsession再生成
- [x] Agent allowlist変更
- [x] callback例外境界
- [x] log rotationとredaction

### セキュリティ監査

- [x] loopback以外でProxyがlistenしない。
- [x] allowlist完全一致以外へAgentが接続しない。
- [x] 任意TCP Forward、SOCKS、VPN、HTTP `CONNECT`として利用できない。
- [x] credential、payload、実IP／URLがsource、test、通常log、配布EXEへ残らない。
- [x] Bridge failureがRDP Client processのcrashへ波及しない。

### 終了条件

- [x] 必須unit／integration testがwarningおよびfailureなしで完了する。
- [x] known limitationと残存riskを`docs/known-limitations.md`へ文書化する。
- [x] Version `0.6.0.0`のx64 portable Client／Agentを生成して監査する。
- [x] Agent異常終了／再起動とRDP画面継続をユーザー実機で回帰確認する。
- [x] 実環境Read-only確認へ進むことをユーザーが承認する。

## 13. Phase 7: 実環境Read-only確認

ユーザーから実行対象と手順の明示指示を受けた場合だけ実施する。
2026-07-31に実環境で操作し、ユーザーから問題なしとの受入結果を得た。

### 確認対象

- [x] Repository Explorer
- [x] Log
- [x] Diff／Compare
- [x] Checkout
- [x] Update
- [x] 許可済みserver上の複数repository
- [x] 実操作中の接続継続
- [x] RDP切断時のcleanup

### 終了条件

- [x] ZscalerをONのまま、支給PCを経由して実SVNのread-only操作が成立する。
- [x] 新しいinbound port、network設定変更、security bypassを必要としない。
- [x] 実環境情報をGit管理文書や通常logへ残さない。

## 14. Phase 8: 実環境Write確認

ユーザーが指定した安全なtest repository／test pathに限定し、各操作の明示指示を受けて実施する。
2026-07-31、ユーザーからCommit等を含む全操作を問題なしとして扱う明示確認を得た。

### 確認対象

- [x] Commit
- [x] Add
- [x] Delete
- [x] Move
- [x] conflict
- [x] Lock
- [x] Unlock

### 終了条件

- [x] 指定範囲でwrite操作と結果確認が完了する。
- [x] 許可されていないrepository／pathへ変更を加えていない。
- [x] credential、diff、source、実URLをlogやGitHubへ残していない。

## 15. 改善フェーズとDocumentation Gate

Phase 8までに成立したVersion `0.6.0.0`をbaselineとし、以後のユーザー要望は
一件ずつ独立した改善項目として扱う。

### 改善項目の進め方

1. 一件の要望と期待結果を確定する。
2. 影響範囲、security boundary、互換性、test方法を確認する。
3. 必要な実装だけを行い、Debug／Releaseと関連testを確認する。
4. 実機確認が必要な場合は、確認項目と期待結果をユーザーへ提示する。
5. ユーザー受入後に項目を完了し、次の要望へ進む。

複数の未完了要望を暗黙にまとめて実装しない。問題が後日判明した場合は、
該当項目だけを再オープンし、受入済み機能を無関係に変更しない。

### 改善I-001: Mylyn Trac HTTP互換性

- [x] 導入済みMylyn Trac `3.26.0`とrepository固有Proxyを監査する。
- [x] 影響範囲とsecurity boundaryを`work/proposals`へ記録する。
- [x] Web GET、XML-RPC POST、Cookie、redirect、gzip responseをMockで検証する。
- [x] production Proxy／SVC／Agent経路を通す。
- [x] allowlist完全一致、HTTPのみ、CONNECT／hostname／HTTPS拒否を維持する。
- [x] production codeとVersion `0.6.0.0` binaryを変更しない。
- [x] Debug／Release全25 CTestを確認する。
- [x] ユーザー環境でMylynの「設定の検証」と同期を確認する。
- [x] ticket query／参照の疎通を確認する。
- [x] ユーザー受入後にI-001を完了する。

Tracが既存SVNと同じ`IPv4:port`を使用する場合、Agent allowlist entryは重複追加
しない。別endpointの場合だけ、業務上の許可を確認して完全一致entryを追加する。
汎用HTTP Proxy、OS全体Proxy変更、HTTPS tunnel、network探索は追加しない。

### 改善I-002: 複数RDP sessionの独立Window化

- [x] 影響範囲、security boundary、失敗時動作、rollbackを
      `work/proposals`へ記録する。
- [x] 接続入力と共通履歴を`ConnectionManagerWindow`へ分離する。
- [x] 各RDP接続を独立した`MainWindow`として別top-level Windowへ生成する。
- [x] 各sessionが専用ActiveX、`ESVNBRG`、Bridge、Proxy workerを所有する。
- [x] `client.ini`のportをbaseとし、最初のsessionへbase、後続へ
      `base + slot`を割り当てる。
- [x] 同時sessionを16件へ制限し、解放slotを安全に再利用する。
- [x] 接続管理Windowに生存session数と次のportを表示する。
- [x] session Windowをtitle bar、表示modeだけの一行、RDP表示にし、接続先、
      RDP／Agent状態、Proxy状態・port・stream数・転送量をtitleへ集約する。
- [x] 接続管理Windowの初期modeを引き継ぎ、session内でも3 modeを切り替える。
- [x] session Windowの閉じる操作で確認後に対象sessionだけを切断・終了する。
- [x] 全画面切替はWindows標準RDP互換の`Ctrl+Alt+Break`を使用する。
- [x] ActiveX instance、event sink、Bridge、Proxyのsession所有を分離する。
- [x] 実機で一session終了時に別sessionも応答不能となる単一COM STAの問題を記録する。
- [x] 各session Window／ActiveXをsession専用COM STAとmessage loopへ分離する。
- [x] session履歴通知をmanager STAへmarshalし、session threadからmanager objectへ
      直接触れない。
- [x] 一方の専用STAでActiveXを破棄後も、他方のSTAが応答する回帰testを追加する。
- [x] Debug全29 CTestを確認する。
- [x] Release全29 CTestとVersion `0.7.10.0` portable成果物を確認する。
- [x] ユーザー環境でsession Windowがtitle bar、表示mode一行、RDP表示になることを
      確認する。
- [x] 接続中に3つの表示modeを切り替え、各modeの既定動作を確認する。
- [x] titleのRDP／Agent／Proxy状態が実接続に追従することを確認する。
- [x] `Ctrl+Alt+Break`の全画面往復と閉じる操作による切断を確認する。
- [ ] ユーザー環境で二つのRDP／Agent／Proxy sessionを同時確認する。
- [x] 片方の終了後も他方が継続し、解放portが再利用されることを確認する。
- [ ] ユーザー受入後にI-002を完了する。

wire protocol、Agent allowlist、HTTP制限は変更しない。Version `0.7.10.0` Clientは
Version `0.6.0.0` Agentとwire互換だが、通常配布では同じ`0.7.10.0` buildを使用する。
通常利用する一件を最初に開始すれば、従来どおりbase `18080`となる。

### 改善I-003: 接続管理UIの背景統一と名前付き接続

- [x] 影響範囲、security boundary、失敗時動作、rollbackを
      `work/proposals`へ記録する。
- [x] 接続管理Windowの親背景をWindows標準ダイアログ色へ統一する。
- [x] 最大64文字の任意接続名コンボボックスを追加する。
- [x] 接続名の選択で接続先、domain、userを一組として復元する。
- [x] 同一接続組と同じ非空接続名を大小文字無視で最新一件へ統合する。
- [x] DPAPI暗号化履歴をVersion 2へ更新し、接続名を保護対象へ追加する。
- [x] Version 1履歴を名前なしとして読める後方互換経路を実装する。
- [x] session titleへ`接続名 (接続先)`を表示し、実接続先を隠さない。
- [x] Debug全26 CTestを確認する。
- [x] Release全26 CTestとVersion `0.7.3.0` portable成果物を確認する。
- [x] ユーザー環境で背景、名前の保存／選択復元、重複排除、title表示を確認する。
- [x] RDP、Agent、Proxy、SVN／Trac操作に回帰がないことを確認する。
- [x] ユーザー受入後にI-003を完了する。

wire protocol、ActiveX接続設定、Proxy、Agent allowlistは変更しない。接続名は
Clientのcurrent-user DPAPI履歴と通常session titleだけで使用し、Agentや
diagnostic logへ送らない。

### 改善I-004: RDP音声リダイレクト設定

- [x] 影響範囲、security boundary、失敗時動作、rollbackを
      `work/proposals`へ記録する。
- [x] 接続管理Windowへ、既定OFFのマイク転送check boxを追加する。
- [x] リモート音声の再生先を3種類から選択できるようにする。
- [x] 接続時設定modelを介してActiveXへ適用し、setter後にreadbackする。
- [x] DPAPI暗号化履歴をVersion 3へ更新し、音声設定をprofileへ追加する。
- [x] Version 1／2履歴を安全な音声既定値で読み込む。
- [x] Debug全26 CTestを確認する。
- [x] Release全26 CTestとVersion `0.7.4.0` portable成果物を確認する。
- [ ] ユーザー環境でマイク入力、リモート音声、profile復元を確認する。
- [ ] RDP、Agent、Proxy、SVN／Trac操作に回帰がないことを確認する。
- [ ] ユーザー受入後にI-004を完了する。

音声設定は接続開始時だけActiveXへ適用し、確立済みsessionへ途中反映しない。
マイクはprivacy上の明示選択として既定OFFとする。GPO、Windows privacy、
device設定をZRdpから変更または回避しない。wire protocol、Proxy、Agent
allowlistは変更しない。

### 改善I-005: RDP session Windowの手動アスペクト比制約

- [x] 影響範囲、Windows標準操作との境界、失敗時動作、rollbackを
      `work/proposals`へ記録する。
- [x] 接続を受理したsessionごとに、要求済みremote desktop幅／高さを保持する。
- [x] title bar、frame、表示mode欄を除いたRDP表示領域の比率を計算する。
- [x] `WM_SIZING`の4辺／4角と反対辺anchorを実装する。
- [x] 一回のdrag開始寸法から角操作の主軸を安定して判定する。
- [x] 最大化、Windowsスナップ、ActiveX全画面を制約対象外にする。
- [x] 最小寸法、不正比率、不正辺、座標overflowを単体テストする。
- [x] Debug全27 CTestを確認する。
- [x] Release全27 CTestとVersion `0.7.5.0` portable成果物を確認する。
- [ ] ユーザー環境で辺／角dragと3つの表示modeを確認する。
- [ ] 最大化、Windowsスナップ、全画面が従来どおり動作することを確認する。
- [ ] RDP、Agent、Proxy、SVN／Trac操作に回帰がないことを確認する。
- [ ] ユーザー受入後にI-005を完了する。

通常の手動resizeだけを接続時desktop比率へ制約する。等倍表示とSmart Sizingでは
remote desktop寸法を変更せず、動的解像度だけが制約後のRDP領域を既存debounce
経路で適用する。wire protocol、Proxy、Agent allowlist、履歴形式は変更しない。

### 改善I-006: 初回動的解像度のログイン完了待機

- [x] 実機logから`OnConnected`後かつ`OnLoginComplete`前の
      `UpdateSessionDisplaySettings`が`0x8000FFFF`を返すことを特定する。
- [x] `OnConnected`では初回動的解像度timerを予約しない。
- [x] `OnLoginComplete`後だけ表示mode切替と動的解像度APIを許可する。
- [x] 自動再接続完了を既存ログイン済みsessionへの復帰として再適用する。
- [x] 接続状態ごとの動的解像度許可条件を単体テストする。
- [x] 一度の実機受入後、別接続で`OnLoginComplete`の約0.24秒後にも
      `E_UNEXPECTED`が再発したことをlogで特定する。
- [x] `E_UNEXPECTED`だけを500 ms、1,000 ms、1,500 msの有限delayで再試行する。
- [x] 恒久HRESULTと再試行上限後は従来どおり等倍表示へ縮退する。
- [x] 再試行policyの単体testを追加する。
- [x] Release全29 CTestとVersion `0.7.10.0` portable成果物を確認する。
- [x] ユーザー環境で最初から動的解像度を選び、エラーなしで接続する。
- [x] 接続直後からremote desktopがsession Windowへ追従する。
- [x] ログイン完了後の3 mode切替とWindow resizeを回帰確認する。
- [x] 再発した接続先でVersion `0.7.10.0`の自動回復を確認する。
- [x] 再確認後にI-006を再完了する。

初回remote desktop寸法は従来どおり接続前設定へ渡す。transport成立だけを示す
`OnConnected`では実行時更新を行わず、Microsoft Controlがログイン完了を通知した
後にだけ250 ms debounce経路へ入れる。wire protocol、Proxy、Agent allowlist、
履歴形式、認証設定は変更しない。

### 改善I-007: 成立済みRDP sessionの外部切断時終了

- [x] ログイン完了済みかを接続試行ごとに保持する。
- [x] 接続先再起動、通信断、remote logoff等の最終外部切断では
      error dialogを表示せずsession Windowを閉じる。
- [x] 自動再接続試行中と自動再接続成功時はWindowを維持する。
- [x] 初回認証失敗等のログイン前切断は従来のerror表示を維持する。
- [x] ZRdp自身の切断要求とcredential prompt取消を外部切断判定から除外する。
- [x] 接続段階と切断起点の全組合せを単体テストする。
- [x] Debug／Release全28 CTestを確認する。
- [x] Version `0.7.7.0` portable成果物を確認する。
- [ ] ユーザー環境で接続先再起動または通信断後、error表示なしで対象session
      Windowだけが閉じることを確認する。
- [ ] 他のsession、接続管理Window、Agent、Proxyへ回帰がないことを確認する。
- [ ] ユーザー受入後にI-007を完了する。

`OnAutoReconnecting2`では既存sessionを維持し、Microsoft Controlが復帰を断念して
`OnDisconnected`を通知した時点でだけ閉じる。切断reasonは従来どおり数値を
診断logへ残すが、dialogには表示しない。wire protocol、Proxy、Agent allowlist、
履歴形式、認証設定は変更しない。

### 改善I-008: 枠なしpopカラーicon

- [x] 既存の濃紺角丸tileを廃止する。
- [x] sky blue／turquoiseのZ型Bridgeとcoral／royal purpleのendpointを
      transparent background上へ配置する。
- [x] black、charcoal、navyの背景を使用しない。
- [x] Version `0.7.8.0`の実機表示から、subjectが小さく黄色endpointの
      視認性が不足するfeedbackを記録する。
- [x] subjectをmaster幅約96%／高さ約92%へ拡大する。
- [x] 黄色、gold、amber、limeを使用せず、lower endpointをroyal purpleへ変更する。
- [x] 16、24、32、48pxを明背景と暗背景へ合成して視認性を確認する。
- [x] 16、20、24、32、40、48、64、128、256pxを持つICOを生成する。
- [x] Client／Agent共通resourceへ同じiconを埋め込む。
- [x] Debug／Release EXEから実iconを抽出して枠なし意匠を確認する。
- [x] Release 32px iconの不透明pixelを185から339へ増加させ、黄色系pixelを0にする。
- [x] Debug全28 CTestを確認する。
- [x] Release全28 CTestとVersion `0.7.9.0` portable成果物を確認する。
- [ ] ユーザー環境でExplorer、taskbar、接続管理Window、session Window、
      Agent consoleの表示を確認する。
- [ ] ユーザー受入後にI-008を完了する。

iconだけを変更し、RDP、wire protocol、Proxy、Agent allowlist、履歴形式、
認証設定は変更しない。Windows icon cacheに旧iconが残る場合は、EXE名や
system設定を自動変更せず、新成果物のtitle bar表示を正本として確認する。

### 回帰修正R-001: 複数session終了分離と動的解像度retry

- [x] 実機logから動的解像度の再発時刻とHRESULTを特定する。
- [x] 複数sessionの終了時刻、Proxy port、後続切断eventを分離して確認する。
- [x] ADR 0005へsession専用COM STAの採用理由と代替案を記録する。
- [x] `RequestClose`後の`Destroy`で重複`Disconnect`を呼ばない。
- [x] close、confirm close、disconnect、動的解像度結果へsession識別用portを
      記録する。
- [x] Debug／Release全29 CTestとVersion `0.7.10.0` portable成果物を確認する。
- [x] ユーザー環境で動的解像度の再発接続を確認する。
- [x] 二つの実RDPの一方を閉じても、他方とmanagerが操作可能なことを確認する。
- [x] 解放済みProxy portで新規sessionを開始できることを確認する。

wire protocol、Agent、allowlist、Proxyのbind先、HTTP制限、履歴形式は変更しない。
Microsoft ActiveX内部の終了が長時間待機しても、別sessionとmanagerのmessage
loopを同じSTAへ巻き込まないことを修正境界とする。

Gate結果: 2026-07-31、ユーザーから動的解像度、異なる二台への同時接続、
Agent接続済みsession終了後の別RDP／manager継続、残存sessionの表示mode変更、
解放Proxy port再利用の全項目について動作確認済みとの受入結果を得た。

### 改善I-009: AplMain形式を参考にした全ソースコメント整備

- [x] `AplMain.java`のfile／class／method／member／処理段階のコメント構成を
      確認する。
- [x] Java固有実装、固有名、著作権、作成者、日付、手書きversion／履歴を
      複写しない境界を`AGENTS.md`へ明記する。
- [x] `src`、`tests`、`assets/icons`の全88 source、合計39,270行を監査する。
- [x] file、型、function、member、resource ID、test entryの説明を整備する。
- [x] 全loopへ目的を付与し、処理段階と非自明な制約理由を補う。
- [x] コメント不足と手書き管理情報を継続検出するCTestを追加する。
- [x] Debug buildをwarningなしで完了し、全30 CTestを成功させる。
- [x] Release Clientを代替出力先へlinkし、全30 CTestと成果物契約を成功させる。
- [x] runtime、wire protocol、Proxy、Agent、allowlist、設定形式を変更しない。
- [x] 監査結果とrollback archiveを`work`配下へ記録する。

コメントと静的検査だけの変更であり、実RDP／実SVN／実Tracの追加実機確認は
不要とする。実行中Clientを無断終了しないため、標準`out`のRelease Clientは
置換せず、一時出力した同一targetでlinkと成果物契約を検証した。

### 改善I-010: セッションWindowの全画面ボタン

- [x] 既存`IDC_FULL_SCREEN`、`OnFullScreen()`、ActiveX event経路を確認する。
- [x] セッションWindowの表示対象へ全画面buttonを追加する。
- [x] 動的解像度radioの右側へDPI対応寸法で配置する。
- [x] 接続前無効、接続成立後有効という既存state policyを維持する。
- [x] ActiveX eventに応じて「全画面」／「ウィンドウ」を切り替える。
- [x] `Ctrl+Alt+Break`のWindows標準互換経路を維持する。
- [x] Debug buildをwarningなしで完了し、全30 CTestを成功させる。
- [x] Version `0.7.11.0`のRelease Client／Agent候補を一時出力へ生成する。
- [x] Release回帰28件と成果物契約2件をすべて成功させる。
- [x] 実行中Client終了後に標準`out`へVersion `0.7.11.0`を生成する。
- [x] 標準`out`を対象にRelease全30 CTestとversion一致を確認する。
- [ ] ユーザー環境でbuttonによる全画面移行とWindow復帰を確認する。
- [ ] 表示mode、Agent、Proxy、SVN／Tracに回帰がないことを確認する。
- [ ] ユーザー受入後にI-010を完了する。

buttonは既存のMicrosoft ActiveX全画面APIを呼び出すだけとし、独自の全画面Window、
画面転送、shortcut hookは追加しない。wire protocol、Proxy、Agent、allowlist、
履歴形式は変更しない。

### 回帰修正R-002: 動的解像度の全画面monitor追従

- [x] Version `0.7.11.0`で全画面Windowだけが拡大し、remote desktop寸法が
      通常Window時のまま残る原因を特定する。
- [x] 全画面中の動的解像度timer除外を廃止する。
- [x] 通常WindowではRDP child領域、全画面ではmonitor全体をtargetにする。
- [x] button、`Ctrl+Alt+Break`、connection barをActiveX event後の同一policyへ
      収束させる。
- [x] 等倍表示とSmart Sizingではremote desktop寸法を変更しない。
- [x] target選択の単体testを追加する。
- [x] ADR 0006へ設計判断とsecurity／protocol非影響を記録する。
- [x] Debug／Release buildをwarningなしで完了し、各30 CTestを成功させる。
- [x] Version `0.7.12.0`のClient／Agentを標準`out`へ生成する。
- [x] version、SHA-256、未署名状態を確認する。
- [ ] ユーザー環境の動的解像度で全画面移行時にremote desktopがmonitor全体へ
      追従することを確認する。
- [ ] Window復帰時にremote desktopがRDP表示領域へ再追従することを確認する。
- [ ] 等倍表示とSmart Sizingのdesktop寸法維持に回帰がないことを確認する。
- [ ] ユーザー受入後にR-002とI-010を完了する。

250 ms debounce、`E_UNEXPECTED`限定有限retry、恒久失敗時の等倍縮退を維持する。
認証、RDP接続、Virtual Channel、Proxy、Agent、allowlist、履歴形式は変更しない。

### 回帰修正R-003: ログイン前SSL handshake一時失敗の限定再試行

- [x] Client診断logとWindows RDP Client event logを時刻対応させ、
      `OnConnected`前の`discReason=263`、`ExtendedDisconnectReason=0`と
      SSL handshake継続失敗を特定する。
- [x] 公開仕様にない通常reason `263`からlicense障害等を断定しない。
- [x] login未完了、local切断でない、credential取消でない、Window終了中でない、
      `263/0`完全一致、未再試行の全条件を純粋policyへ実装する。
- [x] COM callback外のWindow timerで2,000 ms後に一度だけ、同じ検証済み設定を
      再適用してMicrosoft ActiveXの通常`Connect()`を呼ぶ。
- [x] 二度目の同一失敗、timer確保失敗、設定失敗、同期的`Connect()`失敗では
      追加再試行せず、従来のerror表示へ戻す。
- [x] retry予約、開始、成功、上限到達を固定eventと数値fieldだけで記録する。
- [x] password取得、`mstsc.exe`起動、NLA／CredSSP／証明書方針の緩和を行わない。
- [x] ADR 0007へ判断、除外条件、security／protocol非影響を記録する。
- [x] Debug／Release buildをwarningなしで完了し、各30 CTestを成功させる。
- [x] Version `0.7.13.0`のClient／Agent候補を一時出力へ生成し、version、
      SHA-256、未署名状態を確認する。
- [x] 実行中Client停止後にVersion `0.7.13.0`を標準`out`へ生成する。
- [x] ユーザー環境でVersion `0.7.13.0`の起動と通常RDP接続を確認する。
- [x] 固定診断logで同Versionの`process.started`から`rdp.event.login_complete`まで
      到達し、通常成功時にはretry eventがないことを確認する。
- [ ] 自然再発時の一回復旧、credential取消、成立済みsession切断に回帰が
      ないことを継続確認する。
- [ ] ユーザー受入後にR-003を完了する。

この対策は観測済みsignatureだけを対象とし、任意のログイン前失敗を成功扱いに
しない。Agent、Static Virtual Channel wire protocol、Proxy、allowlist、
SVN／Trac、接続履歴形式、新規portは変更しない。

### Version 0.7.13 運用観察

通常のRDP、Mylyn、SVN／Trac利用を続けながら、実装済み改善の実機受入と
まれな接続事象を自然条件で確認する。観察条件と残件は
`work/analysis/20260731_version_0_7_13_operational_soak.md`へ集約する。

- [x] Debug／Releaseをwarningなしでbuildし、各30 CTestを再確認する。
- [x] 標準`out`のVersion、SHA-256、未署名状態が既存成果物から変化していないことを
      確認する。
- [x] Agentを通常ユーザーが書き込める場所へ`agent.ini`とともに配置し、
      `logs` directoryが作成されることを確認する。
- [x] Mylynの定期同期に伴う`C->A`／`A->C`増加が通常のBridge転送量であり、
      同期中もRDP／Clientが継続することを確認する。
- [ ] 通常利用を継続し、予期しないClient終了、Agent切断、Proxy停止、転送停止が
      再発しないことを確認する。
- [ ] 再発時は発生時刻、対象sessionのProxy port、titleのstream／転送量、
      Client／Agent双方の固定診断logを保存して個別改善として再オープンする。

### R-004: 接続中Session終了後の残留process

Version `0.7.13.0`を約2時間接続後にSession Windowを閉じた際、画面は消えたが
Client processが残留した。固定logは`rdp.event.confirm_close`とProxy停止まで到達し、
`process.stopping`へ到達しなかった。Wait Chain Traversalではapplication thread間の
cycleを検出せず、接続中ActiveXを直ちにATL OLE破棄する境界へ範囲を限定した。

- [x] 対象PID、実行path、Window消失、Proxy停止、process残留を確認する。
- [x] Dumpを作成せず、固定logとread-only wait chainだけで機密dataを保護する。
- [x] 接続中Window終了を通常buttonと同じ`Disconnect()`／`OnDisconnected`へ統一する。
- [x] 未接続、接続中、切断中の3状態を純粋policy testへ追加する。
- [x] ADR 0008へ判断、代替案、security／protocol非影響を記録する。
- [x] Version `0.7.14.0`のDebug／Release各30 CTestを成功させる。
- [x] Client／AgentのVersion、未署名SHA-256、署名前正本を確認する。
- [x] 実RDP接続中の閉じる操作後にWindowとClient processが残留しないことを確認する。
- [x] 固定logで`rdp.event.disconnected`と`process.stopping`を確認する。
- [x] ユーザー受入後にR-004を完了し、署名Gateへ進む。

### R-005: ログイン前SSL handshake再試行の二段階化

同一接続先への新規接続でZRdpの初回と一回再試行が`263/0`で終了し、その後の
`mstsc.exe`も二回同じSSL handshake継続失敗となった後、三回目で成立した。
永続的な接続履歴や保存済み資格情報は観測されず、R-003が有限上限へ到達する
一方でWindows標準Clientも同じ回数失敗したことから、対象signatureを変更せず
Clientの試行回数だけを初回を含む最大三回へ拡張する。

- [x] 実行logとWindows event logを時刻対応させ、ZRdpと`mstsc.exe`の失敗／成功
      sequenceを確認する。
- [x] R-003のlogin未完了、非local切断、非credential取消、非終了中、`263/0`
      完全一致という条件を維持する。
- [x] COM callback外のWindow timerで2,000 ms後、5,000 ms後の二回だけ再試行する。
- [x] 三回目の同一失敗、timer確保失敗、設定失敗、同期的`Connect()`失敗では
      追加再試行せず、従来のerror表示へ戻す。
- [x] 二段階delayと上限を単体testへ追加し、Debug／Release各30 CTestを成功させる。
- [x] `ZRdpSvnBridge.exe`だけをRelease buildし、`ZRdpSvnAgent.exe`の作業前後
      SHA-256が一致することを確認する。
- [x] ADR 0009へ判断、代替案、security／protocol非影響を記録する。
- [x] Clientだけを署名し、Windows trust、SHA-256、RFC 3161 timestamp、既存Agentと
      同一署名者であることを確認する。
- [ ] ユーザー環境で通常接続と自然再発時の有限自動復旧を確認する。
- [ ] Agent、Proxy、SVN／Trac、複数session、表示modeに回帰がないことを確認する。

password取得、保存資格情報の作成、`mstsc.exe`起動、NLA／CredSSP／証明書方針の
緩和は行わない。Agent、Static Virtual Channel wire protocol、Proxy、allowlist、
接続履歴形式、port割当は変更しない。

### R-006: certificate警告ダイアログのActiveX互換表示

新しい接続先へのZRdp接続はcredential入力後に二回目の認証warning eventを即時終了し、
`263/0`となった。R-005は同じ失敗を反復してpasswordを三回要求した。一方、Windows
標準Clientはcredential UI後にserver certificate警告を表示し、利用者の明示継続後に
desktopへ到達したため、retry回数ではなくActiveX security dialog境界を修正する。

- [x] ZRdpとWindows標準Clientの認証／certificate warning sequenceを比較する。
- [x] 修正提案とADR 0010へsecurity boundary、失敗時動作、rollbackを記録する。
- [x] `RedirectionWarningDialogVersion=1`を`VT_UI4`で接続前に設定する。
- [x] `ShowRedirectionWarningDialog=TRUE`候補が既存接続を壊すことを確認し撤回する。
- [x] `AuthenticationLevel=2`をreadbackし、警告なしまたはhard-failへの変化を拒否する。
- [x] 認証dialog中のActiveX childへのfocus再設定を抑止する。
- [x] Microsoft認証dialog表示後の`263/0`を自動retryから除外する。
- [x] 対象のActiveX smoke、policy、diagnostic log testを成功させる。
- [x] Debug／Release全CTestとClient限定Release成果物を確認する。
- [x] 回帰撤回後Clientで従来接続先へ接続できることをユーザー実機で確認する。
- [ ] certificate警告の「はい」／「いいえ」とAgent／Proxy／SVN／Tracを
      ユーザー実機で確認する。

certificate警告は自動承認せず、NLA、CredSSP、server authenticationを弱めない。
certificate store、registry、GPO、credential manager、RDP cacheを変更しない。
Agent、Virtual Channel、Protocol、Proxy、allowlist、接続履歴には変更しない。

### R-007: ActiveX既定の認証ダイアログ親Window

R-006回帰撤回版では従来接続先が回復したが、certificate確認が必要な接続先は
`TsSslEventHandshakeContinueFailed (0x80004005)`、`263/0`のまま終了した。
credential UIに続くcertificate UIのmodal ownershipを同じMicrosoft Controlへ
一貫して委譲するため、session top-level Windowへの`UIParentWindowHandle`明示設定を
外し、ActiveX Control Windowの既定親を使用する。

- [x] 修正提案へ影響範囲、security boundary、失敗時動作、rollbackを記録する。
- [x] 回帰撤回版ClientをSHA-256付きで`work/tmp`へ退避する。
- [x] `UIParentWindowHandle` setterを削除し、ActiveX既定親の選択を固定logへ記録する。
- [x] Debug／Release全CTestとClient限定Release成果物を確認する。
- [x] 従来接続先へ回帰なく接続できることをユーザー実機で確認する。
- [x] 問題接続先でcertificate警告を明示承認し、desktopへ到達することを確認する。
- [x] certificate警告で「いいえ」を選ぶと`263/0`後に重複errorが表示されることを
      ユーザー実機で確認する。

NLA、CredSSP、`AuthenticationLevel=2`、certificate検証、警告の利用者判断、
認証UI表示後のretry禁止を維持する。Agent、Virtual Channel、Protocol、Proxy、
allowlist、接続履歴には変更しない。

### R-008: certificate警告拒否後の重複error抑止

R-007でMicrosoft標準certificate警告の「いいえ」を選ぶと、Controlが通知した
`discReason=263`、`ExtendedDisconnectReason=0`をZRdpが通常障害として再表示した。
利用者は標準警告ですでに接続拒否を明示しているため、認証warning二回以上、
login未完了、非local、credential取消以外、`263/0`完全一致に限定して追加errorを
抑止し、対象session Windowだけを閉じる。

- [x] 修正提案へ判定条件、security boundary、誤分類可能性、rollbackを記録する。
- [x] warning表示回数を接続試行単位で有限に追跡し、pure policyで完全一致を判定する。
- [x] 完全一致時にretryと追加errorを行わず、対象session Windowだけを閉じる。
- [x] 固定log field `security_warning_cancelled`と除外条件の自動testを追加する。
- [x] Debug／隔離Release各30 CTest、標準成果物contract、Agent非変更を確認する。
- [x] R-007候補で従来接続先へ回帰なく接続できることをユーザー実機確認する。
- [x] R-008候補で「いいえ」後に追加errorなしでsession Windowだけが閉じることを
      ユーザー実機確認する。

ActiveXは警告buttonの選択値を公開しないため、warning表示段階と切断値の完全一致で
限定分類する。certificate検証、自動承認、NLA、CredSSP、`AuthenticationLevel=2`、
Agent、Virtual Channel、Protocol、Proxy、allowlist、接続履歴は変更しない。

### Documentation Gate

2026-07-31、ユーザーはVersion `0.7.14.0`を社内で試行利用し、不具合を個別報告する
方針を選択した。このため、未完の実機観察を既知の制約として明示したうえで、
社内試行用の最終仕様書と利用手順書を作成する。問題が判明した場合は該当文書と
実装項目だけを再オープンする。

- [x] 最終仕様書に構成、protocol、RDP、Proxy、Agent、設定、security、制約を集約する。
- [x] 利用手順書にportable配布、RDP接続、Agent起動、JavaHL設定、通常操作、
      障害時確認、ログ取得を記載する。
- [x] 実IP、実URL、repository名、ユーザー名、credential、実画面を資料へ含めない。
- [x] 最新成果物のversionと実装に文書内容が一致することを確認する。
- [x] 利用方法と設計書をWord形式へ変換し、目次、図表、メタデータ除去、全35pageの
      表示品質を確認する。
- [ ] ユーザーの文書受入を得る。

## 16. Release Gate

### 配布形態

```text
開発 PC:
  ZRdpSvnBridge.exe

支給 PC:
  ZRdpSvnAgent.exe
  Git管理外の実環境allowlist設定
```

- installerを使用せず、portable EXEをコピーして実行する。
- Windowsにインストール済みのRemote Desktop ActiveX Controlとsystem DLLを利用し、Windows RDP DLLを同梱しない。
- Client／Agentへversion resourceを付与する。
- Release buildのmitigationを無効化しない。
- binary、PDB、log、dump、実環境設定をGitへcommitしない。
- 2026-08-01、ユーザーがClient／AgentをAuthenticode署名した。
- R-004の実RDP終了とprocess消滅を確認してから署名する。
- 署名後のWindows trust `Valid`、SHA-256、RFC 3161 timestamp、hash、versionを
  確認した。配布前に署名済み実行fileのsmoke testと配布ファイル一覧を確認する。
- ユーザーの署名完了連絡と明示指示なしにtag、GitHub Release、asset uploadを行わない。

### 最終完了条件

- RDP接続だけでなく、開発PCのSVNクライアントから支給PCとZscalerを経由して実用的なSVN read／write操作が成立する。
- 既存SVN URLを変更しない。
- ZscalerやWindows security設定を停止、変更、回避しない。
- RDP以外の新しい対外通信経路を作らない。
- loopback ProxyとAgent allowlistのsecurity boundaryがテストで保証される。
- RDP切断時に全HTTP streamとupstream Socketが終了する。
- 汎用Proxy、VPN、SOCKS、任意TCP Forwardとして利用できない。

## 17. 主要な判断Gate

| 判断 | 時点 | 必要な記録／承認 |
|---|---|---|
| ActiveX host方式 | 開発基盤 | `work/proposals`、必要tool追加の影響 |
| CRT `/MT`または`/MD` | 開発基盤 | `docs/adr`、portable配布とservicing |
| Phase 0通過 | Phase 0 | ユーザー環境の接続結果 |
| SVC継続またはDVC検討 | Phase 1／5実測後 | `docs/adr`とユーザー明示承認 |
| 実環境read-only | Phase 7 | 2026-07-31 ユーザー受入記録 |
| 実環境write | Phase 8 | 2026-07-31 ユーザー受入記録 |
| 最終仕様書／利用手順書 | Documentation Gate | 全改善完了とユーザー文書受入 |
| Release | 署名後 | 署名完了連絡とremote operationの明示指示 |

## 18. 直近の作業順序

1. [x] ActiveX host方式の比較提案を`work/proposals`へ作成する。
2. [x] CRT配布方式のADR案を作成する。
3. [x] CMake build scaffoldと最小CTestを作成する。
4. [x] `ZRdpSvnBridge.exe`のWin32 shellとCOM STA lifecycleを実装する。
5. [x] Remote Desktop ActiveX Controlの作成とevent購読を実装する。
6. [x] Window／全画面／clipboard対応を実装してPhase 0ローカル確認を行う。
7. [x] 接続先／ドメイン／ユーザー名の暗号化履歴と重複排除を実装する。
8. [x] monitor全体をbaselineとする等倍scrollと、remote寸法を変えないSmart Sizingを実装する。
9. [x] remote寸法を明示的に変更する動的解像度を独立modeとして実装する。
10. [x] ユーザー環境でPhase 0 Gateを確認する。
11. [x] Phase 1のframe、HELLO、binary Echo、Client SVC登録を実装する。
12. [x] RDP対話session限定のportable AgentとWTS channel I/Oを実装する。
13. [x] fake SVCで合計11,600,897 byteを検証する。
14. [x] Release成果物を検証する。
15. [x] ユーザー環境でPhase 1 Echo GateとAgent再起動を確認する。
16. [x] Phase 2のprotocol／flow-control設計を`work/proposals`へ記録する。
17. [x] transport非依存の複数stream state machineを実装する。
18. [x] fake Socketで複数stream、window、queue、closeを検証する。
19. [x] Phase 2のRelease build／全CTestを確認する。
20. [x] Phase 3のallowlist完全一致parserとMock Server接続を実装する。
21. [x] fake Socketでpolicy拒否、timeout、refused、reset、cleanupを検証する。
22. [x] production Winsockとloopback Mock Serverで単一streamを検証する。
23. [x] Phase 3のRelease build／全CTest／portable成果物を確認する。
24. [x] Phase 4のlocalhost HTTP Proxy設計を`work/proposals`へ記録する。
25. [x] loopback限定listener、incremental HTTP parser、Client relayを実装する。
26. [x] production Client／Agent経路をMock HTTP Serverまで結合する。
27. [x] Phase 4のDebug／Release全CTestと反復結合testを確認する。
28. [x] Phase 4の仕様、security境界、test結果、配布物を文書化する。
29. [x] Phase 5のHTTP／SVN互換性設計を`work/proposals`へ記録する。
30. [x] chunked、`100 Continue`、認証、Keep-Alive、複数connectionを実装する。
31. [x] Client／Agentをproduction上限8 streamの公平なtableへ拡張する。
32. [x] loopback固定のままProxy portだけを`client.ini`で変更可能にする。
33. [x] Mock WebDAVでread／write相当の全対象methodと認証継続を検証する。
34. [x] Phase 5のDebug／Release全CTestと反復結合testを確認する。
35. [x] Version `0.5.0.0`のportable Client／Agentを`out/`へ生成する。
36. [x] Phase 6のHardening設計と異常系matrixを`work/proposals`へ記録する。
37. [x] 5秒間隔／15秒期限のAgent health checkと同一RDP内再handshakeを実装する。
38. [x] timeout前sessionの遅延Frameを新HELLO_ACKまで破棄する。
39. [x] 初回HTTP header 30秒、local／upstream無通信30分のtimeoutを実装する。
40. [x] Agent crash／restart、Client restart、RDP reconnect、allowlist切替を反復検証する。
41. [x] malformed input、資源上限、loopback、allowlist、log redactionを監査する。
42. [x] Phase 6のDebug／Release全24 CTestと反復testを確認する。
43. [x] Version `0.6.0.0`のportable Client／Agentと既知制約を文書化する。
44. [x] ユーザー実機でAgent異常終了後の自動検出／再起動とRDP画面継続を確認する。
45. [x] ユーザー承認後にPhase 7の実環境Read-only確認へ進む。
46. [x] JavaHLから既存URLのまま実SVN read-only操作を確認する。
47. [x] Commit、Add、Delete、Move、conflict、Lock、Unlockをユーザー受入結果として確認する。
48. [ ] ユーザー要望を一件ずつ改善し、項目ごとに検証と受入を完了する。
49. [x] 改善I-001のTrac互換性監査、提案、Mock実装を完了する。
50. [x] 改善I-001をユーザー実機で受け入れる。
51. [x] 改善I-002の複数session設計、実装、Debug検証を完了する。
52. [x] 改善I-002のRelease検証とportable成果物を作成する。
53. [ ] 改善I-002をユーザー実機で受け入れる。
54. [x] 改善I-003の名前付き接続profileを実装し、ユーザー実機で受け入れる。
55. [x] 改善I-004の音声設定を実装し、Debug／Release検証を完了する。
56. [ ] 改善I-004をユーザー実機で受け入れる。
57. [x] 改善I-005の手動アスペクト比制約を実装し、Debug検証を完了する。
58. [x] 改善I-005のRelease検証とportable成果物を作成する。
59. [ ] 改善I-005をユーザー実機で受け入れる。
60. [x] 改善I-006の初回動的解像度適用をログイン完了後へ変更する。
61. [x] 改善I-006のRelease全27 CTestとportable成果物を確認する。
62. [x] 改善I-006をユーザー実機で受け入れる。
63. [x] 改善I-007の成立済みsession外部切断時終了を実装する。
64. [x] 改善I-007のDebug／Release全CTestとportable成果物を確認する。
65. [ ] 改善I-007をユーザー実機で受け入れる。
66. [x] 改善I-008の枠なしpopカラーiconを生成して共通resourceへ反映する。
67. [x] 改善I-008のRelease全CTestとportable成果物を確認する。
68. [ ] 改善I-008をユーザー実機で受け入れる。
69. [x] 動的解像度再発logと複数session相互停止を分析する。
70. [x] 動的解像度の有限retryとsession専用COM STAを実装する。
71. [x] 回帰test、ADR、診断field、Version `0.7.10.0`成果物を更新する。
72. [x] ユーザー実機で動的解像度と複数session終了を再確認する。
73. [x] `AplMain.java`の形式を参考に全sourceのコメント規約を更新する。
74. [x] 全88 sourceを遡及監査し、コメント契約CTestを追加する。
75. [x] 改善I-009のDebug／Release全30 CTestと監査記録を完了する。
76. [x] 改善I-010のセッション全画面buttonを実装する。
77. [x] 改善I-010のDebug／Release候補と全30検証を完了する。
78. [x] Version `0.7.11.0`を標準`out`へ生成し、全30 CTestを確認する。
79. [ ] 改善I-010をユーザー実機で受け入れる。
80. [x] 回帰修正R-002の動的解像度全画面monitor追従を実装する。
81. [x] Version `0.7.12.0`のDebug／Release全30 CTestと成果物を確認する。
82. [ ] 回帰修正R-002をユーザー実機で受け入れる。
83. [x] 回帰修正R-003のログイン前一時失敗限定再試行を実装する。
84. [x] Version `0.7.13.0`候補のDebug／Release全30 CTestと成果物を確認する。
85. [x] 実行中Client停止後にVersion `0.7.13.0`を標準`out`へ生成する。
86. [ ] 回帰修正R-003をユーザー実機で受け入れる。
87. [x] 社内試行方針に基づき、残存観察項目を明示した最終仕様書と利用手順書を作成する。
88. [ ] 最終文書のユーザー受入を得る。
89. [ ] 最終versionへAuthenticode署名し、Release Gateを確認する。
90. [x] Version `0.7.13.0`の終了後残留processを固定logとwait chainで分析する。
91. [x] 回帰修正R-004の明示Disconnect／OnDisconnected終了経路を実装する。
92. [x] Version `0.7.14.0`のDebug／Release全30 CTestと成果物を確認する。
93. [x] 回帰修正R-004をユーザー実機で受け入れる。
94. [x] Version `0.7.14.0`へユーザーがAuthenticode署名し、署名状態、hash、version、
    SHA-256、RFC 3161 timestampの静的検証を完了する。
95. [x] 署名後のClient／Agent、RDP、Bridge、Proxyのsmoke testを完了する。
96. [ ] 最終文書受入、組織固有の署名規定確認、配布package確認を完了し、Release
    Gateを通過する。
97. [x] 回帰修正R-005の二段階接続再試行をClient限定で実装する。
98. [x] Debug／Release各30 CTestとAgent成果物非変更を確認する。
99. [ ] 回帰修正R-005をユーザー実機で受け入れる。
100. [x] R-005 ClientへAuthenticode署名し、静的検証を完了する。
101. [x] R-006のcertificate警告差分をWindows標準Clientと比較して特定する。
102. [x] ActiveX security dialog互換設定、認証level readback、focus保護を実装する。
103. [x] 認証UI表示後をR-005自動retryから除外し、対象testを成功させる。
104. [x] R-006のDebug／Release全CTestとClient限定成果物を確認する。
105. [x] R-006初回候補で警告が即時dismissされ、retry除外だけが機能したことを確認する。
106. [x] warning dialog表示propertyの回帰を確認して撤回し、各30 CTestを再実施する。
107. [x] 撤回後Clientで既存接続先の回復をユーザー実機確認する。
108. [x] certificate警告の次候補を設計し、ActiveX既定dialog親を実装する。
109. [x] R-007のDebug／Release全CTestとClient限定成果物を確認する。
110. [x] R-007で問題接続先のcertificate警告を明示承認し、LoginCompleteへ到達する。
111. [x] R-007で従来接続先への回帰がないことと警告取消時の重複errorを確認する。
112. [x] R-008で警告拒否後の重複error抑止を実装し、全ローカルGateを通過する。
113. [x] R-008で警告拒否後に対象sessionだけが静かに閉じることを実機確認する。
114. [x] R-008 ClientをAgentと同じ署名者で署名し、Windows trust、SHA-256、
     RFC 3161 timestamp、Versionを静的検証する。
115. [ ] R-008署名後Clientの起動、RDP、Bridge、Proxy smoke testを実機確認する。
116. [x] Agent終了logとWindows event logを突合し、一時network link断による
     RDP transport切断とSVC error 233の時系列を特定する。
117. [x] 回帰修正R-009のcurrent RDP session限定channel回復設計を修正提案へ記録する。
118. [x] 旧stream／Socketを破棄して同名`ESVNBRG`を有限backoffで再openするAgentを
     実装し、Version `0.7.15.0`のDebug／隔離Release各30 CTestを成功させる。
119. [x] 起動中Client終了後にVersion `0.7.15.0`のClient／Agentを標準`out`へ生成し、
     Release全30 CTestを成功させる。
120. [x] 一時link断後にAgent processが残り、RDP自動再接続後に再起動なしで
     Proxy利用可へ戻ることをユーザー実機で確認する。
121. [x] ユーザーの明示指示によりVersion `0.7.15.0`のClient／Agentを同一署名者で
     署名し、Windows trust、SHA-256、RFC 3161 timestamp、Versionを静的検証する。
122. [x] R-009主要回復経路の実機受入後、ユーザーの明示指示を受けてVersion
     `0.7.15.0`のPrivate GitHub Release作成へ進む。

日程見積りは、ActiveX host方式を確定し、Phase 0とPhase 1で実環境固有の技術riskを測定した後に更新する。Gate待ち時間を開発工数へ混在させない。
