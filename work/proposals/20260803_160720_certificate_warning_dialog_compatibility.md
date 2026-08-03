# 証明書警告ダイアログ互換性の修正提案

## 1. 観測事象

新しい接続先へのZRdp接続では、Microsoft認証UIを閉じた後に二回目の
`OnAuthenticationWarningDisplayed`と`OnAuthenticationWarningDismissed`が
ほぼ同時に発生し、`discReason=263`、`ExtendedDisconnectReason=0`で終了した。
R-005により同じ処理を二回再試行したため、利用者はpasswordを合計三回入力した。

同じ接続先へ`mstsc.exe`で接続すると、credential入力後にserver certificateの
標準警告が表示され、利用者が明示的に継続を選ぶとdesktopへ到達した。この比較から、
transport到達性やcredentialそのものではなく、ZRdpがhostするActiveXで接続前の
security dialogが利用者操作可能な状態を維持できていない範囲へ原因を限定する。

## 2. 変更対象

- `src/client/rdp/rdp_control_host.cpp`
  - `RdpControlHost::Impl::ApplyConnectionSettings`
  - `RdpControlHost::Impl::ConfigureConnectionSecurityDialog`
- `src/client/app/main_window.cpp`
  - `MainWindow::OnRdpEvent`
  - `MainWindow::HandleMessage`
  - `MainWindow::BeginConnectionAttempt`
- `src/client/app/main_window.h`
  - session単位の認証dialog状態
- `src/client/app/pre_login_connection_retry.h`
  - 認証UI表示後を自動retryから除外する純粋policy
- `src/common/diagnostic_log.h`／`.cpp`
  - 固定eventと数値fieldだけの設定結果
- `tests/unit/rdp_host_smoke_test.cpp`
  - 現在のsystem ActiveXで接続前設定が成功すること
- `tests/unit/session_disconnect_policy_test.cpp`
  - 認証UI表示後に自動retryしないこと
- `tests/unit/diagnostic_log_test.cpp`
  - 新しい固定event語彙と自由形式非記録
- `docs/adr/0010-activex-certificate-warning-dialog-compatibility.md`
  - 設計判断、制約、rollback
- `docs/rdp-activex.md`、`docs/security.md`、`docs/diagnostics.md`、
  `docs/test-plan.md`、`docs/roadmap.md`、`docs/known-limitations.md`
  - 実装契約と実機Gate

## 3. 実装方針

### 3.1 Windows 2026 security dialog互換設定

Microsoftが2026年4月更新後のActiveX application向けに公開した
`IMsRdpExtendedSettings::Property`の
`RedirectionWarningDialogVersion`へ、接続前に`VT_UI4`の`1`を設定する。
これは以前のconnection security dialog表示へ戻す互換設定であり、警告の抑止、
certificateの信頼登録、自動承認ではない。

propertyまたはinterfaceを持たない旧system ActiveXでは、そのsystemの既定dialogを
維持する。propertyを認識した後の予期しない設定失敗は、UIを欠いたままConnectせず
fail-closedにする。write-only propertyのため、未文書化readbackは行わない。

### 3.2 server認証levelのreadback

既存の`AuthenticationLevel=2`設定後に同propertyをreadbackし、完全一致を確認する。
値`2`はserver認証を試み、失敗時に利用者へ取消／継続を選ばせるMicrosoft標準方針で
ある。`0`への緩和、`1`へのhard-fail、自動承認は行わない。

### 3.3 dialog中のfocus保護

`OnAuthenticationWarningDisplayed`から
`OnAuthenticationWarningDismissed`までをsession単位で保持する。この期間に親Windowが
`WM_SETFOCUS`を受けても、ActiveX childへ`SetFocus`を送り直さない。credential dialog
終了直後に続くcertificate dialogからZRdpがfocusを奪う競合を避け、dialogのmodal
所有関係はMicrosoft Controlへ委譲する。

### 3.4 認証UI表示後の自動retry禁止

