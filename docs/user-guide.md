# ZRdp SVN Bridge 利用手順書

- 文書ID: `ZRDP-USER-001`
- 対象: `0.7.20.0` を製品Versionとする現行開発ソース（新Release未作成）
- 対象利用者: 試行利用者・自前ビルド利用者
- 更新日: 2026-10-11

過去の同Versionの署名済み社内配布物とは操作が異なる。現行の接続別設定・
Agent自動管理・セット更新を説明する。過去の受入結果を最新ソースの受入結果として扱わない。

## 1. はじめに

ZRdpは、通常のリモートデスクトップ操作と、RDP session内のAgentを経由した
HTTP方式SVN／承認済みTrac通信を同時に提供する。すべての接続は常に
`RDP + SVN Bridge`として開始する。

ZRdpはZscaler、Windows security設定、OS Proxyを停止または変更しない。利用する
endpointと操作は、所属組織から許可された範囲に限定する。

## 2. 事前条件

### 2.1 開発PC

- Windows x64
- 通常のRDP接続が許可されていること
- WindowsのRemote Desktop ActiveX Controlが利用可能であること
- JavaHLを使用するSVN client、またはMylyn Trac Connector
- `127.0.0.1:18080`を別applicationが使用していないこと

### 2.2 支給PC

- ZRdpから接続したactive RDPユーザーsession
- 通常ユーザーとしてEXEを実行できること
- 許可済みHTTP SVN／Trac endpointへ既存経路で接続可能であること
- RDPの専用drive／clipboard転送が組織policyで許可されていること

管理者権限、service登録、driver、inbound port、Firewall変更は不要である。

## 3. 配布物の確認

受領するEXEは次の2つである。

| 対象PC | File |
|---|---|
| 開発PCで起動 | `ZRdpSvnBridge.exe` |
| 開発PCのClientと同じdirectoryから接続先へ自動配置 | `ZRdpSvnAgent.exe` |

両EXEのVersionを一致させ、信頼できる配布元を使用する。署名済み配布物では
Windows Explorerのプロパティ「デジタル署名」tabから有効性を確認する。
通常起動・Agent配置は署名を必須としない。未署名の自前buildも同じ経路で使用できるが、
Version・製品情報・安全なfileの検証は維持する。セット自動更新では両EXEの署名が必須である。

次が同梱されていても実行しない。

- 出所不明なDLL
- PDB、dump、packet capture
- 他環境の`agent.ini`、credential、RDP file
- 出所が不明、配布元の署名と一致しない、またはVersionが一致しないEXE

## 4. 配置

### 4.1 Client

二つのEXEを、開発PCの通常ユーザーが実行できる同じdirectoryへ配置する。診断logを
見つけやすくするため、社内policyで許可された書込み可能directoryを推奨する。

```text
%LOCALAPPDATA%\ZRdp\
├─ ZRdpSvnBridge.exe
└─ ZRdpSvnAgent.exe
```

既定port `18080`が競合する場合だけ、EXEの横へ`client.ini`を置く。

```ini
[proxy]
port=18080
```

通常は`client.ini`を作成しない。更新確認を無効化する場合は `[update]` の
`enabled=false` も設定できる。詳細は [セット更新](client-updates.md) を参照する。

### 4.2 Agent

Clientが認証後に次の固定directoryへAgentと設定を配置・更新して起動する。
通常は接続先での手動配置・手動起動を行わない。

```text
%LOCALAPPDATA%\ZRdp\
├─ ZRdpSvnAgent.exe
└─ agent.ini
```

Clientは接続マネージャーの「Agent設定」で登録した許可済み宛先からINIを生成する。
以下は生成内容の説明例であり、通常利用でこのINIを手動作成する必要はない。

```ini
[allowlist]
endpoint.1001=192.0.2.10:80
endpoint.1002=198.51.100.20:8080
```

上記addressは説明用であり、実運用には使用しない。実設定をmail、ticket、source
repository、共通資料へ転載しない。

同じ`IPv4:port`にSVNとTracが存在する場合、pathごとにentryを重複追加しない。
設定変更はClientから行う。接続中は自動で協調停止・再起動する。

### 4.3 接続別Agent設定

接続先、ドメイン、ユーザー名を指定して「Agent設定」を開く。
許可済みのHTTP endpointのIP、ポート、用途を入力して「登録」を押す。
削除は一覧の選択後に「削除」を押す。登録・削除は自動保存される。

設定の保存単位は接続先＋ドメイン＋ユーザー名であり、接続を切り替えると
その接続に登録済みの宛先だけを表示する。未設定の場合は空一覧である。
登録は当該endpointの業務利用承認（Tracを含む）として扱う。
登録が一件もない接続ではAgentを自動起動・再起動せず、`Agent:要設定` を表示する。
RDP画面は継続できるがHTTP中継は使用できない。

