# ZRdp SVN Bridge

<p align="center">
  <img src="assets/icons/zrdp-icon.png" width="96" height="96" alt="ZRdp SVN Bridge icon">
</p>

ZRdp SVN Bridgeは、Windows標準のRemote Desktop ActiveX Controlを組み込んだRDP
Clientと、RDPユーザーsession内で動作するAgentを組み合わせ、HTTP方式SVNおよび
明示承認されたHTTP方式Tracへの通信をRDP Static Virtual Channel経由で中継する
Windows x64向けアプリケーションである。

現在の社内配布版はVersion `0.7.20.0`である。installerは使用せず、ClientとAgentを
portable EXEとして配布する。本リポジトリと配布物はPrivate／社内限定で扱う。

> [!IMPORTANT]
> ZRdpはZscalerとの提携、認定、公式製品であることを意味しない。Zscaler、Windows
> Firewall、GPO、RDP security、OS Proxyを停止または変更せず、既存のRDP経路だけを
> 使用する。

## アーキテクチャ

```text
開発PC
  ZRdpSvnBridge.exe
    ├─ Microsoft Remote Desktop ActiveX Control
    ├─ 127.0.0.1:<session専用port> HTTP Forward Proxy
    └─ Static Virtual Channel ESVNBRG
                    │
                    │ 既存のRDP接続
                    ▼
支給PCのRDPユーザーsession
  ZRdpSvnAgent.exe + agent.ini
    └─ allowlist完全一致のHTTP SVN／承認済みTrac endpointだけへ接続
```

| Component | 実行場所 | 主な責務 |
|---|---|---|
| `ZRdpSvnBridge.exe` | 開発PC | RDP UI、ActiveX host、loopback HTTP Proxy、stream多重化 |
| `ZRdpSvnAgent.exe` | 接続中のRDPユーザーsession | Virtual Channel、allowlist検証、許可済みendpointへのTCP接続 |

AgentはWindowsサービスではない。各RDPユーザーsession内で通常ユーザーとして起動し、
Session 0、inbound port、driver、VPN、仮想NICを必要としない。

## 主な機能

- Windows標準互換のRDP認証UI、keyboard、mouse、clipboard、全画面操作
- 等倍表示、Smart Sizing、動的解像度の3表示mode
- 接続名、接続先、ドメイン、ユーザー名、音声設定のDPAPI保護履歴
- 一つの接続管理Windowから最大16 RDP sessionを独立Windowで管理
- RDP sessionごとのloopback HTTP Proxyと最大8同時HTTP stream
- HTTP/1.1、WebDAV／DeltaV、JavaHL SVN、Mylyn Tracの透過中継
- Agent側の`IPv4 address + TCP port`完全一致allowlist
- payloadとcredentialを記録しないbounded診断log
- 一時的なRDP transport断後に同じuser session内でAgent channelを自動回復

## Security boundary

- Client ProxyはIPv4 loopback `127.0.0.1`だけでlistenする。
- Agentは`agent.ini`の完全一致allowlistを通過したendpointだけへ接続する。
- HTTP `CONNECT`、HTTPS tunnel、SOCKS、VPN、任意TCP／UDP forwardは実装しない。
- hostname、DNS探索、CIDR、wildcard、port rangeはAgent設定で許可しない。
- RDP／SVN／Trac credential、HTTP payload、source、diff、Cookieをlogへ記録しない。
- RDPまたはBridgeの異常時に、別port、直接接続、OS Proxy、`mstsc.exe`へ自動fallbackしない。

## 動作・開発環境

- Windows x64
- Visual Studio 2022 C++ toolchain
- Windows SDK
- CMake 3.31以上
- C++20

同梱の`vs2022-x64` presetは、この開発環境のVisual Studio配置
`C:\develop\tools\Microsoft`を使用する。別の配置ではGit管理外の
`CMakeUserPresets.json`等でgenerator instanceを上書きする。

