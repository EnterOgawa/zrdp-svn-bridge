# Phase 9改善I-007 外部切断時Window終了のローカル検証

更新日: 2026-07-31

## 1. 対象

- Client／Agent version: `0.7.7.0`
- 対象要望: 接続先再起動等で成立済みRDP sessionが最終切断された場合、
  error dialogを表示せず対象session Windowを閉じる
- 実接続先、domain、user、画面内容は記録しない

## 2. 動作境界

- 現在の接続試行で`OnLoginComplete`を受信した場合だけ成立済みsessionとする
- `OnAutoReconnecting2`中はWindowを維持する
- `OnAutoReconnected`では成立済みsessionへ復帰した状態を維持する
- 成立済みsessionの非local、非credential取消の`OnDisconnected`だけを
  無通知終了対象とする
- 初回認証失敗等のログイン前切断は、原因確認のため従来のerror表示を維持する
- ZRdp自身の切断要求とcredential prompt取消は既存経路へ委ねる

## 3. 実装

- 接続試行ごとのログイン完了flagをsession Windowに保持する
- 最終外部切断時は動的解像度timerとBridge sessionを停止する
- COM callback中にWindowを直接破棄せず、所有Window messageへ終了を遅延する
- 切断reasonとextended reasonは従来どおり数値だけを診断logへ残す
- wire protocol、Proxy、Agent allowlist、履歴形式、認証設定は変更しない

## 4. 自動検証

- Debug configure／build: 成功
- Debug CTest: `28 / 28`成功
- Release build: 成功
- Release CTest: `28 / 28`成功
- `unit.session_disconnect_policy`で次を確認
  - ログイン完了後の外部切断だけを無通知終了対象にする
  - ログイン前の外部切断はWindowを維持する
  - local切断は既存経路へ委ねる
  - credential prompt取消は未接続復帰へ委ねる
- RDP ActiveX、Agent、Proxy、HTTP／SVN、Trac、Hardeningの既存testも回帰成功

## 5. Portable成果物

| file | size | version | SHA-256 | signature |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 677,888 byte | `0.7.7.0` | `B75E358BFD5BADC9AD552749408B4369977909C8F412EA1DC2AD57AD0A20731B` | NotSigned |
| `out/ZRdpSvnAgent.exe` | 423,424 byte | `0.7.7.0` | `46C6D6A263EF902F0F77BFCB2CA1EDF89AB9188781DE6D2904A654589D28EEF9` | NotSigned |

## 6. 残りGate

- 接続済みsessionで接続先を再起動するか、安全な方法で接続を失わせる
- 自動再接続中はWindowが維持される
- 最終切断後にRDP接続error dialogが表示されない
- 切断された対象session Windowだけが閉じる
- 接続管理Window、他session、再接続profileが維持される

実機Gate通過までは改善I-007を完了扱いにしない。
