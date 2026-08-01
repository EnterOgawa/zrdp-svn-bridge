# ADR 0004: RDP表示modeの明示的な分離

- Status: Accepted
- Date: 2026-07-30
- Supersedes: ADR 0003の「接続後にremote desktop寸法を変更しない」という制限
- Partially superseded by: ADR 0006の全画面monitor追従

## Context

ADR 0003では、ZRdp windowの変更が意図せず接続先application layoutへ波及した
問題を防ぐため、remote desktop寸法を固定し、等倍scrollとSmart Sizingだけを
提供した。その後、利用者から次の2つは別機能として明示的に選択できる必要が
あると指定された。

1. remote desktop解像度を維持し、Client側表示だけをwindowへ縮小する。
2. remote desktop解像度自体をZRdp windowのRDP表示領域へ合わせる。

Smart Sizingは前者だけを意味する。後者は接続先application layoutを変更し得るが、
利用者が副作用を理解できる独立modeとして明示選択する場合は必要な機能である。
3つの状態は同時に成立しないため、checkboxではなくラジオボタンで表現する。

## Decision

表示modeを次の3つに分ける。

| mode | remote desktop解像度 | 表示方法 | 既定 |
|---|---|---|---|
| 等倍表示（スクロール） | 接続開始時のmonitor寸法を維持 | 等倍、必要時scroll | Yes |
| Smart Sizing（縮小表示） | 接続開始時のmonitor寸法を維持 | Client側だけを縮小 | No |
| 動的解像度（Window追従） | RDP表示領域へ追従 | 等倍 | No |

- 初期選択は等倍表示とする。
- 等倍表示とSmart Sizingの初期desktop寸法は、ADR 0003どおり接続開始時の
  現在monitor全体とする。
- Smart Sizingは`IMsRdpClientAdvancedSettings::SmartSizing`だけを有効化し、
  `UpdateSessionDisplaySettings`を呼ばない。
- 動的解像度ではSmart Sizingを無効化する。接続開始時は現在のRDP表示領域を
  初期desktop寸法とし、接続後は`IMsRdpClient9::UpdateSessionDisplaySettings`
  でremote desktop寸法を変更する。
- window resize時はActiveX hostを即時変更し、remote desktop更新は最後の
  `WM_SIZE`から250 ms後に一回だけ実行する。
- 全画面中の動的解像度targetはADR 0006に従う。等倍表示とSmart Sizingは
  remote desktop寸法を維持し、動的解像度だけがmonitor全体へ追従する。
- `Reconnect`による寸法変更は行わない。
- 利用者による接続中のmode切替を許可する。

## Failure handling

- 切替操作の適用に失敗した場合は、ラジオボタンとActiveX設定を直前のmodeへ
  戻す。RDP接続は維持する。
- resize後の動的更新に失敗した場合は等倍表示へ縮退し、追加のremote desktop
  変更を停止する。Smart Sizingへ暗黙fallbackしない。
- HRESULTは固定eventで記録するが、接続先、ドメイン、ユーザー名、画面寸法は記録しない。

## Consequences

### Positive

- Smart Sizingとremote desktop解像度変更の意味がUI上で混同されない。
- 接続先application layoutを維持したい場合は、既定の等倍表示または
  Smart Sizingを明示選択できる。
- layout変更を許容する場合は、ZRdp windowへremote desktopを追従できる。
- 更新頻度を有限化し、連続resize中のCOM呼出しを抑制できる。

### Negative

- 動的解像度では接続先application layoutが変化し得る。
- `IMsRdpClient9`を提供しない古いcontrolでは動的解像度を利用できない。
- session側policyまたはRDP serverが寸法変更を拒否する場合がある。

## Security and protocol impact

認証、NLA、CredSSP、server certificate、clipboard、Virtual Channel、
localhost Proxy、Agent allowlistへ変更はない。新しいnetwork経路や受信portを
作らず、Application Protocolのwire formatも変更しない。

## Alternatives

### 一つのcheckboxで切り替える

Smart Sizingと動的解像度のどちらを意味するか、副作用を含めて判別できないため
不採用とした。

### 2つの独立checkboxを使用する

同時ONという矛盾状態が生じ、優先順位や自動解除が必要になるため不採用とした。

### 動的解像度を提供しない

ADR 0003の安全な既定動作は維持できるが、利用者が明示したwindow追従要件を
満たさないため不採用とした。

## Validation

- 3つのラジオボタンと既定等倍modeをresource testで確認する。
- 接続前設定modelの3 modeと未接続時API拒否をActiveX smoke testで確認する。
- Debug／Release buildとCTestを実行する。
- 実RDP sessionで3 mode、window resize、full screen復帰、自動再接続後の追従を
  確認する。

## References

- Microsoft `IMsRdpClient9::UpdateSessionDisplaySettings`
- Microsoft `IMsRdpClientAdvancedSettings::SmartSizing`
- ADR 0003
