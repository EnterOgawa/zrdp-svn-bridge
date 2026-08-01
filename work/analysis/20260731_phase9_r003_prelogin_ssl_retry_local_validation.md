# Phase 9回帰修正R-003 ログイン前一時失敗のローカル検証

## 対象

- 対象version: Client／Agent `0.7.13.0`
- 対象現象: RDP設定適用とActiveX `Connect()`成功後、`OnConnected`前に
  `discReason=263`、`ExtendedDisconnectReason=0`で切断されるまれな事象
- 対象範囲: Clientのログイン前RDP lifecycleだけ

実IP、接続名、domain、ユーザー名、credential、Windows event本文は本記録へ
複写しない。

## 原因境界

Client診断logでは、設定適用、CredSSP readback、Static Virtual Channel登録、
ActiveX `Connect()`がすべて成功した後、`OnConnecting`から`OnConnected`へ
到達する前に切断されていた。同時刻のWindows RDP Client event logには
SSL handshake継続失敗`0x80004005`が記録されていた。

通常disconnect reason `263`は公開資料で意味が定義されていない。
extended reasonの数値空間に同値が存在しても、実際のextended reasonは`0`で
あるため、license障害等の原因を断定しない。

## 対策

- login未完了
- ZRdp起点切断ではない
- Microsoft認証UIの利用者取消ではない
- Window終了中ではない
- `discReason=263`
- `ExtendedDisconnectReason=0`
- 現在の接続意図で未再試行

上記の全条件へ完全一致した場合だけ、COM callback外のWindow timerから
2,000 ms後に同じ検証済み設定を再適用し、Microsoft ActiveXの通常`Connect()`を
一度呼ぶ。二度目の失敗、timer確保失敗、設定失敗、同期的`Connect()`失敗では
追加再試行しない。

password取得、`mstsc.exe`起動、NLA／CredSSP／証明書方針の緩和は行わない。
Agent、wire protocol、Proxy、allowlist、SVN／Trac、接続履歴形式は変更しない。

## 自動検証

| 項目 | 結果 |
|---|---|
| Debug build | warningなしで成功 |
| Debug CTest | 30/30成功 |
| Release build | 一時出力先でwarningなしに成功 |
| Release CTest | 30/30成功 |
| 純粋retry policy | 完全一致、全除外条件、2,000 ms、一回上限を確認 |
| 固定診断event | 予約、開始、成功、上限到達と自由形式非記録を確認 |
| source comment契約 | 成功 |
| resource契約 | 成功 |
| portable成果物契約 | 成功 |

Release検証は、実行中Clientを無断終了しないため次のGit管理外一時出力を使用した。

```text
work/tmp/release-prelogin-0.7.13/
```

## 成果物

| File | Version | Bytes | SHA-256 | Authenticode |
|---|---:|---:|---|---|
| `ZRdpSvnBridge.exe` | `0.7.13.0` | 676352 | `9DA3ED367BFAD9FEBEB153C016FA40B2085E0AACE9290CEF3EF0B6A1A3B2C9F3` | 未署名 |
| `ZRdpSvnAgent.exe` | `0.7.13.0` | 414208 | `4A4258932A64749E2905476B3D989028B05635AF4BA5C2690F30D79D2465954D` | 未署名 |

一時Release候補の検証後、Client停止の連絡を受けて通常Release buildを行った。
上表は標準`out`の最終値であり、全30 CTest、Version、SHA-256、未署名状態を
再確認した。

## ユーザー実機Gate

- [x] 通常の初回接続が従来どおり成立する
- [ ] 自然再発時に一回だけ自動復旧する
- [ ] 二度目の失敗では追加再試行せず従来のerrorを表示する
- [ ] credential取消では再試行と追加errorを表示しない
- [ ] 成立済みsession切断、表示mode、Agent、Proxy、SVN／Tracに回帰がない

自然再発を作るための接続先再起動、security設定変更、証明書操作は行わない。

2026-07-31、ユーザーから標準`out`のVersion `0.7.13.0`について起動とRDP接続が
成功したとの報告を得た。Client固定診断logでも、同Versionの二回のprocess起動で
それぞれ`rdp.event.login_complete`へ到達し、`rdp.pre_login_retry_*`がない通常成功
経路を確認した。実IP、接続名、domain、ユーザー名、画面内容は記録しない。

## Rollback

`work/tmp/20260731_182839_before_prelogin_ssl_retry.zip`からretry policy、timer、
固定診断event、status resource、Version、test、関連記録だけを戻す。
