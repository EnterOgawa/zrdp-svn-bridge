# ZRdp SVN Bridge 最終仕様書

- 文書ID: `ZRDP-SPEC-001`
- 対象Version: `0.7.14.0`
- 対象Platform: Windows x64
- 文書状態: 社内試行初版
- 更新日: 2026-08-03

## 1. 文書の目的

本書は、ZRdp SVN Bridgeの製品構成、RDP機能、Application Protocol、localhost
HTTP Proxy、Agent、設定、security、診断、制約をVersion `0.7.14.0`の実装に合わせて
集約した仕様書である。

実環境のIP address、URL、repository名、利用者名、credential、画面captureは
記載しない。設定例にはRFC 5737の文書用addressだけを使用する。

## 2. 製品の目的

ZRdpは、Zscalerを常時ONにした開発PCから、既存のRDP接続で到達可能な支給PCを経由し、
支給PCから業務上許可されているHTTP方式SVN／Trac endpointを利用するための専用製品で
ある。

達成する要件は次のとおりである。

- 開発PCと支給PCの間にRDP以外の新しい受信portを設けない。
- 支給PCをVPN Gateway、IP router、汎用Proxyとして動作させない。
- 開発PC上の既存SVN URLを変更せず、JavaHLからread／write操作を行う。
- 承認済みTracをMylynから利用する。
- RDP画面、keyboard、mouse、clipboard、全画面操作を通常用途で利用する。
- 複数の許可済みHTTP endpoint／repositoryと複数RDP sessionを有限範囲で扱う。
- credential、source、diff、HTTP payloadをZRdp自身で保存または記録しない。

## 3. 非目標

ZRdpは次を実装しない。

- RDP protocol、NLA、CredSSP、画面／入力転送、暗号化の独自実装
- `mstsc.exe`のコピー、改変、注入、偽装起動、security設定の緩和
- Zscaler、Firewall、GPO、routing、NIC、OS Proxyの自動変更
- VPN、SOCKS、TUN／TAP、汎用HTTP Proxy、任意TCP／UDP forward
- HTTP `CONNECT`、HTTPS tunnel、`svn://`、`svn+ssh://`、SSH、SMBの中継
- DNS探索、port scan、host列挙、CIDR／wildcard allowlist
- RDP／SVN／Trac credentialの独自保存
- Agentのservice化、driver、inbound TCP listener
- HTTP request／response bodyのcache、inspection、dump

## 4. 製品構成

### 4.1 実行ファイル

| 実行ファイル | 実行場所 | 役割 |
|---|---|---|
| `ZRdpSvnBridge.exe` | 開発PC | RDP Client、接続管理UI、loopback HTTP Proxy、SVC Client |
| `ZRdpSvnAgent.exe` | 支給PCの接続中RDP session | SVC Agent、allowlist判定、許可済みendpointへのoutbound TCP中継 |

両方ともWindows x64のportable EXEであり、installer、service登録、registry登録、
管理者権限を必要としない。Windowsにインストール済みのRemote Desktop ActiveX
Controlとsystem DLLを使用し、RDP DLLを同梱しない。CRTは`/MT`で静的linkする。

### 4.2 通信経路

```text
JavaHL SVN／Mylyn Trac
        │ HTTP Forward Proxy
        ▼
127.0.0.1:<session専用port>
        │
ZRdpSvnBridge.exe
        │ Static Virtual Channel ESVNBRG
        ▼
既存のRDP transport
        │
ZRdpSvnAgent.exe
        │ allowlist完全一致後のoutbound TCP
        ▼
許可済みHTTP SVN／Trac endpoint
```

ClientのlistenerはIPv4 loopbackだけである。Agentはinbound portを作成しない。

## 5. Client機能仕様

### 5.1 接続管理Window

接続管理Windowは次を提供する。

- 任意の接続名
- 接続先PC名またはIP address
- 任意のドメイン
- ユーザー名
- 3種類の初期表示mode
- マイク転送ON／OFF
- リモート音声の再生先
- 新しい接続の開始
- 生存session数と次のProxy port表示

接続名、接続先、ドメイン、ユーザー名は編集可能なcombo boxである。認証完了した
直近10組を大小文字無視で重複排除し、Windows DPAPI current-user scopeで保存する。
接続名は最大64文字である。passwordは履歴modelへ取り込まない。

ドメインとユーザー名は別fieldとしてActiveXへ適用する。ドメインが空で
`DOMAIN\user`形式が入力された場合だけ旧履歴互換として分離する。UPNは推測せず
そのまま使用する。

### 5.2 RDP認証

