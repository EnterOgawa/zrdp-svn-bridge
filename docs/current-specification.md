# ZRdp SVN Bridge 現行仕様

更新日: 2026-10-13。対象: 正式版 `0.7.21.0`。ユーザーの実機確認と正式版承認を受けた。
確認範囲と配布物は [リリースノート](release-notes-0.7.21.md) を参照する。リポジトリはPrivateを維持する。

## 構成とセキュリティ境界

Windows x64／C++20のWin32 Client `ZRdpSvnBridge.exe` とAgent `ZRdpSvnAgent.exe` を使用する。
ClientはWindowsにインストール済みのRDP ActiveXをホストし、既存RDP内のSVC `ESVNBRG` で中継する。
Client／Agentの二EXEは同Versionを開発PCの同じfolderへ置く。
Agentは接続先のactive RDPユーザーsession内で動くtrayアプリであり、サービスではない。

HTTPのIPv4 literal＋非0 TCP portだけを対象に、Agentの完全一致allowlistで接続を制限する。
SVNと明示登録されたTrac以外の汎用転送、CONNECT、HTTPS、hostname、DNS探索は行わない。
OS／Zscaler／Firewall／RDP認証／証明書警告／Proxy設定を変更・回避しない。

## Clientの所有関係

manager STAが接続入力・履歴・Agent設定・最大16件のsession記録とport slotを所有する。
各RDP sessionは専用COM STAとmessage loopでActiveX、event sink、Bridge、Proxyを独立所有する。
COM生成・呼出し・切断・購読解除・破棄は生成元STAへ限定する。
bounded通知だけをmanagerへ渡し、一session終了で別sessionを破棄しない。
詳細は [ADR 0005](adr/0005-dedicated-sta-per-rdp-session.md) と [architecture](architecture.md) を参照する。

Proxyは `127.0.0.1` のみ。基準portは既定18080、追加sessionには有限の空きslotを割り当てる。
一session最大8 HTTP stream。競合したportから別port・別経路へ自動fallbackしない。
Protocolのframe／queue／window上限とHTTP処理は [protocol](protocol.md)／[HTTP Proxy](http-proxy.md) を参照する。

## UIと保存

接続マネージャーに接続名・接続先・ドメイン・ユーザー名、表示mode、音声設定、
「新しい接続」「Agent設定」を置く。menu、手動更新、Agent起動button、入力テストはない。
sessionはtitle、3つの排他的表示modeと全画面操作、RDP画面で構成する。
等倍とSmart Sizingはremote解像度を維持し、動的解像度だけwindowへ追従する。
マイク転送は既定OFF、音声設定は接続時に確定する。

認証済み履歴は最大10組、接続名は64文字。passwordを持たない。
履歴形式3とAgent設定形式2はcurrent-user DPAPIで暗号化して `%LOCALAPPDATA%\ZRdpSvnBridge` へ保存する。
Agent設定は接続先＋domain＋user単位。IP・port・用途の登録／削除を自動保存する。
登録は業務利用承認でありTracも含む。一覧には選択接続の登録済み宛先だけを表示する。
旧未関連付け設定・未承認候補は推定継承しない。

## Agentの自動管理

登録0件ではAgentを起動・復旧せず、titleで要設定を案内する。設定画面は自動表示しない。
登録済み設定がある接続では認証後、前面・focus・入力のidleを確認し、専用RDP driveから
固定Agentの `--rdp-setup` だけを起動する。一般drive、dynamic drive、shell、任意commandは使わない。
Client独自の起動dialogは表示しない。Windows警告は利用者が確認する。

接続先は `%LOCALAPPDATA%\ZRdp` 固定。未配置はcreate-new、同じ二fileは再利用、
異なる二fileはuser限定backupを保全して更新する。link、使用中file、検証／backup失敗は拒否する。
新Agent同士は同じsessionで協調停止する。旧Agentの初回更新では利用者の停止が必要な場合がある。

疎通断は同じ認証世代だけを有限復旧し、5～60秒のbackoff、45秒の応答期限、連続三失敗で停止する。
正常疎通30秒後だけ失敗数を戻す。途中失効・部分入力・設定拒否・Protocol異常は再送しない。
設定変更は既存HTTP streamを閉じ、限定SVC controlでINIをbackup付きatomic更新して協調再起動する。
最新INIのACK前にProxyを利用可能にしない。全削除は協調停止する。

通常起動・配置にAuthenticode署名を必須としない。自前の未署名buildも同じ経路を使う。
metadata、同版、file安全性は検証するが、metadataは信頼性・改ざん防止の証明ではない。
詳細は [Agent設定](agent-settings.md)、ADR [0021](adr/0021-optional-agent-signing.md)／[0022](adr/0022-connection-agent-lifecycle.md) を参照する。

## 更新

Client起動時と24時間周期で匿名のGitHub Release確認を行う。`client.ini` の
`[update] enabled=false` で無効化できる。現在のPrivate repositoryは匿名取得できない。
同意後に同じtag・VersionのClient／Agentセットを取得し、署名・timestamp・同一公開鍵・hash・metadataを検証する。
全RDP／設定Windowを閉じ、保存完了後に適用・再起動する。接続先Agentは次回認証後に更新する。
未署名buildはこの自動更新経路を使わず、二EXEを手動で揃える。
二file同時atomic性、電源断時の完全自動復旧は保証しない。
[セット更新](client-updates.md) と [ADR 0023](adr/0023-paired-client-agent-updates.md) を参照する。

## インストールとアンインストール

Inno Setupのユーザー別Setupで、同版Client／Agentとlicense文書を開発PCへ配置できる。
既定は `%LOCALAPPDATA%\Programs\ZRdp SVN Bridge`。管理者権限は不要で、portable配置も維持する。
アンインストールでは、接続履歴とAgent設定の削除を選択できる。既定は保持で、取消では変更しない。
削除対象はcurrent-userの `connection-history.bin` と `agent-settings.bin` の二fileだけ。
同じWindowsユーザーの全接続分が対象となり、ログ・backup・手動INI・Windows資格情報と接続先Agentは残す。
使用中、read-only、link、確認不能では選択削除を拒否し、フォルダー再帰削除はしない。
詳細と実機Gateは [インストーラー](installer.md) を参照する。

## 残存リスクと確認範囲

- SendInputは接続先を直接指定しない。直前確認後の前面切替との競合で、ローカル側へ誤入力・誤実行するリスクが残る。
- 起動処理はclipboardを置換し、元の内容を復元しない。部分受付のキー残留は手動解放が必要な場合がある。
- Agentのtray終了だけでは、Clientの自動復旧を止めない。意図して止める場合は設定を全削除するかRDPを切断する。
- 設定変更・切断時のHTTP streamを途中再開せず、Commit等を自動再実行しない。結果はserver側で確認する。
- 同じWindowsユーザー／管理者や、process全体のcrashに対する完全隔離は提供しない。
- 通常のRDP／Agent動作、SetupのインストールとTrac Web表示はユーザー実機で受入済み。
  前景競合、policy拒否、全session組合せ、署名済み新版へのセット更新とrollbackは別の確認範囲であり、Mock合格から推定しない。

旧仕様 [specification](specification.md) と [過去受入・制約](known-limitations.md) は歴史記録。
操作は [README](../README.md)／[user-guide](user-guide.md)、公開Gateは [publication-checklist](publication-checklist.md) を正本とする。
