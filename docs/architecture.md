# ZRdp SVN Bridge アーキテクチャ

更新日: 2026-10-11

現行版では、このsession専用STA／Bridge／Proxy構成にmanagerの接続別Agent設定・
自動配置と復旧・設定変更時の協調再起動・署名必須のセット更新を追加している。
規定の正本は [現行仕様](current-specification.md)。以下のVersion／Phaseに言及する
検証経緯は当時の記録であり、旧操作や旧受入を現行へ推定適用しない。

## 1. 製品の到達目標

ZRdpは、Zscalerを通常どおり有効にした支給PCを経由して、開発PC上の既存SVN
クライアントからHTTP方式SVNを利用できるようにする。Phase 9では同じ制限付き
HTTP経路を、ユーザーが明示承認したHTTP TracとMylyn Trac Connectorにも拡張する。
開発PCと支給PCの間で
新たに使用する経路は、既存のRDPセッション内に登録したStatic Virtual Channel
`ESVNBRG`だけである。

```text
開発PC
  SVN client／Mylyn Trac Connector
      |
      | 127.0.0.1:<session専用port> HTTP Forward Proxy
      v
  ZRdpSvnBridge.exe
      |  Remote Desktop ActiveX + ESVNBRG
======|================================================ RDP
      v
  ZRdpSvnAgent.exe
      |
      | allowlist完全一致後のoutbound HTTP接続
      v
支給PCの既存HTTP経路 -> Zscaler -> 許可済みHTTP SVN／Trac endpoint
```

RDP画面接続、Agent接続、HTTP転送は別の状態として管理する。Bridgeが停止しても
RDP画面は可能な限り維持し、RDPだけが接続できた状態をSVN／Trac利用可能とは
表示しない。

## 2. Version 0.7.14 runtimeと複数RDP session

Version `0.7.14.0`は、Version `0.6.0.0`までに成立したAgent死活確認、
slow peer timeout、異常終了後の安全なsession再確立を維持したまま、接続管理
Windowから複数の独立RDP session Windowを生成できるようにする。接続管理には
任意の名前と接続時のマイク／リモート音声設定を保持できるが、wire protocol
versionは`1`のままとし、frame形式、feature bit、Agent設定を変更しない。

```text
ZRdpSvnBridge.exe
  |-> manager COM STA
  |     `-> ClientApplication
  |           |-> ConnectionManagerWindow
  |           |     `-> 名前付き接続入力／音声設定／共通DPAPI履歴
  |           |-> Proxy port allocator
  |           `-> session thread records
  |
  |   thread messageによる完了／履歴通知
  |
  |-> session COM STA 1
  |     `-> MainWindow (session 1)
  |           |-> Remote Desktop ActiveX + ESVNBRG
  |           |-> ClientBridge
  |           `-> ClientProxyWorker -> 127.0.0.1:18080
  |
  `-> session COM STA 2
        `-> MainWindow (session 2)
              |-> Remote Desktop ActiveX + ESVNBRG
              |-> ClientBridge
              `-> ClientProxyWorker -> 127.0.0.1:18081
                      |
                      | bounded Frame queue -> session COM STA
                      v
                 対応RDP session内のZRdpSvnAgent.exe
                   -> AgentBridge
                   -> AgentStreamRelay
                   -> Allowlist
                   -> stream table
                   -> allowlist完全一致済みnon-blocking outbound TCP
```

- `ClientApplication`はmanager COM STAで接続管理Window、最大16件のsession
  専用STA、有限Proxy port slotを所有する。
- 接続管理WindowはRDP ActiveXやProxyを所有せず、任意の接続名を含む接続入力、
  表示mode、共通のDPAPI接続履歴だけを管理する。名前選択時は接続先、domain、
  userを一組で復元する。
- 各session専用STAは一つの`MainWindow`、ActiveX、event sink、Virtual Channel、
  `ClientBridge`、`ClientProxyWorker`を生成から破棄まで独立所有する。同じ
  `ESVNBRG`名でもRDP transportが異なるため、Bridge stateとHTTP streamを
  session間で共有しない。