- Microsoft標準credential UIを使用する。
- CredSSPを有効化してreadbackし、NLAを弱めない。
- server certificate警告や認証警告を自動承認しない。
- Windows更新後もActiveX containerから利用者がcertificate警告を操作できるよう、
  Microsoft公開のconnection security dialog互換設定を接続前に適用する。
- credential UIと後続certificate UIは、session top-level Windowを
  `UIParentWindowHandle`へ上書きせず、ActiveX Control Windowの既定親へ一貫して
  委譲する。
- `AuthenticationLevel=2`を設定後にreadbackし、警告なしまたはhard-failへ変化した
  Controlでは接続を開始しない。
- Microsoft認証dialogを表示した接続試行は、切断reasonが一時失敗と一致しても自動で
  再試行しない。利用者の警告判断やpassword入力を反復しないためである。
- 同一試行で認証warningを二回以上表示した後、login未完了、非local、credential取消
  以外、`263/0`完全一致で切断した場合は、certificate警告の取消候補として追加errorを
  表示せず、そのsession Windowだけを閉じる。
- credentialをINI、log、command line、履歴へ保存しない。
- ActiveXだけが接続できない場合に`mstsc.exe`へfallbackしない。

### 5.3 Session Window

各RDP接続は独立したtop-level Windowと専用COM STAを持つ。Windowはtitle bar、
表示操作一行、RDP表示領域で構成する。

titleには、接続名または接続先、RDP状態、Agent状態、Proxy状態、割当port、active
stream数、`C->A`／`A->C`累積byteを表示する。接続先はUIには表示するが診断logへ
記録しない。

一つのClient processは最大16 RDP sessionを管理する。一sessionを閉じても別sessionの
ActiveX、Agent、Proxy、message loopへ終了処理を波及させない。

### 5.4 表示mode

| Mode | Remote desktop | Client表示 | Window変更時の接続先layout |
|---|---|---|---|
| 等倍表示（スクロール） | 初期寸法を維持 | 等倍、必要時scroll | 変更しない |
| Smart Sizing（縮小表示） | 初期寸法を維持 | Client側だけ縮小 | 変更しない |
| 動的解像度（Window追従） | RDP表示領域へ追従 | 等倍 | 変化し得る |

既定は等倍表示である。等倍表示とSmart Sizingは現在monitor全体を初期remote desktop
寸法とする。動的解像度は現在のRDP表示領域を初期寸法とし、最後のWindow変更から
250 ms後に`UpdateSessionDisplaySettings`を呼ぶ。

動的解像度はログイン完了後だけ適用する。Microsoft Controlが一時的に
`E_UNEXPECTED`を返した場合だけ、500 ms、1,000 ms、1,500 ms後に最大3回再試行する。
回復しない場合はRDPを維持して等倍表示へ縮退する。表示寸法変更に`Reconnect`を
使用しない。

通常Windowを辺または角から手動resizeした場合、title bar、frame、表示操作欄を除く
RDP領域を接続時desktopのアスペクト比へ制約する。最大化、Windowsスナップ、
ActiveX全画面にはこの制約を適用しない。

### 5.5 全画面

Session Windowの「全画面」buttonまたはWindows標準互換の`Ctrl+Alt+Break`で
Microsoft ActiveX全画面へ移行する。全画面状態はActiveX eventを正本とし、button
表示を「全画面」／「ウィンドウ」へ更新する。

動的解像度では全画面monitor全体へremote desktopを追従させ、Window復帰後はRDP
表示領域へ再追従させる。全画面connection barには製品名と接続先を表示する。

### 5.6 Clipboardと音声

- Clipboardは接続前に明示的に有効化し、Microsoft Controlへ委譲する。
- ZRdpはclipboard内容を取得、保存、記録しない。
- マイク転送は既定OFFである。
- リモート音声は「このPCで再生」「接続先PCで再生」「再生しない」から選択する。
- 音声設定は接続開始前に適用し、確立済みsessionへ途中反映しない。
- GPOまたはWindows privacy設定を変更、回避しない。

### 5.7 切断と再接続

- ZRdp自身の切断要求では対象sessionだけを終了する。
- 接続中Session Windowの閉じる操作は`Disconnect()`を呼び、`OnDisconnected`後に
  ActiveXとWindowを破棄する。既に切断中なら二重要求せず完了eventを待つ。
- Microsoft credential UIの所定の取消条件では追加error dialogを出さない。
- ログイン完了後の最終外部切断ではerror dialogを出さず、対象Windowを閉じる。
- Microsoft Controlの自動再接続中はWindowを維持する。
- ログイン前の`discReason=263`かつ`ExtendedDisconnectReason=0`完全一致だけを、
  2秒後に最大一度再試行する。
