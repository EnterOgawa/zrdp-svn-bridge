# ADR 0009: ログイン前ActiveX一時失敗の段階的再試行

- Status: Accepted
- Date: 2026-08-03
- Supersedes: ADR 0007

## Context

ADR 0007では、ログイン前の`discReason=263`、`ExtendedDisconnectReason=0`に
完全一致するSSL handshake継続失敗を2,000 ms後に一回だけ再試行した。

実機で同じsignatureが初回と一回目の再試行に連続し、ZRdpは合計二回で停止した。
その直後、同じ端末のMicrosoft RDP ClientもSSL handshake継続失敗
`0x80004005`を二回記録したが、三回目の接続処理で成功した。成功後にも対象接続先の
永続RDP接続履歴と保存済み`TERMSRV` credentialは存在せず、永続cacheの有無だけでは
現象を説明できなかった。

一回上限は異常な反復を防いだ一方、今回観測したMicrosoft Clientの復旧点より一回
早く停止していた。対象signatureと利用者取消の除外を維持したまま、実測した三回目
まで有限に許可する。

## Decision

- 現在の接続意図でlogin未完了の場合だけ対象とする。
- ZRdp起点切断、credential UI取消、Window終了中は対象外とする。
- `discReason=263`かつ`ExtendedDisconnectReason=0`へ完全一致した場合だけ
  段階的な再試行を予約する。
- 一回目は2,000 ms後、二回目は5,000 ms後に開始する。
- 初回を含む合計三回で停止し、三回目も同じsignatureなら従来のerrorを表示する。
- COM callback内では再接続せず、session専用STAのWindow timerから開始する。
- 各回で同じ非機密設定を検証、再適用し、Microsoft ActiveXの通常`Connect()`を
  呼ぶ。
- timer確保、設定適用、同期的`Connect()`の失敗では追加再試行しない。
- passwordをZRdpへ取り込まず、認証は毎回Microsoft標準UIへ委譲する。
- retry予約、開始、成功、上限到達を固定eventと数値fieldだけで記録する。

## Consequences

### Positive

- Microsoft RDP Clientで観測した三回目の復旧点まで、別Clientなしで到達できる。
- 2,000 msと5,000 msの有限表により、回数と待機時間を一つの境界で管理できる。
- 完全一致条件を維持し、異なる認証失敗や成立済みsession切断を隠さない。
- Agent、Proxy、SVN／Trac経路が成立する前のClient変更に限定できる。

### Negative

- Microsoft Controlがcredentialを保持しない場合、認証UIが最大三回表示される。
- 三回目でも復旧しない恒久障害では、従来どおり利用者操作が必要になる。
- 公開仕様に通常reason `263`の意味がないため、別signatureの障害には適用できない。

## Security and protocol impact

NLA、CredSSP、server certificate検証、認証警告を変更または自動承認しない。
credential、接続先、画面内容、HTTP payloadを追加記録しない。RDP cache、credential
manager、registryを作成または変更しない。

RDP protocol、Static Virtual Channel、Application Protocol、Proxy、Agent、
allowlist、接続履歴形式、新規portに変更はない。Agent配布物の更新は不要である。

## Alternatives

### 一回上限を維持する

今回の実測ではMicrosoft RDP Clientが三回目に成功しており、ZRdpだけが復旧点より
早く停止するため不採用とした。

### RDP履歴、証明書またはcredential cacheを作成する

成功後にも該当する永続entryはなく、原因の裏付けがない。未文書化状態への依存と
証明書判断の代行を避けるため不採用とした。

### 無制限または三回を超えて再試行する

credential再入力、account lockout、恒久障害の隠蔽につながるため不採用とした。

### NLA、CredSSPまたはserver認証を緩和する

接続成功のためにsecurity boundaryを弱めることは要件違反であるため不採用とした。

## Validation

- 純粋policyで完全一致、全除外条件、2,000 ms、5,000 ms、二回上限を確認する。
- 固定診断eventと自由形式非記録を確認する。
- Debug／Release Client buildと全CTestを成功させる。
- Agent実行ファイルのSHA-256が変更前後で一致することを確認する。
- 実RDPで通常成功、三回目までの復旧、上限到達、credential取消を確認する。

## References

- Microsoft `IMsTscAxEvents::OnDisconnected`
- Microsoft `IMsRdpClient::ExtendedDisconnectReason`
- Microsoft `IMsRdpClient5::GetErrorDescription`
- ADR 0007
