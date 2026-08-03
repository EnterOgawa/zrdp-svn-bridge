# ZRdp SVN Bridge セキュリティ方針

更新日: 2026-08-03

## 1. 接続履歴

接続名、RDP接続先、ドメイン、ユーザー名、接続時音声設定はpasswordではないが、
実環境を識別し得るprofile情報として保護する。

- 認証完了した接続だけを直近10組まで保持する。
- Windows DPAPIのcurrent-user scopeでファイル全体を暗号化する。
- 平文JSON、INI、command line、registry、EXE横のportable設定へ保存しない。
- password、Windows資格情報、音声data、device名は履歴data modelに含めない。
- 接続名は64文字、履歴全体は10件へ制限し、埋込みNULと過大fieldを
  保存前・復号後に拒否する。
- domainは暗号化前の履歴内で`DOMAIN\user`としてユーザー名と結合するが、
  DPAPI保護外のfile、log、resourceへは出力しない。
- Version 1／2履歴は音声設定を安全な既定値で補完する。Version 3の不正な
  microphone flag、音声mode、未知versionはfail-closedで拒否する。
- 履歴内容を診断ログ、Git管理ファイル、error messageへ出力しない。
- 復号失敗、形式不正、上限超過、重複recordはfail-closedで扱う。
- 保存時は同じdirectoryの一時ファイルへ暗号化dataを書き、置換する。
- file I/OとDPAPI処理は有限queueの専用workerで直列実行し、UI／COM threadをblockしない。

DPAPIは同じWindowsユーザーとして動作するprocessからのアクセスまで防ぐ境界ではない。履歴は利便性のために保持する非credential情報であり、支給PCの管理policyとWindowsユーザーaccountの保護を前提とする。

マイク転送はprivacy上の明示設定として既定OFFにする。ZRdpは接続時の
ON／OFFだけをMicrosoft RDP ActiveXへ渡し、音声streamを取得、解析、保存、
log出力しない。GPOやWindowsのprivacy設定を変更または回避しない。

ログイン前の限定再試行でもpasswordを取得、保持、再送せず、Microsoft標準認証UIへ
委譲する。login未完了の`discReason=263`、`ExtendedDisconnectReason=0`完全一致
かつMicrosoft認証dialogを一度も表示していない場合だけ、2秒後と5秒後に最大二回
再試行する。認証UI表示、credential取消、local切断、終了処理、成立済みsession、
三回目の失敗では再試行しない。

接続前に`RedirectionWarningDialogVersion=1`を設定し、Windows更新前のMicrosoft
connection security dialogを互換表示する。`AuthenticationLevel=2`をreadbackし、
server certificate検証失敗時の継続可否は利用者だけが選択する。NLA、CredSSP、
certificate検証、認証警告を変更または自動承認せず、certificate store、registry、
GPO、`mstsc.exe`、RDP cache操作へfallbackしない。
credential UIからcertificate UIへ続くmodal ownershipは、session top-level Windowを
`UIParentWindowHandle`へ上書きせず、ActiveX Control Windowの既定処理へ委譲する。
これは警告の表示先だけの互換候補であり、certificate検証結果や利用者判断を変更しない。

certificate警告拒否後の重複error抑止は、同一試行のwarning二回以上、login未完了、
非local、credential取消以外、`263/0`完全一致だけに限定する。ZRdpは警告buttonの
選択値を取得せず、certificateを信頼、登録、記憶しない。完全一致時もWindowsが返した
数値結果を固定logへ残し、対象session Windowだけを閉じる。条件が一つでも異なる場合は
通常のerror経路を維持する。

## 2. Static Virtual ChannelとProtocol

Phase 1以降は、Windowsが確立したRDPセッション内のStatic Virtual Channel
`ESVNBRG`だけをClient／Agent間のdata経路として使用する。

- ClientはActiveXの`Connect`より前に固定channel名を登録する。
- Agentはcurrent active RDP sessionだけでchannelを開く。
- AgentはSession 0、console、idle、disconnected sessionでは動作しない。
- application frameはmagic、version、type、streamId、sequence、lengthを
  個別に検証する。
- payload、decoder、Client／Agent queue、未応答PING数を有限値へ制限する。
- version不一致、malformed frame、順序違反、queue超過、15秒のEcho timeoutは
  fail-closedでBridgeだけを停止する。
- 成立済みBridgeは5秒間隔の一件だけのhealth PINGで確認し、15秒無応答なら
  旧streamを全破棄して同じRDP内で新しいHELLO境界を開始する。
- binary BSTRはbyte長で扱い、文字列変換、Base64変換、NUL終端走査を行わない。
- Phase 2のOPENはClientだけが開始し、IPv4 4 byteと非0 portだけを運ぶ。
- protocol coreは最大32 stream、256 KiB stream window、4 MiB connection
  window、4,096件／4 MiBの送受信queueを絶対上限とする。Version 0.7.14の
  production Client／Agentは同時8 streamだけを交渉する。
- DATAはstream／connection両方のcreditを消費し、downstream消費済みbyteだけを
  WINDOW_UPDATEで戻す。
- 未送信byteを超えるcredit、duplicate stream ID／sequence、invalid transitionを
  fail-closedで拒否する。
- PROTOCOL_ERROR、OPEN_ERROR、CLOSE reasonは固定codeだけを持ち、実endpoint、
  payload、OS error文字列を含めない。

Bridge失敗時もRDP画面を可能な限り維持するが、停止したBridgeをSVN利用可能とは
扱わない。RDP再接続時は旧decoder、sequence、queueを再利用しない。