- 限定再試行でpasswordを取得せず、security設定を変更しない。

## 6. Static Virtual Channel

Static Virtual Channel名は`ESVNBRG`である。ClientはActiveXの`Connect()`より前に
channelを作成する。Agentはcurrent active RDP session内で
`WTSVirtualChannelOpenEx`を使用して同じchannelを開く。

一回のtransport chunkは最大1,600 byteであり、Application Protocolのframe境界とは
みなさない。Client受信callbackはBSTRを`SysStringByteLen`でbinaryとして所有bufferへ
copyし、worker queueへ渡して速やかに復帰する。workerからActiveXへの送信要求は
Window MessageでsessionのCOM STAへmarshalする。

Agentは一つのowner loopだけがWTS read／writeとSocket pumpを行う。同じchannel
handleを複数threadから同時操作しない。

## 7. ESVNBRG Application Protocol

### 7.1 基本規則

- Protocol version: `1`
- Wire byte order: big-endian
- Frame header: 24 byte
- Maximum frame payload: 65,536 byte
- Decoder input upper bound: 1 MiB
- Production maximum streams: 8
- Stream initial window: 262,144 byte
- Connection window: 最大4 MiB
- `streamId=0`: connection-level control予約
- 新規`OPEN`はClientだけが開始する。

### 7.2 Frame header

| Offset | Size | Field | Version 1の規則 |
|---:|---:|---|---|
| 0 | 4 | magic | ASCII `ESVN` |
| 4 | 2 | version | `1` |
| 6 | 2 | header size | `24` |
| 8 | 2 | message type | 固定列挙値 |
| 10 | 2 | flags | `0` |
| 12 | 4 | streamId | connection controlは`0` |
| 16 | 4 | sequence | message規則に従う |
| 20 | 4 | payload length | `0..65536` |

構造体castや`#pragma pack`に依存せず、各整数を明示的にencode／decodeする。不正magic、
version、type、length、sequence、stateは再同期せずfail-closedとする。

### 7.3 Message type

| Value | Type | 目的 |
|---:|---|---|
| 1 | `HELLO` | capability開始 |
| 2 | `HELLO_ACK` | capability応答 |
| 3 | `OPEN` | Clientからstream開始 |
| 4 | `OPEN_OK` | Agentの接続成功 |
| 5 | `OPEN_ERROR` | policy／接続／資源error |
| 6 | `DATA` | binary data |
| 7 | `WINDOW_UPDATE` | flow-control credit回復 |
| 8 | `HALF_CLOSE` | 一方向終了 |
| 9 | `CLOSE` | stream／connection終了 |
| 10 | `PING` | Echo／health確認 |
| 11 | `PONG` | Echo／health応答 |
| 12 | `PROTOCOL_ERROR` | connection-level停止 |

### 7.4 Streamとflow control

`OPEN` payloadはIPv4 4 byteとTCP port 2 byteだけであり、hostname、CIDR、port rangeを
表現できない。stream IDは1から単調増加し、同じBridge session内で再利用しない。

`DATA` sequenceは方向ごとに1から単調増加する。streamとconnection両方のcreditが
ある場合だけ送信し、downstreamで実際に消費したbyteだけを`WINDOW_UPDATE`で返す。
credit水増し、duplicate ID、欠落／重複sequence、invalid transitionはBridge sessionを
停止する。

`HALF_CLOSE`は一方向だけを終了し、反対方向は継続できる。`CLOSE`は固定数値reason
だけを持つ。独自圧縮、暗号化、再送、途中resumeは実装しない。

### 7.5 Health check

Ready後は5秒間隔で一件だけhealth `PING`を送る。15秒無応答なら旧stream、decoder、
queue、sequenceを破棄し、同じRDP channel上で新しい`HELLO`境界を開始する。検出まで
最大約20秒を要する。RDP再接続後も旧stateを再利用しない。

## 8. localhost HTTP Proxy

### 8.1 Listener

- Bind先: IPv4 `127.0.0.1`固定
- 既定base port: `18080`
- 追加session: `base port + 未使用slot`
- 同時session: 最大16
- 一sessionの同時HTTP stream: 最大8
- `SO_EXCLUSIVEADDRUSE`を使用
- IPv6、`0.0.0.0`、LAN／Wi-Fi／VPN NICへlistenしない。

別processとportが競合した場合はProxyだけを`BindFailed`とし、port scanや自動fallbackを
行わない。RDP画面は可能な限り維持する。

### 8.2 HTTP request

対象はIPv4 literalをauthorityに持つ`http://`方式である。absolute-form request-targetを
origin-formへ変換し、URL authorityと`Host`のIPv4／portを完全一致させる。一つのlocal
connectionは最初の`IPv4:port`へ固定する。

