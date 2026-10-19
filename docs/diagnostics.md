# ZRdp SVN Bridge 診断ログ

接続やAgent起動がうまく進まない場合は、ウィンドウの状態表示と診断ログを確認してください。
ログには固定イベント名、状態、件数、エラーコードを記録し、接続先や通信内容は記録しません。

## 保存場所

まず、各EXEの横にある `logs` フォルダーを確認します。

```text
Client: logs\ZRdpSvnBridge.log
Agent:  logs\ZRdpSvnAgent.log
```

EXEの横へ書き込めない場合は、次の場所を使います。

```text
%LOCALAPPDATA%\ZRdpSvnBridge\logs\
```

ClientとAgentは別ファイルです。現在のログは最大1 MiB、バックアップは `.1`～`.3` の3世代です。
ログのキュー上限を超えた場合は、`log.records_dropped` で失われた件数だけを記録します。

## 読み方

各行はUTC時刻を持つkey-value形式です。`event` と `hresult`、`code`、`reason` を確認します。
`session` はプロセス起動ごとの匿名IDで、実RDP接続先ではありません。
複数RDP接続の診断では、タイトルに表示されたProxyの `port` と発生時刻を対応させます。

| イベント | 確認すること |
|---|---|
| `rdp.connect_call_returned` | 接続開始APIの結果。成功でも認証完了とは限らない |
| `rdp.event.login_complete` | RDPログインの完了 |
| `rdp.event.disconnected` | 切断理由、拡張理由、利用者取消の分類 |
| `bridge.ready` | Client／Agentの通信準備 |
| `bridge.peer_timeout` | Agentの死活応答の期限到達 |
| `proxy.client_state_changed` | 待受状態、割当ポート、利用可否 |
| `agent.allowlist_load_returned` | 設定の採用件数と不正な設定の分類 |
| `ui.connection_history_load_returned`／`ui.connection_history_save_returned` | 履歴の読込み・保存結果と件数 |

ログのエラーコードだけで、Windows、転送ポリシー、フォーカス、Agent起動失敗のどれが原因かを
断定できるとは限りません。操作順と状態表示も一緒に確認してください。

## Agent自動起動の停止

Clientの `agent_deployment.input_returned` と `agent_deployment.stopped` は、
固定入力の段階、停止分類、元のHRESULTを記録します。
独自の起動・失敗ダイアログは表示しないため、タイトルのAgent状態も確認します。

| code | 段階 |
|---:|---|
| 1 | owner STAへの要求 |
| 2 | 認証世代、専用ドライブ、Bridge条件 |
| 3 | 固定起動文字列のクリップボード公開 |
| 4 | Win＋R送信 |
| 5 | 入力タイマー |
| 6 | Ctrl＋V前の確認と送信 |
| 7 | Enter前の確認と送信 |
| 8 | Agent応答待ち |
| 9 | 入力フォーカス・Windowsキー転送の準備 |

よく確認する停止分類は `reason=5` のクリップボード世代不一致、
`10` の45秒無応答、`13` の入力期限到達、`14` の入力準備失敗、`15` の部分受付です。
固定入力の受付成功は、配置やAgent起動の成功そのものではありません。

`rdp.deployment_run_input_returned`、`rdp.deployment_paste_input_returned`、
`rdp.agent_start_input_returned` は、SendInputの受付数を `code` に記録します。
Win＋R／Ctrl＋Vは4、Enterは2が全件受付です。この `code` は上の段階番号とは別の意味です。
部分受付では再送せず、キーが押下のまま残った場合は該当キーを手動で押して離してください。

接続先のWindows警告を確認し、利用を許可されたドライブ転送・EXE実行が使えるかを確認します。
ポリシー変更や別経路への切替で回避しないでください。
詳しい動作は [Agent設定](agent-settings.md) をご覧ください。

## 不具合を報告する

版、発生日時、操作順、固定エラーコード、他のRDPウィンドウや接続マネージャーが
応答しているかを添えてください。ログを共有する前に内容を確認してください。

接続先名・IP、ユーザー名、URL、認証情報、HTTP本文、ソース・差分、ticket内容、
クリップボード内容、実画面やダンプは公開Issueへ載せないでください。
脆弱性の報告は [SECURITY.md](../SECURITY.md) の非公開窓口を利用します。
