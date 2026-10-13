# ZRdp SVN Bridge 0.7.21.0 リリースノート

リリース日: 2026-10-13。Windows x64向け正式版です。作成時点ではPrivate repositoryで配布しました。

Agentの配置と起動をRDP接続にまとめ、接続ごとの設定で普段の操作を簡素化しました。
ClientとAgentをまとめて導入できる署名済みSetupとportable ZIPを用意しています。

## 主な変更

- RDP認証後にAgentを自動配置・更新・起動し、疎通断では有限の再起動を試みます。
- Agentは接続先のタスクトレイで動作し、状態の確認と終了ができます。
- Agent設定をRDP接続先・ドメイン・ユーザーの組ごとに保存します。IP・ポート・用途の登録と削除は自動保存です。
- Agent設定が空なら起動せず、設定変更後は通信を閉じて新設定で再起動します。
- マネージャーのメニュー、手動Agent起動の導線、開発用入力テストを廃止しました。
- 起動時と24時間周期で更新を確認し、同意後に同版のClient／Agentをセットで取得・検証します。
- Inno Setupのユーザー別インストーラーを追加しました。アンインストール時は接続履歴・Agent設定の保持／削除を選べます。
- Apache-2.0のLICENSE、NOTICE、依存物一覧、利用手順、Windows x64のMock回帰CIを整備しました。

## インストールと更新

初めての導入には `ZRdp_SVN_Bridge_Setup_0.7.21.0.exe` を実行してください。
既定の配置先は `%LOCALAPPDATA%\Programs\ZRdp SVN Bridge` で、管理者権限は不要です。
portable版ではZIPを展開し、同じフォルダーの `ZRdpSvnBridge.exe` を起動します。

更新・アンインストールの前には、ClientとすべてのRDP画面を終了してください。
接続情報は既定で保持します。削除を選ぶと、このWindowsユーザーの全接続分が対象になります。
ログ、backup、手動INI、Windows資格情報と接続先PCのAgentは削除しません。

旧 `0.7.20.0` からは初回手動導入が必要です。登録したAgent設定に従い、次のRDP認証後に接続先へ反映します。
非公開の取得元では匿名の自動更新取得はできません。署名済み新版の取得・置換・rollbackを通した実機受入は、次の新版で確認します。

## SVNとTrac

登録はIPv4とHTTPポートの完全一致です。承認済みSVNとTracが同じIP・ポートなら、一件の登録で両方を利用できます。
用途は記録・表示用で、URLや中継動作を切り替えるものではありません。

Eclipse内蔵のEdge／WebView2には、MylynやEclipseのProxy設定がそのまま適用されません。
Web表示だけ失敗する場合の確認方法は [利用手順](https://github.com/EnterOgawa/zrdp-svn-bridge/blob/v0.7.21.0/docs/user-guide.md#91-eclipse内蔵ブラウザーでのweb表示) を参照してください。
HTTP、登録済みIPv4・ポートだけを対象とし、HTTPS、CONNECT、hostname、未承認宛先は中継しません。

## 検証と確認範囲

- Debug／隔離ReleaseのCTestは各47件成功しました。配布用の署名済みEXEは再ビルド・再署名していません。
- Python self-testは48件成功し、repositoryの実設定・秘密情報・リンク・license境界を機械検査しました。
- Setupの保持・削除・取消・lock・read-only・link拒否等は、実接続情報を使わない隔離回帰で確認しました。
- Client／Agent／Setupとcompilerが保持した埋込み用アンインストーラーは、Windows trust `Valid`、timestampと同一署名者を確認しました。
- ユーザー実機で通常Setupのインストール、RDP／Agentの期待した動作、SVN接続、Trac Web表示を確認し、正式版として承認を受けました。

最終監査後の対応では、インストール済みClient／Agent／アンインストーラーの版・有効な署名・timestamp・同一署名者も確認しました。
最終確認後にも、ユーザーが現行版の実機利用は全て問題なしと確認しました。
故障注入・入力競合の境界は関連回帰の結果として区別し、次の署名済み新版へのセット更新は次版で実機確認します。
Setupのマウス操作が進まなかった事象は、同じSetupのままPC再起動後に解消しました。Windows／driverの具体的な原因は未特定です。

## 配布物

- `ZRdp_SVN_Bridge_Setup_0.7.21.0.exe`: 署名済みインストーラー。
- `ZRdp_SVN_Bridge_0.7.21.0.zip`: 同版Client／Agent、LICENSE、NOTICE、依存物一覧とformat 2更新manifestを含むportable版。
- `ZRdpSvnBridge.exe`、`ZRdpSvnAgent.exe`、`zrdp-client-update.ini`: 同じRelease tagのセット更新用asset。
- `SHA256SUMS.txt`: 配布fileのSHA-256一覧。

実INI、履歴、credential、鍵、ログ、dump、PDB、実環境の画像は含めません。
現行の公開対象と確認状況は [公開チェックリスト](publication-checklist.md) を参照してください。

| File | byte数 | SHA-256 |
|---|---:|---|
| `ZRdpSvnBridge.exe` | 971880 | `FDB9C14B122B4B38A23D24DDC294702B8BE93E4272EF0CAA95D11999CA53205B` |
| `ZRdpSvnAgent.exe` | 499816 | `0DF736BE19DCCA2DB449874E663E53EEC87ED9F3D5C2D70CDE2C3B080A4C52A2` |
| `ZRdp_SVN_Bridge_Setup_0.7.21.0.exe` | 2636440 | `DA6E36770C95E4E0F9BFE7010FC9A011BE1A1FB7DD0B87A9000A923B1EEAC520` |

## 利用時の注意

Agent自動起動中はRDP画面を前面にし、入力や画面切替を控えてください。
固定SendInputと前面切替が競合すると、手元の別画面へ入力・実行されるリスクが残ります。
クリップボードは置き換わり、元の内容は復元しません。Windows警告は自動承認しません。

Agent設定の変更や切断では実行中のHTTP streamが終了します。Commit等を自動再実行せず、結果は利用先で確認してください。
セキュリティ境界とその他の制約は [現行仕様](https://github.com/EnterOgawa/zrdp-svn-bridge/blob/v0.7.21.0/docs/current-specification.md) を参照してください。
