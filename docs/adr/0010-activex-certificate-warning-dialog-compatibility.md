# ADR 0010: ActiveX certificate警告ダイアログの互換表示

- Status: Accepted
- Date: 2026-08-03
- Amends: ADR 0009

## Context

新しいRDP接続先へのZRdp接続で、Microsoft credential UIの後に
`OnAuthenticationWarningDisplayed`と`OnAuthenticationWarningDismissed`が
ほぼ同時に発生し、`discReason=263`、`ExtendedDisconnectReason=0`で終了した。
R-005の有限retryも同じ経路を反復したため、利用者はpasswordを合計三回入力した。

同じ接続先へWindows標準Remote Desktop Clientで接続すると、credential入力後に
server certificateの標準警告が表示され、利用者が明示的に継続を選んだ後は
desktopへ到達した。したがってcredentialやnetwork到達性を推測で変更せず、
ActiveX container内のsecurity dialog表示とfocus所有へ修正範囲を限定する。

Microsoftは2026年4月のsecurity updateで新しいconnection security dialogを導入し、
ActiveX application向けに`IMsRdpExtendedSettings::Property`の
`RedirectionWarningDialogVersion`を公開した。値`1`は以前のdialog動作を復元し、
既定値`2`は新しいdialogを使用する。

## Decision

- 接続前に`RedirectionWarningDialogVersion`へ`VT_UI4`の`1`を設定する。
- この設定はMicrosoft dialog実装の互換選択だけに使用し、警告を抑止または
  自動承認しない。
- property追加前の旧ActiveXがinterfaceまたはpropertyを返さない既知結果だけは、
  そのsystemの既定dialogを維持して接続設定を続ける。
- propertyを認識した経路の予期しない失敗は、Connect前にfail-closedとする。
- write-only propertyへ未文書化readbackを行わない。
- `AuthenticationLevel=2`をsetter後にreadbackし、完全一致しなければ接続しない。
- `IMsRdpClientNonScriptable2::UIParentWindowHandle`へsession top-level Windowを
  明示設定せず、Microsoft Control Windowの既定親へcredential UIと後続certificate
  UIのmodal ownershipを一貫して委譲する。
- `OnAuthenticationWarningDisplayed`から`Dismissed`まで、親Windowの
  `WM_SETFOCUS`からActiveX childへfocusを移し直さない。
- 現在の一回のConnect試行でMicrosoft認証dialogを表示した場合、その後の
  `263/0`をR-005の自動retry対象から除外する。
- R-005の2,000 ms、5,000 ms、最大二回という有限retryは、認証UIへ到達する前の
  完全一致失敗にだけ維持する。

## Consequences

### Positive

- server certificate警告をWindows標準UIとして利用者へ提示できる候補になる。
- credential dialog終了直後のcertificate dialogからZRdpがfocusを奪う競合を防ぐ。
- credential UIとcertificate UIを同じActiveX既定親に所属させ、container側の
  親Window切替に依存しない。
- 利用者が入力または警告判断を行った接続を、自動retryで最大三回反復しない。
- server authentication、NLA、CredSSPを弱めずにClientだけで修正できる。

### Negative

- 値`1`はMicrosoftが互換目的として提供する以前のdialogであり、将来のWindows
  updateで削除される可能性がある。
- ActiveX内部dialogの最終挙動は実RDP接続でしか確認できない。
- ActiveX既定親はControl Windowであるため、session top-level Windowを明示する場合と
  modal ownerの階層が異なる。
- 認証dialog表示後に真の一時network失敗が起きた場合は自動retryせず、利用者が
  新しい接続を開始する必要がある。

## Security and protocol impact

`AuthenticationLevel=2`、NLA、CredSSP、server certificate検証を維持する。
`AuthenticationLevel=0`、証明書警告の自動承認、certificate store／registry／GPO／
credential managerの変更、`mstsc.exe` fallbackは行わない。certificate名、接続先、
domain、ユーザー名、credentialはlogへ記録しない。

