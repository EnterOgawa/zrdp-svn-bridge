# AGENTS.md

## 1. プロジェクト概要
- 本プロジェクトは、Windows 標準の Remote Desktop ActiveX Control を組み込んだ専用 RDP クライアントを開発し、現在すでに成立している RDP セッション内の Virtual Channel を利用して、開発 PC から取引先の HTTP 方式 SVN リポジトリ、およびユーザーが明示承認した HTTP 方式 Trac へアクセスできるようにするものである。
- 本プロジェクトおよびアプリケーションの正式名称は **`ZRdp SVN Bridge`** とする。
- UI の短縮表示、会話上の略称、内部識別子として **`ZRdp`** を使用してよいが、正式文書、配布物、version resource では `ZRdp SVN Bridge` を使用する。
- Client の実行ファイル名は **`ZRdpSvnBridge.exe`**、Agent の実行ファイル名は **`ZRdpSvnAgent.exe`** とする。
- `ZRdp` の名称は Zscaler との提携、認定、公式製品であることを意味しない。Zscaler の logo、公式製品名に見える表記、提携や認定を示す表現を使用しない。
- 製品名を変更しても、protocol 識別子である Static Virtual Channel 名 **`ESVNBRG`** は変更しない。
- 実行時は次の 2 アプリ構成とする。
  1. `ZRdpSvnBridge.exe`
     - 弊社開発 PC 上で動作する。
     - Windows 標準 RDP ActiveX Control をホストする。
     - RDP 接続画面、localhost HTTP Proxy、Virtual Channel、複数 HTTP 接続の多重化を一つのアプリに統合する。
  2. `ZRdpSvnAgent.exe`
     - 取引先支給 PC の RDP ユーザーセッション内で動作する。
     - Virtual Channel と、支給 PC から既に接続可能な取引先 HTTP SVN／承認済み Trac endpoint との間を中継する。
- Eclipse RCP、Java、既存の独自リモートデスクトップアプリは本プロジェクトの実装対象に含めない。
- Eclipse の Repository Explorer、SVN Connector、Mylyn Trac Connector は、本製品の localhost HTTP Proxy を利用する外部クライアントとしてのみ扱う。
- 現在の SVN URL は、複数の取引先サーバーを IP アドレスで直接指定した `http://<IP>[:port]/...` 形式である。既存 URL、既存 Repository Location、既存作業コピーの URL を変更しない運用を目標とする。

## 2. 目的
- Zscaler を常時 ON のまま使用する。
- 開発 PC と支給 PC の間に、RDP 以外の新しい受信ポートを開放しない。
- 開発 PCから支給 PC への RDP 接続は、現在利用可能な経路をそのまま使用する。
- 開発 PC 上の SVN クライアントから、Update、Synchronize、Compare、Checkout、Commit、Add、Delete、Move、Lock、Unlock 等を従来に近い操作で利用できるようにする。
- 複数の HTTP SVN サーバー、複数ポート、複数リポジトリを、一つの localhost HTTP Proxy と一つの RDP セッションで扱えるようにする。
- Mylyn Trac Connector から、承認済み HTTP Trac の設定検証、ticket query、ticket参照・更新、attachment転送を段階的に利用できるようにする。
- 支給 PC は IP ルーターや VPN Gateway とせず、RDP セッション内のアプリケーションチャネルと、既存の業務上許可された SVN／Trac 接続だけを使用する。

## 3. 非目標および禁止事項
- RDP プロトコル、NLA、CredSSP、画面転送、入力転送、暗号化、再接続機能を独自に再実装しない。
- `mstsc.exe` をコピー、改変、プロセス注入、DLL Injection、API Hook、偽装起動しない。
- Windows の RDP システム DLL を製品へコピーまたは同梱しない。実行環境にインストールされている Remote Desktop ActiveX Control を利用する。
- Zscaler Client Connector の停止、設定変更、改変、アンインストール、証明書操作、トラフィック捕捉、通信制御回避を実装しない。
- Windows Firewall、EDR、GPO、ルーティングテーブル、IP Forwarding、ICS、NIC Bridge、Proxy 設定を自動変更しない。
- SoftEther、VPN、SOCKS、汎用 HTTP Proxy、汎用 TCP Forward、UDP Forward、TUN/TAP、WFP Driver、仮想 NIC を実装しない。
- HTTP `CONNECT`、HTTPS トンネル、`svn://`、`svn+ssh://`、SSH、RDP、SMB 等の中継を初期製品へ実装しない。
- 任意の IP アドレス、任意のホスト名、任意のポートへ接続できる機能を実装しない。
- 取引先ネットワークの探索、ポートスキャン、ホスト列挙、DNS 探索を実装・実行しない。
- RDP ActiveX 版 Client が現在の環境で接続できない場合、`mstsc.exe` への偽装やセキュリティ製品の回避を試みない。検証結果を記録し、設計判断をユーザーへ返す。

## 4. 基本アーキテクチャ

```text
開発 PC
┌──────────────────────────────────────────────┐
│ ZRdpSvnBridge.exe                            │
│                                              │
│  ├─ Windows Remote Desktop ActiveX Control  │
│  ├─ RDP 接続 UI                              │
│  ├─ 127.0.0.1:18080 HTTP Forward Proxy      │
│  ├─ HTTP Request Parser                      │
│  ├─ Stream Multiplexer                       │
│  ├─ Static Virtual Channel Adapter           │
│  └─ 状態・エラー・転送量表示                 │
└───────────────────┬──────────────────────────┘
                    │
                    │ 現在成立している RDP 接続
                    │ Static Virtual Channel: ESVNBRG
                    ▼
支給 PC
┌──────────────────────────────────────────────┐
│ ZRdpSvnAgent.exe                             │
│                                              │
│  ├─ Static Virtual Channel Adapter           │
│  ├─ Stream Multiplexer                       │
│  ├─ 接続先 allowlist                         │
│  ├─ Winsock TCP Client                       │
│  └─ RDP 切断時の全接続終了                   │
└───────────────────┬──────────────────────────┘
                    │
                    │ 支給 PC の既存 HTTP 通信
                    ▼
                 Zscaler
                    │
                    ▼
       取引先 HTTP SVN／承認済み Trac endpoint
```

## 5. 技術選択
- 主実装言語は **C++20** とする。
- 対象プラットフォームは **Windows x64** とする。
- UI は Win32 デスクトップアプリとし、Remote Desktop ActiveX Control をホストする。
- ビルドは Visual Studio 2022 と CMake を基本とする。
- Windows SDK と標準 C++ ライブラリを優先し、外部ランタイムを必須としない。
- Client、Agent、共通 Protocol を同一リポジトリで管理する。
- 初期実装では Static Virtual Channel を使用する。
- Virtual Channel 実装は `IVirtualChannelTransport` 相当の境界で抽象化し、計測の結果 Static Virtual Channel が明確な性能・安定性要件を満たさない場合に限り、Dynamic Virtual Channel へ差し替えられる構造とする。
- DVC への変更は、実測値、問題点、代替案、追加配布物、登録方式を `docs/adr` に記録し、ユーザーの明示承認後に行う。

## 6. Remote Desktop ActiveX 方針
- Windows にインストール済みの最新利用可能な Microsoft RDP Client Control を使用する。
- ActiveX のバージョン選択と生成処理は `RdpControlHost` 等の専用クラスへ閉じ込め、UI や SVN Bridge ロジックへ CLSID やバージョン依存コードを散在させない。
- Remote Desktop ActiveX Control の作成、イベント購読、接続、切断、破棄は COM STA の UI スレッドで行う。
- アプリ起動時に `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)` 相当を行い、終了時に対応する解放を行う。
- `IMsTscAxEvents` の Connection Point を正しく購読・解除し、切断後や破棄後にイベントコールバックが残らないようにする。
- 最低限、次のイベントを状態管理へ反映する。
  - `OnConnecting`
  - `OnConnected`
  - `OnLoginComplete`
  - `OnDisconnected`
  - `OnFatalError`
  - `OnWarning`
  - `OnAutoReconnecting2` または利用可能な再接続イベント
  - `OnChannelReceivedData`