次を扱う。

- HTTP/1.1 Keep-Alive
- `Content-Length`
- 単一`chunked` transfer encoding
- `Expect: 100-continue`
- Basic／Digest／NTLM等の認証challenge／response透過
- Cookie、redirect、gzip等のend-to-end data透過
- request／response bodyのstreaming

次を拒否する。

- HTTP `CONNECT`、HTTPS、Upgrade、WebSocket
- hostname、IPv6、DNS解決
- authorityと`Host`の不一致
- Keep-Alive中の宛先変更
- `Content-Length`と`chunked`の併用
- 未知のTransfer-Encoding

### 8.3 対象method

`OPTIONS`、`GET`、`HEAD`、`POST`、`PROPFIND`、`REPORT`、`MKACTIVITY`、
`CHECKOUT`、`PUT`、`DELETE`、`COPY`、`MOVE`、`MERGE`、`LOCK`、`UNLOCK`を
拒否しない。

### 8.4 Error mapping

| 状態 | Local HTTP response |
|---|---:|
| request不正 | `400` |
| 許可しないmethod／CONNECT | `405` |
| header上限超過 | `431` |
| 未対応転送形式 | `501` |
| allowlist拒否 | `403` |
| upstream接続失敗 | `502` |
| Bridge未ready／資源不足 | `503` |
| timeout | `504` |

error responseへURL、endpoint、header、body、OS error文字列を含めない。

## 9. Agent仕様

### 9.1 起動条件

AgentはZRdp Clientが接続したcurrent active RDPユーザーsessionで通常ユーザーとして
起動する。Session 0、console、idle、disconnected sessionでは安全に終了する。

### 9.2 Allowlist

AgentはEXEと同じdirectoryの`agent.ini`を起動時に一度だけ読む。設定全体の検証に
成功した場合だけstream capabilityを有効化する。

```ini
[allowlist]
endpoint.1001=192.0.2.10:80
endpoint.1002=198.51.100.20:8080
```

- file最大64 KiB、一行最大256 byte、最大128 entry
- `endpoint.<非0 uint32>`とstrict IPv4 literal／`1..65535` portだけを許可
- IDと`IPv4:port`の重複を拒否
- hostname、CIDR、wildcard、port range、IPv6、未知keyを拒否
- 一行でも不正なら部分採用せずEcho-onlyへ縮退
- `OPEN`と完全一致した後だけoutbound Socketを作成
- connect timeoutは5秒
- 別address、port、DNS、OS Proxyへfallbackしない。

Agentは設定path、IP、port、匿名ID、入力行内容をconsole／logへ出力しない。

### 9.3 Agent停止

Ctrl+Cではconnection-level `CLOSE`を送ってchannelを閉じる。ClientはRDPを維持して
Agent待機へ戻り、同じsession内でAgentを再起動できる。異常終了はhealth timeoutで
検出する。旧HTTP requestを自動再送またはresumeしない。

## 10. 設定仕様

### 10.1 Client history

保存先は次である。

```text
%LOCALAPPDATA%\ZRdpSvnBridge\connection-history.bin
```

DPAPI current-user scopeで暗号化する。Version 3は接続名、接続先、結合済み
domain／user、マイク設定、音声再生先だけを含む。passwordは含まない。

### 10.2 Optional client.ini

EXE隣接の`client.ini`でbase portだけを指定できる。

```ini
[proxy]
port=18080
```

不正fileでは別値へfallbackせずProxyをFailedにする。bind address、endpoint、
credential、timeoutは設定できない。

### 10.3 agent.ini

実環境設定はGit管理、共通配布、資料への転載を禁止する。変更はAgent再起動後に
反映し、live reloadしない。

## 11. Resource上限とtimeout

| 項目 | 上限／期限 |
|---|---:|
| Client processのRDP session | 16 |
| 一sessionのHTTP／Agent stream | 8 |
| Frame payload | 64 KiB |
| Decoder input | 1 MiB |
| Stream window | 256 KiB |
| Connection window | 4 MiB |
| Client Frame queue | 4,096件／4 MiB |
| Client SVC queue | 4,096 chunk／5 MiB |
| Agent writer queue | 256 chunk／256 KiB |
| HTTP header | 64 KiB |
| 一connection一方向の未送信data | 256 KiB |
| 全connection一方向の未送信data | 2 MiB |
| 初回HTTP header完成 | 30秒 |
| Upstream connect | 5秒 |
| Agent health応答 | 15秒 |
| OPEN後の完全無通信 | 30分 |