- `client.ini`の`[proxy] port`はbase portである。最初のsessionは既定`18080`、
  後続sessionは`18081`以降の未使用最小slotを使い、終了時にそのslotだけを返す。
- session Windowはtitle bar、表示modeだけのコンパクトな一行、RDP表示で構成する。
  titleは名前があれば`接続名 (接続先)`、なければ接続先を先頭に置き、
  RDP／Agent／Proxy状態、実際の`127.0.0.1:<割当port>`、active stream数、
  方向別転送byte数を表示する。
- 各sessionは接続時desktop比率を独立保持する。通常の辺／角dragだけは、
  非client領域と表示mode欄を除くRDP表示領域を同じ比率へ制約する。
  最大化、Windowsスナップ、ActiveX全画面はWindows標準動作へ委譲する。
- 一sessionのRDP切断、Agent停止、Proxy bind失敗、Window終了は、そのsessionの
  stream、Socket、worker、ActiveXだけを停止する。

- ClientはActiveXの`Connect`より前に`ESVNBRG`を登録する。
- ClientとAgentは`HELLO`／`HELLO_ACK`でversionと有限上限を確認する。
- Clientは1 byte、1 KiB、64 KiB、1 MiB、10 MiBのbinary Echoを実行する。
- Echo-only交渉時は、Agentが同じpayloadを`PONG`として返しTCP接続を作成しない。
- AgentはCtrl+C終了時にconnection-level `CLOSE`を送信し、ClientはRDPを
  維持したまま新AgentのHELLO待機へ戻る。
- Clientは成立済みBridgeへ5秒間隔で一件の`PING`を送り、15秒以内に対応する
  `PONG`がなければ旧streamを全破棄して同じRDP内で新しい`HELLO`を開始する。
- 有効な`agent.ini`があるAgentだけがstream capabilityを提示する。
- Agentは`OPEN`のIPv4とportがallowlistへ完全一致した後だけSocketを作成する。
- Clientは`127.0.0.1`だけをlistenし、compatible HELLO成立後に最大8本の
  local HTTP connectionを別streamへ割り当てる。
- base portは既定`18080`とし、EXE隣接の固定`client.ini`にある
  `[proxy] port=<1..65535>`だけを任意設定として受け付ける。
- absolute-formをorigin-formへ変換し、URL authorityと`Host`の一致を検証する。
- `Content-Length`と検証済み単一`chunked` request body、およびopaque responseを
  有限queueとflow controlでstreamingする。
- 一local connectionは一upstream Socketへ固定し、Keep-Aliveと
  connection-oriented認証でSocketを継続する。
- `Expect: 100-continue`、Authorization、Cookie、WebDAV headerは内容を解釈せず
  end-to-end byteとして中継する。
- Bridge未ready、allowlist拒否、接続失敗は、endpointを含まない固定HTTP responseで
  local接続を終了する。
- 最初のHTTP headerを30秒以内に完成できないlocal peerと、30分間完全に
  無通信のlocal／upstream connectionは`CLOSE(Timeout)`で有限に終了する。

Phase 5では二つのproduction local SocketとSVC encode／decodeを通し、
異なるserver／port／repository、401 NTLM challenge後の同一Socket再request、
chunked、`100 Continue`、対象WebDAV／DeltaV全methodをMockで確認した。
2026-07-31にはJavaHLから既存repository URLを変更せず実環境へ接続し、
Phase 7 read-onlyとPhase 8 writeの全対象操作についてユーザー受入を得た。
操作内容は通常logへ記録せず、固定stateとerrorだけで通信経路を監査する。

Phase 2では、Socket／COM／WTS APIを参照しない共有`StreamMultiplexer`を
`zrdp_protocol`へ追加した。

```text
fake Client Socket events
    -> StreamMultiplexer(Client)
    -> bounded Frame queue
    -> in-memory transport
    -> StreamMultiplexer(Agent)
    -> fake Agent Socket events
```

- Clientだけが非0 stream IDを単調増加で割り当てる。
- 最大32 stream、stream初期window 256 KiB、connection window最大4 MiBとする。
- DATA受信callerがdownstreamへ消費したbyteだけWINDOW_UPDATEでcreditを返す。
- outbound／inbound queueは各4,096件かつ4 MiBへ制限する。
- duplicate ID／sequence、invalid state、credit水増し、queue超過をfail-closedにする。
- HALF_CLOSE、normal close、timeout、peer reset、crossed closeを有限状態で扱う。

