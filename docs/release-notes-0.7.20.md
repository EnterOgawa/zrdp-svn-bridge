# ZRdp SVN Bridge 0.7.20.0 リリースノート

公開日: 2026-08-25

## 概要

Version `0.7.20.0`は、Windows標準RDP互換の全画面表示とconnection bar操作を
安定させる保守Releaseである。Version `0.7.15.0`以降に実装・検証した、Session Window
全体の最小化、borderless全画面、Microsoft connection barの前面表示、Window配置の
復元を収録し、検証用に追加したZRdp独自の全画面buttonは最終版から削除した。

## 修正内容

- Microsoft connection barの最小化要求をSession Window全体の標準最小化へ接続し、
  RDP表示だけが退避して白いZRdp Windowが残る問題を修正した。
- `ContainerHandledFullScreen`使用時にZRdp Session Window自身を現在monitor全体の
  borderless Windowへ切り替え、title bar、frame、表示mode欄を全画面中だけ隠す。
- Window復帰時は、全画面前の通常配置または最大化配置をsession単位で復元する。
- `DisplayConnectionBar`を明示的に有効化し、connection barの最小化／Window復帰buttonを
  接続前に設定してreadbackする。
- 全画面開始時はcontainerを先にmonitorへ配置し、top-level Z-orderを変更せず、その後に
  ActiveX全画面を有効化する。Microsoft connection barをcontainerが覆わない順序とした。
- connection barの前面表示を実機確認できたため、全画面右上へ重ねていたZRdp独自の
  `最小化`／`ウィンドウ`buttonを削除した。
- 全画面操作はMicrosoft connection barとWindows標準互換の`Ctrl+Alt+Break`へ統一した。
- 最小化／復元、全画面開始／終了を固定診断eventとして記録し、接続先、credential、
  RDP画面内容はlogへ追加しない。

## 互換性とsecurity境界

- Static Virtual Channel名`ESVNBRG`、Application Protocol、`agent.ini`形式、allowlist、
  localhost Proxy、HTTP SVN／承認済みTracの中継方式は変更していない。
- RDP、NLA、CredSSP、server certificate検証、credential UIはMicrosoft Controlへ
  引き続き委譲し、警告の自動承認やsecurity設定の緩和は行わない。
- 全画面切替、最小化、Window復帰を理由としてRDP、Virtual Channel、Proxy、既存HTTP
  streamを切断または再接続しない。
- Agent runtimeの中継動作はVersion `0.7.15.0`から変更していない。配布物の組を明確に
  するため、Client／AgentのVersionをともに`0.7.20.0`へ揃えた。

## 検証結果

- Debug CTest: 30／30成功
- Release CTest: 30／30成功
- 署名後resource／portable binary contract: 2／2成功
- Client／Agent File Version／Product Version: `0.7.20.0`
- Client／Agent Authenticode: Windows trust `Valid`
- SHA-256署名、RFC 3161 timestamp、両EXEの署名者一致: 確認済み
- ユーザー実機でMicrosoft connection barの表示、最小化、Window復帰、taskbar復元、
  `Ctrl+Alt+Break`、三表示mode、動的解像度を確認済み
- ユーザー実機でRDP、Agent、Proxy、HTTP stream、別sessionへの回帰がないことを確認済み

Client SHA-256:

`B5321D67BC7FB3127611A98BCF223856F59DB22B226D683DC446CE320BB19E2F`

Agent SHA-256:

`D52EA278A9C9508F272CD5B4519B00BC154D8C781B61B30CB3D17E0F8A98EA31`

## 配布物

- `ZRdpSvnBridge.exe`: 開発PCで使用する署名済みClient
- `ZRdpSvnAgent.exe`: 支給PCのRDPユーザーsession内で使用する署名済みAgent
- `agent.ini.example`: Agent allowlistの説明用template
- `client.ini.example`: Client Proxy base portの説明用template
- `SHA256SUMS.txt`: 配布物のSHA-256一覧
- 日本語README、利用手順、Agent手順、既知の制約

設定templateにはRFC 5737の文書用addressだけを収録する。実環境用`agent.ini`、
`client.ini`、password、log、payload、source、diff、ticket本文はReleaseへ含めない。

## 更新方法

1. 利用中のClient／Agentを終了する。
2. 実環境用`agent.ini`と`client.ini`を上書きせず保持する。
3. `ZRdpSvnBridge.exe`と`ZRdpSvnAgent.exe`をVersion `0.7.20.0`へ入れ替える。
4. 両EXEのVersionとAuthenticode署名を確認する。
5. RDPへ接続し、Microsoft connection barから全画面、最小化、Window復帰を確認する。
6. Agent接続、Proxy利用可、SVN／Tracの安全な操作を確認する。

## 既知の制約

- 全画面表示はWindowsにインストール済みのRemote Desktop ActiveX Controlを使用するため、
  Windows／Controlの更新や接続先policyによる動作差があり得る。
- 全画面中はZRdp独自buttonを表示しない。Window復帰はMicrosoft connection barまたは
  `Ctrl+Alt+Break`を使用する。
- `agent.ini.example`の文書用addressは実環境で使用できない。明示承認済みHTTP SVN／
  Trac endpointだけを`IPv4 address + TCP port`完全一致で設定する。
- 実環境固有の制約と継続観察項目は`known-limitations.md`を参照する。