上限超過はbufferを無制限に増やさず、該当streamまたはBridgeをfail-closedで終了する。

## 12. Security仕様

- Client listenerはloopback以外から接続できない。
- Agentはallowlist完全一致以外へ接続できない。
- Agentはinbound port、DNS、network探索を使用しない。
- Clientだけが新しいstreamを開始できる。
- HTTP／SVN／Trac payloadとcredentialを解析、保存、記録しない。
- RDP password、SVN password、Trac passwordをZRdpのdata modelへ取り込まない。
- Logは固定event名と数値fieldだけを記録する。
- Malformed frame、queue超過、protocol不一致はBridgeを停止する。
- Bridge障害時もRDP画面を可能な限り維持するが、Proxyを利用可能とは表示しない。
- Zscaler、certificate、GPO、Firewall、routing、OS Proxyを自動変更しない。

## 13. 診断仕様

ClientとAgentは役割別のfile名を使用する。

```text
EXE隣接\logs\ZRdpSvnBridge.log
EXE隣接\logs\ZRdpSvnAgent.log
```

EXE隣接へ書き込めない場合は次へfallbackする。

```text
%LOCALAPPDATA%\ZRdpSvnBridge\logs\
```

current logは最大1 MiB、backupは`.1`～`.3`の3世代である。timestampはUTC、
各recordは固定key-value形式である。RDP接続先、domain、user、URL、HTTP header／body、
Cookie、Authorization、source、diff、clipboard、SVC payloadを記録しない。

## 14. 配布仕様

- Installerを使用しない。
- ClientとAgentは同じVersionの組を配布する。
- 実環境`agent.ini`は許可された管理経路で別途用意する。
- Optional `client.ini`はbase port変更が必要な場合だけ用意する。
- PDB、log、dump、capture、実設定、credentialを配布物へ含めない。
- 社内配布前にClient／Agent両方へAuthenticode署名し、署名、Version、hashを確認する。
- Repository、Release、配布先はPrivate／社内限定とし、公開Releaseを作成しない。

## 15. 検証状態

Version `0.7.14.0`は次のローカル検証を通過している。

- Visual Studio 2022 x64 Debug／Release build、warning 0
- Debug／Release各30 CTest
- Protocol、flow control、queue、malformed input、allowlist、loopback bind
- Mock HTTP／WebDAV／TracとClient／SVC／Agent production経路
- 未接続、接続中、切断中のWindow終了開始policy

基礎となるVersion `0.7.13.0`以前では、次の実機検証を通過している。

- 実RDP、credential UI、keyboard、mouse、clipboard、3表示mode
- 実SVN Repository Explorer、Log、Diff、Checkout、Update
- 実SVN Commit、Add、Delete、Move、conflict、Lock、Unlock
- 実Mylyn Tracの設定検証と同期
- Agent Ctrl+C、異常終了検出、同一RDP session内の再起動
- 複数RDP sessionの独立COM STAと、一session終了後の別Window継続

Version `0.7.14.0`では、2026-07-31に実RDP接続中の閉じる操作後にSession Windowと
Client processが残留しないことをユーザー環境で確認し、固定診断logでも
`rdp.event.disconnected`と`process.stopping`への到達を確認した。社内試行では、
長時間利用、二つのAgent／Proxy同時利用、音声device／GPO別条件、最終icon表示、
外部切断、まれなログイン前限定再試行を継続観察する。

## 16. 既知の制約

- HTTP IPv4 literalだけを対象とし、hostname／HTTPSへ対応しない。
- Agent設定変更には再起動が必要である。
- RDP切断、Agent restart、timeout時に途中HTTP requestをresumeしない。
- 30分完全無通信のKeep-Aliveは終了する。
- 複数Client process間でProxy port slotを共有しない。
- JavaHL等の一つのProxy port設定は一つのRDP sessionへ対応する。
- 音声設定は接続中に変更できない。
- 最大化、Windowsスナップ、全画面は手動アスペクト比制約の対象外である。
- Process全体のcrash isolationは提供しない。
- Static Virtual Channelの実環境性能が不足しても、実測と承認なしにDVCへ変更しない。

詳細は[既知の制約](known-limitations.md)を参照する。

## 17. 関連文書

- [利用手順書](user-guide.md)
- [設定仕様](configuration.md)
- [RDP ActiveX設計](rdp-activex.md)
- [Application Protocol](protocol.md)
- [localhost HTTP Proxy](http-proxy.md)
- [Agent設計](agent.md)
- [Security方針](security.md)
- [診断ログ](diagnostics.md)
- [テスト計画](test-plan.md)
- [社内配布・署名確認書](release-checklist.md)
