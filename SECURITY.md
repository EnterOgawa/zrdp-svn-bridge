# セキュリティ報告

ZRdp SVN Bridgeの脆弱性は、非公開の窓口でお知らせください。
認証情報や再現手順などの詳細を、公開IssueやPRへ投稿しないでください。

## 報告先

repositoryの **Security → Advisories → Report a vulnerability** が利用できる場合は、そちらを使用してください。
受付先は [非公開報告画面](https://github.com/EnterOgawa/zrdp-svn-bridge/security/advisories/new) です。
この画面が利用できない場合、公開Issueへ詳細を投稿しないでください。
既存利用者は、配布時の非公開連絡経路でmaintainerへ連絡してください。
公開用の別メールアドレスは指定していません。

GitHubの非公開報告機能はPublic repository向けです。公開時に有効化し、受付と通知を確認します。
運用状況は [公開チェックリスト](docs/publication-checklist.md#非公開報告機能public化時の運用gate)、
有効化の手順は [公式手順](https://docs.github.com/en/code-security/how-tos/report-and-fix-vulnerabilities/configure-vulnerability-reporting/configure-for-a-repository) を参照してください。

## 対象と保守範囲

現行mainと、今後公開する最新の正式Releaseを対象に報告を受け付けます。
旧社内配布物はPublic配布・継続的なbackportの対象としません。
保守対応はbest effortです。対応期限、全旧Versionの修正、組織policyへの適合を保証しません。
Windows／RDP ActiveX／Visual Studio自体の問題は、それぞれの提供元にも報告してください。

## 含める情報

- Versionとcommit、WindowsのVersion、問題の分類、影響範囲
- 文書用IPv4・架空データでの最小再現手順、期待動作と実際の動作
- 固定エラーコード、機密部分を除去した診断情報
- 修正候補があれば、その概要

実IP、接続先名、ドメイン、利用者名、URL、資格情報、Cookie、HTTP本文、
ソース・差分、ticket本文、署名鍵を送らないでください。
dumpや実画面には機密情報が残るため、まず必要性と安全な共有方法を相談してください。
専用test以外の実取引先で脆弱性の再現・探索・書込み操作を行わないでください。

公開Issue／PRは、セキュリティ上の影響がない一般的な不具合だけに使用します。
脆弱性の詳細は、maintainerと公開時期・範囲を調整してから公開してください。
この文書は報告方針であり、機密情報の送付許可や製品の安全性の保証ではありません。

実装上の境界は [security設計](docs/security.md)、残存リスクは
[現行仕様](docs/current-specification.md) を参照してください。
