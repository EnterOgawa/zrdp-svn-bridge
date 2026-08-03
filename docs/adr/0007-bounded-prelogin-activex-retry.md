# ADR 0007: ログイン前ActiveX一時失敗の限定再試行

- Status: Superseded by ADR 0009
- Date: 2026-07-31

## Context

ZRdpの診断logでは、接続設定、CredSSP、Static Virtual Channel登録、ActiveX
`Connect()`が成功した後、`OnConnected`より前に`discReason=263`、
`ExtendedDisconnectReason=0`で切断される事象を複数回確認した。Windowsの
`TerminalServices-RDPClient/Operational`には同時刻に、SSL handshake継続失敗
`0x80004005`が記録されていた。

同じ接続先へ別のMicrosoft RDP Clientで接続した後はZRdpでも成立したが、
ZRdpから`mstsc.exe`を起動することや、NLA、CredSSP、server認証を弱めることは
製品境界に反する。一方、利用者へ毎回別Clientでの事前接続を要求するのも
通常運用として不安定である。

公開されている通常disconnect reasonとextended reasonは別の数値空間であり、
通常reasonの`263`だけからlicense障害等の原因を断定しない。対策条件は
ローカルで観測した完全な数値組と接続段階に限定する。

## Decision

- 現在の接続意図でlogin未完了の場合だけ対象とする。
- ZRdp起点切断、credential UI取消、Window終了中は対象外とする。
- `discReason=263`かつ`ExtendedDisconnectReason=0`へ完全一致した場合だけ
  2,000 ms後に一回再試行する。
- 同じ検証済み接続設定を未接続ActiveXへ再適用し、Microsoft Controlの通常
  `Connect()`を呼ぶ。
- COM callback内では再接続せず、session専用STAのWindow timerから開始する。
- 二度目の失敗、timer確保失敗、設定適用失敗、同期的`Connect()`失敗では
  追加再試行しない。
- login完了時に再試行状態を破棄する。新しい利用者接続意図でも回数を初期化する。
- passwordをZRdpへ取り込まず、必要な認証はMicrosoft標準UIへ委譲する。
- retry予約、開始、成功、上限到達を固定eventと数値fieldだけで記録する。

## Consequences

### Positive

- 一時的なMicrosoft RDP SSL／CredSSP状態から、別Clientを起動せず復旧できる
  可能性がある。
- 完全一致と一回上限により、異なる認証失敗や恒久障害を隠さない。
- UI threadをblockせず、COM callback終了後に同じSTA上でActiveXを操作できる。
- Agent、Proxy、SVN／Trac経路が成立する前の変更に限定できる。

### Negative

- 再試行時にMicrosoft標準認証UIがもう一度表示される場合がある。
- 原因そのものが恒久的なserver／client状態であれば、一回再試行後に従来の
  error表示となる。
- 公開仕様に通常reason `263`の意味がないため、Windows更新でsignatureが変われば
  この対策は発動しない。

## Security and protocol impact

NLA、CredSSP、server certificate検証、認証警告を変更または自動承認しない。
credential、接続先、画面内容、HTTP payloadを追加記録しない。RDP protocol、
Static Virtual Channel、Application Protocol、Proxy、Agent、allowlist、
接続履歴形式、新規portに変更はない。

## Alternatives

### `mstsc.exe`を自動起動する

製品がWindows標準Clientの外部状態へ依存し、複数sessionとportable運用を崩すため
不採用とした。

### 認証level、NLA、CredSSPを緩和する

接続成功のためにsecurity boundaryを弱めることは要件違反であるため不採用とした。

### 無制限または複数回再試行する

credential再入力、account lockout、利用者取消の無視、恒久障害の隠蔽につながるため
不採用とした。

### すべてのログイン前切断を再試行する

password誤り、account制限、証明書拒否等も含むため不採用とした。

## Validation

- 純粋policyで全一致、全除外条件、2,000 ms、一回上限を確認する。
- 固定診断eventと自由形式非記録を確認する。
- Debug／Release buildと全CTestを成功させる。
- 実RDPで通常成功、対象signatureからの復旧、二回失敗時停止、
  credential取消の非再試行を確認する。

## References

- Microsoft `IMsTscAxEvents::OnDisconnected`
- Microsoft `IMsRdpClient::ExtendedDisconnectReason`
- Microsoft `IMsRdpClient5::GetErrorDescription`