- ActiveX の接続前に Static Virtual Channel を作成する。
- Server、Domain、UserName、初期 desktop baseline、全画面、表示 mode、音声リダイレクト等の RDP 設定は専用設定モデルを介して適用する。
- パスワードを INI、JSON、ログ、コマンドライン、Git 管理ファイルへ平文保存しない。
- 初期版では ActiveX または Windows の標準認証 UI を利用し、証明書警告や認証警告を自動承認しない。
- NLA、CredSSP、サーバー認証を弱める設定を既定にしない。
- `ClearTextPassword` 等を使用する必要が生じた場合は、メモリ保持時間、ゼロクリア、保存禁止を設計書へ明記し、ユーザー承認なしに導入しない。

## 7. Static Virtual Channel 方針
- Static Virtual Channel 名は **`ESVNBRG`** とする。
- `ESVNBRG` は 7 文字の英字であり、Remote Desktop ActiveX の scriptable virtual channel 名制約内とする。
- Client は RDP 接続前に `CreateVirtualChannels("ESVNBRG")` 相当を呼び出す。
- Client から Agent への送信には `SendOnVirtualChannel` 相当を使用する。
- Agent から Client へのデータは `OnChannelReceivedData` 相当で受信する。
- Agent は RDP ユーザーセッション内で、次と同等の条件で SVC を開く。

```cpp
WTSVirtualChannelOpenEx(
    WTS_CURRENT_SESSION,
    "ESVNBRG",
    0
);
```

- Agent は Windows サービスとして実行しない。RDP セッション 0 ではなく、実際に Client が接続しているユーザーセッション内で実行する。
- Agent 起動時に自身の Session ID を確認し、対話ユーザーの RDP セッションでない場合は安全に終了する。
- `WTSVirtualChannelRead` と `WTSVirtualChannelWrite` は同一ハンドルに対して無秩序に複数スレッドから呼び出さない。
- 初期版では Virtual Channel ごとに reader と serialized writer を明確に分離し、書き込みキューを一つに集約する。
- 高スループット化が必要な場合は、`WTSVirtualChannelQuery(WTSVirtualFileHandle)` と OVERLAPPED I/O の採用を別フェーズで評価する。

## 8. BSTR とバイナリデータの扱い
- `SendOnVirtualChannel` のデータは BSTR であるが、文字列として扱わない。
- Virtual Channel の payload は任意のバイナリを含むため、埋め込み NUL を許容する。
- BSTR の受信長は終端文字や `wcslen` に依存せず、`SysStringByteLen` 等の長さ情報で扱う。
- バイナリと UTF-16 の相互変換、Base64 変換、文字コード変換をデータプレーンに持ち込まない。
- COM イベントで受信した BSTR はコールバック中に所有バッファへコピーし、コールバック終了後に COM 所有メモリを参照し続けない。
- COM UI スレッド上で HTTP 解析、Socket I/O、大容量コピー、待機処理を行わない。
- worker thread から `SendOnVirtualChannel` を直接呼ばず、UI/COM スレッドへ bounded queue と Window Message 等で送信要求を marshal する。

## 9. Application Protocol
- RDP Virtual Channel はメッセージ境界、完全なフレーム境界、1 回の callback と 1 フレームの一致を保証するものとして扱わない。
- Agent 側では 1 回の `WTSVirtualChannelRead` で受信できる量に上限があるため、必ず application frame の分割・再構成を実装する。
- wire format はコンパイラ依存の構造体 cast や `#pragma pack` のみに依存せず、固定幅整数を明示的に encode/decode する。
- 整数は wire format 上の byte order を一つに固定し、仕様書へ明記する。
- すべての length、offset、count、sequence、streamId は overflow と上限を検証する。
- protocol version、magic、message type、streamId、payload length を持つフレーム形式とする。
- 初期プロトコルは少なくとも次を持つ。
  - `HELLO`
  - `HELLO_ACK`
  - `OPEN`
  - `OPEN_OK`
  - `OPEN_ERROR`
  - `DATA`
  - `WINDOW_UPDATE`
  - `HALF_CLOSE`
  - `CLOSE`
  - `PING`
  - `PONG`
  - `PROTOCOL_ERROR`
- `streamId = 0` は connection-level control 用の予約値とする。
- Client だけが新しい HTTP stream を開始できるものとし、Agent から開発 PC への逆方向接続開始機能を持たせない。
- Client と Agent は `HELLO` で protocol version、機能 bit、最大 frame payload、最大 stream 数、初期 window size を交換する。
- protocol version が互換でない場合は fail-closed で Bridge を無効化し、RDP 画面接続自体は可能な限り維持する。
- 1 stream の受信 window、全体送信 queue、全体受信 queue、最大同時 stream 数を有限値にする。
- memory pressure 時に無制限に buffer を増加させず、backpressure を返す。
- 初期実装では独自圧縮、暗号化、再送、途中再開、RDP 再接続後の stream resume を実装しない。
- RDP 再接続後は新しい Bridge session とし、旧 streamId、旧 Socket、旧 sequence を再利用しない。

## 10. localhost HTTP Proxy
- Client の最初の RDP session は、既定で `127.0.0.1:18080` のみを listen する。
- 同一 Client process で複数の RDP session を開始した場合、各 session は
  `127.0.0.1` 固定のまま、重複しない専用 Proxy port を所有する。
- `0.0.0.0`、LAN NIC、Wi-Fi NIC、VPN NIC、全 IPv6 アドレスで listen しない。
- IPv6 loopback `::1` 対応は、明示的に実装・テストした場合だけ有効にする。
- `client.ini` の proxy port は最初の session に割り当てる base port とし、
  既定値は `18080` とする。追加 session は未使用の最小 slot に
  `base port + slot` を割り当てる。
- session 終了時は対応する port slot だけを解放し、生存中 session の
  listener、Bridge、RDP 接続へ影響させない。
- 実際の割当 port は接続管理 Window、session Window title、Proxy 状態欄へ
  表示し、SVN／Trac client が使用すべき port を利用者が識別できるようにする。
- 別 process が割当 port を使用中の場合は対象 session の Proxy だけを
  fail-closed で停止し、別 port や別経路へ自動 fallback しない。
- Proxy Authentication、cache、content inspection、request body 保存、response body 保存を実装しない。
- 対象は `http://` 方式の SVN、およびユーザーが明示承認した `http://` 方式 Trac に限定する。
- HTTP `CONNECT` は拒否する。
- `Upgrade`、WebSocket、UDP、HTTP/3 は対象外とする。
- absolute-form request-target を origin-form へ正しく変換する。

```http
PROPFIND http://192.0.2.10/svn/ProjectA HTTP/1.1
Host: 192.0.2.10
```

を、実サーバーへ送信する際は次のようにする。

```http
PROPFIND /svn/ProjectA HTTP/1.1
Host: 192.0.2.10
```

- method、path、query、HTTP version、end-to-end header、body は必要以上に変更しない。
- `Proxy-Connection` 等の proxy 固有または hop-by-hop header は HTTP の規則に従って処理する。
- `Authorization`、`Proxy-Authorization`、Cookie、認証 challenge、SVN 差分内容、Trac ticket／comment／attachment内容をログへ出力しない。
- 一つの incoming TCP connection は一つの `IP:port` に固定する。Keep-Alive 中に宛先が変化する要求は拒否するか、新しい接続を要求する。
- Client は URL の authority と `Host` header の不一致、欠落、不正形式を検証する。
- 初期対象は IPv4 literal と明示ポートまたは既定 HTTP port 80 とする。
- host name 解決、PAC、OS 全体 Proxy、transparent interception は初期対象外とする。

