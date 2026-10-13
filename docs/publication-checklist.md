# 公開・配布Gate

更新日: 2026-10-13。対象: 正式版 `0.7.21.0`。旧三Releaseの除外は完了。Public化は未承認。
チェック済みは証拠を伴うものだけとし、文書作成と運用上の有効化を分ける。
このfileはRelease作成・Public化・旧配布物削除の許可ではない。

## 合意済みの方針

- [x] ユーザーが自作コード・文書・画像の権利帰属と組織としてのApache-2.0許諾を確認済み（2026-10-11の回答）。
- [x] 旧三Releaseの配布物は保全し、将来の公開対象から外す。
- [x] Git履歴のauthor／committer identityと過去の設計・Word文書は、現状の履歴を公開する方針でユーザー確認済み（2026-10-11）。
- [x] 脆弱性はPublic化時にGitHubの非公開報告機能を有効にする。
- [x] GitHub Actionsはread-only source取得・Windows x64／Mock検証のみ、実設定・鍵・実接続・成果物uploadなしの設計で承認済み。
- [x] 2026-10-11の準備作業では署名、Release作成、tag・asset操作、visibility変更を行わなかった。
- [x] 2026-10-13に署名済み版の実機受入と正式版承認を得て、commit・push・Release作成の明示指示を受けた。
      当該Release作成時はPrivateを維持し、旧Releaseとvisibilityを変更しなかった。
- [x] 最終監査の四点の修正指示を受け、旧三Release／全14assetの削除を実施。
      ユーザーは旧版の今後の利用と追加保全は不要と回答。既存ローカル保全は残し、タグ・履歴と最新版を変更しない。

## ソース・文書・build

