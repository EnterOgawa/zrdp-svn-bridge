# ZRdp SVN Bridge 利用手順書

- 文書ID: `ZRDP-USER-001`
- 対象Version: `0.7.14.0`
- 対象: 社内試行利用者
- 更新日: 2026-08-01

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
- 実環境endpointだけを記載した`agent.ini`

管理者権限、service登録、driver、inbound port、Firewall変更は不要である。

## 3. 配布物の確認

受領するEXEは次の2つである。

| 対象PC | File |
|---|---|
| 開発PC | `ZRdpSvnBridge.exe` |
| 支給PC | `ZRdpSvnAgent.exe` |

社内配布版では両EXEのVersionが`0.7.14.0`で一致し、Authenticode署名が有効である
ことを確認する。Windows ExplorerでEXEのプロパティを開き、「デジタル署名」tabから
署名状態を確認できる。

次が同梱されていても実行しない。

- 出所不明なDLL
- PDB、dump、packet capture
- 他環境の`agent.ini`、credential、RDP file
- 署名が無効またはVersionが一致しないEXE

## 4. 配置

### 4.1 Client

`ZRdpSvnBridge.exe`を通常ユーザーが実行できるdirectoryへ配置する。診断logを
見つけやすくするため、社内policyで許可された書込み可能directoryを推奨する。

```text
%LOCALAPPDATA%\ZRdp\
└─ ZRdpSvnBridge.exe
```

既定port `18080`が競合する場合だけ、EXEの横へ`client.ini`を置く。

```ini
[proxy]
port=18080
```

Port以外は設定できない。通常は`client.ini`を作成しない。

### 4.2 Agent

`ZRdpSvnAgent.exe`と実環境用`agent.ini`を同じ書込み可能directoryへ配置する。

```text
%LOCALAPPDATA%\ZRdp\
├─ ZRdpSvnAgent.exe
└─ agent.ini
```

`agent.ini`には組織から明示的に許可されたHTTP endpointだけを記載する。

```ini
[allowlist]
endpoint.1001=192.0.2.10:80
endpoint.1002=198.51.100.20:8080
```

上記addressは説明用であり、実運用には使用しない。実設定をmail、ticket、source
repository、共通資料へ転載しない。

同じ`IPv4:port`にSVNとTracが存在する場合、pathごとにentryを重複追加しない。
設定変更後はAgentを再起動する。

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
3. RDP画面が表示され、keyboard、mouse、clipboardを確認する。
4. Session Windowのtitleで`RDP:接続済み`を確認する。

Credential UIをCancelまたは閉じた場合、ZRdpは追加の接続errorを表示せず接続を
取り消す。認証失敗や接続障害ではWindowsが返したerrorを確認する。

## 6. Agentを起動する

Agentは必ずZRdpで接続した支給PCのRDP画面内から起動する。

1. RDP接続が完了していることを確認する。
2. 支給PCで`ZRdpSvnAgent.exe`を起動する。
3. `allowlist限定SVN streamを有効にしました。`と表示されることを確認する。
4. `Clientと接続しました。`と表示されるまで待つ。
5. Clientのsession titleで`Agent:接続済み`を確認する。
6. `Proxy:利用可 127.0.0.1:<port>`を確認する。

設定欠落または不正時はAgentがEcho-onlyで起動し、Proxyは利用可能にならない。
`agent.ini`を修正してAgentを再起動する。

通常停止はAgent consoleでCtrl+Cを押す。RDP画面は維持され、ClientはAgent待機へ
戻る。同じRDP sessionでAgentを再起動すると自動的にBridgeを再確立する。

RDPを切断または再接続した場合は、新しいRDP session内でAgentを起動し直す。

## 7. Session titleの見方

Session titleには次の情報を表示する。

| 表示 | 意味 |
|---|---|
| `RDP:接続済み` | RDPログインが完了している |
| `Agent:待機中` | 対応Agentが未接続 |
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

- 各RDP session内でAgentを一つずつ起動する。
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

「全画面」buttonまたは`Ctrl+Alt+Break`を使用する。Windowへ戻る場合も同じbutton、
`Ctrl+Alt+Break`、またはMicrosoft connection barを使用する。

動的解像度では全画面monitorへremote desktopが追従する。等倍表示とSmart Sizingでは
remote desktop寸法を変更しない。

## 12. 終了

- Session Windowの閉じるbuttonは対象RDP sessionだけを切断する。
- 接続中は切断完了を待ってからWindowが閉じる。連打やタスク終了は行わない。
- Agentを先に止めてもRDPは継続する。
- RDP接続先の再起動や最終外部切断では、対象Windowがerror dialogなしで閉じる。
- 接続管理Windowと別sessionは継続する。
- すべてのsessionを終了後、接続管理Windowを閉じてClientを終了する。

社内配布版Version `0.7.14.0`では、すべてのWindowを閉じた後にタスクマネージャーで
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
| `Agent:待機中`のまま | Agentを同じactive RDP sessionで起動したか確認 |
| Proxyが利用可にならない | Agent console、allowlist、Client／Agent Versionを確認 |
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

1. SVN／Trac処理が完了していることを確認する。
2. AgentをCtrl+Cで終了する。
3. すべてのRDP Session WindowとClientを終了する。
4. Client／Agent両方を同じ新Versionへ置き換える。
5. Authenticode署名とVersionを確認する。
6. `agent.ini`を維持し、Agentを起動してProxy利用可を確認する。

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
