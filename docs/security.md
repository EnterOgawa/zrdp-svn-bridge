# ZRdp SVN Bridge セキュリティ方針

更新日: 2026-10-10

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

## 6. Client／Agentセット更新の境界

Client側のセット更新だけに、固定GitHub Release配布hostへのHTTPS取得を追加する。
Agentの取得は開発PC側だけで行う。HTTP SVN／Trac中継の対象、allowlist、Virtual Channelは変更しない。
更新通知前は有限manifestだけを取得し、同じRelease tagの二EXEは利用者の同意後に取得する。
署名・timestamp・現行Clientとの公開鍵一致・SHA-256・製品metadata・protocolを
二EXEとも完全検証し、同一製品版を必須とする。全RDP sessionと設定画面終了・保存完了後にだけ固定二EXEを置換する。
片方欠落のmanifestや取得失敗では更新しない。通常の部分失敗では元hash照合済みの旧セットへ復元する。
電源断等の二file同時atomic性は保証せず、復元不能ではstageと旧二backupを保持する。

署名・通信・file権限の検証失敗では更新だけを停止する。認証token、任意URL、
管理者昇格、security policy変更、RDP強制切断、別process強制終了を実装しない。
同じWindowsユーザーまたは管理者による既存EXE・書込み可能配置の改変への
完全防御を保証するものではない。復旧条件と残存stageの扱いは
[Clientの自動更新](client-updates.md)を参照する。

## 7. 接続別設定と自動Agent管理

接続マネージャーがRDP接続先・domain・userの組と承認済みallowlistを
current-user DPAPIで保存する。別接続へ設定を推定継承しない。
未設定・未承認・破損ではProxyを使えず、旧allowlistで通信を継続しない。

簡素化した設定画面では、IP・HTTPポート・用途を指定した `登録` を当該宛先の
業務利用承認と自動保存の明示操作にする。HTTP Tracも登録した完全一致宛先だけを許可する。
その意味を画面に表示し、読込みや別接続の未関連付け設定から承認を推定しない。
登録・削除は保存成功後だけ一覧に公開し、失敗時は旧設定を維持する。

RDP認証後に専用driveの固定二fileと固定setupだけを使って自動更新・起動する。
設定変更は相互合意した限定SVC controlへINIだけを渡し、任意path・command・PIDは渡さない。
Agentは旧Socketを閉じ、INIをbackup付きatomic renameで更新して固定自身を再起動する。
最新INIのACK前にClient ProxyとAgent relayを有効化しない。削除・承認撤回では協調停止する。

固定入力は対象前面で一度に一sessionだけとし、認証・世代・focus・clipboardを各段階で検証する。
途中失効・部分受付・protocol異常では再送せず、復旧失敗が続く場合も有限停止する。
強制終了、任意shell、追加port、startup／service登録、policy変更は行わない。
Windows警告は自動承認しない。設定と入力の詳細は [接続別Agent設定](agent-settings.md) を参照する。

## Agent配置と起動の任意署名

2026-10-10の追加指示により、Client／Agentの署名は起動とAgent配置の許可条件にしない。
署名付きと未署名を同じ経路で扱い、未署名だけの選択と警告、別CLIを廃止する。
署名なしでも正式metadata、同一製品版、setup対応、有限サイズ、承認済みINIを検証する。
metadataは発行者や改ざん防止の証明ではないため、信頼できるsource・配布元を使用する。

接続先では専用drive直下の固定Agentに`--rdp-setup`だけを渡す。
active RDP session、固定LocalAppData配置先、二file、allowlist、reparse・hardlink拒否、
バックアップ付き更新とrollback、固定EXEだけの起動を維持する。
Agent応答まで成功とは表示しない。Windowsや組織の実行・転送制限、警告確認は変更しない。
Client自動更新の署名・公開鍵・hash検証は別機能として維持する。
[Agentの任意署名](adr/0021-optional-agent-signing.md)が過去ADRの署名必須を置き換える。

## Agent固定入力のSendInput境界

初回自動配置は専用RDP driveと固定Agentの`--rdp-setup`だけを使い、Client／Agentのmetadataと
全宛先承認済みINIを検証する。固定ユーザー配置先、バックアップ付き更新、45秒の応答期限を維持する。
従来のclipboard配置とは別で、専用sourceの平文INIをcurrent-user限定の一時folderへ公開する。

2026-10-10の明示承認により固定Win＋R／Ctrl＋V／EnterはSendInputを使用する。
所有STA、認証・警告、実Control focus・前景、関連キー解放を確認し、COM照会後にも
元の認証世代・phase・drive・Bridge・clipboard・取消条件を同期確認する。
初回だけfocusを準備し、後続は奪い返さない。早着・再入・残留timer、失効、途中切替では入力しない。

SendInputは接続先を直接指定しないため、直前確認後の切替競合でローカル側へ貼付け・Enterが届き、
誤実行するリスクを完全には排除できない。操作手順に明記し、入力中の操作・Window切替を控えてもらう。
部分受付では追加解放・再送・別経路fallbackをせず、手動解放を案内する。入力済みの起動は取り消せない。
任意キー・文字列・commandのAPI、入力capture、権限昇格、policy変更は追加しない。
詳細は[Agent固定入力のSendInput方式](adr/0017-sendinput-agent-controls.md)を参照する。
