# ZRdp SVN Bridge

<p align="center">
  <img src="assets/icons/zrdp-icon.png" width="96" height="96" alt="ZRdp SVN Bridgeのアイコン">
</p>

接続先PCでは使えるSVNを、手元の開発環境からも使いたい。
ZRdp SVN Bridgeは、その間をRDP接続でつなぐWindowsアプリです。HTTP方式のSVNと、利用を承認したTracに対応しています。

リポジトリURLや作業コピーはそのまま。SVN／TracクライアントのProxy設定を対象ごとに指定して利用します。
Agent設定を一度登録すれば、次からはRDP接続するだけでAgentが自動起動します。

正式版は `0.7.21.0` です。[Releaseからダウンロード](https://github.com/EnterOgawa/zrdp-svn-bridge/releases/tag/v0.7.21.0)できます。
変更点と確認範囲は [リリースノート](docs/release-notes-0.7.21.md) をご覧ください。

## ✨ できること

- 最大16件のRDP接続を、別々のウィンドウで利用
- 等倍・縮小・ウィンドウ追従の3つの表示モードと全画面操作
- 接続ごとのAgent設定、自動配置・起動、疎通が途絶えたときの自動復旧
- 既存のURLを変えずに、HTTP方式のSVN／Tracへアクセス
- ClientとAgentをセットで取得する、確認付きの更新

開発PCで動くClientと、接続先で動くタスクトレイアプリのAgentを使います。
Agent用の受信ポートやWindowsサービス、ドライバーを追加する必要はありません。

## 🚀 はじめて使う

### 1. アプリを用意する

インストーラーを使う場合は、Setupを実行すると二つのEXEがまとめて配置されます。
スタートメニューからClientを起動してください。手動配置のportable版も利用できます。
アンインストール時には、接続履歴とAgent設定を残すか削除するかを選べます。
Setupの作成方法とアンインストールについては [インストーラー](docs/installer.md) をご覧ください。

手動で配置する場合は、同じバージョンのClientとAgentを開発PCの書込み可能なフォルダへ置きます。

```text
ZRdp/
├─ ZRdpSvnBridge.exe  ← こちらを起動
└─ ZRdpSvnAgent.exe   ← Clientが接続先へ配置
```

両PCはWindows x64、開発PCにはWindows標準のRDP ActiveXが必要です。
自前の未署名ビルドも使えます。ビルド方法は [こちら](#-ソースからビルドする) をご覧ください。

利用前に、RDP接続・接続先でのEXE実行・対象サーバーのHTTP利用が許可されていることを確認してください。
Agentの自動配置には、RDPのドライブ転送とクリップボード転送も必要です。
信頼できる配布元を使い、Windowsや組織の実行制限に従ってください。

### 2. 接続情報を入力する

`ZRdpSvnBridge.exe` を起動し、接続先、ドメイン、ユーザー名を入力します。
ドメインと接続名は必要に応じて指定してください。接続名を付けておくと、次回の選択がしやすくなります。

![接続マネージャー](docs/images/connection-manager.png)

パスワードはMicrosoftの認証画面で入力し、ZRdpには保存しません。
接続履歴とAgent設定は、Windowsユーザーごとに暗号化して保存します。

### 3. Agentの接続先を登録する

「Agent設定」を開き、SVN／TracサーバーのIPアドレス、ポート、用途を入力して「登録」を押します。
不要になった宛先は、一覧から選んで「削除」できます。どちらも自動保存です。

![Agent設定](docs/images/agent-settings.png)

設定はRDP接続先・ドメイン・ユーザー名の組み合わせごとに保存されます。
接続先を切り替えると、その接続に登録した宛先だけが表示されます。
登録は業務利用の承認を兼ねるため、Tracを含め、利用が許可された宛先だけを追加してください。

一件も登録されていない場合、Agentは起動しません。RDP画面はそのまま利用できます。
詳しくは [Agent設定と自動管理](docs/agent-settings.md) をご覧ください。

### 4. RDPで接続する

「新しい接続」を押して認証すると、ClientがAgentを配置・更新して起動します。
接続先の配置場所は `%LOCALAPPDATA%\ZRdp` です。通常は手動コピーや起動操作は必要ありません。

RDP画面を前面にしたまま、タイトルに `Agent:接続済み` と `Proxy:利用可 127.0.0.1:<port>` が出るのを待ってください。
Windowsの警告が出た場合は、内容を確認して対応してください。警告は自動承認しません。

> [!CAUTION]
> 自動起動中は、入力やウィンドウの切り替えを控えてください。「ファイル名を指定して実行」を自動操作するため、切り替えと重なると手元の別の画面へ貼り付け・実行されるリスクがあります。
> クリップボードの内容も置き換わり、元の内容は復元されません。

### 5. SVN／TracのProxyを設定する

対象のSVN／Tracクライアントに、次のProxyを指定します。既存の `http://<IPv4>[:port]/...` のURLは変更しません。

| 項目 | 値 |
|---|---|
| Proxyホスト | `127.0.0.1` |
| Proxyポート | RDPウィンドウのタイトルに表示されたポート |
| Proxy認証 | 使用しない |

最初の接続は既定で `18080`、追加接続には `18081`、`18082`…の空き枠を割り当てます。
SVN／Tracの認証方法も従来どおりです。
JavaHLやMylynでの設定例は [利用手順](docs/user-guide.md#8-javahl-svnのproxy設定) にまとめています。
Eclipse内蔵のEdgeブラウザーでTracを開く場合は、[ブラウザー側のProxy指定](docs/user-guide.md#91-eclipse内蔵ブラウザーでのweb表示) も必要です。

## 🖥️ 普段の使い方

設定済みの接続は、履歴から選んでRDP接続するだけです。
Agentとの通信が途絶えたときは、自動で復旧を試みます。繰り返し失敗した場合は停止します。

接続先のタスクトレイアイコンから、Agentの状態確認と終了ができます。

![Agentのタスクトレイメニュー](docs/images/agent-tray.png)

トレイから終了しても、RDP接続中は自動復旧の対象になります。
停止したままにしたいときは、RDP接続を閉じるか、その接続のAgent設定をすべて削除してください。

Agent設定を変更すると、新しい設定でAgentが再起動します。
通信中の処理はいったん終了するので、SVN／Tracの操作が終わってから変更してください。

### 画面の表示を選ぶ

| 表示モード | 動き |
|---|---|
| 等倍表示（スクロール） | 接続先の解像度を維持。既定の表示 |
| Smart Sizing（縮小表示） | 解像度を変えず、画面を拡大・縮小して表示 |
| 動的解像度（Window追従） | ウィンドウの大きさに合わせて、接続先の解像度を変更 |

表示モードは接続後も切り替えられます。
全画面ボタンまたは `Ctrl+Alt+Break` で全画面にし、上部の接続バーから元の表示に戻せます。
マイク転送と音声の再生先は、接続前に選んでください。

## 🔄 更新について

Clientの起動時と、その後24時間ごとにGitHub Releaseを確認します。
更新するかどうかは選べます。取得するのは、同じバージョンのClientとAgentのセットです。
適用前に設定画面とすべてのRDP接続を閉じ、更新後にClientを再起動します。
接続先のAgentは、設定が登録されていれば次のRDP接続時に更新されます。

自動更新には、現在の二つのEXEと更新候補の有効な署名・同じ署名者などの検証が必要です。
未署名の自前ビルドは、ClientとAgentを手動で揃えて更新してください。
現在はPrivateリポジトリのため、自動確認からReleaseを取得できません。

更新確認を止めたい場合は、Clientの横に次の `client.ini` を置きます。

```ini
[update]
enabled=false
```

Proxyの基準ポートも `client.ini` で変更できます。
詳しくは [セット更新](docs/client-updates.md) と [設定仕様](docs/configuration.md) をご覧ください。

## 🛡️ 対応範囲と気をつけたいこと

- 中継できるのは、登録したIPv4アドレス・ポートに一致するHTTP方式のSVN／Tracです。HTTPS、ホスト名指定、汎用TCP／UDP転送には対応していません。
- IP・ポートの登録は、リポジトリごとのアクセス権の代わりにはなりません。
- Zscaler、Firewall、GPO、RDP認証、OSのProxy設定は変更・回避しません。
- パスワード、Cookie、HTTP本文、ソースや差分を独自に保存・ログ出力しません。
- AgentやProxyに問題があっても、RDP画面は可能な限り維持します。HTTP通信を別ポートや直接接続へ自動で切り替えることはありません。
- 切断した通信の途中再開やCommitの自動再実行はしません。操作中に切断した場合は、サーバー側で結果を確認してください。

利用環境でのSVN／Mylynの動作確認もお願いします。
詳しい対応範囲は [現行仕様](docs/current-specification.md)、注意点は [セキュリティ](docs/security.md) と [既知の制約](docs/known-limitations.md) にまとめています。

## 🛠️ 困ったとき

| 表示・状況 | まず確認すること |
|---|---|
| `Agent:要設定` | 選んだ接続のAgent設定に宛先が登録されているか |
| Agentが接続済みにならない | RDP画面が前面か、Windowsの警告が出ていないか、転送・実行が許可されているか |
| 設定変更後に通信できない | Agentの再接続を待ち、SVN／TracのProxyポートを確認 |
| Proxyポートが使用中 | 別のClientやアプリが同じポートを使っていないか |
| 更新確認に失敗する | 現在はPrivateのため取得不可。RDPの利用は続けられる |

不具合に気づいたら、バージョン、発生時刻、行った操作、エラーコードを添えてお知らせください。
接続先・ユーザー名・URL・認証情報や、実環境の画面画像・ダンプは、公開IssueやPRに載せないでください。
ログに機密情報が含まれていないかも、確認をお願いします。
ログの場所と内容は [診断ログ](docs/diagnostics.md)、脆弱性の報告方法は [SECURITY.md](SECURITY.md) をご覧ください。

## 🔧 ソースからビルドする

Windows x64、Visual Studio 2022のC++開発ツールとATL、Windows SDK、CMake 3.31以上を用意します。
リポジトリのルートで、次を実行してください。

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug
ctest --preset debug
cmake --build --preset release
ctest --preset release
```

Releaseの二つのEXEは `out/` に出力されます。ビルド前に実行中のClient／Agentを終了し、必要な旧EXEは退避してください。
ビルドだけでは署名されません。RDP ActiveXはWindowsにインストール済みのものを使い、DLLは同梱しません。

Debug／Release各47件のテストと、Windows x64の [GitHub Actions](https://github.com/EnterOgawa/zrdp-svn-bridge/actions/runs/38095003422) で検証しています。
実機確認の範囲や開発手順は [テスト計画](docs/test-plan.md) と [コーディング規約](docs/coding-standard.md) をご覧ください。

### リポジトリ構成

| 場所 | 内容 |
|---|---|
| `src/`、`tests/` | Client・Agent・共通処理とテスト |
| `assets/`、`cmake/` | アイコンとビルド補助 |
| `config/examples/` | 設定例 |
| `docs/`、`docs/adr/` | 操作・設計・設計判断の記録 |
| `scripts/` | ソース検査と配布準備 |
| `out/`、`build/`、`work/` | ビルド結果・ローカル作業用。Git管理外 |

改善の提案やバグ報告も歓迎します。開発に参加する際は [貢献方法](CONTRIBUTING.md) をご覧ください。
実設定、署名鍵、EXE、PDB、ログ、ダンプはコミットしないでください。

## 📚 もう少し詳しく知りたい方へ

- [利用手順](docs/user-guide.md) ／ [Agent設定と自動管理](docs/agent-settings.md)
- [アーキテクチャ](docs/architecture.md) ／ [通信プロトコル](docs/protocol.md)
- [インストーラー](docs/installer.md) ／ [portable配布の準備](docs/portable-package.md) ／ [公開準備の状況](docs/oss-readiness.md)

## 📄 ライセンス

自作ソース・文書・画像のライセンスは [Apache License 2.0](LICENSE) です。
利用・改変・再配布の条件は [LICENSE](LICENSE) と [NOTICE](NOTICE)、依存物については [ライセンス一覧](docs/dependencies.md) をご覧ください。
Windows、RDP ActiveX、開発ツールには、それぞれの利用条件が適用されます。

ZRdpは、Zscalerとの提携・認定や公式製品であることを示す名称ではありません。