## 11. HTTP / SVN / Trac 互換性要件
- HTTP/1.1 Keep-Alive を扱う。
- `Content-Length` を扱う。
- chunked transfer encoding を扱う。
- `Expect: 100-continue` を扱う。
- request body と response body をストリーミングし、全体を一括メモリ保持しない。
- Basic、Digest、NTLM 等、現行環境で使用される認証 challenge／response を中継できるよう、認証ヘッダーを透過的に扱う。
- connection-oriented authentication が必要な場合、同一 upstream Socket の継続性を壊さない。
- Subversion の WebDAV／DeltaV を含む次の method を拒否しない。
  - `OPTIONS`
  - `GET`
  - `HEAD`
  - `POST`
  - `PROPFIND`
  - `REPORT`
  - `MKACTIVITY`
  - `CHECKOUT`
  - `PUT`
  - `DELETE`
  - `COPY`
  - `MOVE`
  - `MERGE`
  - `LOCK`
  - `UNLOCK`
- Repository Explorer の参照だけで完了扱いにせず、Checkout、Update、Commit、Add、Delete、Move、競合、Lock/Unlock まで段階的に確認する。
- SVN のユーザー名・パスワードは SVN クライアントと実 SVN サーバーの間で扱い、本製品独自の認証情報保存機構を作らない。
- Mylyn Trac Connector の Web fallbackで使用するGET、およびXML-RPCで使用するHTTP/1.1 POSTを中継する。
- Tracのredirect、Cookie、XML body、Content-Encoding、attachment payloadは内容を解析・保存せず、HTTP規則の範囲で透過する。
- Mylyn Tracではrepository固有のProxy設定を優先し、OSまたはEclipse全体のProxy設定変更を必須にしない。
- Tracのユーザー名・パスワードはMylynと実Trac serverの間で扱い、本製品独自の認証情報保存機構を作らない。

## 12. Agent の接続先制限
- Agent は Client から受信した宛先へ無条件に接続しない。
- 実環境の HTTP SVN／承認済み Trac endpoint を `IPv4 address + TCP port` の完全一致 allowlist で管理する。
- allowlist にない接続先は必ず拒否する。
- 拒否時に別ポート、OS Proxy、DNS、直接接続等へ自動フォールバックしない。
- private address 全体を許可する設定、CIDR wildcard、port range、`*` は初期版で禁止する。
- 接続先設定をソースコードへ埋め込まない。
- 実環境設定は Git 管理対象外とする。
- リポジトリには文書例示用アドレスだけを使用した template を置く。
- 設定ファイル例では `192.0.2.0/24`、`198.51.100.0/24`、`203.0.113.0/24` を使用する。
- Agent 起動時に allowlist を検証し、重複、不正 IP、不正 port、空設定を明示する。
- allowlist の reload を実装する場合、途中状態を公開せず、検証済み設定へ atomic に切り替える。

## 13. RDP UI の範囲
- Windows 標準 RDP の見た目と操作感を参考にするが、`mstsc.exe` の完全複製を目標にしない。
- 製品としての Client は、すべての RDP 接続を **RDP + SVN Bridge** として開始する。RDP 専用モードや、利用者が SVN Bridge を無効化する切替機能は設けない。
- Client はすべての製品接続で、RDP 接続前に Static Virtual Channel `ESVNBRG` を作成する。
- RDP の利用手順は Windows 標準の「リモート デスクトップ接続」に合わせ、接続先 PC 名または IP アドレス、任意のドメイン、ユーザー名を指定して接続し、ActiveX または Windows の標準認証 UI で認証する形を基本とする。
- 任意の接続名、接続先、ドメイン、ユーザー名は直接入力可能なコンボボックスとし、認証完了した直近の接続履歴から選択できるようにする。
- 接続名を履歴から選択した場合は、対応する接続先、ドメイン、ユーザー名を一組として復元する。接続名は64文字を上限とし、空欄の場合は従来どおり名前なし履歴として扱う。
- Remote Desktop ActiveXへは`Domain`と`UserName`を別propertyとして接続前に設定する。ドメイン欄が空でユーザー名が`DOMAIN\user`形式の場合だけ、既存入力との互換性のため二項目へ分離する。
- 接続履歴は「任意の接続名 + 接続先 + ドメイン + ユーザー名」で管理する。大小文字だけが異なる同一接続組、または同じ非空接続名を重複保持せず、最新の定義一件へ統合する。各コンボボックス内でも同一候補を重複表示しない。
- 接続履歴の暗号化平文形式はVersion 3とし、任意の接続名、接続先、結合済みユーザー名、マイク転送、リモート音声再生先を保存する。ドメインがある場合は`DOMAIN\user`として保存・復元する。従来のVersion 1／2は、マイクOFF、リモート音声をClient側で再生する既定値で読込可能とし、次回保存時にVersion 3へ更新する。
- 接続履歴へ password を含めない。履歴は件数を有限にし、Windows ユーザー単位の DPAPI で暗号化して `%LOCALAPPDATA%` 以下へ保存する。
- 接続管理 Windowの親背景はWindows標準ダイアログ色へ統一し、ラベルやラジオボタンの背景矩形が異なる色で浮いて見えないようにする。
- Client 起動時は、接続入力と履歴を所有する接続管理 Window を一つ生成する。
  「新しい接続」は、RDP ActiveX、`ESVNBRG`、Bridge、Proxy worker を独立所有する
  session Window を別 top-level Window として生成する。
- 一つの Client process で同時に所有できる RDP session は 16 件を上限とする。
  全 ActiveX は同じ COM STA UI thread に所属させるが、event sink、Virtual
  Channel、Bridge state、HTTP stream、Proxy port を session 間で共有しない。
- 一つの session Window を切断または終了しても、接続管理 Windowと他の
  session Windowを維持する。全 Window が終了した場合だけ Client processを
  終了する。
- session Windowはtitle bar、表示modeだけのコンパクトな一行、RDP表示領域で
  構成し、接続入力、切断button、Bridge状態欄、status barを重複表示しない。
  接続条件と初期表示modeは接続管理 Windowで確定し、session Window右上の
  閉じる操作で確認後にそのsessionだけを切断・終了する。
- session Window titleには、接続名がある場合は`接続名 (接続先)`、名前なしの場合は接続先PC名またはIPアドレス、RDP／Agent状態、
  `Proxy 127.0.0.1:<割当port>`、active stream数、方向別転送byte数を表示する。
  Proxy設定異常時はportを偽装せず、固定の設定error分類を表示する。
- RDP protocol は接続開始時に Client が remote desktop の幅と高さを要求するため、接続前に未知の既存 session 解像度を ActiveX だけで自動取得・維持できるものとして扱わない。
- RDP の表示方式は、同時選択できない次の 3 modeをラジオボタンで選択する。
  - `等倍表示（スクロール）`
    - 既定 mode とする。
    - 初期 remote desktop 寸法は、Windows 標準 RDP の全画面接続と同じく、接続開始時に ZRdp が存在する現在の monitor 全体の解像度を使用する。
    - 接続後の ZRdp window変更では remote desktop寸法を変更せず、ActiveX標準のscroll barを使用する。
  - `Smart Sizing（縮小表示）`
    - 初期 remote desktop 寸法は現在の monitor 全体の解像度を使用する。
    - `IMsRdpClientAdvancedSettings::SmartSizing`によるClient側描画の拡縮だけを行い、remote desktop寸法を変更しない。
  - `動的解像度（Window追従）`
    - 接続開始時はZRdpのRDP表示領域を初期 remote desktop 寸法に使用する。
    - 接続後は最後のwindow size変更から250 ms後に、`IMsRdpClient9::UpdateSessionDisplaySettings`でremote desktop寸法をRDP表示領域へ合わせる。
    - このmodeでは接続先applicationのlayoutが変化し得ることを、他の2 modeと区別できる名称で明示する。