Version `0.6.0.0`で、このengineのAgent roleを`AgentStreamRelay`、Client roleを
`ClientProxySession`へ組み込んだ。Version `0.7.14.0`でも一session当たりの
production capabilityを同時8 streamへ制限する。
一stream各方向256 KiB、Client／Agent全stream合計各2 MiBを上限とし、各owner
loopは最大32 operationでround-robin処理する。Phase 4の512 KiB超request／
384 KiB超response Gateも回帰testとして維持する。

## 3. Clientの責務とthread境界

`ZRdpSvnBridge.exe`のmanager COM STAは次を所有する。

- 接続管理Window
- session専用STAの有限な所有一覧
- sessionごとのProxy port slot
- sessionからmarshalされた接続履歴通知

各session専用COM STAは、対応するsession Window、Remote Desktop ActiveX、
event subscription、`ClientBridge` protocol state、`CreateVirtualChannels`と
`SendOnVirtualChannel`呼出しを所有する。session WindowのmessageはそのSTAだけで
dispatchし、COM interfaceを別STAへ渡さない。一sessionのActiveX終了処理が待機
しても、managerと他sessionのmessage loopは継続できる。

sessionの`WM_NCDESTROY`後に専用STAがActiveXとCOMを解放し、完了thread messageを
managerへ送る。managerは終了済みthreadだけをjoinし、対応するProxy slotを一度だけ
解放する。接続履歴も所有queueを介してmanager STAへmarshalし、session threadから
接続管理Windowへ直接触れない。詳細な判断はADR 0005に記録する。

`OnChannelReceivedData`では、BSTRを`SysStringByteLen`でbinary dataとして
所有bufferへcopyする。callback内ではActiveXへ応答を返さず、decode済みstream
Frameを最大4,096件／4 MiBのbounded queueへ登録し、Proxy workerへ渡す。
workerが生成したFrameは逆向きの同上限queueからUI Window Messageへmarshalする。
UI側のSVC送信queueは最大4,096 chunk／5 MiBとし、一回のdispatchで送るchunk数を
8件へ制限してRDP描画と入力処理へ制御を戻す。

各sessionのProxy workerは専用listener、最大8 local Socket、connection別HTTP
parser、Client multiplexerを一threadで所有し、一回のloopで有限operationだけを
行う。
connection別local／upstream未送信queueは各256 KiB、各方向の全connection合計は
2 MiB、UI／worker command queueは最大4,096件／4 MiBである。

Agent待機中だけ1秒間隔でHELLOを再送する。handshake成立後の周期HELLOはAgentの
active streamをresetするため送らず、既存`PING`／`PONG`を死活確認にも使用する。
Agentの正常終了ではconnection-level `CLOSE`、異常終了では最長約20秒
（最初の5秒intervalと15秒deadline）のhealth timeoutにより、RDPを維持したまま
旧Proxy sessionを破棄して再handshakeする。

Echoでは一度に一つのPINGだけを未応答とし、15秒以内に完全なPONGを確認できなければ
Bridgeだけをfail-closedで停止する。Echo開始前にProxy streamを停止し、Echo完了後に
同じnegotiated capabilityでProxy sessionを再開する。

Proxy workerは接続受付から30秒の絶対期限で最初のHTTP header完成を要求する。
headerの部分受信では期限を延長しない。OPEN開始後はlocal Socketの送受信進捗を
単調clockで記録し、30分間進捗がなければ対象connectionだけを終了する。

## 4. Agentの責務とthread境界

`ZRdpSvnAgent.exe`はinstaller、service、driver、管理者権限を必要としない
x64 portable console EXEである。起動時に現在processのsessionだけを検査し、
次をすべて満たす場合に限り`ESVNBRG`を開く。

- Session 0ではない
- WTS stateが`WTSActive`
- `WTSClientProtocolType`がRDPを表す`2`

