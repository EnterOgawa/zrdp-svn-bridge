# ZRdp SVN Bridge 0.7.14 リリースノート

- 対象Version: `0.7.14.0`
- 配布区分: 社内試行候補
- 更新日: 2026-08-03

## 概要

ZRdp `0.7.14.0`は、Version `0.7.13.0`で確認された長時間RDP接続後の終了残留と、
ログイン前の限定されたActiveX接続失敗を改善する社内試行候補である。RDPの
credential／certificate UI互換性だけをClient側で追加調整し、SVN／Trac Bridge、
Proxy、Agent allowlist、wire protocolの機能境界は変更していない。

## 修正内容

- Session Windowの閉じる操作を、通常の「切断」buttonと同じ
  `Disconnect()`／`OnDisconnected`経路へ統一した。
- 接続中ActiveXを`OnConfirmClose`直後に破棄せず、切断完了後にWindowを破棄する。
- 既に切断中の場合は二重Disconnectを行わず、進行中の完了eventを待つ。
- 未接続の場合は従来どおり直ちにWindowを閉じる。
- 未接続、接続中、切断中の終了開始policyを回帰CTestへ追加した。
- 認証UIへ到達する前の`263/0`完全一致だけを2秒後、5秒後に有限再試行し、
  Microsoft認証UIを表示した試行は自動retryしない。
- `RedirectionWarningDialogVersion=1`と`AuthenticationLevel=2` readbackを接続前へ
  追加し、警告の自動承認やserver認証の緩和を行わない。
- R-006追加候補の`ShowRedirectionWarningDialog=TRUE`は既存接続を壊したため撤回し、
  回帰撤回版で従来接続先が復旧することをユーザー実機で確認した。
- R-007候補では`UIParentWindowHandle`のsession top-level Window指定を外し、
  credential UIと後続certificate UIをActiveX Control Windowの既定親へ委譲した。
- R-008候補ではcertificate警告拒否後の`263/0`を限定分類し、重複errorを表示せず
  対象session Windowだけを閉じる。警告表示、検証、利用者判断は変更しない。

## 検証状態

- Visual Studio 2022 x64 Debug build: 成功、warning 0
- Debug CTest: 30／30成功
- Visual Studio 2022 x64 Release build: 成功、warning 0
- Release CTest: 30／30成功
- 署名後resource／portable binary contract: 2／2成功
- Client／Agent File Version、Product Version: `0.7.14.0`
- 実RDPでの接続中Window終了とprocess消滅: 2026-07-31ユーザー実機確認成功
- 終了logの`rdp.event.disconnected`と`process.stopping`: 確認成功
- 現在のR-008 Client Authenticode: Windows trust `Valid`、SHA-256、RFC 3161
- 既存Agent Authenticode: Windows trust `Valid`
- R-008署名前Client SHA-256: `6DEFEA345BEE01379E0F3FA3546FC78586F8C59FEAC6CD4ED8EB433317E1259D`
- R-008署名済みClient SHA-256: `6743D2B9F4200F36D12B3D2891A9B92842747C48D4ECEA46F170A8C2ACC37165`
- R-008でAgent SHA-256不変: `E8E245EAC803D16FDE8EBA2040A205F808169981B096834F97836F5DA2DB61A2`

Version `0.7.13.0`では実RDP、実SVN read／write、実Mylyn同期が成立している。
終了経路の`0.7.14.0`は実機受入済みである。R-007 Clientはcertificate確認が必要な
接続先で警告の明示承認後に`OnLoginComplete`へ到達し、従来接続先も回帰なく接続した。
R-008 ClientはDebug／隔離Release各30 CTestを通過し、警告の「いいえ」後に追加error
なしで対象session Windowだけが閉じることを実機確認した。受入後Clientは既存Agentと
同じ署名者で再署名した。残る実RDP Gateは署名後Clientの起動、RDP、Bridge、Proxyである。

## 配布前Gate

1. `ZRdpSvnBridge.exe`で実RDPへ接続する。
2. Session Windowの閉じるbuttonを押して切断確認へ同意する。
3. Session Windowと接続管理Windowを閉じる。
4. タスクマネージャーで`ZRdpSvnBridge.exe`が残留しないことを確認する。
5. R-008 Clientで従来接続先へ回帰なく接続できることを確認する。
6. certificate確認が必要な接続先で警告の「いいえ」後に追加errorが出ないことを確認する。
7. 署名後Clientで起動、RDP、Bridge、Proxyを確認する。

2026-07-31、手順1から4までをユーザー環境で完了し、署名前の実機Gateを通過した。
2026-08-01、当時のClient／Agent署名と静的検証を完了し、署名後の起動、RDP接続、
Bridge Ready、Proxy経由のread-only操作もユーザー環境で成立した。2026-08-03の
R-008 Clientは警告取消経路の受入と再署名を完了した。残る配布前確認は、
署名後Clientのsmoke test、組織固有の署名規定確認、最終文書受入、
配布package確認である。

署名手順と確認項目は[社内配布・署名確認書](release-checklist.md)を参照する。

## Security境界と既知の制約

Loopback限定Proxy、Agent完全一致allowlist、HTTP `CONNECT`拒否、credential／payload
非記録、Zscaler／Firewall／GPO非変更を維持する。その他の機能と制約は
[Version 0.7.13リリースノート](release-notes-0.7.13.md)および
[既知の制約](known-limitations.md)を参照する。