- [x] LICENSE、NOTICE、依存物inventory、SECURITY、CONTRIBUTING、Issue／PR templateを整備。
- [x] STA方針を現行実装とADR0005に同期し、旧仕様・ADR・受入記録を歴史記録として区分。
- [x] 開発者固有tool pathを標準presetから除き、インストール済みVS2022を自動選択する。
- [x] 新規cloneでも `BUILD_TESTING=ON` のconfigureで隔離fixture基点だけを作る。
- [x] 公開tree検査、配布準備・同梱file検査scriptを用意。
- [x] ローカルDebug／Release各47 CTest、Python self-test 48件、整形・コメント監査、同梱file／hash・拒否経路を確認。四点の対応時にも隔離Release47件とPython48件を再実行して成功。
- [x] 承認されたCIをGitHub上で実行。正式版commit `120e178` のDebug／Release各47件、Release smoke20反復、script・同梱検査が成功（[実行記録](https://github.com/EnterOgawa/zrdp-svn-bridge/actions/runs/38342932101)）。文書修正後のcommit・push・CI結果は別途確認する。
- [x] 到達可能な履歴を機械走査し、commit identity・過去文書の公開方針をユーザー確認。走査の限界は監査書に明記。
- [x] 正式版 `120e178` の242file・18commit・582 distinct blob、Word metadata／media、全4 Actions log、最新6assetを最終再確認。既知形式の鍵・tokenや実endpointの混入を検出せず。
- [ ] 今回の文書修正をGitHubへ反映した後、Public化直前の最終tree／CI／公開対象を再確認。
- [x] GitHubの新規Windows Server 2022環境で、cloneから標準presetのbuild・全回帰が成功。
- [x] 現行正式版の実機受入を確認。2026-10-13にユーザーが「実機での利用では全て問題ないことが確認できている」と回答。

## 旧Releaseの保全と除外

対象は `v0.7.14.0`、`v0.7.15.0`、`v0.7.20.0` のReleaseと全asset。
これらのZIPはLICENSE未同梱であり、現行と配置・更新の操作も異なる。
新しいpackageへ流用しない。

- [x] 三Releaseの全14assetとmetadata、size／SHA-256、tag・commit対応をGit管理外の `work/archives/legacy-release-20261011_074541` へ保全。全fileとreceiptを再検証。
- [x] 全14fileのsize／hashをreceiptと現行GitHub digestへ再照合。別媒体への追加保全はユーザーの「今後利用しない・保全不要」の回答により行わない。
- [x] 最終監査の四点を修正する指示に基づき、対象旧Release／assetの除外を実施。
- [x] 全14assetと旧三Releaseを削除し、各APIがHTTP 404、Release一覧が `v0.7.21.0` 一件だけであることを確認。最新版の6assetと旧タグ・Git履歴は保持。
- [ ] Public化時に、未ログインのRelease一覧・旧download URLでも除外を確認。今回のbrowser接続は利用できず、UI確認は未実施。

2026-10-11の回答は除外方針の合意であり、削除は2026-10-13の追加指示後に実施した。
既存のローカル保全は削除していない。復旧はmetadataと全assetに基づく別の承認作業となり、
GitHub上のRelease／asset IDをそのまま復元するものではない。
tag／Git履歴は承認済みの公開対象として保持する。無断rewrite・force push・tag削除は行わない。

## 非公開報告機能（Public化時の運用Gate）

- [x] SECURITY.mdとGitHub非公開report採用方針を整備。
- [x] Public状態の確認、有効化API・読戻し、報告form・通知確認の [運用手順](publication-validation.md#公開時の非公開脆弱性報告) を整備。受付未確認でも利用可とは表示しない。
- [ ] Public化の承認を得た時点で、対応するGitHub private reportingを有効化。
- [ ] 「Report a vulnerability」の受付・maintainerの通知設定を確認。
- [x] 受付・通知を未確認のまま利用可能・通知確認済みと表示しない。

Private状態では有効なpublic reporting窓口と見なさない。
APIの404は有効化成功ではない。公開前の利用者は既存の非公開連絡経路を使う。

## 自動管理の詳細な実機確認

2026-10-13に通常Setupのインストール、Agentの期待した動作、SVN接続、Eclipse内蔵ブラウザーの
Proxy指定によるTrac Web表示をユーザーが確認し、現状を正式版として承認した。
最終確認後にも「実機での利用では全て問題ないことが確認できている」との回答を受け、
現行正式版 `0.7.21.0` の実機利用を包括的に受入済みとした。

関連Mock／Windows file回帰47件も成功。故障注入、入力競合の境界、期限・backoffの数値検証は
回帰結果として記録し、利用者の包括的な実機確認とは区別する。
R01～R09の操作方法は [実機確認手順](publication-validation.md#agentの確認項目) に保持し、
今後の変更時の回帰観点とする。実endpoint・user・画面・INIは公開記録へ含めない。

SendInputの前景競合・誤実行リスク、clipboard非復元は [現行仕様](current-specification.md) に明記する。
実機受入の完了を、競合の完全排除や次の新版への更新・rollbackの確認と同義には扱わない。

## 署名と正式Release

- [x] 過去配布物と区別するVersionを `0.7.21.0` に更新し、ユーザーが正式版として承認。
- [x] Client／Agent／Setupとcompilerが保持した埋込み用アンインストーラーのWindows trust・timestamp・同一署名者・hash・metadataを確認。
- [x] 四点の対応時に、インストール済みClient／Agent／`unins000.exe`の署名・timestamp・同一署名者をread-onlyで確認。
- [x] 署名済みClientを基準としてWindows trustとtimestampを実検証し、unsigned候補を拒否。隔離Windows file置換・rollback回帰も成功。実新版への更新受入とは分ける。
- [ ] 次の署名済み正式版で、`0.7.21.0` からの同意付きセット更新・rollback・接続先反映を実機確認。初回導入版のため、現在の受入とは別の次版Gateとする。
- [x] signed packageにLICENSE／NOTICE／inventory・同版二EXE・format 2 manifestを同梱し、検査。
- [x] ユーザーがREADMEを確認し、署名済みSetupとアプリを実機確認後に正式版として承認。
- [x] ユーザーからPrivate Release／tag／asset作成の明示指示を受けた。
- [ ] 署名以外の公開・実機Gateも完了した後、別の明示指示でPublicへ変更する。

正式版の確認範囲、署名済みfileのhashと配布内容は [リリースノート](release-notes-0.7.21.md) を参照する。
Private Releaseは次の新版への匿名自動更新の実機確認を代替しない。
未署名development packageを公式Releaseへ流用しない。archive作成scriptはGitHubを操作しない。
署名・remote操作を自動化する権限を、このチェックリストの存在から推定しない。