Agentの一つのI/O loopだけが`WTSVirtualChannelRead`と
`WTSVirtualChannelWrite`を順番に呼ぶ。Protocol応答は最大256 chunkかつ
256 KiBのserialized writer queueへ登録し、同じloopがwire順にdrainする。
同じloopが100 msのWTS timeoutごとに最大8 upstream Socketの
non-blocking connect／send／receiveを合計最大32 operationだけround-robinで
pumpする。同一WTS channel handleまたはSocketを複数threadから同時に操作しない。

100 msの有限read timeoutでCtrl+Cを確認する。正常終了時は空payloadの
connection-level `CLOSE`をwriter queueから排出してchannelを閉じる。
RDP切断、read／write異常、protocol異常ではqueueとchannelを破棄し、
全upstream Socketも閉じ、別経路へfallbackしない。Version `0.7.15.0`では、
一時RDP切断後もcurrent user sessionが存続する間はAgent processを維持する。
`WTSActive`復帰後に同名SVCだけを有限backoffで再openし、新しい`HELLO`からBridgeを
再確立する。旧streamのresumeやHTTP requestの自動再送は行わない。
接続済みupstream Socketもsend／receive進捗を単調clockで管理し、30分間
完全に無通信なら対象streamを`CLOSE(Timeout)`へ移してSocketを閉じる。

## 5. Security boundary

- RDP／SVN password、credential、HTTP／SVN payloadを保存または記録しない。
- 実接続先、ドメイン、ユーザー名、Virtual Channel payloadを診断logへ出さない。
- BSTRを文字列変換、Base64変換、NUL終端走査しない。
- malformed frame、非互換version、順序違反、queue上限超過はfail-closedとする。
- Phase 2のOPEN targetはIPv4 4 byteとport 2 byteだけを持ち、DNS名やport rangeを
  表現しない。
- streamとconnection両方のcreditがある場合だけDATAをqueueする。
- PROTOCOL_ERRORは固定数値codeだけを持ち、endpointやpayloadを含めない。
- Agentはallowlist完全一致後のoutbound Socketだけを作成し、TCP listen、DNS、
  network探索を一切行わない。
- 同時RDP session数は16件、各sessionのlocal HTTP connectionは8件に固定し、
  UI操作から無制限にActiveX、thread、Socket、queueを生成しない。
- 複数sessionでも全listenerはIPv4 loopbackだけをbindし、他sessionや別processの
  portへ自動fallbackしない。
- Zscaler、Firewall、EDR、GPO、routing、OS Proxy、RDP security設定を変更しない。

## 6. 後続Phaseとの境界

Phase 6ではwire protocolを変えず、malformed input、資源枯渇、timeout、
Agent crash／restart、Client restart、RDP disconnect／reconnect、allowlist、
loopback bind、log redactionをローカル検証した。Agent強制終了後の自動検出、
同一RDP内の再起動、実SVN read／write操作もユーザー環境で確認済みである。
Phase 9改善I-001ではwire protocolとproduction codeを変えず、Mylyn Tracの
repository固有Proxyが生成するWeb GET／XML-RPC POSTをMock経路で確認する。
改善I-002ではwire protocolとAgentを変えず、ClientのWindow所有とloopback
Proxy port割当を複数sessionへ拡張した。Version `0.7.14.0`では実機で判明した
session終了時の相互停止を防ぐため、各RDP ActiveXとWindowをADR 0005の
session専用COM STAへ分離した。
回帰修正R-003ではAgent、Proxy、wire protocolを変えず、ログイン未完了の
`discReason=263`、`ExtendedDisconnectReason=0`完全一致だけを同じsession STAの
Window timerから2秒後に一度再試行する。再試行時も接続設定を検証し直し、
Microsoft ActiveXと標準認証UIへ委譲する。
回帰修正R-005では、Microsoft RDP Clientが同じSSL失敗を二回経て三回目に成功した
実測に合わせ、一回目2秒、二回目5秒の有限表へ拡張する。Agent、Proxy、wire
protocolを変更せず、credential取消と異なる切断reasonの除外も維持する。
現版の検証境界と残存riskは`docs/known-limitations.md`に記録する。
