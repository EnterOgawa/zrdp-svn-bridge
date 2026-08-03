# certificate警告の明示拒否後に重複errorを表示しない修正提案

## 1. 観測事象

R-007によりcredential UI後のserver certificate警告が操作可能となり、「はい」では
desktopへ接続できた。一方、「いいえ」を選ぶとMicrosoft Controlは
`discReason=263`、`ExtendedDisconnectReason=0`を通知し、ZRdpが通常接続失敗として
追加のRDP接続error dialogを表示した。

利用者はMicrosoft標準certificate警告で既に接続拒否を明示しているため、同じ結果を
ZRdp errorでもう一度通知する必要はない。ただし`263/0`だけでは通常のSSL handshake
失敗と区別できないため、reasonだけを広く正常扱いしない。

## 2. 変更対象

- `src/client/app/session_disconnect_policy.h`
  - certificate警告拒否を判定するpure policy
- `src/client/app/main_window.cpp`／`.h`
  - 一接続試行内のMicrosoft認証warning表示段階と取消後のWindow終了
- `src/common/diagnostic_log.h`／`.cpp`
  - 固定数値field`security_warning_cancelled`
- `tests/unit/session_disconnect_policy_test.cpp`
  - 完全一致と全除外条件
- `tests/unit/diagnostic_log_test.cpp`
  - 固定fieldと自由形式非記録
- `docs/diagnostics.md`、`docs/rdp-activex.md`、`docs/security.md`、
  `docs/test-plan.md`、`docs/roadmap.md`、`docs/known-limitations.md`
  - 実装契約、診断、実機Gate

## 3. 実装方針

ActiveX eventはcertificate警告の「はい」／「いいえ」を直接通知しない。このため、
次の条件がすべて一致した場合だけ、certificate警告の拒否または閉じる操作として
分類する。

1. 現在試行で`OnLoginComplete`へ到達していない。
2. ZRdp自身の切断要求ではない。
3. credential UI取消の既存完全一致ではない。
4. 一接続試行で`OnAuthenticationWarningDisplayed`を二回以上受信した。
5. `discReason=263`である。
6. `ExtendedDisconnectReason=0`である。

一回目はcredential UI、二回目は実機で確認したcertificate UIに対応する。表示回数は
二回で飽和させ、異常event反復でcounterをoverflowさせない。接続試行開始時に0へ戻す。

完全一致時は追加errorをqueueせず、利用不能な未接続session Windowを閉じる。接続管理
Windowと他sessionは維持する。固定logには`security_warning_cancelled=1`だけを記録し、
certificate情報や利用者の選択内容は記録しない。

## 4. Security boundaryと失敗時動作

- certificate検証、警告表示、利用者判断をMicrosoft Controlへ維持する。
- NLA、CredSSP、`AuthenticationLevel=2`を変更しない。
- certificate警告を自動承認、記憶、登録しない。
- 一回しか認証warningがない場合、reason／extended reasonが異なる場合、ログイン済み、
  local切断は通常の既存経路を維持する。
- 完全一致でerror dialogを抑止しても、固定数値の切断recordは診断logへ残す。

ActiveXは利用者の選択値を公開しないため、「はい」の直後に同じ`263/0`で失敗した場合も
この分類へ一致し得る。その場合でもMicrosoft警告自体は表示済みであり、固定logには
切断値を残す。より広いreasonや認証UI未表示の失敗は抑止しない。

## 5. 他境界への影響

- ActiveX／COM: event購読、設定、認証強度は変更せず、既存eventのUI分類だけを追加する。
- Virtual Channel: `ESVNBRG`、Bridge、frame、queueに変更しない。
- HTTP Proxy: listener、session、port割当に変更しない。
- Agent Socket: Agent、allowlist、upstream接続に変更しない。
- Protocol compatibility: wire format、version、message typeに変更しない。
- 配布範囲: `ZRdpSvnBridge.exe`だけを更新する。

## 6. Test観点

- 未ログイン、非local、credential取消でない、warning二回以上、`263/0`だけがtrue。
- warning一回、ログイン済み、local切断、credential取消、reason違い、extended reasonありは
  すべてfalse。
- 完全一致時はretry、追加error表示を行わず、対象session Windowだけを閉じる。
- 固定logへ`security_warning_cancelled=1`を記録する。
- Debug／Release全CTestと標準`out`成果物契約を成功させる。
- Agent SHA-256が作業前後で一致する。
- 実機で「いいえ」後にerror dialogなしでsession Windowが閉じる。
- R-007の「はい」、従来接続先、Agent／Proxyに回帰がない。

## 7. Rollback

warning表示回数、pure policy、固定field、取消時の早期Window終了を戻す。永続設定、
接続履歴、protocol、Agentにmigrationはない。作業前R-007 Clientへ
`ZRdpSvnBridge.exe`を差し替えるだけでも復旧できる。

## 8. ローカル検証結果

- Debug CTest: 30／30成功
- 隔離Release CTest: 30／30成功
- 標準`out` resource／portable binary contract: 2／2成功
- Client Version: `0.7.14.0`
- Client size: 677,888 byte
- Client SHA-256: `6DEFEA345BEE01379E0F3FA3546FC78586F8C59FEAC6CD4ED8EB433317E1259D`
- 署名前Client Authenticode: `NotSigned`
- Agent SHA-256: `E8E245EAC803D16FDE8EBA2040A205F808169981B096834F97836F5DA2DB61A2`
  （作業前後で不変）

2026-08-03、実機でcertificate警告の「いいえ」を選び、追加errorなしで対象session
Windowだけが閉じる期待動作を確認した。受入後Clientを既存Agentと同じ署名者で再署名し、
Windows trust `Valid`、SHA-256 signature、RFC 3161 timestamp、Version `0.7.14.0`を
静的検証した。署名済みClientのSHA-256は
`6743D2B9F4200F36D12B3D2891A9B92842747C48D4ECEA46F170A8C2ACC37165`である。
残るGateは署名後Clientの起動、RDP、Bridge、Proxy smoke testである。
