# RDP表示modeのラジオボタン化

## 目的

ZRdpのwindowへ画面を合わせる動作には、remote desktop解像度を維持して
Client側表示だけを縮小するSmart Sizingと、remote desktop解像度自体を
windowへ追従させる動的解像度がある。両者を一つのcheckboxで表現せず、
意味と副作用が異なる3つの排他的な表示modeとして選択できるようにする。

## 変更対象

- `src/client/app/resource.h`
- `src/client/app/ZRdpSvnBridge.rc`
  - 「等倍表示（スクロール）」「Smart Sizing（縮小表示）」
    「動的解像度（Window追従）」のラジオボタン用resourceを追加する。
- `src/client/app/main_window.h`
- `src/client/app/main_window.cpp`
  - 3つのラジオボタン、表示mode状態、切替時のrollbackを実装する。
  - 動的解像度では`WM_SIZE`後250 msのdebounce timerから更新を要求する。
- `src/client/rdp/rdp_control_host.h`
- `src/client/rdp/rdp_control_host.cpp`
  - `RdpDisplayMode`を接続設定modelへ追加する。
  - `IMsRdpClient9::UpdateSessionDisplaySettings`をActiveX境界へ閉じ込める。
- `src/common/diagnostic_log.h`
- `src/common/diagnostic_log.cpp`
  - 表示mode番号と動的解像度APIのHRESULTを固定語彙で記録する。
- `tests/unit/rdp_host_smoke_test.cpp`
- `tests/unit/resource_contract_test.cpp`
- `tests/unit/diagnostic_log_test.cpp`
  - 既定mode、接続前設定、未接続時拒否、resource、log契約を検証する。
- `AGENTS.md`、`docs/adr`、Phase 0関連文書
  - 3 modeの意味、失敗時動作、実機確認項目を仕様として固定する。

## 表示mode

| mode | remote desktop解像度 | Client側表示 | window resize |
|---|---|---|---|
| 等倍表示（既定） | 接続開始時のmonitor寸法を維持 | 等倍、必要時scroll | ActiveX hostだけを変更 |
| Smart Sizing | 接続開始時のmonitor寸法を維持 | window内へ縮小表示 | ActiveX hostと表示倍率だけを変更 |
| 動的解像度 | RDP表示領域へ変更 | 等倍 | 250 ms debounce後にsession寸法を変更 |

- 接続中にも3 modeを切り替えられる。
- Smart Sizingでは`UpdateSessionDisplaySettings`を呼ばない。
- 動的解像度へ切り替える前にSmart Sizingを無効化する。
- 動的解像度の接続開始時は現在のRDP表示領域を初期desktop寸法とし、
  接続後は`UpdateSessionDisplaySettings(width, height, 0, 0, 0, 100, 100)`
  で追従させる。
- 全画面中はMicrosoft controlへ表示管理を委譲し、ZRdpのtimer更新を停止する。
  window modeへ戻った時と自動再接続完了時に更新を再予約する。
- `Reconnect`は使用しない。

## ActiveX／COMへの影響

- 新規COM呼出しは`IMsRdpClient9::UpdateSessionDisplaySettings`だけとし、
  `RdpControlHost`の生成元STAで実行する。
- 寸法は既存の200～8192 pixel制限を適用する。
- `WM_SIZE`ごとの連続COM呼出しを避け、最後の変更から250 ms後に一回だけ行う。
- ActiveX event購読、認証、clipboard、全画面、接続・切断の契約は変更しない。

## Security boundary

- RDP credential、接続先、ユーザー名、clipboard、Virtual Channel payloadの
  取得・保存・log方針は変更しない。
- 新しいnetwork通信、listen port、Proxy、routing、security設定変更は追加しない。
- logにはmode番号とHRESULTだけを記録し、画面寸法、接続情報、payloadを記録しない。
- 動的解像度は表示制御に限定し、SVN Bridgeの通信境界には影響しない。

## 失敗時動作

- 利用者操作直後のmode適用に失敗した場合は、ラジオボタンとActiveX設定を
  直前のmodeへ戻し、HRESULTを表示する。RDP接続は切断しない。
- resize timerからの動的解像度更新に失敗した場合は、remote desktopへの
  追加変更を停止するため等倍表示へ縮退し、Smart Sizingは暗黙に有効化しない。
- timer開始失敗時は同じUI thread上で一度だけ即時更新し、同じ失敗処理を適用する。

## Protocol compatibility

Static Virtual Channel `ESVNBRG`およびApplication Protocolのwire formatに
変更はない。表示mode切替やdesktop寸法をVirtual Channelへ送信しない。

## Test

- x64 Debug／Release buildとCTest。
- 3つのラジオボタンresourceと既定等倍mode。
- 接続前に各modeを`RdpConnectionSettings`へ適用できること。
- 未接続時の`SetSmartSizing`と`UpdateSessionDisplaySettings`拒否。
- 診断logがmode番号とHRESULTだけを含むこと。
- 実RDP sessionで、等倍scroll、Smart Sizing縮小、動的解像度追従、
  3 mode間切替、全画面復帰、clipboardを確認する。

## Rollback

本変更のコード、resource、test、文書を`0.1.7.0`の固定解像度／Smart Sizing
checkbox構成へ戻す。接続履歴formatと保存先は変更しないためdata migrationは
不要である。動的解像度に問題がある場合はそのmodeだけを無効化しても、
既定の等倍表示とRDP接続を維持できる。
