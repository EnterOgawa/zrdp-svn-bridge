# ログイン前SSL handshake失敗の有限再試行提案

## 現象

Microsoft Remote Desktop ActiveXへ接続要求を渡した後、`OnConnected`より前に
`discReason=263`、`ExtendedDisconnectReason=0`で切断される場合がある。
WindowsのRDP Client event logでは同時刻にSSL handshake継続失敗
`0x80004005`が記録されている。別のMicrosoft RDP Client接続後は同じZRdp接続が
成立した。

## ローカルlogの根拠

- `ApplyConnectionSettings`、CredSSP readback、Static Virtual Channel登録、
  ActiveX `Connect()`はすべて成功している。
- 失敗試行は`OnConnecting`とMicrosoft内部認証dialog eventまで進むが、
  `OnConnected`／`OnLoginComplete`へ到達しない。
- 同じsignatureはVersion `0.7.4.0`で5回、Version `0.7.12.0`で1回あり、
  全画面追従変更に起因する新規回帰ではない。
- Agent、Proxy、表示mode API、Virtual Channel payload処理の失敗はない。

実IP、接続名、domain、ユーザー名、認証情報、Windows event本文は本記録へ
複写しない。

## 対策

次の全条件へ完全一致した場合だけ、Microsoft ActiveXの通常`Connect()`を
2,000 ms後に一度だけ再実行する。

1. 現在の接続試行で`OnLoginComplete`へ到達していない。
2. ZRdp自身が切断を要求していない。
3. Microsoft認証dialogの利用者取消として分類されていない。
4. Window終了処理中ではない。
5. `discReason == 263`である。
6. `ExtendedDisconnectReason == 0`である。
7. 現在の接続意図に対する再試行をまだ一度も予約していない。

再試行前に同じ検証済み接続設定をActiveXへ再適用する。passwordは取得、保持、
再送せず、必要な場合はMicrosoft標準認証UIへ再度委譲する。

## 失敗時動作

- timer確保、接続前設定、`Connect()`のいずれかが失敗した場合は追加再試行しない。
- 二度目も同じ切断になった場合は従来どおり状態とerror dialogを表示する。
- 利用者がWindowを閉じた場合は予約を取消す。
- 異なるreason、成立済みsessionの切断、credential取消、local切断は再試行しない。
- `mstsc.exe`の起動、認証警告の自動承認、NLA／CredSSP／証明書方針の緩和は行わない。

## 診断

次の固定eventと数値fieldだけを追加する。

- 再試行予約
- 再試行開始
- 再試行成功
- 再試行上限到達

記録対象はattempt、最大attempt、disconnect reason、extended reason、
session専用loopback port、HRESULTに限定する。接続先や認証情報は記録しない。

## 影響範囲

- Clientのログイン前RDP lifecycleだけを変更する。
- Microsoft ActiveX、Windows標準認証UI、既存COM STAを引き続き使用する。
- Agent、Static Virtual Channel wire protocol、Proxy、allowlist、SVN／Trac、
  接続履歴形式は変更しない。
- 新しいnetwork経路、受信port、credential保存を追加しない。

## 検証

- 完全一致だけを再試行する純粋policyを単体testする。
- 最大一回、2,000 ms、各除外条件、上限到達を単体testする。
- 固定診断eventが自由形式データを受け付けないことを確認する。
- Debug／Releaseを`/W4 /WX`でbuildし、全CTestを実行する。
- 実機で初回成功、該当失敗からの一回復旧、二回失敗時の停止、
  credential取消、成立済みsession切断の非再試行を確認する。

## ローカル結果

- [x] Debug buildを`/W4 /WX`でwarningなしに完了した。
- [x] Debug CTestを30/30成功させた。
- [x] 実行中Clientを停止せず、一時出力先でRelease buildをwarningなしに完了した。
- [x] 一時Release CTestを30/30成功させた。
- [x] Client／Agent候補のVersionが`0.7.13.0`で一致した。
- [x] 候補のSHA-256と未署名状態を確認した。
- [x] 実行中Client停止後に標準`out`へVersion `0.7.13.0`を生成する。
- [x] 標準`out`を対象にRelease CTestを30/30成功させた。
- [x] ユーザー実機でVersion `0.7.13.0`の起動と通常接続を確認した。
- [ ] 自然再発時に完全一致した限定再試行を確認する。

標準`out`の実行ファイルは動作中Clientを無断終了せず、停止連絡後に同じ
Version `0.7.13.0`へ更新した。検証候補と最終成果物の詳細は
`work/analysis/20260731_phase9_r003_prelogin_ssl_retry_local_validation.md`へ記録する。

## Rollback

retry policy、timer、固定診断event、status resource、Version、test、関連記録だけを
次の作業前archiveから戻す。

`work/tmp/20260731_182839_before_prelogin_ssl_retry.zip`
