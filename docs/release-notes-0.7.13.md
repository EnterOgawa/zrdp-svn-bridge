# ZRdp SVN Bridge 0.7.13 リリースノート

- 対象Version: `0.7.13.0`
- 配布区分: 社内試行版
- 更新日: 2026-07-31

## 概要

ZRdp `0.7.13.0`は、通常のRDP操作と、RDP Static Virtual Channelを利用した
allowlist限定HTTP SVN／承認済みTrac中継を一つのClientへ統合した社内試行版である。

Phase 7／8の実環境確認では、既存SVN URLを変更せず、Repository Explorer、Log、
Diff、Checkout、Update、Commit、Add、Delete、Move、conflict、Lock、Unlockが
成立した。Mylyn Tracの設定検証と同期も成立している。

## 主な機能

- Windows Remote Desktop ActiveXによるRDP接続
- Windows標準credential UI、NLA／CredSSP、clipboard
- 等倍表示、Smart Sizing、動的解像度
- 全画面buttonと`Ctrl+Alt+Break`
- マイク転送と3種類のリモート音声設定
- DPAPI保護された名前付き接続profile
- 独立Windowによる複数RDP session
- Sessionごとのloopback HTTP Proxy port
- 最大8同時HTTP SVN／Trac stream
- AgentのIPv4／port完全一致allowlist
- Agent異常終了のhealth検出と同一RDP session内の再起動
- Payloadを記録しないClient／Agent別診断log

## Version 0.7系の主な改善

- 接続管理WindowとRDP Session Windowを分離した。
- SessionごとにActiveXとCOM STAを分離し、一方の終了が別Windowを停止させる問題を
  修正した。
- 接続先に任意の名前を付け、接続情報と音声設定を一組で復元できるようにした。
- RDP Windowの手動resizeで表示領域のアスペクト比を維持するようにした。
- 動的解像度をログイン完了後に適用し、一時的な`E_UNEXPECTED`だけを有限再試行する
  ようにした。
- 成立済みRDP sessionの最終外部切断ではerror dialogを出さず対象Windowを閉じる
  ようにした。
- Session Windowへ全画面buttonを追加し、動的解像度を全画面monitorとWindowへ
  再追従させた。
- 観測済み`discReason=263`／`ExtendedDisconnectReason=0`完全一致のログイン前失敗
  だけを2秒後に一度再試行するようにした。
- Client／Agent共通iconを枠なしの明色デザインへ変更した。

## Security境界

本Versionでも次を維持する。

- Client Proxyは`127.0.0.1`だけでlistenする。
- Agentはallowlist完全一致後だけoutbound Socketを作成する。
- HTTP `CONNECT`、HTTPS、hostname、DNS、任意TCP forwardを拒否する。
- RDP、SVN、Trac credentialとpayloadを保存またはlog出力しない。
- Zscaler、Firewall、GPO、certificate、OS Proxyを変更または回避しない。
- Agentはservice、driver、inbound portを使用しない。

## 配布と更新

Installerは使用しない。`ZRdpSvnBridge.exe`と`ZRdpSvnAgent.exe`を同じVersionの組で
置き換える。実環境用`agent.ini`は支給PC側で維持する。

社内配布前に両EXEのAuthenticode署名、Version、hashを確認する。PDB、log、dump、
実環境設定、credentialは配布物へ含めない。

更新手順は[利用手順書](user-guide.md)を参照する。

## 運用観察中の項目

- Mylyn定期同期を含む長時間連続利用
- 二つのRDP sessionでAgent／Proxyを同時利用する条件
- 接続先GPO／device別のマイクとリモート音声
- 辺／角resize、最大化、Windowsスナップ、全画面の組合せ
- 最終iconのExplorer／taskbar／title bar表示
- 接続先再起動または通信断後の対象Window終了
- まれなログイン前`263/0`失敗の自然再発と一回復旧

問題が判明した場合は、合格済み機能全体を無効化せず、該当項目だけを個別に
再オープンする。

## 既知の制約

- 対象はIPv4 literalの`http://`方式SVN／承認済みTracだけである。
- Agent設定変更にはAgent再起動が必要である。
- RDP／Agent切断時に途中HTTP requestをresumeしない。
- 一つのClient processは最大16 RDP session、一sessionは最大8 streamである。
- Audio設定は接続中に変更できない。
- Release binary以外の詳細は[既知の制約](known-limitations.md)を参照する。
