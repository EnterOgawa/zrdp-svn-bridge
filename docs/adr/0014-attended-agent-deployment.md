# Client 操作による確認付き Agent 配置

> Status: Historical / amended. 旧手動配置方式の歴史的判断。通常入口・署名条件・上書き方針は後続ADRで置き換えられている。現行は [ADR 0021](0021-optional-agent-signing.md)／[0022](0022-connection-agent-lifecycle.md)。

日付: 2026-10-08

状態: Release buildへ実装。ビルド更新後の再署名と実RDPでのファイル転送確認は別Gate。
初回配置の通常入口は2026-10-09の承認により[自動初期配置](0016-initial-agent-bootstrap.md)へ変更する。本ADRは旧手動方式の実装境界として保持し、自動方式から無断fallbackしない。
2026-10-09のセッション入口・起動操作追加は[ADR 0015](0015-session-agent-controls.md)に記録する。

## 背景

Agentが存在しない接続先へ、Agent自身を受信者とする`ESVNBRG`では初回配置できない。
既存RDPのファイルクリップボードを使用し、Agentと承認済みINIを配置する。
配置操作ではAgent起動を行わない。別の確認付き起動操作はADR 0015に分離する。
remote shell、drive redirection、サービス登録、転送制限の変更は行わない。

## 決定

ClientのAgent設定画面に`接続先へ配置...`を追加する。認証済みのRDPが一件だけのときに
接続名と実際の接続先を表示して確認し、Client隣接の固定名`ZRdpSvnAgent.exe`と、
承認済みdraftから生成した`agent.ini`を準備する。設定名とRDP履歴の対応は推定しない。

Agentはx64 console PE、正式製品名、OriginalFilename、Clientと同一製品versionを確認する。
Windows trust、SHA-256署名、timestamp、Clientとの署名公開鍵一致を既存の検証で確認する。
未署名buildや他製品は拒否する。署名検証と読込みの間もAgentをwrite/delete共有なしで
保持し、reparse point、network volume、複数hard link、16 MiB超過を拒否する。

設定workerの要求一件・結果一件を再利用する。設定は既存Agent parserと全entryの承認で
再検証し、backendの返却INIも要求snapshotと比較する。平文INIの配置用一時fileは作らない。
最大16 MiBのAgentと64 KiB以下のINIを不変memoryで保持し、OLE`IDataObject`が
`FileGroupDescriptorW`と`FileContents`のreadonly`IStream`で二ファイルだけを提供する。
内容streamの生成でAgent全体を再コピーしない。最後のpackage参照でINIをゼロ化する。
CLIPRDR、file転送の通信・暗号化・再送はWindows標準ActiveXに任せる。

利用者が接続先のExplorerで`%LOCALAPPDATA%\ZRdp`を作成して開き、ファイル一覧に
フォーカスを移したことを確認する。Clientの配置画面から、対象session所有STAへ固定の
Ctrl+V要求だけをmarshalする。送信直前に認証世代とclipboard世代を再確認し、
再接続したsession、終了したsession、別のclipboardへキーを送らない。
この経路はADR 0005の既存session専用STA構成に従い、workerやmanagerからCOMを直接呼ばない。

## 共有と失敗の境界

Windows clipboardは他アプリや別RDPと共有される。置換の確認は既定「いいえ」とし、
設定を含むこと、他RDPを閉じること、以前のclipboardを復元しないことを説明する。
準備・公開中はClient内の追加RDPを拒否する。終了時は自分のOLE objectである場合だけ
clipboardを消し、別アプリが置換した内容には触れない。消去直前にはWin32のclipboard排他と
世代の再照合で所有判定後の競合も拒否する。判定不能・clear失敗時は公開状態を保持し、
新規接続を許可しない。古いOLE streamは最後の参照までsnapshotを保持する。

clipboard公開とCtrl+VのAPI成功は、接続先での配置成功ではない。最終操作は利用者が
二ファイルと転送終了を確認した旨の応答であり、Clientがremote結果を検証したとは表示しない。
Windowsの上書き確認は自動承認しない。中止時もremoteの既存fileや途中fileを削除しない。
転送中はClientを終了せず、終了・中断を確認してから公開を終える。

GPO等でfile転送が拒否された場合は中止する。テキストclipboardが使えることだけでは
file転送許可の確認としない。別経路、任意コマンド、policy操作へfallbackしない。
RDP画面、`ESVNBRG`、HTTP Proxy、Agent socket、allowlist形式は変更しない。

## 検証と rollback

通常testはfake clipboardと例示IPだけを使い、実clipboardを置換しない。
descriptorの名前とbyte長、NUL入りbinary、readonly stream、seek/index/format拒否、
承認gate、worker再検証、folder確認、二重キー要求、clipboard差替え、clear失敗を確認する。
接続数、認証世代、clipboard世代の拒否条件は入力を固定した単体testで確認する。
2026-10-08のDebug／Release各43 CTestは合格した。画面表示は実設定を読まないfakeで
96 DPIを確認し、所有STA外・未接続ActiveXへの配置キーを拒否することも確認した。

実RDP Gateでは利用者の許可を得た環境で、初回配置、上書き、稼働Agent終了後の置換、
file転送禁止、途中clipboard変更、切断・再接続、keyboard layoutを確認する。
起動と実SVN／Trac操作は配置Gateに含めない。自動testはこのGateの代わりにならない。

Rollbackは配置の追加source・resource・callbackだけを取り除く。更新、Agent設定、
workspace移動の変更は維持する。remoteの二ファイルは利用者が保全した旧版へ戻す。

## API 根拠

[Shell clipboard formats](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard) のvirtual file、
[MS-RDPECLIP Clipboard Format](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpeclip/f293d8e4-7858-4f5f-b00e-8e67323d0538) のFileGroupDescriptorW、
[OleSetClipboard](https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-olesetclipboard) と
[SendKeys](https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpclientnonscriptable-sendkeys) を使用する。
終了時の競合確認は
[OpenClipboard](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openclipboard) と
[GetClipboardSequenceNumber](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getclipboardsequencenumber)
の標準APIに従う。
WindowsのAPI仕様を参照し、外部サンプルcodeは転用していない。
