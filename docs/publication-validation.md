# 公開前の実機確認と公開時の操作

対象は正式版 `0.7.21.0` です。通常のインストール、RDP／Agent動作、SVN接続、Trac Web表示は
2026-10-13に受入済みです。最終確認後にもユーザーが現行版の実機利用は全て問題なしと確認しました。
以下は、今後の変更時の回帰と公開後の受付・配布を確認する手順です。
結果は [公開チェックリスト](publication-checklist.md) に記録します。

## 実機確認の準備

利用が許可された検証用WindowsユーザーとRDP接続を使います。業務用のAgent設定を削除したり、
SVN／Tracの書込み操作中に切断したりしないでください。転送や実行が禁止されている環境では、
その制限を変更せずに停止することを確認します。

Agentの動作確認だけなら、接続設定には文書用の `192.0.2.10:80` を登録できます。
登録だけではHTTP接続は発生しません。ProxyへHTTP要求を送らず、実サーバーの探索や通信試験は行いません。
前面切替の試験は、未保存の作業や機密情報を表示していない検証PCで行います。

## Agentの確認項目

| ID | 利用者が行う操作 | 確認する結果 | 関連する隔離回帰 |
|---|---|---|---|
| R01 | 検証用の接続で登録を0件にし、RDP接続 | `Agent:要設定`、配置・起動なし。RDP操作は可能 | 設定model／window、bootstrap guard |
| R02 | 宛先を登録し、未配置の検証ユーザーへ接続 | 独自確認dialogなしで配置・tray起動。`Agent:接続済み`、`Proxy:利用可` | Agent setup、bootstrap UI、設定ACK前のstream拒否 |
| R03 | 検証用の既存Agent／INIを、同版二EXEの配置で更新 | 異なる二fileをbackupして反映。使用中fileは協調停止し、停止できなければ変更しない | Agent setupのbackup・file lock・reparse拒否 |
| R04 | 安定疎通後にAgentをtrayから終了 | 自動復旧し、RDPを維持。繰返し失敗時は3回で停止 | 仮想時刻のbackoff・45秒期限・再起動状態、Bridge死活監視 |
| R05 | 検証用の宛先を登録・削除し、最後に0件へ変更 | 保存後だけ反映。設定変更で再起動、全削除で協調停止。旧streamは再開しない | 設定service／window、Client／Agentの設定ACK・停止control |
| R06 | 起動の待機中に別の無害な画面へ切替え、またはclipboardへ架空textをcopy | 条件失効後の残りの入力を停止。再送せず、RDPを維持 | 固定入力guard・各待機段階のclipboard世代・部分受付・送信直前の再確認 |
| R07 | すでに転送／実行が禁止された検証環境、またはAgent応答が得られない状態で接続 | 別経路へfallbackせず停止。無応答は45秒期限と有限復旧、Windows警告は自動承認しない | drive拒否、queue／入力失敗、仮想時刻timeout |
| R08 | 許可された二つの検証sessionを開き、一方だけを終了 | 設定・Proxy port・入力の分離。他方とmanagerは存続 | port allocator、専用STA smoke、session controls |
| R09 | 公式配布先とは別のfolderで自前未署名二EXEを使う | 通常起動・配置は可能。署名必須の自動更新だけ拒否 | unsigned setup許可、更新署名拒否、metadata・混在版拒否 |

R06の合格でも、SendInput直前確認と前面切替の競合を完全には排除できません。
自動起動中はRDP画面を前面にし、入力や画面切替を控えてください。clipboardは復元しません。
異常を確認したら試験を止め、エラーコードと操作順だけを記録します。

## インストーラーの確認項目

接続履歴の削除試験は検証専用Windowsユーザーで行います。

1. 通常Setupを導入し、スタートメニュー、任意のデスクトップshortcut、アプリ一覧から製品を識別できることを確認する。
2. アンインストールを開始して取り消し、アプリ・接続履歴・Agent設定が残ることを確認する。
3. 削除を選ばずアンインストールし、接続履歴とAgent設定が残ることを確認する。
4. 再導入後に「接続履歴・Agent設定も削除する」を選び、この検証ユーザーの二fileだけが削除されることを確認する。
   ログ、backup、手動INI、Windows資格情報、接続先Agentは残ることも確認する。

