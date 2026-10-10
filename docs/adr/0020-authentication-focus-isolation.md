# RDP認証中のフォーカス操作の分離

- Status: Implemented, 実機Gate待ち
- Date: 2026-10-10
- Amends: ADR 0010のfocus契約

## 対象と確認結果

ZRdpでは認証前に`0x800706BE`と`9991/0`が発生し、同時刻のWindows Application Errorに
`CredentialUIBroker.exe`、`Windows.UI.XamlHost.dll`、例外`0xc0000409`が記録された。
Windows標準RDPは、資格情報の入力を要求する`mstsc /prompt`でも接続できた。
Agent起動やBridge交渉より前の失敗であり、対象PCやAgent設定を変更する理由にはならない。

`MainWindow::BeginConnectionAttempt`は`Connect`受付直後に`Focus`を呼び、
`WM_SETFOCUS`も認証前の`Connecting`と`Connected`を同じ経路へ渡していた。
`RdpControlHost::Focus`は警告状態を確認しないため、MainWindow側の警告flagだけでは
event前やdialog間、直接呼出しを保護できない。
このfocus経路の存在はsourceと非接続の実Windowで確認したが、Broker crashとの因果関係は
未確定である。本変更は認証前の介入を除く比較候補であり、接続修復の実機受入は別に行う。

導入済みATLはhostの`WM_CREATE`で`OleInitialize`、`WM_NCDESTROY`で`OleUninitialize`を
呼ぶため、OLE未初期化を原因とする変更は採用しない。

## フォーカスの条件

通常focusの純粋契約`CanFocusRemoteDesktop`は、`LoggedIn`かつ認証警告なしだけを許可する。
未知状態、未認証、再接続中、切断中、失敗は拒否する。
実hostの`Focus`はさらに所有STA、Window生存、親とhostの有効化、
当該sessionがactiveかつforegroundであることを確認する。
拒否時はfocus、入力、接続状態を変更せず、前景化やretryを行わない。

接続開始直後の明示`Focus`を削除する。`WM_SETFOCUS`も同じ認証契約を用い、
未認証sessionから非表示の接続入力欄へ移さない。
`LoginComplete`と`AutoReconnected`はfocus要求をWindow messageへ予約し、
callback外で最新の認証・警告・Window状態を再確認する。
取消、切断、終了後に残ったmessageからfocusを復活させない。

物理keyboard入力、Windows標準認証画面、認証後のAgent固定入力は維持する。
画面をクリックしてfocusを戻す通常操作は変更しない。

## 安全境界

ADR 0010で実機受入済みのActiveX既定dialog親と`RedirectionWarningDialogVersion=1`を維持する。
`AuthenticationLevel=2`、NLA、CredSSP、標準credential prompt、credential保存禁止を維持する。
`9991/0`を取消やcertificate拒否と推測せず、自動retry対象へ追加しない。
警告の自動承認、独自password収集、system DLLの複写、policy変更、mstsc偽装を行わない。

ESVNBRG、protocol、Proxy、Agent、allowlist、配布方式、保存設定、製品versionは変更しない。
修正対象はClientと回帰testであり、Agent更新は不要である。

## 検証と実機Gate

非接続の実ActiveXへの`Focus`と、実session Windowへの`WM_SETFOCUS`は、
修正前にはfixture controlのfocusを奪い、追加回帰testが失敗した。
修正後は同じtestでfocusを維持する。全stateと警告flagの純粋契約、残留focus message、
`9991/0`のretry／取消除外も確認する。これらのtestでは実RDP接続を行わない。

実機ではClient再起動後の初回接続と、同条件の新しい接続がdesktopへ到達することを確認する。
認証取消、certificate拒否、通常keyboard、複数sessionのfocus、Agent起動と
トレイ終了後の二回目起動を回帰確認する。
Brokerの例外や`9991/0`が続く場合、本変更で接続修復が成功したとは扱わない。

## 復旧

変更前の対象sourceと文書、Client／Agent EXEをworkspace内へhash照合済みで保全する。
復旧では本変更だけを比較して戻し、他の未commit変更を維持する。
実機で改善しない場合は結果を記録し、次の比較範囲またはActiveX版の制約を判断する。

## API契約

- [OnAuthenticationWarningDisplayed](https://learn.microsoft.com/en-us/windows/win32/termserv/imstscaxevents-onauthenticationwarningdisplayed)は認証dialog表示前の通知と既定Control親を説明する。
- [SetFocus](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setfocus)はthreadに所属するWindowへのfocus移動と親activationを説明する。
- [OleInitialize](https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-oleinitialize)はSTAとOLE初期化、成功時の対称解放を説明する。

これらの契約は、今回のBroker crash原因や修正による解消を保証しない。
