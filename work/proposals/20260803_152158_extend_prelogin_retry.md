# 回帰修正R-005 ログイン前SSL handshake再試行の段階化

## 対象

- Client `ZRdpSvnBridge.exe`のログイン前RDP lifecycle
- `src/client/app/pre_login_connection_retry.h`
- `src/client/app/main_window.cpp`／`main_window.h`
- `tests/unit/session_disconnect_policy_test.cpp`
- ADR、RDP、security、diagnostics、test planの関連記述

Agent、Static Virtual Channel、Application Protocol、localhost Proxy、SVN／Trac
中継処理は変更しない。配布用Release `ZRdpSvnAgent.exe`は再ビルドしない。

## 変更理由

同一の新規接続先に対し、ZRdpはログイン前の`discReason=263`、
`ExtendedDisconnectReason=0`で初回と一回目の再試行が終了した。一方、同じ端末の
Microsoft RDP ClientもSSL handshake継続失敗`0x80004005`を二回記録した後、
三回目の接続処理で成功した。

Windowsの永続RDP接続履歴と保存済み`TERMSRV` credentialは、Microsoft RDP
Clientの成功後も対象接続先について存在しなかった。この観測結果から、永続cacheを
作成または変更するのではなく、既知signatureだけを合計三回まで段階的に再試行する。

## 修正方針

- 初回接続に加え、再試行上限を二回とする。
- 一回目は2,000 ms後、二回目は5,000 ms後に開始する。
- `login未完了`、非local切断、credential UI非取消、Window非終了中、
  `discReason=263`、`ExtendedDisconnectReason=0`の完全一致を維持する。
- 三回目も同じsignatureで失敗した場合は上限到達として従来のerrorを表示する。
- timer確保、設定再適用、同期的`Connect()`のいずれかが失敗した場合は追加試行しない。
- passwordを取得、保存、log出力せず、各接続試行の認証をMicrosoft標準UIへ委譲する。

変更要旨は次のとおりとする。

```diff
- maximum retries: 1
- retry delays: 2,000 ms
+ maximum retries: 2
+ retry delays: 2,000 ms, 5,000 ms
```

## 影響

### ActiveX／COM

既存のsession専用STAとWindow timerを使用し、COM callback内から`Connect()`を
呼ばない。二回目の予約でも同じ検証済み設定を再適用する。

### Security boundary

NLA、CredSSP、server certificate検証、認証警告、credential保存方針を変更しない。
認証拒否を表す別reason、利用者取消、成立済みsession切断は再試行対象外のままとする。
無制限再試行、`mstsc.exe` fallback、registry／credential cache操作を追加しない。

### Protocol／Agent／Proxy

wire format、protocol version、SVC名`ESVNBRG`、Agent allowlist、Proxy port、HTTP
処理に変更はない。Agent配布物の更新は不要である。

## Test

- retry index 0が2,000 msを選ぶ。
- retry index 1が5,000 msを選ぶ。
- retry index 2以降が`Exhausted`となる。
- login完了、local切断、credential取消、終了中、reason／extended reason不一致を
  引き続き`None`とする。
- Debug／ReleaseのClient targetをwarningなしでbuildする。
- Client関連を含む全CTestを成功させる。
- Release build前後で既存`ZRdpSvnAgent.exe`のSHA-256が変化しないことを確認する。
- 実機では同じ自然発生接続先に対し、三回目までに接続できることを確認する。

## Rollback

再試行上限を一回、delayを2,000 ms一件へ戻し、R-005のtest／文書差分を除去する。
Agent、Protocol、Proxyのrollbackは不要である。