## ビルドとテスト

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug
ctest --preset debug
cmake --build --preset release
ctest --preset release
```

portable EXEは`out/`へ出力される。`out/`、`build/`、PDB、log、dump、実環境設定は
Git管理対象外である。

## フォルダの用途

| フォルダ | 内容 |
|---|---|
| `src/`、`tests/` | アプリケーションのソースと回帰テスト |
| `cmake/`、`assets/` | ビルド補助設定と製品アイコン |
| `config/examples/` | 配布可能な設定テンプレート |
| `docs/` | 利用手順、設計、ADR、リリース記録 |
| `build/vs2022-x64/` | 標準presetのビルド・テスト生成物（Git管理外） |
| `out/` | 実行・配布に使用するEXEと診断ログ（Git管理外） |
| `work/analysis/`、`work/proposals/` | ローカルの解析結果と修正提案（Git管理外） |
| `work/tmp/` | 作業中だけ使用する一時ファイル（Git管理外） |

利用者への配布にはReleaseのZIPを使用する。ワークスペース全体や`build/`、`work/`
を配布物へ含めない。一時検証の生成物は作業終了後に整理し、署名済みEXE、
実行時設定、診断に必要なログは別途保持する。
確定した設計判断と受入結果は`docs/`と`docs/adr/`へ集約する。終了済みの作業記録は
Git履歴から参照でき、今後のローカル作業記録はcommitしない。

## 利用の流れ

1. 開発PCで`ZRdpSvnBridge.exe`を起動し、接続情報と表示modeを選んでRDP接続する。
2. 接続先RDP画面内で、同じVersionの`ZRdpSvnAgent.exe`を起動する。
3. Session Window titleで`Agent:接続済み`と`Proxy:利用可 127.0.0.1:<port>`を確認する。
4. JavaHL SVNまたはMylyn Tracの対象設定だけを、表示されたloopback Proxyへ向ける。
5. 既存のrepository URLを維持したまま、許可されたSVN／Trac操作を行う。

実環境用`agent.ini`は、`config/examples/agent.ini.example`を基に承認済みendpointだけを
設定し、EXEと同じdirectoryへ置く。実設定、`client.ini`、credential、log、dumpは
source repositoryや共通配布物へ含めない。

## 文書

- [初版の設計書（Markdown）](docs/specification.md)
- [利用手順](docs/user-guide.md)
- [Architecture](docs/architecture.md)
- [Application Protocol](docs/protocol.md)
- [RDP ActiveX設計](docs/rdp-activex.md)
- [localhost HTTP Proxy](docs/http-proxy.md)
- [Agent設計](docs/agent.md)
- [Security方針](docs/security.md)
- [設定仕様](docs/configuration.md)
- [テスト計画](docs/test-plan.md)
- [既知の制約](docs/known-limitations.md)
- [Agentチャネル回復の設計判断](docs/adr/0011-agent-channel-recovery.md)
- [全画面・接続バー制御の設計判断](docs/adr/0012-container-full-screen-and-connection-bar.md)
- [Version 0.7.20リリースノート](docs/release-notes-0.7.20.md)

<details>
<summary>過去のリリース記録</summary>

- [Version 0.7.15リリースノート](docs/release-notes-0.7.15.md)
- [Version 0.7.14リリースノート](docs/release-notes-0.7.14.md)
- [Version 0.7.14社内配布・署名確認記録](docs/release-checklist.md)

旧版Word文書と終了済みの`work`記録は、整理前のcommit `cfdaa8c`から参照・復元できる。
現行版の操作は利用手順とVersion `0.7.20.0`のリリースノートを参照する。

</details>

## 検証状態

Version `0.7.20.0`はDebug／Release各30 CTestと署名後の実機確認を通過した。
Client／Agentは同一署名者によるSHA-256署名とRFC 3161 timestampを持つ。
全画面表示、Microsoft connection barによる最小化／Window復帰、Agent回復、
SVN／Trac中継を受入済みである。修正内容と配布物のhashは
[Version 0.7.20リリースノート](docs/release-notes-0.7.20.md)を参照する。

継続利用中に問題が判明した場合は、発生時刻、操作分類、Session Window titleのProxy
port／stream／転送量、およびClient／Agent双方の固定診断logだけを保存する。実IP、URL、
credential、payload、source、diff、screen captureは、明示的に必要とされない限り共有しない。

## Repository policy

- GitHub repository、Project、配布先はPrivate／社内限定とする。
- GitHub Pages、公開Release、公開Package、公開Forkを作成しない。
- 実環境allowlist、実IP、RDP接続情報、SVN／Trac接続情報をcommitしない。
- Binary Release、tag、asset uploadは、明示されたRelease Gateを通過した場合だけ行う。