- 3 modeは接続管理 Windowでsession開始前に一つだけ選択する。session Windowには
  3 modeのラジオボタンだけを一行で表示し、接続成立後も切り替え可能とする。
  Smart Sizingと動的解像度を同時に有効化しない。
- sessionごとに、接続開始時にActiveXへ要求したremote desktop幅／高さを
  手動Window変更の基準アスペクト比として保持する。通常Windowで利用者が辺または
  角をdragした場合は、title bar、非client frame、表示mode欄を除いた実際のRDP
  表示領域がこの比率を維持するよう`WM_SIZING`候補を補正する。
- 手動アスペクト比制約は最大化、Windowsスナップ、ActiveX全画面へ介入しない。
  等倍表示とSmart Sizingではremote desktop寸法を変更せず、動的解像度では
  制約後のRDP表示領域だけを既存debounce経路で接続先へ適用する。
- 接続管理 Windowには、接続開始前のローカルリソース設定として次を表示する。
  - `マイクを接続先で使用する`
    - `IMsRdpClientAdvancedSettings7::AudioCaptureRedirectionMode`で設定する。
    - privacy上の明示選択とし、既定はOFFとする。
  - `リモート音声`
    - `このPCで再生する`、`接続先PCで再生する`、`再生しない`を排他的に選択する。
    - 既定は`このPCで再生する`とする。
- 音声リダイレクトはRDP接続時に確定し、接続後のsessionへ途中反映しない。
  接続管理 Windowで変更した値は次に開始するsessionから適用する。接続先GPO等が
  転送を禁止する場合は、policy変更や別経路へのfallbackを行わない。
- 接続名または接続先履歴を選択した場合は、接続情報と同じprofileに保存された
  マイク転送およびリモート音声再生先も一組として復元する。
- 接続前に`ContainerHandledFullScreen`を有効化し、Microsoft controlの全画面状態と
  ZRdp container Windowを一体として切り替える。全画面開始時は現在monitor全体へ
  borderless Session Windowを配置し、title bar、frame、表示mode欄を隠してRDP表示を
  client領域全体へ広げる。Window復帰時は元styleと通常／最大化配置をsession単位で
  復元する。
- session Windowには表示modeと並べて全画面buttonを表示し、Windows標準RDP互換の
  `Ctrl+Alt+Break`要求と同じActiveX／container event経路で処理する。全画面中の
  Window復帰はMicrosoft connection barまたは`Ctrl+Alt+Break`を使用する。
- 接続前に`DisplayConnectionBar`を有効化し、`PinConnectionBar`をraised状態へ設定する。
  connection barの最小化buttonとWindow復帰buttonを明示的に有効化してreadbackする。
- 全画面開始時はZRdp containerを先にborderless monitor配置し、その後にActiveXの
  `FullScreen`を有効化する。container配置ではtop-level Z-orderを変更せず、後から
  表示されるMicrosoft connection barを覆わない。Window復帰時はActiveX全画面を
  先に解除し、その後にcontainerのstyleと配置を復元する。
- 全画面中はZRdp独自のbuttonや表示mode欄をRDP領域へoverlayしない。Microsoft
  connection barを画面操作の正本とし、keyboard操作として`Ctrl+Alt+Break`を維持する。
- 全画面connection barの最小化要求ではSession Window全体を標準最小化し、taskbar
  からの復元後も全画面を維持する。最小化中は動的解像度timerを停止し、Window mode
  復帰時と自動再接続完了時に再適用する。
- 全画面connection barと全画面window titleには、`ZRdp SVN Bridge - <接続先>`の形式で、現在入力された接続先PC名またはIPアドレスを表示する。
- 全画面識別表示は`FullScreenTitle`の`MAX_PATH - 1`文字制約内で構築する。通常のIPアドレスとhostnameは完全表示し、制約を超える長い入力は接続先設定を変更せず、表示末尾だけを`...`として省略する。
- 全画面識別表示の接続先はruntime UIだけに使用し、診断log、resource、追加設定fileへ出力しない。hostname入力時にDNS解決してIPを探索せず、入力値をそのまま表示する。
- 動的解像度の更新失敗時もRDP接続は維持し、等倍表示へ縮退する。Smart Sizingを暗黙fallbackとして有効化しない。
- window size変更を理由として`Reconnect`を呼び出さない。
- Full HD／WQHD／Full の選択 UI は、remote desktop 寸法を変更して接続先 application layout へ影響するため設けない。
- 接続管理 Windowの初期 UI は次を優先する。
  - 任意の接続名
  - 接続先 PC 名または IP
  - ドメイン
  - ユーザー名
  - 新しい接続
  - 3つの排他的な表示mode（等倍表示／Smart Sizing／動的解像度）
  - マイク転送
  - リモート音声の再生先
- session Windowは3つの排他的な表示modeだけを操作欄へ表示し、title barは次を
  表示する。
  - RDP 接続状態
  - Agent 接続状態
  - SVN Proxy listen 状態
  - active stream 数
  - 送受信 byte 数
  - エラー分類
- SVN Bridge が利用不能でも、RDP 画面接続を可能な限り継続する。
- RDP 切断時は HTTP Proxy の既存 stream を終了し、新規 SVN 接続を拒否する。
- Agent 未起動時は UI に明確に表示し、HTTP Proxy 接続を長時間 hang させない。
- UI thread 上で blocking I/O を行わない。
- 最初から多言語化せず、日本語 UI を基本とする。表示文字列は resource 化し、将来差し替え可能にする。

## 14. Client の並行処理
- 複数 session の ActiveX／COM 操作は一つの STA UI thread に限定し、
  process共通のmessage loopで全top-level Windowをdispatchする。
- `ClientApplication`は接続管理 Window、最大16件のsession Window、有限の
  Proxy port slotを所有する。各session Windowは自身の`RdpControlHost`、
  `ClientBridge`、`ClientProxyWorker`をRAIIで所有する。
- localhost Proxy の accept、HTTP header 解析、Socket read/write、frame reassembly は worker 側で行う。
- worker から COM UI thread への送信要求は bounded queue と message dispatch で marshal する。
- `OnChannelReceivedData` は受信データを所有 buffer へコピー後、速やかに worker queue へ渡して戻る。
- thread-per-connection を無制限に作らない。
- PoC で thread-per-connection を使う場合も、最大同時 stream 数を小さな有限値に制限し、製品化前に方式を再評価する。
- thread、Socket、COM interface、HANDLE、event sink、timer は明確な所有者を持ち、RAII で解放する。

## 15. Agent の並行処理
- Virtual Channel read loop、serialized write queue、Socket I/O の責務を分離する。
- 同一 Virtual Channel handle への write は一つの writer 経路に集約する。
- Agent から任意の inbound TCP listen を行わない。
- stream ごとの upstream Socket は allowlist 確認後にだけ作成する。
- Agent 終了、RDP disconnect、protocol error、Client timeout の際は全 Socket を閉じる。
- Socket close、half-close、peer reset、timeout を protocol の `HALF_CLOSE`／`CLOSE` へ正しく対応させる。
- Agent は単体でバックグラウンド常駐サービス化せず、RDP ユーザーセッション内でユーザーが起動するポータブル EXE を基本とする。

