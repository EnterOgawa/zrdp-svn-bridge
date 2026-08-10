# ZRdp SVN Bridge 0.7.15.0 リリースノート

公開日: 2026-08-10

## 概要

Version `0.7.15.0`は、一時的なnetwork link断でRDP transportが切断された後も、
同じRDP user sessionが存続する間は`ZRdpSvnAgent.exe`を終了させず、Microsoft RDP
Controlの自動再接続後にStatic Virtual Channel `ESVNBRG`を再確立する保守Releaseである。

## 修正内容

- network link断後、旧SVC handleのreadがWin32 error 233となった時点でAgent processが
  終了していた動作を修正した。
- channel断時は旧HTTP stream、upstream Socket、送受信queueをすべて破棄する。
- current RDP user sessionだけを監視し、`WTSActive`へ戻った場合だけ同名SVCを再openする。
- 再openは250 ms、500 ms、1秒、最大2秒の有限backoffとし、失敗logを1、2、4、8、
  ...回目へ間引く。
- 新しい`HELLO`／`HELLO_ACK`が成立してからBridge Readyへ戻す。旧request、旧streamId、
  旧sequenceは再利用しない。
- logoff、非RDP化、`WTSReset`、`WTSDown`では回復せず安全に停止する。

## 互換性とsecurity境界

- Application Protocol、Static Virtual Channel名`ESVNBRG`、`agent.ini`形式、allowlist、
  localhost Proxy方式は変更していない。
- 別RDP sessionの探索、別transport、直接接続、OS Proxy等へのfallbackは行わない。
- Client runtimeのRDP／Proxy処理は変更せず、Client／Agentの配布Versionだけを
  `0.7.15.0`へ揃えた。
- 切断時に処理中だったSVN／Trac操作を自動再送しない。利用者が結果を確認してから
  必要な操作だけを再実行する。

## 検証結果

- Debug CTest: 30／30成功
- 隔離Release CTest: 30／30成功
- 標準Release CTest: 30／30成功
- 署名後resource／portable binary contract: 2／2成功
- Client／Agent File Version／Product Version: `0.7.15.0`
- Client／Agent Authenticode: Windows trust `Valid`
- SHA-256署名、RFC 3161 timestamp、両EXEの署名者一致: 確認済み
- 2026-08-10、ユーザー実機で一時link断後のAgent process維持と、RDP自動再接続後の
  Bridge／Proxy回復を確認済み

Client SHA-256:

`090D937A0AB183AAC37BFF436AFCF44A7940497F2D505947212A69E1D2C1DBB9`

Agent SHA-256:

`9512FA46928604D442DCFE6BCD9901C3CDFF940209D1616DF60BF702599B3A51`

## 更新方法

1. 利用中のClient／Agentを終了する。
2. 実環境用`agent.ini`と`client.ini`を上書きせず保持する。
3. `ZRdpSvnBridge.exe`と`ZRdpSvnAgent.exe`をVersion `0.7.15.0`へ入れ替える。
4. 両EXEのVersionとAuthenticode署名を確認する。
5. RDP接続、Agent接続、Proxy利用可、SVN／Tracの安全な操作を確認する。

## 既知の制約

- 本修正はnetwork link断、NIC、switch、cable等の原因自体を修復しない。
- RDP user sessionが`WTSDisconnected`のまま残る場合、AgentはCtrl+Cまたはlogoffまで
  低頻度で回復を待機する。
- 切断中のSVN／Trac操作、複数session分離、固定log内容は継続利用で追加確認する。
- 実環境用`agent.ini`、password、log、payload、source、diff、ticket本文をGitHub
  Releaseへ含めない。

詳細は`docs/known-limitations.md`、`docs/test-plan.md`、`docs/agent.md`を参照する。