## 3. Client Proxyのnetwork境界

Version `0.7.14.0`のClientは、RDP sessionごとのIPv4 loopback
`127.0.0.1:<割当port>`だけでHTTP Forward Proxyをlistenする。

- bind addressは利用者設定やcommand lineへ公開せず、production codeで
  `127.0.0.1`へ固定する。
- optional `client.ini`は`[proxy] port=<1..65535>`のbase portだけを表現でき、
  未知keyや不正fileではProxyをfail-closedにする。
- 同時RDP sessionを16件へ制限し、各sessionへ重複しない有限port slot、
  ActiveX、Bridge、Proxy workerを割り当てる。
- session終了時はそのlistener、stream、queue、slotだけを破棄し、他sessionへ
  close、fallback、state共有を行わない。
- `AF_INET`と`SO_EXCLUSIVEADDRUSE`を使用し、IPv6、wildcard、LAN、Wi-Fi、
  VPN NICへlistenしない。
- Hostはstrict IPv4 literalとportだけを受け付け、DNSやOS Proxyを使用しない。
- URL authorityと`Host`を完全一致させ、一local connectionの宛先を固定する。
- `CONNECT`、HTTPS、Upgrade、未知Transfer-Encoding、hostname、IPv6を拒否する。
- HTTP Trac対応後も、対象を承認済み`IPv4:port`へ限定し、redirectを理由に
  別endpoint、hostname、HTTPSへ接続しない。
- 単一`chunked`だけを構文検証し、`Content-Length`との併用を拒否する。
- request headerは64 KiB、chunk lineは8 KiB、trailerは64 KiB、
  一streamのlocal／upstream未送信dataは方向ごとに256 KiB、全stream合計は
  方向ごとに2 MiB、UI／worker queueは各4,096件／4 MiBへ制限する。
- Bridge未readyでは固定`503`を返し、長時間hangまたは直接接続へfallbackしない。
- 最初のHTTP headerは接続受付から30秒の絶対期限、OPEN後は30分の無通信期限を
  適用し、部分dataでSlowloris期限を更新しない。
- RDP／Bridge停止時にlocal Socket、stream、parser、queueを破棄する。
- URL、path、query、header、body、endpointをUIまたは診断logへ渡さない。

## 4. Agentのnetwork境界

Version `0.7.14.0`のAgentは、検証済みallowlistに登録された
`IPv4 address + TCP port`とClientの`OPEN`が完全一致した場合だけoutbound TCP
Socketを作成する。

- allowlist parserは設定全体を検証してからatomicに公開する。
- 空、重複、不正IPv4、不正port、未知key／section、上限超過を拒否する。
- hostname、DNS、CIDR、wildcard、port range、IPv6を表現できない。
- `Allowlist::Find`成功前に`IUpstreamConnector::BeginConnect`を呼ばない。
- IPv4 4 octetを直接`AF_INET`へ変換し、名前解決を行わない。
- 不一致は`OPEN_ERROR(RejectedByPolicy)`とし、別経路へfallbackしない。
- production Agentは`bind`、`listen`、`accept`を呼ばず、inbound portを持たない。
- Winsock Socketはnon-blockingで、connectを5秒で打ち切る。
- Phase 5は同時8 stream、一Socketの書込み待ち256 KiB、全Socket合計2 MiB、
  一read 16 KiB、一Pump最大32 operationに制限する。
- 接続済みupstream Socketはsend／receive進捗が30分ない場合に
  `CLOSE(Timeout)`として対象streamだけを終了する。
- RDP切断、Agent終了、protocol error、timeout、peer resetでSocketを閉じる。

設定欠落／不正またはWinsock初期化失敗時は、stream capabilityを広告せず
Echo-onlyで継続する。RDP画面を切断したり、security設定を変更したりしない。

Version `0.6.0.0`でproduction Client Proxyからこの境界までを接続し、
Mock WebDAVで複数stream／repositoryを確認した。Phase 9改善I-001では同じ
allowlist境界のままMock Trac Web／XML-RPCを追加した。実取引先SVNはPhase 7／8の
Gateであり、Phase 5完了を実環境接続の承認とは解釈しない。
2026-07-31にPhase 7／8のユーザー受入を得た後も、allowlist完全一致、
loopback限定listen、payload非記録の境界は変更していない。
Version `0.7.14.0`の複数session対応でも、各Agentは対応する一つのRDP sessionの
`ESVNBRG`だけを開き、Client側の別sessionへ接続しない。
実Tracはユーザーが明示承認したendpointと操作だけを対象とし、自動testや
network探索を行わない。

## 5. 診断

履歴の読込／保存結果は固定event名、`HRESULT`、record件数だけを記録する。
接続先、ドメイン、ユーザー名、復号data、ファイル内容は記録しない。

Bridgeは固定state、固定error分類、HELLO attempt、Echo成功時の合計byte数だけを
記録する。Agent無応答は固定`bridge.peer_timeout`だけを記録する。
Agent allowlistは採用件数、固定parser error、行番号、Winsock初期化code、
有効／無効だけを記録する。Virtual Channelごとのfragment、PING／PONG payload、
sequenceごとの内容、実IP、port、設定path／入力行は記録しない。

Client Proxyはlistener state、固定error分類、port、Bridge ready、active stream数、
方向別合計byte数だけを保持する。acceptしたlocal peer address、HTTP request、
endpoint、response内容は記録しない。

この方針はRDP password、SVN／Trac credential、HTTP payload、SVN diff、
Trac ticket／comment／attachmentを一切保存しない既存の診断方針を変更しない。