## 16. セキュリティ要件
- Client Proxy が loopback 以外から接続できないことをテストで保証する。
- Agent が allowlist 完全一致以外へ接続できないことをテストで保証する。
- RDP 接続先証明書の警告を自動無視しない。
- SVN／Trac 認証情報、RDP 認証情報、HTTP payload、ソースコード、diff、Trac ticket／comment／attachment、Cookie、Authorization header をログへ出力しない。
- debug build でも payload dump を既定で実装しない。
- packet capture、credential capture、TLS interception を補助機能として追加しない。
- malformed frame、巨大 length、streamId collision、sequence error、unknown mandatory message は fail-closed で処理する。
- RDP Bridge の障害で RDP Client 本体が crash しないよう、callback 内例外を境界で捕捉し、COM 境界から C++ 例外を漏らさない。
- SEH、HRESULT、Win32 error、Winsock error を分類し、秘密情報を含まない形で報告する。
- 実環境の IP、URL、リポジトリ名、ドメイン、ユーザー名、画面キャプチャ、イベントログ、packet capture を GitHub へ登録しない。

## 17. ログおよび診断
- ログは状態確認に必要な最小限とする。
- 記録可能な項目は原則として次に限定する。
  - timestamp
  - process role
  - Bridge session ID
  - streamId
  - 匿名化した endpoint ID
  - state transition
  - 送受信 byte 数
  - timeout／close reason
  - HRESULT／Win32／Winsock の分類済み error code
- URL path、query、HTTP header、HTTP body、SVN／Trac payload、RDP password を記録しない。
- 実 IP は既定ログへ出さず、allowlist entry の匿名 ID を使用する。
- ログはサイズ上限と世代管理を持つ。
- crash dump は機密情報を含む可能性があるため Git 管理対象外とし、ユーザーの明示指示なく外部へ送信しない。
- `OutputDebugString` は開発時の補助に限定し、payload を出力しない。

## 18. 開発環境
- Visual Studio 2022 を使用する。
- C++ Language Standard は C++20 とする。
- Windows SDK を使用する。
- x64 を初期対象とし、Win32/x86 対応は明示要求があるまで行わない。
- Unicode build を使用する。
- project source には `/W4` 相当を適用し、warning を残さない。
- project source の warning は原則 error として扱う。ただし Windows SDK や外部 header の warning を無理に修正しない。
- `/permissive-`、`/Zc:__cplusplus` 等、標準準拠を高める設定を使用する。
- Release build では Windows の標準的な stack protection、ASLR、DEP、Control Flow Guard 等を無効化しない。
- Agent の配布を簡素化するため、CRT の扱いは配布方式と合わせて ADR に記録する。
- 外部依存ライブラリは原則追加しない。追加が必要な場合は、ライセンス、更新頻度、脆弱性、配布物、private repository への取り込み方法を事前に提示する。
- vcpkg、Conan、NuGet 等をユーザー承認なく導入しない。

## 19. 標準ディレクトリ

```text
/
├─ AGENTS.md
├─ CMakeLists.txt
├─ CMakePresets.json
├─ cmake/
├─ src/
│  ├─ common/                 共通型、RAII、エラー、ログ
│  ├─ protocol/               frame encode/decode、state machine
│  ├─ client/
│  │  ├─ app/                 Win32 entry、UI
│  │  ├─ rdp/                 ActiveX host、event sink
│  │  ├─ proxy/               localhost HTTP Proxy
│  │  └─ bridge/              stream multiplexer、SVC adapter
│  └─ agent/
│     ├─ app/                 Agent entry、session check
│     ├─ channel/             WTS Virtual Channel
│     ├─ network/             allowlist、Winsock
│     └─ bridge/              stream multiplexer
├─ tests/
│  ├─ unit/
│  ├─ protocol/
│  ├─ proxy/
│  ├─ agent/
│  └─ integration/
├─ config/
│  └─ examples/               架空値のみの設定例
├─ docs/
│  ├─ adr/
│  ├─ architecture.md
│  ├─ protocol.md
│  ├─ http-proxy.md
│  ├─ security.md
│  ├─ configuration.md
│  └─ test-plan.md
└─ work/
   ├─ analysis/
   ├─ proposals/
   └─ tmp/
```

- CMake の中間生成物、test binary、PDB 等は source tree 外または `build/` 以下へ置く。
- ユーザーが実機確認・portable配布に使用するRelease版`ZRdpSvnBridge.exe`と、Phase 1以降の`ZRdpSvnAgent.exe`は、workspace直下の`out/`へ出力する。
- `out/`は生成物専用かつGit管理外とし、Release EXEと実行時に自動生成される`out/logs/`以外のsource file、設定、credentialを置かない。
- Visual Studio が生成する `.vs/`、`build/`、binary、log を source directory に混在させない。

## 20. GitHub およびソース管理
- ソース管理には GitHub を利用し、`https://github.com/EnterOgawa` 配下にリポジトリを作成する。
- リポジトリ名の候補は **`zrdp-svn-bridge`** とするが、実作成前にユーザーの指示を優先する。
- GitHub リポジトリは、**作成時点から必ず Private** とする。
- Public リポジトリとして作成後に Private へ変更する運用は禁止する。
- リポジトリの visibility を Public または Internal へ変更しない。
- GitHub Project を利用する場合も Private とする。
- GitHub Pages を有効化しない。
- 公開 GitHub Release、公開 Package、公開 Container Image、公開 Fork を作成しない。
- Private Repository の Release であっても、ユーザーの明示指示と署名完了連絡があるまでは作成しない。
- GitHub Actions は既定で導入しない。導入する場合は、実取引先情報、実設定、credential、binary artifact を runner へ送らない設計を提示し、ユーザー承認を得る。
- 外部 Action を使用する場合は、mutable tag ではなく検証済み commit SHA へ固定する。
- issue、milestone、PR、review、release note、コメント本文は原則として日本語で記載する。
- ユーザー側の動作確認が完了し、ユーザーから明示的な指示があるまでは、`git push`、tag push、PR 作成、Release 作成、asset upload 等の remote operation を行わない。
- collaborator、team、deploy key、GitHub App、PAT をユーザーの明示指示なく追加しない。
- Private Repository であっても、取引先機密情報を登録してよいとは解釈しない。

## 21. `.gitignore` 必須項目
- 少なくとも次を Git 管理対象外とする。

```gitignore
.vs/
out/
build/
cmake-build-*/
CMakeUserPresets.json
*.user
*.suo
*.VC.db
*.VC.opendb
*.obj
*.pch
*.pdb
*.ilk
*.exe
*.dll
*.lib
*.exp
logs/
dumps/
captures/
*.dmp
*.pcap
*.pcapng
*.etl
*.rdp
*.log
config/local/
config/*.local.*
agent.ini
client.ini
work/tmp/
.svn/
```

- 実環境 allowlist、実 IP、RDP 接続情報、SVN／Trac 接続情報を含む設定は commit しない。
- 秘密情報を誤って commit した場合、削除 commit だけで済ませず、履歴除去と credential rotation の必要性をユーザーへ直ちに報告する。

## 22. テスト方針
- 通常確認は、取引先環境、Zscaler、支給 PC、実 SVN／Trac に依存しない単体テストを優先する。
- protocol layer は in-memory transport で検証できるようにする。
- Virtual Channel Adapter は fake transport と差し替え可能にする。
- HTTP Proxy は mock HTTP/WebDAV／Trac server を使用する。
- 実在する取引先 IP をテストへ埋め込まない。
- 実取引先 SVN／Trac への自動テストを作成しない。
- GitHub Actions から取引先環境へ接続しない。
- Commit 系テストは専用 test repository または明示指定された安全な対象だけで行う。
- 実環境での Commit、Delete、Move、Lock 操作は、ユーザーから個別に明示された場合だけ行う。

### 22.1 必須単体テスト
- frame encode/decode
- fragmented frame reassembly
- multiple frames in one receive buffer
- invalid magic/version/type
- oversized payload
- integer overflow
- duplicate streamId
- invalid state transition
- flow-control window exhaustion/recovery
- queue upper bound
- allowlist exact match
- allowlist rejection
- invalid IPv4／port
- absolute-form から origin-form への変換
- malformed request line
- Host mismatch
- `Content-Length`
- chunked transfer encoding
- `100 Continue`
- Keep-Alive
- half-close
- timeout
- peer reset
- RDP disconnect equivalent cleanup
- Agent absent／version mismatch
- loopback-only bind
- session Proxy port の順次割当、重複防止、解放、再利用、上限、overflow
- 同一 COM STA 上での複数 RDP ActiveX 同時生成と、一方の破棄後の他方の存続
- Mylyn Trac absolute-form GET／XML-RPC POST
- Trac redirect／Cookie／gzip responseのopaque透過

