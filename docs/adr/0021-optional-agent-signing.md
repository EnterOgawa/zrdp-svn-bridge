# Agent配置と起動の任意署名

日付: 2026-10-10

## 決定

ユーザーの追加指示により、署名をClient／Agentの起動、Agent配置の許可条件にしない。
自前ビルドを通常の経路で使用できるようにし、未署名テストのcheckbox、別警告、状態、
固定argument `--rdp-setup-unsigned-test`を廃止する。配置argumentは`--rdp-setup`だけとする。
ADR 0014／0016／0018／0019の署名必須・未署名別経路に関する記述は本決定で置き換える。

署名は任意の配布時の発行者証明として使用できる。署名があることとOSSであることは
別の条件であり、本決定はライセンスやリポジトリ公開範囲を変更しない。

## 維持する境界

Clientは実行中EXEに隣接する固定名Agentだけを読み、正式製品metadata、同一版、x64 PE、
setup対応、通常file、有限サイズと全承認済み設定を検証する。Agent側もmetadataと
allowlistを再検証し、active RDP user sessionと固定のLocalAppData配置先に限定する。
バックアップ付き更新、失敗時rollback、readonly専用drive、世代とclipboardの照合を維持する。

metadataの一致は発行者や改ざん防止の証明ではない。信頼できるsourceからの自前ビルド、
または信頼できる配布元の二EXEを使用する。Windowsと組織の実行・転送制限を回避せず、
Windows警告を自動承認しない。Client自動更新の署名・公開鍵・hash検証は変更しない。

## 互換性と検証

SVCのwire format、allowlist、Proxy、COM STA構成は変更しない。
旧未署名テストargumentは拒否するため、ClientとAgentは両方を更新する。
単体テストは通常経路で未署名の実Agentを隔離fixtureへ配置し、同一二fileの再利用、
backup更新、metadataでの別製品拒否、承認済みINIと固定CLI、廃止UIの不在を確認する。
Client自動更新の署名検証も既存回帰テストで維持する。実RDPと実宛先の操作は利用者のGateとする。

検証結果: Debug／Release全47 CTest合格、書式140 file不適合0、formatter試験16件合格。
Doxygenの変更対象契約に新規警告なし。既存の入力診断に関する8警告は本変更の対象外。

## 自動ライフサイクルとの関係

本決定は署名方針だけの変更である。RDP接続後の自動起動、疎通断の自動復旧、
設定変更時の協調停止と再起動、接続マネージャーの設定button、入力テスト機能の削除は
[ADR 0022](0022-connection-agent-lifecycle.md)で実装した。
公開済み配布driveを変更せず、限定SVC controlでINIを更新し、Agentを強制終了しない。
