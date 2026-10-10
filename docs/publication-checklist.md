# 公開・配布Gate

更新日: 2026-10-11。対象: 公開準備版 `0.7.21.0`。
チェック済みは証拠を伴うものだけとし、文書作成と運用上の有効化を分ける。
このfileはRelease作成・Public化・旧配布物削除の許可ではない。

## 合意済みの方針

- [x] ユーザーが自作コード・文書・画像の権利帰属と組織としてのApache-2.0許諾を確認済み（2026-10-11の回答）。
- [x] 旧三Releaseの配布物は保全し、将来の公開対象から外す。
- [x] Git履歴のauthor／committer identityと過去の設計・Word文書は、現状の履歴を公開する方針でユーザー確認済み（2026-10-11）。
- [x] 脆弱性はPublic化時にGitHubの非公開報告機能を有効にする。
- [x] GitHub Actionsはread-only source取得・Windows x64／Mock検証のみ、実設定・鍵・実接続・成果物uploadなしの設計で承認済み。
- [x] 署名、Release作成、tag・asset操作、visibility変更は今回行わない。

## ソース・文書・build

- [x] LICENSE、NOTICE、依存物inventory、SECURITY、CONTRIBUTING、Issue／PR templateを整備。
- [x] STA方針を現行実装とADR0005に同期し、旧仕様・ADR・受入記録を歴史記録として区分。
- [x] 開発者固有tool pathを標準presetから除き、インストール済みVS2022を自動選択する。
- [x] 新規cloneでも `BUILD_TESTING=ON` のconfigureで隔離fixture基点だけを作る。
- [x] 公開tree検査、配布準備・同梱file検査scriptを用意。
- [x] ローカルDebug／Release各47 CTest、Python self-test 41件、整形・コメント監査、同梱file／hash・拒否経路を確認。
- [ ] 承認されたCIをGitHub上で実行し、結果を確認。
- [x] 到達可能な履歴を機械走査し、commit identity・過去文書の公開方針をユーザー確認。走査の限界は監査書に明記。
- [ ] Public化直前の最新tree・履歴・公開対象の最終再確認。
- [ ] 別Windows環境での再現性確認（CI結果とは別に、必要な利用環境を確認）。

## 旧Releaseの保全と除外

対象は `v0.7.14.0`、`v0.7.15.0`、`v0.7.20.0` のReleaseと全asset。
これらのZIPはLICENSE未同梱であり、現行と配置・更新の操作も異なる。
新しいpackageへ流用しない。

- [x] 三Releaseの全14assetとmetadata、size／SHA-256、tag・commit対応をGit管理外の `work/archives/legacy-release-20261011_074541` へ保全。全fileとreceiptを再検証。
- [ ] 保全receiptと実fileの対応を再確認し、長期保管先へ別copyを保持。
- [ ] Public化前に、対象旧Release／assetを公開対象から外す操作についてユーザーの実行承認を得る。
- [ ] 承認された対象だけを処理し、公開予定repositoryに旧assetが残っていないことを確認。

今回の回答は除外**方針**の合意であり、旧Release削除・差替えの実行承認とは扱わない。
Releaseの非表示だけでassetや履歴の公開範囲を保証せず、操作後のAPIとUIを確認する。
tag／Git履歴の扱いは別確認する。無断rewrite・force push・tag削除は行わない。
復旧は保全したmetadataと全assetに基づく別の承認作業とする。

## 非公開報告機能（Public化時の運用Gate）

- [x] SECURITY.mdとGitHub非公開report採用方針を整備。
- [ ] Public化の承認を得た時点で、対応するGitHub private reportingを有効化。
- [ ] 「Report a vulnerability」の受付・maintainerの通知設定を確認。
- [ ] 動作確認が済むまで、受付可能・通知済みと表示しない。

Private状態では有効なpublic reporting窓口と見なさない。
APIの404は有効化成功ではない。公開前の利用者は既存の非公開連絡経路を使う。

## 署名を使わない実機受入

次は**未実施**。利用者が許可したRDP sessionだけで行う。
実endpoint・user・画面・INIをrepository、CI、報告templateへ転載しない。

| ID | 操作 | 確認する結果 | 結果 |
|---|---|---|---|
| R01 | 設定なしで接続 | Agentを起動せず要設定、RDPは利用可 | 未実施 |
| R02 | 初回の設定あり接続 | dialogなしで配置・tray起動、最新設定ACK後にProxy可 | 未実施 |
| R03 | 旧／新Agentの更新 | 旧二fileをbackup、同Version二fileへ更新、強制killなし | 未実施 |
| R04 | tray終了・疎通断 | 有限backoffで復旧、3失敗で停止、RDP維持 | 未実施 |
| R05 | 登録変更・全削除 | 保存成功後だけ反映、旧stream終了、新設定ACK／協調停止 | 未実施 |
| R06 | 前面切替・clipboard変更 | 残りの入力停止、再送なし、RDP維持 | 未実施 |
| R07 | 転送／実行policy拒否・無応答 | 別経路なし、45秒の応答期限、固定診断 | 未実施 |
| R08 | 複数sessionと一件終了 | 設定・Proxy・入力の分離、他sessionとmanager維持 | 未実施 |
| R09 | unsigned自前build | 通常起動・配置可、自動更新だけ拒否 | 未実施 |

SendInputの前景競合・誤実行リスク、clipboard非復元は [現行仕様](current-specification.md) に明記する。
以前のbutton起動合格をこの表へ自動転記しない。

## 後日の署名・正式Release

- [x] 過去配布物と区別する開発Versionを `0.7.21.0` に更新（正式Release未作成）。
- [ ] Client／Agentを同版で署名し、Windows trust・timestamp・hash・metadataを確認。
- [ ] 検証済み旧署名セットからの同意付き更新、rollback、接続先反映を実機確認。
- [ ] signed packageにLICENSE／NOTICE／inventory・同版二EXE・format 2 manifestを同梱し、検査。
- [ ] README・公開file・配布fileをユーザーが最終確認。
- [ ] ユーザーの別の明示指示でRelease／tag／assetを作成する。
- [ ] 署名以外の公開・実機Gateも完了した後、別の明示指示でPublicへ変更する。

未署名development packageを公式Releaseへ流用しない。archive作成scriptはGitHubを操作しない。
署名・remote操作を自動化する権限を、このチェックリストの存在から推定しない。