### 22.2 結合テスト
- Client Proxy -> fake Virtual Channel -> Agent -> Mock HTTP Server
- multiple simultaneous repositories
- large Checkout equivalent stream
- large Commit equivalent stream
- long-running Keep-Alive
- RDP channel fragmentation equivalent
- Agent restart
- Client restart
- RDP disconnect/reconnect equivalent
- Agent allowlist change
- Mylyn Trac Web／XML-RPC相当sequence

## 23. 実装フェーズと Gate

### Phase 0: ActiveX 接続成立確認
- RDP ActiveX をホストする最小 Client を作成する。
- Zscaler ON の状態で、現在 `mstsc.exe` から接続可能な支給 PC へ接続できることをユーザー環境で確認する。
- NLA、認証画面、キーボード、マウス、画面更新、切断、再接続を確認する。
- この Gate を通過するまで SVN Bridge 本実装を拡大しない。
- ActiveX Client だけ接続できない場合、process spoofing 等を試さず結果を報告する。

#### Phase 0 初回成果物の必須範囲
- 初回成果物は、RDP ActiveX による接続および切断、キーボードおよびマウスによる通常操作を実装する。
- 任意のドメインとユーザー名を別欄で入力し、Microsoft標準credential UIへ`Domain`と`UserName`を分離して渡せることを必須とする。
- ウィンドウモードでリモート画面を表示し、通常操作できることを必須とする。
- ウィンドウモードの既定動作では接続開始時の remote desktop 寸法を維持し、狭いウィンドウで scroll 操作できることを必須とする。
- `等倍表示（スクロール）`、`Smart Sizing（縮小表示）`、`動的解像度（Window追従）`を、排他的なラジオボタンとして表示する。
- `Smart Sizing（縮小表示）`ではClient側表示だけを拡縮し、remote desktop寸法を変更しない。
- `動的解像度（Window追従）`では利用者の明示選択時だけremote desktop寸法をwindowへ追従させ、接続先application layoutが変化し得ることを他modeと混同させない。
- 全画面モードで通常操作でき、UI 操作および Windows 標準の全画面切替キー操作（`Ctrl+Alt+Break`）でウィンドウモードとの間を安全に切り替えられることを必須とする。
- 全画面connection barに現在の接続先PC名またはIPアドレスを表示し、複数sessionを識別できることを必須とする。
- クリップボードリダイレクトは必須機能とし、RDP 接続前に `IMsRdpClientAdvancedSettings5::RedirectClipboard` 相当を明示的に有効化する。
- クリップボードは ActiveX 標準の自動同期を使用し、開発 PC から接続先 PC、および接続先 PC から開発 PC の双方向で動作させる。
- Phase 0 Gate では、少なくとも双方向のプレーンテキストのコピーおよび貼り付けを必須合格条件とする。画像およびファイルは、Windows と接続先の GPO が許可する範囲で確認する。
- 接続先または開発 PC の GPO、EDR、RDP ポリシー等によってクリップボードが禁止されている場合、設定変更や回避を行わず、適用中の制約と検証結果をユーザーへ報告する。
- 認証情報入力、NLA、CredSSP、サーバー証明書警告は Windows 標準動作を使用し、警告を自動承認したりセキュリティ設定を弱めたりしない。
- Microsoft 標準 credential UI を利用者が close または Cancel した場合は、追加の RDP 接続 error dialog を表示せず未接続状態へ戻す。ログイン完了後の切断、認証失敗、通信失敗、証明書 error 等は同じ扱いにせず、従来どおり理由を表示する。
- Phase 0 の接続確認は Zscaler を ON のまま実施する。
- `mstsc.exe` の全コマンドライン option、任意の `.rdp` ファイル、および Windows バージョン固有機能の完全互換は Phase 0 の完了条件に含めない。
- Phase 0 は製品化前の技術 Gate であり、製品へ RDP 専用モードを設けるものではない。Phase 1 以降の製品接続では、常に RDP 接続前に `ESVNBRG` を作成する。

### Phase 1: Static Virtual Channel Echo
- Client が `ESVNBRG` を接続前に登録する。
- Agent が同名 SVC を RDP セッション内で開く。
- `HELLO`／`HELLO_ACK`、`PING`／`PONG` を実装する。
- 1 byte、1 KB、64 KB、1 MB、10 MB 相当を分割送信し、欠落・重複・順序を検証する。

### Phase 2: Protocol と多重化
- frame parser、stream state machine、queue upper bound、flow control を実装する。
- fake Socket で複数 stream を検証する。

### Phase 3: Agent の単一 TCP 接続
- allowlist に登録した Mock HTTP Server 一台への接続だけを実装する。
- 実取引先 SVN にはまだ接続しない。

### Phase 4: localhost HTTP Proxy
- `127.0.0.1:18080` を実装する。
- absolute-form／origin-form 変換を実装する。
- Mock HTTP/WebDAV Server で確認する。

### Phase 5: HTTP/SVN 互換性
- Repository browse 相当を確認する。
- Checkout、Update、Commit 相当の HTTP sequence を Mock／test SVN で確認する。
- 複数 server、複数 port、複数 repository を確認する。

### Phase 6: Hardening
- malformed input、resource exhaustion、timeout、disconnect、reconnect、Agent crash を確認する。
- log redaction、allowlist、loopback bind を監査する。

### Phase 7: 実環境 Read-only 確認
- ユーザーの明示指示がある場合に限り、実支給 PC と実 SVN で Repository Explorer、Log、Diff、Checkout、Update を確認する。

### Phase 8: 実環境 Write 確認
- ユーザーが指定した安全な test repository／test path に限り、Commit、Add、Delete、Move、Lock、Unlock を確認する。

### Phase 9: 個別改善
- ユーザー要望を一件ずつ独立した改善項目として扱う。
- Trac対応は、承認済みHTTP endpointに対するMylyn repository固有Proxy利用に限定する。
- Mock Tracで互換性とsecurity boundaryを確認した後、ユーザーが実環境で設定検証、ticket参照、必要な更新操作を段階的に確認する。
- 複数RDP接続は、一つのClient process内の接続管理 Windowから独立session
  Windowを生成し、sessionごとにbase portから重複しないloopback Proxy portを
  割り当てる方式とする。
- session Windowはtitle bar、表示modeだけの一行、RDP表示へ簡素化し、接続先、
  RDP／Agent状態、Proxy port、stream数、転送量をtitleへ集約する。
- RDP音声リダイレクトは接続管理 Windowのprofile設定とし、マイク転送と
  3種類の音声再生先を接続前にActiveXへ適用する。接続中の変更機能は設けず、
  新しいsessionまたは再接続で反映する。
- 最終仕様書と利用手順書は、全改善項目の完了後に作成する。

## 24. 完了条件
- Windows 標準 RDP ActiveX Control を使用して支給 PC へ安定接続できる。
- Zscaler の停止・変更を行わない。
- 開発 PC と支給 PC の間に RDP 以外の新しい通信待受を作らない。
- Client 側は単一の `ZRdpSvnBridge.exe` として動作する。
- 支給 PC 側は通常ユーザー権限の `ZRdpSvnAgent.exe` として動作し、Windows サービス、driver、inbound port を必要としない。
- localhost HTTP Proxy は loopback 以外から接続できない。
- Agent は allowlist 完全一致以外へ接続できない。
- 接続管理 Windowから複数の独立RDP session Windowを開始でき、各sessionの
  Proxy portを識別できる。
