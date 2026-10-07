# ADR 0011: 同じRDPユーザーsession内でのAgentチャネル回復

- Status: Accepted
- Date: 2026-08-10
- 記録整理日: 2026-10-07

## 背景

一時的なnetwork link断で旧`ESVNBRG`handleのreadが失敗し、Agentが終了すると、
Microsoft RDP Controlが画面接続を自動回復してもBridgeだけが復旧しなかった。
RDPユーザーsessionが存続する間はAgentを維持し、同じsessionのapplication channelを
開き直す方式を採用した。Version `0.7.15.0`で実機受入済みである。

## 判断

- channel read／write失敗時は旧channel、decoder、writer queue、stream、upstream
  Socketを破棄する。
- current processのSession IDだけを照会し、activeなRDP sessionでのみ同名SVCを開く。
- 一時切断中やWTS照会失敗中は接続せず待つ。session終了、非RDP化、Ctrl+Cで停止する。
- 再openは250 msから最大2秒のbackoffを使用し、100 msのpollで終了要求を確認する。
- open成功だけでは回復完了とせず、新しい`HELLO`／`HELLO_ACK`からBridgeを成立させる。
- open失敗のlogは試行回数が1、2、4、8、...のときだけ記録する。
- RDP自体の再接続はMicrosoft Controlに委譲する。

## 影響と失敗時動作

同じsession内のAgent processは維持できるが、切断時のHTTP操作は失敗し、旧stream、
旧Socket、sequenceを新Bridge sessionへ持ち越さない。通信が利用可能になるまで
Proxyはfail-closedとする。requestの再送や途中resumeは行わない。

protocol errorやresource exhaustionはチャネル回復の対象とせず従来の終了条件を維持する。
allowlist、`ESVNBRG`、wire format、protocol version、認証方式に変更はない。
別sessionの探索、別経路、受信port、service化は追加しない。

## 検証と代替案

`session_validation_test`でsession状態、backoff上限、log間引きを検証する。
ユーザー実機では、一時link断後のAgent process維持とRDP復帰後のBridge回復を確認した。
無条件の即時終了は画面回復後のAgent手動起動を必要とし、旧streamの再利用は
HTTP操作の重複や途中状態の混入を招くため採用しない。

## 参照

- [Agent設計](../agent.md)
- [Version 0.7.15リリースノート](../release-notes-0.7.15.md)
- `src/agent/app/channel_recovery_policy.h`
- `src/agent/app/main.cpp`
