# Phase 9改善I-006 初回動的解像度のローカル検証

更新日: 2026-07-31

## 1. 対象

- Client／Agent version: `0.7.6.0`
- 対象不具合: 動的解像度を選択して新規接続すると、RDP接続自体は成立するが
  `UpdateSessionDisplaySettings`が`0x8000FFFF`を返し、等倍表示へ縮退する
- 実接続先、domain、user、画面内容は記録しない

## 2. 原因

Client診断logでは、`OnConnected`から250 ms後に
`UpdateSessionDisplaySettings`が呼ばれ、その約3秒後に`OnLoginComplete`が
通知されていた。`OnConnected`はtransport成立であり、remote desktopの
ログイン完了を保証しないため、初回動的解像度の適用が早すぎた。

## 3. 修正

- `OnConnected`では動的解像度timerを予約しない
- `OnLoginComplete`後だけ表示mode切替と
  `UpdateSessionDisplaySettings`を許可する
- 自動再接続完了は既存ログイン済みsessionへの復帰として扱い、動的解像度を
  再予約する
- 未ログイン状態で表示更新APIを直接呼ばれても
  `ERROR_INVALID_STATE`としてActiveX呼出し前に拒否する
- wire protocol、Proxy、Agent allowlist、履歴形式、認証設定は変更しない

## 4. 自動検証

- Release configure: 成功
- Release build: 成功
- Release CTest: `27 / 27`成功
- `unit.rdp_host_smoke`で次を確認
  - `Connected`では動的解像度更新を許可しない
  - `LoggedIn`だけで動的解像度更新を許可する
  - `Reconnecting`では更新せず再接続完了を待つ

## 5. Portable成果物

| file | size | version | SHA-256 | signature |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 677,888 byte | `0.7.6.0` | `0A8CF7FBA754E0D6311C0795C69CEBC3E2D3B2350A713F25B7E107FEA210F9C0` | NotSigned |
| `out/ZRdpSvnAgent.exe` | 423,424 byte | `0.7.6.0` | `044650E6D56C4B9CC18FFE254E5FAAA00A8FB5FB992DEB117D6F89EAC3B9CC22` | NotSigned |

## 6. ユーザー実機Gate

- [x] 動的解像度を選択した状態で新しい接続を開始する
- [x] 接続途中に表示mode変更errorが表示されない
- [x] ログイン完了後、remote desktopがsession Windowへ追従する
- [x] 等倍表示、Smart Sizing、動的解像度を接続後に往復できる

2026-07-31、ユーザーから先に報告された動的解像度問題が解決しているとの
実機結果を受領した。実接続先、domain、user、画面内容は記録せず、
改善I-006のユーザー実機Gateを通過扱いとする。