`Test-Installer.ps1` の隔離test modeは保持・選択削除・取消・file lock・read-only・link拒否を検証します。
通常画面での選択操作とは別の結果として記録してください。

## 公開時の非公開脆弱性報告

この操作は、Public化の別の明示承認とvisibility変更の後に行います。
Privateのまま受付済みと表示したり、受付のためだけにPublicへ変更したりしません。

1. Settings → Security and quality → Advanced Securityで **Private vulnerability reporting** を有効化する。
2. Security → Advisoriesに **Report a vulnerability** があることと、
   [報告フォーム](https://github.com/EnterOgawa/zrdp-svn-bridge/security/advisories/new) が開くことを確認する。
3. 保守者がrepositoryの **Security alerts** 通知を購読し、通知を受け取れる設定になっていることを確認する。
4. 受付・通知設定の確認結果をチェックリストへ記録してから、SECURITY.mdの案内を受付済みの状態へ更新する。
   実際の通知到達は、保守者が確認した場合だけ完了とする。試験報告の送信は別途承認を得て行う。

APIでの有効化・読戻しは、Publicになっていることを確認してから次の固定endpointで行えます。
visibility変更や認証tokenの作成・表示は、この手順には含めません。

```powershell
$repository = gh api repos/EnterOgawa/zrdp-svn-bridge | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $repository.visibility -ne 'public') {
    throw 'Public化の承認と変更が完了してから実行してください。'
}
gh api --method PUT repos/EnterOgawa/zrdp-svn-bridge/private-vulnerability-reporting
if ($LASTEXITCODE -ne 0) { throw '非公開報告の有効化に失敗しました。' }
$reporting = gh api repos/EnterOgawa/zrdp-svn-bridge/private-vulnerability-reporting | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $reporting.enabled -ne $true) {
    throw '非公開報告が有効になったことを確認できません。'
}
```

公式の [設定・通知手順](https://docs.github.com/en/code-security/how-tos/report-and-fix-vulnerabilities/configure-vulnerability-reporting/configure-for-a-repository) と
[有効化API](https://docs.github.com/en/rest/repos/repos#enable-private-vulnerability-reporting-for-a-repository) を参照してください。
APIの404やフォームへのリンクの存在だけでは、有効化成功とは扱いません。

## 匿名取得と次の新版への更新

Public化後、GitHubにログインしていない環境から最新版Releaseと6配布fileを取得し、
SHA256SUMS、format 2 manifest、Client／Agentの同版・同署名者・timestampを確認します。
旧三Release／assetが公開一覧と旧download URLに残っていないことも確認します。

`0.7.21.0` はセット更新の初回導入版です。現行版と同じ版への取得確認を、
新版への更新・再起動・rollbackの合格とは扱いません。次の署名済み正式版で以下を確認します。

- 更新を見送ると二EXEを取得しない。同意後も全RDP／設定画面と保存の終了を待つ。
- 新版の同版二EXEへ置換してClientを再起動し、次のRDP接続で接続先Agentへ反映する。
- 隔離した検証環境で置換・再起動失敗を確認し、旧セットとbackupが保持／復元される。
  本番配布物の改ざん、署名無効化、稼働processの強制終了で試験しない。

署名済み基準のWindows trust、未署名候補拒否、二fileの置換失敗・rollbackは、
実設定を使わない回帰で検証しています。ネットワークから新版を取得する一連の実機結果とは分けます。
詳細は [セット更新](client-updates.md) を参照してください。

## 記録する情報

各IDについて、版、確認日、合格／不合格／未確認、操作順、固定エラーコードを記録します。
接続先IP・domain・user・URL・INI・画面画像・payloadは公開記録へ含めません。
確認できない項目を、通常動作の承認やMockの合格だけで完了へ変更しないでください。