画像と入力・保存仕様は [README](../README.md#3-この接続のagent設定を登録する) と
[Agent設定](agent-settings.md) を参照する。

## 5. RDP接続

### 5.1 Clientを起動する

開発PCで`ZRdpSvnBridge.exe`を起動する。接続管理Windowが表示される。

複数接続する場合もClient EXEを複数起動せず、一つの接続管理Windowから
「新しい接続」を繰り返す。

### 5.2 接続項目を入力する

| 項目 | 入力内容 |
|---|---|
| 接続名 | 任意。利用者が識別しやすい名前 |
| 接続先 | 許可されたPC名またはIP address |
| ドメイン | 必要な場合だけ入力 |
| ユーザー名 | RDPで使用するユーザー名 |

ドメインとユーザー名は別々に入力する。UPNを使用する場合はユーザー名欄へそのまま
入力し、ドメイン欄は接続環境の指定に従う。passwordはZRdpへ入力せず、後から表示
されるWindows標準credential UIへ入力する。

認証完了した接続は直近10組まで履歴へ保存される。接続名を選ぶと接続先、ドメイン、
ユーザー名、音声設定が一組で復元される。passwordは保存されない。

### 5.3 表示modeを選ぶ

| Mode | 使用場面 |
|---|---|
| 等倍表示（スクロール） | 接続先desktopのlayoutを変えず、必要箇所をscrollする。既定 |
| Smart Sizing（縮小表示） | 接続先解像度を維持したまま、Window内へ縮小表示する |
| 動的解像度（Window追従） | Windowに合わせて接続先desktop解像度も変更する |

接続先applicationのlayout変更を避けたい場合は、等倍表示またはSmart Sizingを使用する。

### 5.4 音声を選ぶ

- マイクを接続先で使用する場合だけcheckを入れる。既定はOFFである。
- リモート音声は「このPCで再生」「接続先PCで再生」「再生しない」から選ぶ。

音声設定は接続開始時に確定する。接続後に変更した場合は新しいsessionを開始するか、
対象sessionを再接続する。GPOまたはWindows privacy設定で禁止されている場合、ZRdpは
そのpolicyを変更しない。

### 5.5 接続する

1. 「新しい接続」を押す。
2. Windows標準credential UIへpasswordを入力する。
3. 接続先certificateをWindowsが信頼できない場合は、Microsoftの警告内容と接続先を
   確認し、今回の接続を継続するか利用者が選ぶ。ZRdpは警告を自動承認しない。
4. RDP画面が表示され、keyboard、mouse、clipboardを確認する。
5. Session Windowのtitleで`RDP:接続済み`を確認する。

Credential UIをCancelまたは閉じた場合、ZRdpは追加の接続errorを表示せず接続を
取り消す。certificate警告で「いいえ」を選んだ場合はpassword入力や同じ警告を自動で
反復せず、そのSession Windowだけを閉じる。接続管理Windowと他の接続は維持する。
認証失敗や接続障害ではWindowsが返したerrorを確認する。

## 6. Agentの自動起動と復旧

登録済みAgent設定のある接続では、認証完了後、RDP画面が前面かつ入力が
落ち着いた状態を確認してClientが配置・更新・起動する。独自の起動dialogは表示しない。
固定のWin+R／貼り付け／実行を送信するため、接続先の実行画面が一時表示されることがある。
自動処理中は入力を避け、Windowsの警告が出た場合は利用者が確認する。
前面状態や転送policy等の検証が失敗しても別経路へfallbackせず、RDPを維持する。

最新設定のAgent応答を待ち、session titleの `Agent:接続済み` と
`Proxy:利用可 127.0.0.1:<port>` を確認してからHTTP通信を開始する。

Agentはconsoleではなく接続先のタスクトレイに常駐する。疎通断ではClientが
有限回・backoff付きで再起動を試みる。トレイで終了しても設定がある限り復旧対象となる。
意図して停止する場合はRDPを切断するか、その接続の登録をすべて削除する。
設定変更時は既存HTTP streamを終了し、新しい設定で協調再起動するため、
SVN／Trac操作の完了後に行う。再接続は新しいBridge sessionとして扱う。

## 7. Session titleの見方

Session titleには次の情報を表示する。

| 表示 | 意味 |
|---|---|
| `RDP:接続済み` | RDPログインが完了している |
| `Agent:待機中` | 対応Agentが未接続 |
| `Agent:要設定` | この接続の登録済み宛先がなく、自動起動しない |
| `Agent:接続済み` | ESVNBRG handshakeが成立している |
| `Proxy:利用可 127.0.0.1:<port>` | 新しいHTTP streamを開始できる |
| `stream=N` | 現在activeなHTTP stream数 |
| `C->A=N` | Local HTTP clientからAgent方向への累積byte |
| `A->C=N` | AgentからLocal HTTP client方向への累積byte |

Mylynの定期同期やSVN操作中に`C->A`／`A->C`が増えることは正常である。これはRDP
画面転送量ではない。操作していない状態で継続的に増える場合は、Mylynの自動同期や
SVN clientのretryを確認する。

## 8. JavaHL SVNのProxy設定

既存repository URLは変更しない。JavaHLが使用するSubversion `servers`設定で、
対象HTTP endpointだけをZRdpのloopback Proxyへ向ける。

通常のWindowsユーザーprofileでは次のfileを使用する。

```text
%APPDATA%\Subversion\servers
```

既存設定を上書きせず、管理者または既存運用の指定に従って対象groupを追加する。

```ini
[groups]
zrdp-approved = 192.0.2.10, 198.51.100.20

[zrdp-approved]
http-proxy-host = 127.0.0.1
http-proxy-port = 18080
```

上記endpointは説明用である。実際には許可済みSVN serverを指定し、portは対象RDP
session titleに表示された値を使用する。

設定後は次を順に確認する。

1. Repository Explorer
2. Log／履歴
3. Diff／Compare
4. Checkout
5. Update
6. Commit、Add、Delete、Move
7. Conflict
8. Lock／Unlock

Write操作は、所属組織で許可されたrepository／pathへだけ実施する。結果が不明な状態で
Commit等を自動再実行しない。

## 9. Mylyn TracのProxy設定

Mylynでは対象Trac repositoryの「プロキシー・サーバー構成」を使用する。

| 項目 | 値 |
|---|---|
| System／default Proxy | 使用しない |
| Proxy host | `127.0.0.1` |
| Proxy port | 対象session titleの割当port |
| Proxy認証 | 使用しない |
| Proxy user／password | 空欄 |
| Trac server URL | 既存の`http://<IPv4>[:port]/...`を維持 |
| Trac認証 | Mylynのrepository認証欄を使用 |

設定後に「設定の検証」を行い、ticket queryと同期を確認する。Comment、ticket更新、
attachmentは必要な許可済み対象だけで段階的に確認する。

Redirect先がhostname、HTTPS、別の未許可endpointの場合はZRdpのsecurity境界により
失敗する。ZRdpへTrac credentialを入力しない。

## 10. 複数RDP session

一つの接続管理Windowから「新しい接続」を繰り返す。Proxy portは次のように割り当て
られる。

```text
1件目: 127.0.0.1:18080
2件目: 127.0.0.1:18081
3件目: 127.0.0.1:18082
```

- 登録済み設定に基づき、Clientが各RDP session内でAgentを自動起動する。
- 一つのAgentを複数sessionから共有しない。
- SVN／Trac側には対応sessionのportを設定する。
- 終了したsessionのport slotは次の新規sessionで再利用される。
- Client processを複数起動するとbase portが競合するため避ける。
- 最大16 RDP session、一session最大8 HTTP streamである。

通常利用する一件を最初に開始すれば、そのsessionを常にbase port `18080`として扱える。

## 11. 表示操作

### 11.1 Window size

通常の辺／角dragではRDP表示領域のアスペクト比を維持する。最大化、Windowsスナップ、
全画面はWindows標準の矩形を使用する。

### 11.2 表示mode切替

ログイン完了後はSession Window上部のradio buttonから3 modeを切り替えられる。
切替に失敗した場合もRDP接続は維持し、直前または等倍表示へ戻る。

### 11.3 全画面

「全画面」buttonまたは`Ctrl+Alt+Break`を使用する。Windowへ戻る場合は
Microsoft connection barまたは`Ctrl+Alt+Break`を使用する。local desktopへ一時的に
戻る場合はconnection barの最小化buttonを使用する。taskbarから復元すると全画面を
維持する。全画面中にZRdp独自buttonや表示mode欄は表示しない。

動的解像度では全画面monitorへremote desktopが追従する。等倍表示とSmart Sizingでは
remote desktop寸法を変更しない。

## 12. 終了

- Session Windowの閉じるbuttonは対象RDP sessionだけを切断する。
- 接続中は切断完了を待ってからWindowが閉じる。連打やタスク終了は行わない。
- Agent停止でもRDPは継続するが、設定があれば自動復旧を試みる。
- RDP接続先の再起動や最終外部切断では、対象Windowがerror dialogなしで閉じる。
- 接続管理Windowと別sessionは継続する。
- すべてのsessionを終了後、接続管理Windowを閉じてClientを終了する。

社内配布版Version `0.7.20.0`では、すべてのWindowを閉じた後にタスクマネージャーで
`ZRdpSvnBridge.exe`が残っていないことを確認する。残留した場合は配布・利用を中止し、
発生時刻とClient logを報告する。

処理中のSVN／Trac通信は切断される。Commit等の結果はserver側で確認してから必要な
操作を再実行する。

## 13. 診断ログ

第一候補はEXE隣接の`logs` directoryである。

```text
Client: logs\ZRdpSvnBridge.log
Agent:  logs\ZRdpSvnAgent.log
```

`C:\Program Files`等でEXE隣接へ書き込めない場合は次へ出力する。

```text
%LOCALAPPDATA%\ZRdpSvnBridge\logs\
```

Current logは最大1 MiB、backupは`.1`～`.3`である。ClientとAgentは別file名を使用する。

Logには通常、接続先、ユーザー名、URL、credential、HTTP payload、source、diffを
記録しない。外部共有前に固定field以外が含まれていないことを目視確認する。

## 14. Troubleshooting

| 状況 | 確認内容 |
|---|---|
| AgentがEcho-onlyになる | `agent.ini`の場所、section、重複、IPv4／portを確認してAgent再起動 |
| `Agent:要設定` | マネージャーの接続情報と当該接続の登録を確認 |
| `Agent:待機中`のまま | 前面のRDP画面、Windows警告、転送・実行policyを確認 |
| Proxyが利用可にならない | Agentのtray状態、設定、Client／Agent Versionを確認 |
| Proxy `BindFailed` | 別Client processや別applicationがportを使用していないか確認 |
| SVNが`403` | 宛先`IPv4:port`がAgent allowlistと完全一致するか確認 |
| SVNが`503` | Agent／BridgeがReadyになるまで待つ |
| SVNが`502`／`504` | 支給PCから許可済みendpointへの接続状態とtimeoutを確認 |
| Mylynがtimeout | Repository固有Proxy、port、Agent状態、redirect先を確認 |
| 表示modeが戻る | Logの`rdp.dynamic_resolution_call_returned`またはSmart Sizing結果を確認 |
| Agent再起動後に通信しない | 最大約20秒のhealth検出後、titleが利用可へ戻るか確認 |
| LogがEXE横にない | `%LOCALAPPDATA%\ZRdpSvnBridge\logs`を確認 |
| RDP接続できない | Windows標準RDPの許可状態、credential、Windows側errorを確認 |

ZRdpはpolicy拒否時に別port、DNS、直接接続、OS Proxy、`mstsc.exe`へ自動fallbackしない。

## 15. 不具合報告

次を保存して管理者へ報告する。

- 発生日時
- 操作分類（RDP接続、Mylyn同期、SVN参照、Update、Commit等）
- 対象session titleのProxy port、`stream`、`C->A`、`A->C`
- Clientの`ZRdpSvnBridge.log`
- 対応Agentの`ZRdpSvnAgent.log`
- Error dialogに表示されたreason／HRESULTの数値
- Client process、接続管理Window、別sessionが応答しているか

Password、repository URL、source、diff、ticket本文、screen capture、packet captureは
要求されない限り送付しない。Crash dumpは機密dataを含む可能性があるため、明示依頼
なしに作成または外部送付しない。

## 16. 更新とrollback

更新確認は起動時と24時間ごとに自動実行する。ダウンロード・適用は同意後に行い、
同VersionのClient／Agentをセットで取得する。適用前にSVN／Trac操作を完了し、
すべてのRDP sessionとAgent設定画面を閉じ、設定保存の完了を待つ。
接続先Agentは次回RDP接続時に更新する。署名・manifest・部分失敗時のrollbackと
現在のPrivate取得制約は [セット更新](client-updates.md) を参照する。

未署名の自前buildは自動更新対象外である。手動更新ではClientを終了して、開発PCの
二つのEXEを同Versionで置き換える。次の接続で接続先へ反映する。
既存Agentが旧方式で動作中の場合や更新が安全に行えない場合は、強制終了・上書きしない。

接続履歴Version 3を旧Clientが読めない場合は、Client終了後に次のfileだけを退避または
削除すると空の履歴で開始できる。

```text
%LOCALAPPDATA%\ZRdpSvnBridge\connection-history.bin
```

`agent.ini`、実endpoint、credentialをsource repositoryや共通資料へコピーしない。

## 17. 関連文書

- [最終仕様書](specification.md)
- [既知の制約](known-limitations.md)
- [診断ログ詳細](diagnostics.md)
- [社内配布・署名確認書](release-checklist.md)
