# ZRdp SVN Bridge 0.7.14 リリースノート

- 対象Version: `0.7.14.0`
- 配布区分: 社内試行候補
- 更新日: 2026-08-01

## 概要

ZRdp `0.7.14.0`は、Version `0.7.13.0`で確認された長時間RDP接続後の終了残留を
改善する候補である。RDP、SVN／Trac Bridge、Proxy、Agent allowlistの機能境界は
Version `0.7.13.0`から変更していない。

## 修正内容

- Session Windowの閉じる操作を、通常の「切断」buttonと同じ
  `Disconnect()`／`OnDisconnected`経路へ統一した。
- 接続中ActiveXを`OnConfirmClose`直後に破棄せず、切断完了後にWindowを破棄する。
- 既に切断中の場合は二重Disconnectを行わず、進行中の完了eventを待つ。
- 未接続の場合は従来どおり直ちにWindowを閉じる。
- 未接続、接続中、切断中の終了開始policyを回帰CTestへ追加した。

## 検証状態

- Visual Studio 2022 x64 Debug build: 成功、warning 0
- Debug CTest: 30／30成功
- Visual Studio 2022 x64 Release build: 成功、warning 0
- Release CTest: 30／30成功
- Client／Agent File Version、Product Version: `0.7.14.0`
- 実RDPでの接続中Window終了とprocess消滅: 2026-07-31ユーザー実機確認成功
- 終了logの`rdp.event.disconnected`と`process.stopping`: 確認成功
- Client／Agent Authenticode: Windows trust `Valid`
- Signature digest: SHA-256、timestamp: RFC 3161
- 署名後File Version／Product Version: `0.7.14.0`

Version `0.7.13.0`では実RDP、実SVN read／write、実Mylyn同期が成立している。
`0.7.14.0`の変更範囲はClientの終了開始経路とそのpolicy testに限定している。

## 配布前Gate

1. `ZRdpSvnBridge.exe`で実RDPへ接続する。
2. Session Windowの閉じるbuttonを押して切断確認へ同意する。
3. Session Windowと接続管理Windowを閉じる。
4. タスクマネージャーで`ZRdpSvnBridge.exe`が残留しないことを確認する。
5. Gate通過後にClient／Agent両EXEへユーザーがAuthenticode署名する。

2026-07-31、手順1から4までをユーザー環境で完了し、署名前の実機Gateを通過した。
2026-08-01、手順5の署名と静的検証を完了し、署名後のClient／Agent起動、RDP接続、
Bridge Ready、Proxy経由のread-only操作もユーザー環境で成立した。残る配布前確認は
組織固有の署名規定確認、最終文書受入、配布package確認である。

署名手順と確認項目は[社内配布・署名確認書](release-checklist.md)を参照する。

## Security境界と既知の制約

Loopback限定Proxy、Agent完全一致allowlist、HTTP `CONNECT`拒否、credential／payload
非記録、Zscaler／Firewall／GPO非変更を維持する。その他の機能と制約は
[Version 0.7.13リリースノート](release-notes-0.7.13.md)および
[既知の制約](known-limitations.md)を参照する。
