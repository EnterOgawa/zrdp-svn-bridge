# 全ソースコメント整備提案

## 目的

ユーザー提示の`AplMain.java`の記述形式を参考に、ZRdp の全 C++ ソースへ
責務、契約、処理段階、分岐理由、反復目的を日本語で明示する。

参照ソースの内容、固有名、著作権表示、作成者、手書き更新履歴は取り込まず、
コメントの構造と説明密度だけを C++20／Doxygen／Git 管理へ適合させる。

## 対象

- `AGENTS.md` のソースコメント付与ルール。
- `src/**/*.h`、`src/**/*.cpp`。
- `tests/**/*.cpp`。
- 手書きの resource ID／RC source。
- コメント整備の監査記録とロードマップ。

## 変更方針

1. 全ファイルに `@file` とファイル責務を維持する。
2. class、struct、enum、定数群、member 群を意味のある section comment で区切る。
3. public／private function、callback、thread entry、test entry point に Doxygen 契約を
   付与する。
4. 引数、戻り値、ownership、thread affinity、fail-closed 動作がある場合は明記する。
5. function 本体は、意味のある処理段階ごとに短い日本語コメントを付ける。
6. 全 loop に目的を付け、security／length／state／resource 境界の分岐には理由を付ける。
7. 自明な代入の逐語説明、作成者、日付、手書き version、変更履歴は追加しない。

## 影響

- ActiveX／COM: コメントのみ。STA 所有関係と callback 境界を明文化する。
- Virtual Channel: コメントのみ。frame／chunk 境界と serialized writer を明文化する。
- HTTP Proxy: コメントのみ。loopback、HTTP parser、redaction 境界を明文化する。
- Agent Socket: コメントのみ。allowlist と有限 queue を明文化する。
- Protocol compatibility: wire format、message type、version、動作は変更しない。
- Security boundary: 実 endpoint、credential、payload をコメントへ追加しない。

## 検証

- 全対象ファイルの `@file` 存在確認。
- loop 直前コメントの静的確認。
- Debug／Release build。
- Debug／Release CTest。
- 実行 binary の機能変更がないことを local startup smoke で確認する。

## Rollback

作業前に `work/tmp` へ `src`、`tests`、`AGENTS.md`、関連文書の archive を作成する。
問題がある場合は、コメント整備差分だけを作業前 archive から復元できる。

## 実施結果

- [x] `AGENTS.md`へJava参照ソースの採用範囲と禁止事項を追加した。
- [x] `src`、`tests`、`assets/icons`の全88 source、合計39,270行を監査した。
- [x] 全file契約、型、function、member、loop目的、test entry契約を整備した。
- [x] 将来の不足を検出する`unit.source_comment_contract`を追加した。
- [x] Debug buildをwarningなしで完了し、全30 CTestを成功させた。
- [x] Releaseの全30 CTestを成功させた。
- [x] 実行中Clientを上書きせず、一時出力先でRelease Clientのlinkと成果物契約を
      成功させた。

詳細は`work/analysis/20260731_all_source_comment_alignment_validation.md`へ記録した。
作業前archiveは
`work/tmp/20260731_165301_before_all_source_comments.zip`である。
