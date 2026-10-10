# 貢献方法

不具合報告、文書修正、test、patchを歓迎します。公開準備中はrepositoryにアクセス権のある
利用者だけがIssue／PRを作成できます。公開済みと誤解しないでください。

## はじめに

[README](README.md)、[現行仕様](docs/current-specification.md)、
[コメント・書式規約](docs/coding-standard.md) を確認してください。
`AGENTS.md` はmaintainer／coding agent向けの作業規則です。追加のlicense条件ではありません。
自分や所属組織が提供する権利を持つ内容だけを送ってください。
第三者のコード・画像を取り込む場合は出典、license、変更内容、必要なnoticeを示してください。
意図して提出する自作の貢献は、本projectのApache-2.0に基づくものとして扱います。

脆弱性は [SECURITY.md](SECURITY.md) に従い、公開Issueへ詳細を投稿しないでください。

## 安全な変更範囲

- 通常testはin-memory transport、fake backend、loopback上のMockだけで行う。
- 実RDP、取引先SVN／Trac、実資格情報、実履歴・設定をtestやGitHub Actionsへ持ち込まない。
- 実IP・URL・利用者情報、画面画像、payload、鍵、EXE・PDB・dumpをPRへ含めない。
- 文書用IPv4と `example.test` の架空値を使用する。
- policy回避、汎用転送、任意command、追加の受信portへ機能を広げない。
- Protocolやセキュリティ境界の変更は、理由・代替案・互換性・rollbackをADRで説明する。

## ビルドと検証

Windows x64、Visual Studio 2022のC++／ATL、Windows SDK、CMake 3.31以上を用意します。
開発ツールやOS componentはrepositoryに同梱しません。

```powershell
cmake --preset vs2022-x64
cmake --build --preset debug
ctest --preset debug
cmake --build --preset release
ctest --preset release
python scripts/test_format_source.py
python scripts/test_audit_source_comments.py
python scripts/format_source.py --check
python scripts/audit_source_comments.py
python scripts/check_repository.py
```

Pythonは開発時の追加監査だけに使用し、アプリの外部runtime要件ではありません。
整形にはclang-format 19を使用します。他の配置では `--clang-format` で指定します。
全CTestはMock／ローカルfixtureです。実RDP受入の代わりにはなりません。
Release二EXEの既定出力は `out/` です。実行中のEXEを終了し、既存配布物を退避してからbuildしてください。

## PRに記載すること

- 目的、対象Issue、変更範囲、利用者への影響
- fail-closed／スレッド・所有関係／保存形式／Protocolへの影響
- test結果と未確認事項、rollback方法
- 参考・転用した第三者資料があれば出典とlicense

動作変更、整形、文書整理をできるだけ別の論理変更に分けてください。
C++ file版は論理変更単位で更新し、履歴と一致させます。
過去の実機受入を、新実装の実機合格として転記しないでください。
公開、Release、署名、asset差替えはmaintainerが別Gateに従って行います。

意見の相違は再現条件・test・設計上の理由を示して相談してください。
個人攻撃や機密情報の投稿は避けてください。maintainerは安全で建設的なreviewのために
投稿の修正や非公開での連絡を求める場合があります。
