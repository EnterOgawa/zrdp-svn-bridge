# ActiveX既定の認証ダイアログ親Windowを使用する修正提案

## 1. 観測事象

R-006回帰撤回版は従来接続先へ接続できることを実機確認できた。一方、server
certificateを利用者が確認する必要がある接続先では、credential UIを閉じた後に
二回目の`OnAuthenticationWarningDisplayed`と
`OnAuthenticationWarningDismissed`がほぼ同時に発生し、`discReason=263`、
`ExtendedDisconnectReason=0`で終了する。

Windows RDP ClientのOperational logでは、ZRdp側の終了に対応して
`TsSslEventHandshakeContinueFailed (0x80004005)`が記録された。Windows標準
Remote Desktop Clientでは同じ接続先でcertificate警告を利用者が明示承認した後、
次のSSL handshakeへ進み接続できる。この差から、network、credential、認証強度を
変更せず、ActiveX containerが指定する認証dialogの親Window境界を次の候補とする。

## 2. 変更対象

- `src/client/rdp/rdp_control_host.cpp`
  - `RdpControlHost::Impl::Create`
  - `RdpControlHost::Impl::ConfigureDialogParent`の削除
- `src/common/diagnostic_log.h`／`.cpp`
  - ActiveX既定親Windowを選択したことだけを示す固定event
- `tests/unit/diagnostic_log_test.cpp`
  - 固定event語彙と自由形式非記録の確認
- `docs/adr/0010-activex-certificate-warning-dialog-compatibility.md`
  - 次候補、security境界、実機Gate
- `docs/rdp-activex.md`、`docs/diagnostics.md`、`docs/security.md`、
  `docs/test-plan.md`、`docs/roadmap.md`、`docs/known-limitations.md`
  - 実装契約、回帰撤回確認、未完了の実機確認

## 3. 実装方針

現在はActiveX生成直後に`IMsRdpClientNonScriptable2::UIParentWindowHandle`へ
session top-level Windowを明示設定している。Microsoft公開仕様では、このpropertyを
設定しない場合の既定親はActiveX ControlのWindowである。本候補では明示setterを
呼ばず、credential UIから続くcertificate UIを含むmodal ownershipをActiveXの
既定処理へ一貫して委譲する。

ZRdpが認証warning event中にActiveX childへfocusを送り直さない既存保護は維持する。
`RedirectionWarningDialogVersion=1`、`AuthenticationLevel=2`のsetter／readback、
CredSSP、NLA、証明書検証、認証UI表示後の自動retry禁止も変更しない。

診断logには接続先、certificate、利用者の選択を記録せず、固定event
`rdp.activex_default_dialog_parent_selected`だけを一ActiveX生成につき一回記録する。

## 4. Security boundaryと失敗時動作

- certificate警告を抑止、自動承認、記憶しない。
- `AuthenticationLevel=0`、NLA／CredSSP無効化を行わない。
- certificate store、registry、GPO、credential manager、RDP cacheを変更しない。
- 親Windowの変更は表示ownershipだけに限定し、接続先設定やcredentialを変更しない。
- ActiveX既定親で警告が表示されない場合も自動fallbackや`mstsc.exe`起動を行わず、
  `263/0`を従来どおり利用者へ報告する。

## 5. 他境界への影響

- ActiveX／COM: 接続前の`UIParentWindowHandle` setterだけを削除する。
- Virtual Channel: `ESVNBRG`、frame、queue、Bridge状態に変更しない。
- HTTP Proxy: listener、session、port割当、HTTP処理に変更しない。
- Agent Socket: Agent、allowlist、upstream接続に変更しない。
- Protocol compatibility: wire format、version、message typeに変更しない。
- 配布範囲: `ZRdpSvnBridge.exe`だけを更新し、Agentは変更しない。

## 6. Test観点

- system ActiveXを既定dialog親のまま二instance生成、破棄できること。
- 接続前security設定、SVC登録、表示mode、音声設定のsmoke testが従来どおり成功すること。
- 固定eventが出力され、接続先や認証情報をlog APIへ渡さないこと。
- Debug／Releaseの全CTestを成功させること。
- Release Clientだけを更新し、AgentのSHA-256が作業前後で一致すること。
- 実機では先に従来接続先への回帰がないことを確認すること。
- 問題接続先ではcredential UI後にcertificate警告が操作可能な状態で表示され、
  「はい」でdesktopへ到達し、「いいえ」では追加password promptなしに終了すること。

## 7. Rollback

`UIParentWindowHandle`の明示setterと従来の固定eventを戻す。永続設定、接続履歴、
protocol、Agentにmigrationはない。作業前に退避するR-006回帰撤回Clientへ
`ZRdpSvnBridge.exe`を差し替えるだけでも復旧できる。

## 8. 参考

- Microsoft `IMsRdpClientNonScriptable2::UIParentWindowHandle`
- Microsoft `IMsTscAxEvents::OnAuthenticationWarningDisplayed`
- ADR 0010

## 9. 実機結果

2026-08-03、問題接続先でcredential UI後のcertificate警告が操作可能となり、利用者の
明示継続後に`OnConnected`、`OnLoginComplete`へ到達した。固定logでは二つ目のwarningが
約4秒間維持され、従来の約1 ms即時dismissと`263/0`は発生しなかった。このため
ActiveX既定親の採用目的は成立した。

残る確認は、同じR-007 Clientでの従来接続先の回帰確認、certificate警告の取消経路、
および接続後Bridge機能である。
