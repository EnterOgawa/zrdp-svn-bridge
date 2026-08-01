# ZRdp SVN Bridge 設定

更新日: 2026-07-31

## 1. Clientが保持する設定

ZRdp `0.7.14.0`は、認証が完了したRDP接続について次の6項目だけを接続履歴として保持する。

- 任意の接続名
- 接続先PC名またはIPアドレス
- 任意のRDPドメイン
- ユーザー名
- マイク転送のON／OFF
- リモート音声の再生先

RDP password、資格情報、証明書判断、画面内容、clipboard内容は保持しない。接続ボタンを押しただけの失敗接続も履歴へ追加しない。

接続画面ではドメインとユーザー名を別欄へ入力する。ZRdpは接続前に
`IMsTscAx::Domain`と`IMsTscAx::UserName`へ分離して設定し、password入力は
Microsoft標準credential UIへ委譲する。接続名はActiveXやAgentへ渡さず、
Clientの接続管理と通常session titleだけで使用する。

ドメイン欄が空でユーザー名が`DOMAIN\user`形式の場合は、旧版からの入力と
履歴に対応するため二項目へ自動分離する。UPN形式の`user@example.test`は
ドメインを推測せず、そのままユーザー名として扱う。ドメイン欄を入力した場合は
ユーザー名も必須とし、ユーザー名欄へ`\`を含む二重指定を受け付けない。

## 2. 保存場所と保護

履歴はEXEの横やregistryではなく、現在のWindowsユーザーprofile内の次のファイルへ保存する。

```text
%LOCALAPPDATA%\ZRdpSvnBridge\connection-history.bin
```

ファイル全体をWindows Data Protection API（DPAPI）のcurrent-user scopeで暗号化する。このため、同じWindowsユーザーのZRdpから使用することを前提とし、別ユーザーprofileや別PCへファイルだけをコピーしても履歴として読み込めない。

暗号化file内部の平文形式はVersion 3であり、任意の接続名、接続先、
結合済みユーザー名、マイク転送、リモート音声再生先を保持する。ドメインが
ある場合は暗号化前のユーザー名fieldへ`DOMAIN\user`として格納し、読込み後に
分離する。password、音声data、device名はどのfieldにも含めない。

Version `0.7.2.0`以前が保存したVersion 1履歴は、接続名が空の履歴として
読み込む。Version `0.7.3.0`が保存したVersion 2履歴も接続名を維持して読み込む。
どちらも音声設定はマイクOFF、リモート音声をこのPCで再生する既定値で補完する。
次に認証が完了して履歴を保存すると、既存entryを保持したままVersion 3へ更新する。
旧Clientへrollbackした場合、Version 3履歴は読めないため、必要ならこの一file
だけを退避または削除する。

読込不能または破損した履歴は採用せず、ZRdpのRDP接続機能は空の履歴で継続する。

file I/OとDPAPI処理はUI threadでは行わず、有限queueを持つ専用workerで直列実行する。完了結果と復号済み候補の所有copyだけをWindow Message経由でUIへ反映する。

## 3. 件数と重複排除

- 保持上限は直近10組である。
- 接続名は任意で、64文字を上限とする。
- 同じ「接続先＋ドメイン＋ユーザー名」は、大小文字を区別せず1組として扱う。
- 空でない同じ接続名は、大小文字を区別せず1件として扱い、最新の接続組へ
  再割当てする。
- 同じ組で認証が完了すると、複製せず最新位置へ移動する。
- 同じ接続先に異なるドメインまたはユーザー名を使用した組は保持できる。
- 接続名コンボボックスでは、空でない同じ接続名を1候補だけ表示する。
- 接続先コンボボックスでは、同じ接続先を1候補だけ表示する。
- ドメインコンボボックスでは、空でない同じドメインを1候補だけ表示する。
- ユーザー名コンボボックスでは、同じユーザー名を1候補だけ表示する。
- 接続名候補を選ぶと、対応する接続先、ドメイン、ユーザー名、マイク転送、
  リモート音声再生先を一組で復元する。
- 接続先候補を選ぶと、その接続先で最も新しい組の接続名、ドメイン、
  ユーザー名、マイク転送、リモート音声再生先を補完する。

接続名、接続先、ドメイン、ユーザー名は候補から選択できるほか、新しい値を
直接入力できる。認証完了前の接続失敗は履歴へ追加しない。

## 4. 履歴の消去

履歴を消去する場合はZRdpを終了し、次の1ファイルだけを削除する。

```text
%LOCALAPPDATA%\ZRdpSvnBridge\connection-history.bin
```

次回起動時は空の履歴として開始する。EXE、診断ログ、WindowsのRDP設定を削除または変更する必要はない。

## 5. RDP表示モード

RDPは接続開始時にClientからremote desktopの幅と高さを要求する。ZRdpは、意味と副作用が異なる次の3 modeをラジオボタンで排他的に選択する。

| 表示mode | remote desktop解像度 | ZRdp内の表示 | 接続先application layout |
|---|---|---|---|
| `等倍表示（スクロール）` | 接続開始時のmonitor寸法を維持 | 等倍、必要時scroll | window変更では変化しない |
| `Smart Sizing（縮小表示）` | 接続開始時のmonitor寸法を維持 | Client側だけを縮小 | window変更では変化しない |
| `動的解像度（Window追従）` | RDP表示領域へ追従 | 等倍 | 変化し得る |

既定は`等倍表示（スクロール）`である。等倍表示とSmart Sizingの接続開始時は、Windows標準RDPの全画面接続と同様に、ZRdpが存在する現在のmonitor全体を初期remote desktop寸法として使用する。

`Smart Sizing（縮小表示）`はMicrosoft ActiveXの`SmartSizing`だけを有効化する。`DesktopWidth`、`DesktopHeight`、`UpdateSessionDisplaySettings`、`Reconnect`による接続先desktop領域の変更は行わない。

`動的解像度（Window追従）`では、接続開始時に現在のRDP表示領域を初期remote
desktop寸法として使用する。接続後は最後のwindow変更から250 ms後に
`UpdateSessionDisplaySettings`でremote desktop寸法を更新する。ログイン直後に
Microsoft Controlが一時的な`E_UNEXPECTED`を返した場合だけ、500 ms、1,000 ms、
1,500 ms後に有限再試行する。全画面ではセッションWindowが属するmonitor全体、
Window表示ではRDP表示領域へ追従する。全画面移行、Window復帰、自動再接続完了時に
再適用する。`Reconnect`は使用しない。

3 modeは接続中にも切替可能であり、Smart Sizingと動的解像度は同時に有効化
しない。一時失敗の有限再試行で回復しない場合もRDP接続は切断せず、等倍表示へ
戻す。

Full HD／WQHD／Fullの固定値選択欄は設けない。固定値と表示modeが混在すると、利用者が接続先application layoutへの影響を判断しにくくなるためである。

表示modeは保存せず、起動時は常に`等倍表示（スクロール）`へ戻す。接続先applicationのlayoutを意図せず変更しないことを優先する。

なお、ActiveXは接続前の支給PCにある既存sessionの現在解像度を取得できない。以前のRDP sessionが現在のmonitor全体とは異なる寸法で作成されていた場合、初回接続時の完全な自動維持は保証できない。その場合は実機結果を記録し、別の固定baseline設定を追加するかを判断する。

## 6. RDP音声リダイレクト

接続管理Windowの`ローカル リソース`行で、次の接続時設定を選択する。

| 設定 | 選択肢 | 既定 |
|---|---|---|
| マイク | 接続先で使用する／使用しない | 使用しない |
| リモート音声 | このPCで再生する／接続先PCで再生する／再生しない | このPCで再生する |

設定は新しいRDP sessionの接続開始前に適用する。確立済みsessionへ途中反映
しないため、変更後は新しいsessionを開始するか対象sessionを再接続する。
接続先GPOまたはローカルWindowsのprivacy設定がマイクや音声転送を禁止する場合、
ZRdpからpolicyを変更しない。

## 7. Phase 3 Agent allowlist

`ZRdpSvnAgent.exe` version `0.7.14.0`は、EXEと同じdirectoryの固定名
`agent.ini`を起動時に一度だけ読み込む。current directory、registry、OS Proxy、
別fileへfallbackしない。

設定例は`config/examples/agent.ini.example`にある。repository内の例はRFC 5737の
文書用addressだけを使用している。支給PCへ配置する際は、組織上明示的に許可された
HTTP SVN endpointへ置換し、次のようにEXEの横へ配置する。

```text
配布directory/
├─ ZRdpSvnAgent.exe
└─ agent.ini
```

```ini
[allowlist]
endpoint.1001=192.0.2.10:80
endpoint.1002=198.51.100.20:8080
```

`endpoint.`の後は、通常logで接続先の代わりに使用できる非0の匿名数値IDである。
実IPやrepository名をIDへ埋め込まない。現行logは起動時の件数と固定error分類だけを
記録し、ID、IP、port、file path、入力行内容を記録しない。

parser契約は次のとおりである。

- file sizeは最大64 KiB、一行は最大256 byte、entryは最大128件
- UTF-8 BOMは任意、内容はASCII subset
- sectionは`[allowlist]`一つだけ
- keyは`endpoint.<1..4294967295>`だけ
- valueはstrict dotted-decimal IPv4 literalと`1..65535`のTCP port
- hostname、CIDR、wildcard、port range、IPv6、先頭0による別表記を拒否
- unspecified、multicast、broadcast addressを拒否
- 匿名IDと`IPv4:port`の重複を両方拒否
- 空設定、未知section、未知key、余分な文字を拒否

Agentは設定全体の検証成功後にだけallowlistを公開する。欠落または一行でも不正な
場合は部分採用せず、SVN stream capabilityを無効化してEcho-onlyで継続する。
Winsock 2.2を初期化できない場合も同じ縮退動作となる。RDP画面接続自体は切断しない。

有効設定時も、Clientの`OPEN`に含まれるIPv4とportが完全一致した場合だけ
outbound TCP Socketを作成する。不一致では`OPEN_ERROR(RejectedByPolicy)`を返し、
DNS、別port、OS Proxy、別経路へfallbackしない。Phase 5は同時8 streamを
production上限として広告する。

実環境`agent.ini`は既存`.gitignore`対象である。templateを上書きして実設定として
使用したり、実設定を`config/examples`、`docs`、`work`へコピーしたりしない。

Version `0.7.14.0`のClient Proxyはこのallowlistと同じ`IPv4:port`をOPENに使用する。
設定にない宛先は固定HTTP `403`となり、DNS、別port、直接接続へfallbackしない。
allowlistはAgent起動時に一度だけ読み込む。設定変更時はAgentを再起動し、Clientの
health timeoutまたは正常CLOSE後の新HELLOで再handshakeする。既存streamを
新設定へ引き継がない。

## 8. Client Proxy

Version `0.7.14.0`のClientは、RDP sessionごとにIPv4 loopback
`127.0.0.1:<session専用port>`だけをlistenする。bind addressは設定できず、
loopback以外へ拡張できない。最初のsessionの既定portは`18080`である。

既定base portと競合する場合だけ、`config/examples/client.ini.example`を
`ZRdpSvnBridge.exe`と同じdirectoryへ`client.ini`としてcopyし、base portを
変更する。

```text
配布directory/
├─ ZRdpSvnBridge.exe
└─ client.ini
```

```ini
[proxy]
port=18080
```

設定契約は次のとおりである。

- file欠落時はbase portとして既定`18080`を使用する
- file sizeは最大64 KiB、一行は最大1 KiB
- UTF-8 BOMは任意、内容はASCII subset
- sectionは`[proxy]`一つ、keyは`port`一つだけ
- valueはdecimal `1..65535`
- bind address、hostname、endpoint、credential、OS Proxyを表現できない
- unknown section／key、重複、空設定、不正portでは別値へfallbackせず
  ProxyだけをFailedにする
- `client.ini`はClient起動時、接続管理Windowを利用可能にする前に一度だけ、
  最大64 KiBの固定path設定として読込みと検証を完了する
- 各sessionのProxy workerは検証済みportだけを受け取り、`client.ini`を重複して
  読み直さない

実`client.ini`は`.gitignore`対象であり、repositoryへ登録しない。

base portからの割当規則は次のとおりである。

- 最初に開始したsessionは`base port`を使用する
- 追加sessionは未使用の最小slotを取り、`base port + slot`を使用する
- 同時session上限は16件であり、65535を超えるportは割り当てない
- session終了時にそのslotを解放し、次の新規sessionから再利用する
- 割当portは接続管理Windowの次port表示とsession Window titleで確認する
- 別processがportを使用中の場合は対象sessionのProxyを`BindFailed`とし、
  別portへ自動fallbackしない

通常利用する一件を最初に開始すれば、従来どおり`127.0.0.1:18080`を使用できる。
複数接続では`ZRdpSvnBridge.exe`を複数起動せず、一つの接続管理Windowから
「新しい接続」を繰り返す。別Client process間ではport slotを共有しない。

各Client ProxyはRDP／Agentのcompatible HELLO成立前でもlistenerを維持するが、
local接続には固定`503 Service Unavailable`を返して閉じる。Agentが有効な
`agent.ini`を読み込み、Clientとstream capabilityを交渉した後だけOPENを開始する。

一sessionは最大8 local connection／streamを扱う。各connectionは最初の
`IPv4:port`と一upstream Socketへ固定し、Keep-Alive中の宛先変更を拒否する。
SVN client側には対象RDP session Windowに表示されたHTTP Forward Proxy
`127.0.0.1:<session専用port>`を指定するが、
既存repository URL自体は変更しない。

初回HTTP headerの30秒期限と、OPEN後の30分無通信期限はsecurity既定値であり、
`client.ini`やcommand lineから変更できない。timeout時に途中requestを自動再送
しないため、SVN client側の結果を確認してから必要な操作を再実行する。

## 9. Mylyn Tracのrepository固有Proxy

Mylyn Trac Connectorでは、Eclipse全体のnetwork設定ではなく、対象Trac repository
の「プロキシー・サーバー構成」を使用する。これにより、他のEclipse通信をZRdpへ
送らず、対象repositoryだけをloopback Proxyへ向けられる。

設定値は次のとおりである。

| 項目 | 値 |
|---|---|
| system／default Proxyを使用 | 無効 |
| Proxy host | `127.0.0.1` |
| Proxy port | 対象RDP session Windowに表示された割当port |
| Proxy認証 | 無効 |
| Proxyユーザー名／パスワード | 空 |
| Trac server URL | 既存の`http://<IPv4>[:port]/...`を変更しない |
| TracユーザーID／パスワード | Mylynのrepository認証欄で従来どおり管理する |