一回の接続試行でMicrosoft認証dialogが表示された場合、その後の`263/0`をR-005の
自動retry対象から除外する。利用者のpassword入力、Cancel、certificate警告での
継続可否を別のConnectで自動反復しない。R-005は認証UIへ到達する前の完全一致失敗に
だけ残し、2,000 ms／5,000 msと最大二回の有限上限は変更しない。

## 4. Security boundaryと失敗時動作

- NLA、CredSSP、`AuthenticationLevel=2`、server certificate検証を維持する。
- certificate警告はMicrosoft標準UIで利用者が明示判断し、ZRdpは結果を代行しない。
- certificate store、registry、GPO、credential managerを変更しない。
- certificate名、接続先、domain、user名、credentialをlogへ追加しない。
- 互換propertyの予期しない失敗時はConnectを開始せず、固定HRESULTだけを記録する。
- 認証UI表示後に接続が成立しなければ一回で停止し、従来の分類済みerrorを表示する。

## 5. 他境界への影響

- ActiveX／COM: 接続前property、認証event状態、focus routingだけを変更する。
- Virtual Channel: `ESVNBRG`登録、frame、queue、Bridge状態に変更しない。
- HTTP Proxy: listener、session、port割当、HTTP処理に変更しない。
- Agent Socket: Agent、allowlist、upstream接続に変更しない。
- Protocol compatibility: wire format、version、message typeに変更しない。
- 配布範囲: `ZRdpSvnBridge.exe`だけを更新し、Agentの再buildを不要とする。

## 6. Test観点

- 現system ActiveXで互換propertyを含む`ApplyConnectionSettings`が成功する。
- `AuthenticationLevel=2`のsetterとreadbackが一致しない場合はfail-closedとなる。
- 認証UI未表示の`263/0`だけが既存の2秒／5秒retryを選ぶ。
- 認証UI表示後の`263/0`、credential取消、local切断、成立済みsessionはretryしない。
- Debug／Releaseの全CTestを成功させる。
- ReleaseではClientだけを更新し、AgentのSHA-256が作業前後で一致する。
- 実機ではcredential入力後にcertificate警告が表示され、「はい」で接続、
  「いいえ」で追加password promptなしに終了することを確認する。

## 7. Rollback

ActiveX互換property設定、認証level readback、dialog状態flag、retry除外引数を同時に
戻す。永続設定、履歴形式、protocol、Agentにmigrationはないため、以前のClient
binaryへ差し替えるだけでもrollbackできる。

## 8. 参考

- Microsoft `IMsRdpExtendedSettings::Property`
- Microsoft `IMsRdpClientAdvancedSettings4::AuthenticationLevel`
- Microsoft `IMsTscAxEvents::OnAuthenticationWarningDisplayed`
- Microsoft `IMsRdpClientNonScriptable2::UIParentWindowHandle`
- ADR 0009

## 9. 初回実機結果と追加修正

2026-08-03の初回実機確認では、固定logに
`dialog_version=1 enabled=1`と`authentication_level=2`が記録され、両設定は成功した。
credential UIは約6秒表示されたが、続く二回目の認証warning eventは約1 msで
dismissされ、`263/0`で終了した。認証UI表示後のretry除外は機能し、password要求は
一回で停止したが、certificate警告の利用者操作は未成立である。

`IMsRdpClientNonScriptable3::ShowRedirectionWarningDialog=TRUE`を追加候補として実装したが、
実機では既存接続先を含め`Connect()`が`S_FALSE`、`disconnect code=1`で即時終了した。
このpropertyはRDP fileのredirection warning向けであり、今回のcertificate warningへ
適用できない。既存接続を壊すため不採用として実装と固定eventを撤回した。

撤回後は初回R-006候補と同じ、Version 1設定、`AuthenticationLevel=2`、dialog親Window、
focus保護、認証UI表示後のretry除外へ戻す。次の調査はWindows event logで確認した
`TsSslEventHandshakeContinueFailed (0x80004005)`とdialog親Window境界へ限定する。

回帰撤回後Clientでは従来接続先が再び接続できることをユーザーが確認した。問題接続先
だけは同じ`263/0`で残ったため、次候補は
`20260803_165308_use_activex_default_authentication_dialog_parent.md`へ分離した。