- 任意の接続名を保存・選択でき、選択時に対応する接続先、ドメイン、
  ユーザー名、マイク転送、リモート音声再生先を一組として復元できる。
- 一sessionの切断、Agent停止、Proxy失敗、Window終了が他sessionへ波及しない。
- 複数 HTTP SVN server／repository を扱える。
- 既存 SVN URL を変更せず利用できる。
- Repository browse、Checkout、Update、Commit、Add、Delete、Move、Lock/Unlock が確認できる。
- 承認済みHTTP Tracについて、Mylynの設定検証、ticket query／参照、およびユーザーが必要とする更新操作が確認できる。
- RDP 切断時に全 HTTP stream と upstream Socket が終了する。
- payload、credential、実 IP／URL がログや GitHub に残らない。
- 汎用 VPN、SOCKS、任意 TCP Forward として利用できない。

## 25. C++ コーディング規約
- 既存コードが存在する場合は、より厳しい既存規約を優先する。

### 25.1 ソースコメント付与ルール

#### 25.1.1 基本方針
- 本規約は、参照ソース `winsystem.cpp` とユーザー提示の `AplMain.java` に共通する
  「ファイル／class概要」
  「定義群の区切り」「function契約」「処理段階」「分岐理由」「loop目的」を
  明示する構成を、本プロジェクトの C++20、Doxygen、Git、RAII 方針へ
  適合させたものとする。
- 参照ソースの実装、固有名、著作権表示、作成者、日付、手書きversion、
  手書き変更履歴は本projectへ複写しない。参照するのはコメントの構造と
  説明密度だけとする。
- すべての `.h`、`.cpp`、resource source、test source に適用する。
  通常は変更箇所と周辺を同じ変更で整備し、全source監査をユーザーが
  明示した場合は、未変更箇所も含めてcommentだけの一括整備を行ってよい。
- ソースコメントの説明文は原則として日本語で記載する。API 名、class 名、function 名、protocol 名、error code 等は正確な英語表記を維持する。
- コメントは UTF-8 で記録し、文は原則として句点 `。` まで記載する。
- コメントは code の言い換えではなく、目的、理由、前提、制約、所有権、失敗時動作のうち、code だけでは判別しにくい事項を記載する。
- 接続先、ドメイン、ユーザー名、実 IP、実 URL、credential、payload、取引先固有名等の機密情報を、例示を含めコメントへ記載しない。
- コメントと実装は同じ変更で更新する。実装と矛盾したコメントを残すことは、コメントがない状態より重大な defect として扱う。

#### 25.1.2 ファイル先頭コメント
- 新規 `.h`／`.cpp` には、include guard または最初の `#include` より前に Doxygen の file comment を付与する。
- file comment には最低限 `@file`、`@brief`、そのファイルの責務を記載する。thread affinity、security boundary、主要な所有関係がある場合は `@details` または `@note` で追記する。
- 作成者名、会社名、作成日、手書き version、手書き変更履歴は記載しない。これらは Git history と version resource で管理する。

```cpp
/**
 * @file connection_history.cpp
 * @brief RDP 接続履歴の重複排除と暗号化永続化を実装する。
 *
 * @note file I/O と DPAPI は UI thread では実行しない。
 */
```

#### 25.1.3 ファイル内の区切り
- 定数、内部型、内部 helper、公開実装等の責務が複数まとまる場合は、短い section comment で区切る。
- section comment は内容を示す名称を持たせ、装飾だけの区切りや、数行ごとの過剰な区切りは使用しない。
- 定数群、member群、callback群、test helper群、test case群は、参照sourceの
  定義見出しに相当するsection commentで目的を示す。同種の宣言が連続する
  場合は、一行ごとの見出しではなく群全体を一つの見出しで説明する。
- namespace scopeまたはclass実装でfunctionが長く連続する場合は、次の形式で
  責務単位を区切る。罫線だけを置かず、中央行には必ず意味のある名称を書く。
- namespace、class、function の終了コメントは、nest が深い場合または対象が画面外になり得る場合にだけ付与する。

```cpp
// -----------------------------------------------------------------------------
// 接続履歴の wire-independent 永続化形式
// -----------------------------------------------------------------------------
```

#### 25.1.4 class・型・function の契約
- public class、public function、protocol type、security boundary には Doxygen comment を必須とする。
- private function、free function、callback、thread entry、Win32／COM 境界にも、処理が自明な getter／setter でない限り Doxygen comment を付与する。
- test sourceの`main`／`wmain`、test用callback、fake transport、mock server、
  test helperにもDoxygen commentを付与し、どの契約または失敗条件を検証するか
  明示する。
- function comment は最低限 `@brief` を持ち、引数がある場合は `@param[in]`、`@param[out]`、`@param[in,out]` のいずれかで方向と意味を示す。
- 戻り値がある場合は `@return` を記載する。複数の終了値に意味がある場合は `@retval` で値ごとの意味を記載する。
- ownership、lifetime、thread affinity、lock 前提、blocking の有無、callback 中だけ有効な data、fail-closed 動作等は `@note`、`@warning`、`@pre`、`@post` を使って明示する。
- `noexcept` function は、内部失敗をどの error value または状態へ変換するかを必要に応じて記載する。
- header に公開契約がある function は、`.cpp` 側へ同じ説明を複製しない。実装側では algorithm や API 制約等、その場所でしか分からない事項だけを補足する。
- function signature を comment 内へそのまま複写しない。型や引数名の正本は declaration とし、comment は意味と契約に集中する。

```cpp
/**
 * @brief 認証完了した接続組を最新位置へ移動して保存する。
 * @param[in] server 接続先を表す bounded 文字列。
 * @param[in] domain 任意のRDPドメイン。password は受け付けない。
 * @param[in] user_name ユーザー名。password は受け付けない。
 * @return 保存要求の受付結果を表す HRESULT。
 * @note 実際の file I/O は専用 worker が直列実行する。
 */
```

#### 25.1.5 定数・変数・状態値
- 定数には、名前だけで単位または制約が明確でない場合、用途、単位、上限理由を記載する。
- member、static state、HANDLE、SOCKET、COM interface、buffer には、所有者、解放責任、sentinel 値、thread 制約が自明でない場合にコメントを付与する。
- class／structのmemberは、参照sourceの内部定義欄と同様に、役割が名前だけで
  完全に説明できない場合はdeclaration直前または行末へDoxygen commentを付ける。
  同一責務のWin32 control handle群等はsection commentでまとめてよい。
- `bool`、enum、bit flag の各値が業務状態または互換性条件を表す場合は、その意味を declaration の近くへ記載する。
- 短い単位や値の対応は行末コメントを使用してよい。複数行になる説明や理由は declaration の直前へ記載する。
- 変数名から明らかな内容を「件数」「結果」等と言い換えるだけのコメントは付与しない。

#### 25.1.6 function 本体の処理コメント
- function 本体は、初期化、入力検証、変換、外部 API 呼出し、状態更新、後処理等の意味のある処理段階ごとに、必要な場合は直前へ短い日本語コメントを付与する。
- 連続する複数 statement が一つの目的を達成する場合は、statement ごとではなく処理 block 全体へ一つのコメントを付与する。
- test本体では、入力準備、実行、期待値検証、cleanupの各段階を示し、数値の
  return codeだけでは失敗条件が分からない状態を残さない。
- API 名を言い換えるだけの「ファイルを開く」「値を設定する」ではなく、なぜその時点で必要か、どの制約を守るかを優先して記載する。
- 正常系から離れる cleanup、rollback、fallback、retry、partial failure の処理には、その経路の目的を明示する。
- RAII で自明に解放される resource に対して、単に「解放する」とだけ記載するコメントは不要とする。