Agentのallowlistはpathではなく`IPv4:port`の完全一致で判定する。同じendpointが
SVN用として既に登録済みなら、Trac pathのためだけにentryを重複追加しない。
Tracが別portまたは別IPv4を使用する場合は、そのendpointが業務上許可済みである
ことを確認したうえで、実環境`agent.ini`へ完全一致entryを追加しAgentを再起動する。
CIDR、wildcard、hostname、port rangeは使用できない。

実機確認は次の順に行う。

1. ZRdpでRDP接続し、Agentを起動してBridge ReadyとProxy有効を確認する。
2. Trac repositoryの「プロキシー・サーバー構成」を展開し、上表を設定する。
3. 「設定の検証」を実行する。
4. 成功後にticket query、ticket参照を確認する。
5. 更新が必要な場合だけ、comment／ticket更新、attachmentを段階的に確認する。

複数RDP sessionを同時利用する場合、Mylyn repositoryごとに対応するsessionの
割当portを指定する。JavaHLのSubversion `servers`設定等、一つのProxy portを
共有する設定では、通常利用するsessionを最初に開始してbase portへ固定する。
ZRdpはEclipse、JavaHL、Mylynの設定を自動変更せず、repositoryとRDP sessionの
対応も自動推測しない。

redirect先が別endpoint、hostname、またはHTTPSの場合はsecurity boundaryにより
失敗する。失敗時もZRdpへcredentialやticket内容を入力せず、Mylynのerror表示と
payloadを含まないZRdp診断logだけを確認する。
