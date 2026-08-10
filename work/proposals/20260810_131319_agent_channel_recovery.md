# Agent Virtual Channel 再接続の修正提案

## 対象

- `src/agent/app/main.cpp`
  - `wmain` の WTS Virtual Channel I/O loop。
- `src/agent/app/session_validation.h`／`.cpp`
  - current RDP session の回復待機可否を判定する policy。
- `tests/agent/session_validation_test.cpp`
  - active／disconnected／終了状態の pure policy test。
- `docs/agent.md`、`docs/architecture.md`、`docs/test-plan.md`
  - 実行時動作と実機確認項目。

## 変更理由

一時的なネットワークリンク断と Microsoft RDP Control の自動再接続により、
旧 `ESVNBRG` handle の `WTSVirtualChannelRead` が Win32 error 233 で失敗する。
現状はこの一回の失敗を Agent process の終了条件としているため、RDP 画面が
自動回復しても Bridge だけが復旧しない。

## 変更内容

- channel read／write failure では旧 channel、protocol decoder、writer queue、
  active stream、upstream Socket を直ちに破棄する。
- current process の RDP session が存続している間は、有限 backoff 付きで
  `ESVNBRG` の再 open を試行する。
- `WTSDisconnected` 等の一時状態では open を行わず待機し、`WTSActive` 復帰後に
 だけ再試行する。
- `WTSReset`／`WTSDown` 等で session 終了を確認した場合、または Ctrl+C の場合は
  Agent を終了する。
- channel open だけを回復完了とせず、新しい `HELLO`／`HELLO_ACK` が成立して
  `Ready` へ戻った時点を回復完了とする。
- retry 失敗は既存の固定 `agent.channel_open_returned` eventを使用し、試行回数が
  2 の冪のときだけ記録して長時間断で log を無制限に増やさない。

## 影響

- ActiveX／COM: 変更なし。Client は既存の `OnAutoReconnected` 後の
  `StartBridgeSession` を使用する。
- Virtual Channel: 旧 handle を閉じて同名 SVC を再 open する。別 channel や
  別経路への fallback は行わない。
- HTTP Proxy: wire session 回復まで利用不可となり、既存 stream は終了する。
- Agent Socket: channel 断の時点で全 upstream Socket を閉じ、新 session へ
  持ち越さない。
- security boundary: current process の Session ID だけを照会し、session 列挙、
  endpoint 探索、任意接続は追加しない。
- protocol compatibility: wire format と protocol version は変更しない。
  回復後は仕様どおり新しい `HELLO` から開始する。

## 失敗時動作

- RDP session が一時切断中は Agent process を維持するが、Proxy traffic は
  fail-closed とする。
- session state を一時的に照会できない場合は SVC open を行わず待機する。
- protocol error、resource exhaustion 等の channel 断以外の fatal error は
  従来どおり非 0 で終了する。

## テスト

- session state の回復分類を全 `WTS_CONNECTSTATE_CLASS` について単体検証する。
- retry delay の上限と log 間引き policy を pure function で検証する。
- 全 unit／integration test と Release build を実行する。
- 実機では安全な一時リンク断後に、Agent process が残り、RDP 自動再接続後に
  Agent 接続済みへ戻ることを確認する。
- 断の瞬間に存在した SVN／Trac 操作は失敗し、再試行した新操作だけが成功する
 ことを確認する。

## ロールバック

本変更の commit を revert するか、直前の署名済み `ZRdpSvnAgent.exe` へ戻す。
Client EXE、`agent.ini`、wire protocol の変更は不要である。