#### 25.1.7 条件分岐・loop・switch
- `if` 文では、条件の意図が自明でない場合、条件式の直前に理由を示すコメントを付与する。
- security check、length check、overflow check、allowlist check、state check、thread check、ownership check の `if` には必ず、拒否または許可する理由を記載する。
- guard clause には、どの不正状態または不要処理を早期終了させるのかを記載する。
- `else` が異なる業務状態または互換経路を表す場合は、各 branch の意味を明示する。単なる真偽の反転を繰り返すコメントは不要とする。
- `for`、range-for、`while`、`do while` の直前には、loop の目的を記載する。
- 上限、終了条件、順序、重複排除、fragment 再構成等が重要な loop では、invariant または有限である理由も記載する。
- `switch` の各 `case` が code から明確でない状態遷移を表す場合は、その意味を記載する。
- fallthrough は `[[fallthrough]]` と理由コメントを必須とする。

#### 25.1.8 platform・並行処理・security 境界
- Win32、COM、RDP ActiveX、WTS Virtual Channel、Winsock、DPAPI 等の境界では、API の非自明な前提、byte／character 単位、ownership 移転、error code の出所を記載する。
- COM STA、worker thread、callback thread 等、実行 thread が correctness に影響する function には thread affinity を明記する。
- mutex を取得する処理には、lock が保護する状態を明記する。複数 lock が関係する場合は lock order と callback／COM 再入の可能性を記載する。
- queue、buffer、frame、stream の上限 check には、resource exhaustion を防ぐ境界であることを記載する。
- credential、payload、実 endpoint 等を log へ出さない処理には、redaction または非記録の理由を境界 comment として残す。

#### 25.1.9 禁止事項
- 大量の code を `//` または `/* ... */` で無効化して残さない。不要 code は Git history に任せて削除する。
- `#if 0` は、issue、残す理由、再有効化または削除の完了条件が明確な短期的検証 code に限る。
- TODO、FIXME は issue 番号、理由、完了条件を伴う場合だけ使用する。
- 個人名、作業日誌、変更履歴、review 会話、暫定 debug 出力を恒久 comment として残さない。
- API document を長文で転載しない。必要な制約だけを要約し、判断に依存する場合は公式文書の名称または ADR を参照する。
- 自明な行ごとの逐語 comment、実装と同じ情報しか持たない comment、将来保証できない推測 comment を付与しない。

### 25.2 その他の C++ 規約

- `if`、`for`、`while` 等の block 終了後は、次の独立処理との間に原則 1 行空ける。
- `if ... else`、`try ... catch`、`try ... finally` 相当の連続構造では間を空けない。
- `switch` では case 間の視認性を保つ。
- raw owning pointer を使用しない。
- HANDLE、SOCKET、BSTR、COM interface、event subscription は RAII wrapper で管理する。
- `new`／`delete` を application code へ直接散在させない。
- buffer は `std::span`、`std::vector<std::byte>`、固定幅整数等で表現し、文字列型と混同しない。
- C style cast を使用しない。
- `reinterpret_cast` は protocol／Win32 境界の限定箇所に閉じ込め、理由をコメントする。
- wire data を C++ struct へ直接 cast しない。
- COM callback、Win32 callback、thread entry から C++ 例外を外へ漏らさない。
- HRESULT、GetLastError、WSAGetLastError を失わず、共通 error type へ変換する。
- `catch (...)` で無言に握りつぶさない。
- log message と user-facing message を分離する。
- mutex 保持中に COM call、Socket blocking call、log file I/O を行わない。
- lock order を文書化し、callback から再入される可能性を考慮する。
- magic number、timeout、queue size、frame size は定数または設定へ集約する。

## 26. 作業前提
- 作業対象はカレントリポジトリ配下に限定し、他の階層を変更しない。
- 既存ソース、既存設定、既存テスト、既存 ADR を確認してから変更する。
- 環境依存の接続、支給 PC 操作、実 SVN 操作を勝手に実行しない。
- 実装前に影響範囲、security boundary、失敗時動作、rollback 方法を明示する。
- 大きな技術選択は、コード着手前に `docs/adr` または `work/proposals` へ記録する。
- 不明点を推測で固定せず、Windows API、既存実装、再現テストから確認する。
- 作業を止める必要がない不明点は、安全側の仮定を明記して進める。
- ユーザーから明示的に依頼されていない機能拡張を「便利だから」という理由で追加しない。

## 27. 作業ルール
1. 実ソースは `src`、テストは `tests`、文書は `docs` に置く。
2. 解析結果、修正提案、一時ファイルは `work` 以下に置く。
3. `work` 直下は常にクリーンにし、用途別サブフォルダを使用する。
4. ファイル出力が必要な解析結果は `work/analysis` 配下へ格納する。
5. 修正提案は `work/proposals` 配下に `.md` 形式で作成する。
6. 修正提案のファイル名は `YYYYMMDD_HHMMSS_***.md` 形式とする。
7. 一時ファイルは `work/tmp` を使用し、永続保管を前提としない。
8. binary、log、dump、capture、real environment config を source folder へ混在させない。
9. ユーザーの明示指示なく build system、dependency manager、license、repository visibility を変更しない。
10. 実取引先情報を source、test、docs、issue、PR、commit message に記載しない。

## 28. 修正提案の提示ルール
- 変更対象ファイル、class、function、可能であれば行番号を明示する。
- 変更理由を記載する。
- ActiveX／COM、Virtual Channel、HTTP Proxy、Agent Socket、security boundary への影響を記載する。
- protocol compatibility への影響を記載する。
- test 観点と rollback 方法を記載する。
- コード提示は原則として変更箇所に限定し、diff 形式で `-` と `+` を示す。
- 新規ファイルの場合は、役割、公開 interface、依存関係を先に説明する。
- 修正提案は `work/proposals` 配下へ保存する。

## 29. ドキュメント方針
- `docs` 配下は原則として日本語で記載する。
- API 名、class 名、method 名、protocol 名、configuration key、log ID 等は正確性のため英語表記を使用してよい。
- 少なくとも次を作成・維持する。
  - `docs/architecture.md`
  - `docs/protocol.md`
  - `docs/http-proxy.md`
  - `docs/rdp-activex.md`
  - `docs/agent.md`
  - `docs/security.md`
  - `docs/configuration.md`
  - `docs/test-plan.md`
  - `docs/adr/*.md`
- 実取引先情報を含む運用メモは Git 管理対象の `docs` に置かない。
- Microsoft 公式サンプルや外部コードを参考・転用する場合は、出典、license、変更点を記録し、必要な copyright notice を保持する。

## 30. 署名およびリリース（無効）

- 本節の規定は、2026-07-31のユーザー明示指示により無効とする。
- 署名およびリリース作業には、本節以外の有効なsecurity要件、Private運用規則、
  remote operation制限、およびその時点のユーザー明示指示を適用する。

## 31. 重要な設計判断
- 本プロジェクトは「RDP のクローンを一から作る」のではなく、「Windows 標準 RDP ActiveX Control を専用 Client に組み込み、SVN Bridge を追加する」方式である。
- Client 側を単一 EXE に近づけるため、初期版は ActiveX の scriptable Static Virtual Channel を使用する。
- Agent 側アプリは Virtual Channel の server module と upstream TCP client の役割を持つため、省略できない。
- Static Virtual Channel の制約は application protocol の framing、fragmentation、flow control で吸収する。
- Static Virtual Channel の性能が不足するかは推測で決めず、Checkout／Commit 相当の実測で判断する。
- DVC、FreeRDP、独自 RDP protocol 実装は初期スコープに含めない。
- 複数RDP接続は複数process起動ではなく、一つのClient processと一つのCOM STA
  message loop内に独立session Windowを生成する。Proxy portは`client.ini`の
  base portから有限slotで割り当て、最初の通常利用sessionを`18080`へ固定する。
