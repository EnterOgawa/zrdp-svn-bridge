# C/C++コーディング規約の適用

ZRdp SVN Bridgeの自作C++20 sourceとtestには、ユーザー提示の
`c_cpp_coding_standard.md` ver01.01.03および
`c_cpp_coding_standard_sample.cpp`の書式・comment規則を適用する。
project内の要約は[AGENTS.md](../AGENTS.md)第25節に記載する。

## 適用範囲

- `src/`と`tests/`の自作`.h`／`.cpp`、自作`.rc`のfile headerを対象とする。
- Windows SDK、ActiveXのtype libraryから生成されるheader、第三者sourceは変更しない。
- 既存の名前、公開API、protocol識別子`ESVNBRG`、設定key、resource IDは維持する。
  命名例へ合わせるための一括改名やruntime動作変更は行わない。
- includeは自身のheaderと依存境界を明示する。Winsock／Windows／COMの前提を
  壊さないため、既存のplatform include順序を機械的に並べ替えない。
- 見本の他project固有namespace、機能、ADR番号、移行識別子は取り込まない。

## 書式とcomment

indentはtab幅4、functionのbraceは次行、制御文・型・namespaceのbraceは同じ行とする。
call／宣言／制御文は`Function( argument)`の形式、引数なしは`Function()`とし、
statement末尾は` ;`とする。`for` headerの区切りとgrouping括弧は別扱いとする。

file、名前付き型、function／methodの契約は`/** ... */`で記載する。
すべてのfieldとenum値は直前の`///`、定数・変数・処理説明は通常の`//`を使う。
すべての`if`／`else if`、loop、および`switch`／各`case`の目的を説明する。
commentは日本語を基本とし、実endpoint、credential、payloadを記載しない。

file headerは著作権・SPDX、責務、file版、作成日、制作者、更新履歴を持つ。
今回の初回移行は独立file版`01.00.00`とし、2026/10/07の移行記録を一件付ける。
既存の作成日記録がないため、`@date`にはGitへの初回登録日を使用した。
記録されていない過去の制作者・変更履歴は推測して追加しない。
以後は論理変更単位で版を上げ、最新履歴と`@version`を一致させる。
保存・再整形・testのたびには採番しない。
製品version`0.7.20.0`とfile版は別管理であり、書式整備だけで製品版は変更しない。

## 整形と確認

`.clang-format`と`scripts/format_source.py`を共通の整形入口として使う。
clang-format 19で検証する。別配置では`--clang-format`または`CLANG_FORMAT`で指定する。

```powershell
python scripts/format_source.py
python scripts/format_source.py --check
python scripts/test_format_source.py
```

特定fileだけの場合はrepository相対pathを引数へ追加する。
formatterはliteralとcomment以外のtoken列を比較し、token変更を検出したら保存を拒否する。
file版、履歴、説明本文は自動更新しない。commentの意味とDoxygenの要素別対応は
formatterだけでは保証できないため、reviewとDoxygen XML確認を併用する。
削除したcopy／move操作とCOM署名内の未使用引数は既存の無名宣言を維持する。
実行可能な境界の無名引数は、使用しない理由と出力を変更しない契約を`@note`で説明する。
`source_comment_contract_test`はfile metadata、直前目的comment、test入口契約を確認する。
通常のDebug／Release buildと全CTestも実行する。

局所lambdaの契約にも`@brief`・名前付き引数の方向・値を返す場合の`@return`を
記載する。Doxygenが局所lambdaの契約を外側の関数へ誤結合しないよう、
その契約block内を`@cond LOCAL_CALLBACK_CONTRACT`／`@endcond`で囲む。
公開契約や名前付きmember functionをこの条件で隠してはならない。
局所契約はソースで読み、`scripts/audit_source_comments.py`で不足を検査する。

```powershell
python scripts/test_audit_source_comments.py
python scripts/audit_source_comments.py
python scripts/audit_source_comments.py --xml-dir <DoxygenのXMLディレクトリ>
```

XMLは`EXTRACT_ALL=YES`、`EXTRACT_PRIVATE=YES`、`EXTRACT_STATIC=YES`、
`EXTRACT_LOCAL_CLASSES=YES`、`EXTRACT_ANON_NSPACES=YES`、`INHERIT_DOCS=NO`で
生成する。継承元の説明をoverride自身の説明の代用としない。
局所lambda抽出は既存lexerによるdelimiter検査であり、完全なC++ AST解析ではない。
文面の意味・所有関係・thread前提はreviewを併用する。

## ライセンスと公開

自作sourceには`Copyright (c) 2026 EnterSystem CO.,LTD.`と
`SPDX-License-Identifier: Apache-2.0`を記載し、rootの[LICENSE](../LICENSE)に
Apache License, Version 2.0の公式英語全文を置く。
第三者の権利表示やWindows componentの許諾条件は置換しない。
WindowsのRDP system DLLは同梱しない。

OSS公開に向けたsourceの許諾表示であり、repositoryの公開操作ではない。
現在のPrivate設定と既存社内配布物は維持し、公開前には権利帰属、依存物、
履歴中の機密情報、配布内容を別途確認する。公開・Release uploadは明示指示を受けて行う。
