# OSS公開前監査（2026-10-11）

対象ソース: `1c29d61fd89a44f814d2745522916401274c4e33`。
本書とREADMEの残存リスク追記は、その後の文書のみの変更である。

## 結論

**現時点ではPublic化の準備完了とは判定しない。**
ソースのライセンス表示、README、ビルド・回帰検証は整っているが、権利確認、
既存Releaseの扱い、脆弱性報告窓口、旧記述の整理、最新実装の実機受入が残っている。
今回の監査でRelease、tag、asset、visibility、GitHub設定の変更やGit履歴rewriteは行っていない。

この判定はプロジェクトの公開準備状況であり、OSSライセンスの適格性と同義ではない。
署名・CI・Issue template・Code of Conductの有無だけでOSSではなくなるわけではない。
署名付き公式更新と、利用者が変更して自前buildを使う自由も区別する。
OSSの判断基準は [OSIの定義](https://opensource.org/osd)、ライセンス条件は
[Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0) を参照する。

## 確認済みの範囲

| 項目 | 結果 |
|---|---|
| root `LICENSE` | Apache公式英語全文と改行・末尾空白を正規化して一致。GitHubも `Apache-2.0` と認識 |
| 自作C++／resource | Copyright・SPDX・file契約検査を通過。145 fileのheaderを確認 |
| コメント修正 | 37 C++ file。135局所lambdaとoverride自身の契約を監査し、不足・誤説明を修正 |
| 動作不変性 | 作業開始時snapshotと141 C++ fileのコード・literal token列が一致。今回のコメント修正は実行内容を変更していない |
| Doxygen | 継承補完なし（`INHERIT_DOCS=NO`）で生成。警告0件、追加監査の不足0件 |
| 回帰 | Debug／Release各47 CTest通過。formatter self-test 16件、監査script self-test 12件通過 |
| 別build directory | 固定presetを使わないVS2022汎用configure、Release build、47 CTestを通過。元の `out/` はこの検証では置換しない |
| 新規local clone | `core.autocrlf=true` でcheckoutし、README手順のfixture基点作成後にReleaseの47 CTestとPython self-test 28件を通過。初回の三失敗と原因は下記G06に記録 |
| README | 接続マネージャー・Agent設定・trayの架空fixture画像。相対file参照の存在、GitHub生成HTMLの画像4点と主要見出しを確認 |
| 最新tree | 221 fileを走査。鍵・既知tokenパターン、実設定・実行binary、既知の実接続情報の混入を検出せず |
| 到達可能Git履歴 | 10 commit、499 distinct blobを走査。数値検出154件は製品Versionと拒否testのmulticast／Version値であり、実endpointではなかった |
| 履歴中のWord | 5 distinct OOXML blobのXML・関係・metadataを確認。creator等のidentity property、comments、embeddingsなし。画像は二種類の一般的な設計図であり実画面ではない |
| 既存Release | `v0.7.14.0`、`v0.7.15.0`、`v0.7.20.0` をreadonlyで確認。ZIPの公表SHA-256と一致。実設定・追加DLL・PDB・鍵はなし。ただしLICENSEなし |
| GitHub | mainのlocal／remote SHA一致、Private。既存Release三件・tag三件は保持。Issueなし、Actions runなし |

コメント監査はlexer・Doxygen・既存test・目視reviewの組合せであり、完全なC++ AST解析や
全コメントの意味を数学的に保証するものではない。GitHub HTML確認はブラウザによる全画面目視確認の代わりではない。
別生成directoryと新規local cloneを検証した。別PC／新規Windows環境での再現性までは確認していない。

## 公開・配布前に解消する項目

### G01: 権利帰属と許諾範囲（公開前）

自作ソースには `EnterSystem CO.,LTD.` のcopyright表示があるが、repositoryから
雇用・委託契約、第三者コードの取り込み履歴、法人としての公開権限までは検証できない。
AI生成アイコンの制作経路は [資産README](../assets/icons/README.md) にある。
これだけでコード・文書・画像すべての権利処理完了を証明するものではない。

完了条件:

- 権利者がApache-2.0でのソース公開を承認し、文書・画像の許諾範囲も確定する。
- Microsoft等の参考・転用コードがないか、権利者／開発者が確認する。
- 転用がある場合は出典・ライセンス・変更点・必要なnoticeを記録する。
- CRT／ATLを使用するbinaryの配布条件を、利用しているVisual Studioの契約で確認する。

Windows RDP DLLはtreeへ同梱していない。OSS表示でMicrosoft componentの条件を置き換えない。
Microsoftの配布対象と条件は [VS2022再配布文書](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution) に基づく。
本監査は法的な権利帰属の認定ではない。

### G02: 既存Releaseと履歴の公開範囲（Public化前）

既存三ZIPは社内配布向けで、`LICENSE` を含まない。配置もClient／Agent別folderであり、
最新版READMEの「同じfolderに二EXE」とは異なる。旧配布物は更新manifest format 2にも対応しない。
過去のWord、作業記録、署名済みbinaryは最新treeから消しても履歴・Releaseに残る。

完了条件:

- 旧Releaseを公開対象として維持するか、Privateに保全する別の公開方法を採るか、ユーザーが決定する。
- 公開する旧配布物について、当時のライセンス、署名者情報、同梱文書、利用手順を確認する。
- 新しい配布packageにはLICENSEを含める。NOTICEは第三者からの承継義務等を確認して必要な場合に含める。
- commitのauthor／committer情報と過去の文書・作業記録を、権利者が公開してよいと確認する。

今回、旧Releaseの削除・再作成、asset差替え、履歴rewrite、force pushは行っていない。
Private→Publicでは既存コードが公開対象になるため、最新treeだけの点検で完了としない。
[GitHubのvisibility変更の注意](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/managing-repository-settings/setting-repository-visibility) も確認する。

### G03: 脆弱性の非公開報告窓口（公開前）

`SECURITY.md` はなく、非公開で脆弱性を報告する確認済み窓口がない。
既存の [security設計文書](security.md) は脅威・実装境界の説明であり、報告窓口の代わりではない。
APIによるprivate vulnerability reporting確認はPrivate状態で404となり、未確認である。

完了条件:

- 保守責任者と非公開連絡先、またはPublic化時に有効化するGitHub private reportingを決定する。
- `SECURITY.md` に対象Version、報告手順、機密情報を公開Issueへ投稿しない規則を記載する。
- 選択した窓口が実際に使用できることを確認する。架空のメールアドレスや対応SLAを記載しない。

GitHubのprivate reportingはpublic repository向けの機能である。
[公式設定手順](https://docs.github.com/en/code-security/how-tos/report-and-fix-vulnerabilities/configure-vulnerability-reporting/configure-for-a-repository) を参照する。

### G04: 過去方針と現行方針の分離（公開前）

- `AGENTS.md` 第13・14・31節等には全sessionを一つのSTAへ置く旧方針が残るが、
  現行コードと [ADR 0005](adr/0005-dedicated-sta-per-rdp-session.md) はsession専用STAである。
- 同fileのAgent操作欄には旧button・署名必須・異なるfileを上書きしない記述が残り、
  現行のmanager設定・自動管理・任意署名・backup付き更新とは異なる。
- `AGENTS.md` 第20節のPrivate／公開禁止は現在の作業制限として有効である。
  将来公開時は明示承認に基づいて運用記述を更新し、Apache-2.0の利用者の権利制限として読めないようにする。
- `known-limitations.md`、`specification.md`、`roadmap.md`、一部ADRは過去の検証時点を扱う。
  現行の手順と、過去記録・superseded判断の境界をさらに明示する。

READMEと利用手順は今回現行操作へ更新した。開発規則・旧ADRを独断で新しい設計へ置換していない。
完了条件は、権利者／保守者が現行の開発規則を確定し、旧文書に対象時点・superseded関係を明記することである。

### G05: 最新実装の受入とRelease識別（新Release前）

現在の開発binaryは未署名で製品Versionが `0.7.20.0` のまま。
過去の署名済み同Versionと同一成果物ではない。既存のbutton起動・tray終了後の再起動の
ユーザー確認を、現在の完全自動管理・セット更新の実機合格と見なさない。

完了条件:

- 設定あり／なし、初回配置、既存版更新、疎通断復旧、設定変更・全削除、複数sessionの実RDP受入。
- 前景切替、clipboard変更、部分入力、policy拒否、無応答で、安全停止とRDP維持を確認する。
- SendInputの前景競合による誤入力・誤実行の残存リスクと、clipboardを復元しない動作を保守者が確認する。
- 新しい製品Versionを付け、Client／Agentを同じ版で署名し、manifestと配布物を検証する。
- 署名済み旧セット→新版の取得・適用・再起動・接続先更新と、失敗時rollbackを実機で確認する。
- READMEの最終確認後、ユーザーの明示指示でのみReleaseを作成する。

未署名自前buildの通常起動・配置は可能。公式自動更新で署名を要求することを、
forkや改変版の使用禁止として扱わない。

### G06: 新規cloneでのtest基点（今回解消）

新規local cloneの最初のCTestは44/47合格で、`unit.rdp_host_smoke`、
`unit.agent_setup`、`unit.agent_bootstrap_ui` が失敗した。
Git管理外の `work/tmp` が存在せず、固定fixtureの基点検証が拒否したためである。
RDP host testでは正しい引数を与えても45（専用drive作成失敗）を返し、基点の不存在を確認した。

当該clone内に `work/tmp` を作成すると、ソース・binaryを変更せず全47件が合格した。
READMEの汎用build手順にその作成を追加した。build systemを独断で変更していない。
初回失敗を隠さず記録し、別Windows環境での確認は引き続き推奨する。

## 改善を推奨する項目（OSSライセンスの必須条件ではない）

- `CONTRIBUTING.md`: build・test・コメント規約、実接続禁止、機密情報の扱い、PRの提出方法。
- Issue／PR template: Versionと再現条件、機密情報を含めない報告項目。
- 保守範囲とCode of Conduct: 必要な場合に責任者と実行可能な運用を定める。
- CI: 現在は未導入。`AGENTS.md` の承認条件を満たす設計後にWindows x64 build・testを追加する。
- 汎用presetと別Windows環境でのclone検証: 同梱presetは開発者tool配置固定。
- 第三者・build tool inventory、SBOM、Release再現手順、LICENSE同梱のpackage検査。
- 機械的secret scanの拡充と、人による履歴・配布物・commit identityの最終確認。

## 監査の限界

鍵・token形式、既知実環境識別子、非文書用IPv4、file種類、OOXML構造を検査した。
専用secret scannerの全ルール、全ての任意文字列credential、binary内の全data、
すべてのアカウント情報・組織名の機密性を網羅したものではない。
未知の漏えい・脆弱性がないことの証明ではなく、外部penetration testでもない。
実取引先へのRDP／SVN／Trac接続、ネットワーク探索、認証・policy変更は実施していない。

公開はG01～G04を含む公開対象の確認とユーザーの最終承認後、
新ReleaseはさらにG05と署名・README確認後とする。今回どちらも実行しない。