Static Virtual Channel、Application Protocol、Proxy、Agent、allowlist、接続履歴形式、
Proxy port割当に変更はない。Agent配布物の更新は不要である。

## Alternatives

### `AuthenticationLevel=0`へ変更する

server認証を無効化しsecurity boundaryを弱めるため不採用とした。

### `AuthenticationLevel=1`へ変更する

certificate検証失敗時に利用者が明示継続できず、今回のWindows標準Client動作と
一致しないため不採用とした。

### 証明書を自動登録または警告を記憶する

ZRdpが利用者の信頼判断を代行し、実環境certificate情報の保存も必要になるため
不採用とした。

### retry回数をさらに増やす

同じcertificate判断とpassword入力を反復するだけで、account lockoutと操作負担を
増やすため不採用とした。

### `ShowRedirectionWarningDialog=TRUE`を設定する

初回実機結果後に候補実装したが、既存接続先を含め`Connect()`が`S_FALSE`、
`disconnect code=1`で即時終了した。RDP fileのredirection warning向けpropertyであり、
certificate warningの代替にならないため実装から撤回した。

### session top-level Windowを`UIParentWindowHandle`へ明示設定する

credential UI自体は表示できたが、その直後のcertificate warning eventが約1 msで
dismissされ、SSL handshake継続に失敗した。R-007候補では明示setterを外し、Microsoft
Controlの既定親へ一連の認証UIを委譲する。実機結果が不成立の場合はこの候補も撤回し、
ActiveX版Clientの既知制約として設計判断をユーザーへ返す。

## Validation

- system ActiveXで互換propertyを含む接続前設定を適用するsmoke testを行う。
- `UIParentWindowHandle`を明示設定しない状態でActiveXを複数生成、破棄できることを
  smoke testで確認する。
- `AuthenticationLevel=2`のsetter／readbackを同じ接続前境界で確認する。
- 認証UI未表示だけがR-005 retryを選び、表示後は選ばない純粋policyを確認する。
- Debug／Releaseの全CTestを成功させる。
- Release Clientだけを更新し、AgentのSHA-256が変化しないことを確認する。
- 実機でcredential UI後にcertificate警告が表示されることを確認する。
- 警告の「はい」でdesktopへ到達し、「いいえ」では追加password promptなしに
  未接続へ戻ることを確認する。
- 接続後のAgent、Proxy、SVN／Trac、複数session、表示modeを回帰確認する。

## References

- [IMsRdpExtendedSettings::Property](https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpextendedsettings-property)
- [IMsRdpClientAdvancedSettings4::AuthenticationLevel](https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpclientadvancedsettings4-authenticationlevel)
- [IMsTscAxEvents::OnAuthenticationWarningDisplayed](https://learn.microsoft.com/en-us/windows/win32/termserv/imstscaxevents-onauthenticationwarningdisplayed)
- [IMsRdpClientNonScriptable2::UIParentWindowHandle](https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpclientnonscriptable2-uiparentwindowhandle)
- ADR 0009

## Field result

2026-08-03、ActiveX既定親を使用するR-007 Clientでcertificate警告が利用者操作可能と
なり、明示継続後に`OnConnected`、`OnLoginComplete`へ到達した。二つ目のauthentication
warning eventは従来の約1 ms即時dismissから約4秒間の表示へ変化し、`263/0`や自動retryを
挟まなかった。従来接続先もR-007で回帰なく接続した。

警告で「いいえ」を選ぶとControlは`263/0`を返し、ZRdpが重複errorを表示した。R-008は
warning二回以上、login未完了、非local、credential取消以外、`263/0`完全一致に限定して
追加errorを抑止し、対象session Windowだけを閉じる。certificate検証や利用者判断は
変更しない。ユーザー実機でこの期待動作を確認し、受入後Clientを既存Agentと同じ
署名者で再署名した。Windows trust、SHA-256 signature、RFC 3161 timestamp、Version
維持は静的検証済みであり、署名後smoke testをRelease Gateとして残す。
