# Phase 9 改善I-002 複数RDP sessionローカル検証

検証日: 2026-07-31

## 1. 対象

- Client／Agent product version: `0.7.0.0`
- Platform: Windows x64
- Build: Visual Studio 2022、C++20、Unicode、static CRT
- Wire protocol: version `1`のまま変更なし
- Agent設定／allowlist形式: 変更なし

## 2. 実装境界

- `ClientApplication`が接続管理Window、最大16件のsession Window、共通の
  COM STA message loop、有限Proxy port slotを所有する。
- 接続管理Windowは接続先、domain、user、表示mode、DPAPI接続履歴だけを所有する。
- 各session Windowは独立した`RdpControlHost`、`ClientBridge`、
  `ClientProxyWorker`を所有する。
- `client.ini`のportをbaseとし、最初のsessionへbase、後続へ未使用の
  `base + slot`を割り当てる。
- session終了時は対応slotだけを解放する。
- session titleと状態欄へ実際の`127.0.0.1:<割当port>`を表示する。

## 3. 自動テスト

Debug／Releaseの両方で全26 CTestが成功した。

追加または拡張した主要Gate:

- `unit.session_proxy_port_allocator`
  - `18080`からの重複なし順次割当
  - 中間slotの解放と最小port再利用
  - 16 session上限
  - base `65535`のoverflow拒否
  - base `0`の拒否
- `unit.rdp_host_smoke`
  - 同一COM STA上で二つのRemote Desktop ActiveXを同時生成
  - 二つのActiveX child Windowが別instanceであること
  - 一つ目の破棄後も二つ目が生存すること
  - 残存ActiveXが接続前設定を引き続き受け付けること
- `unit.resource_contract`
  - 接続管理Window title
  - 「新しい接続」
  - 次のProxy port表示
  - 接続先とsession専用Proxy portを含むWindow title
- `unit.portable_binary_contract`
  - Client／Agentのx64種別
  - File／Product version `0.7.0.0`

実RDP、Zscaler、支給PC、実SVN／Tracへの自動接続は行っていない。

## 4. Portable成果物

| File | Size | File／Product version | SHA-256 | Authenticode |
|---|---:|---|---|---|
| `out/ZRdpSvnBridge.exe` | 664,064 byte | `0.7.0.0` | `0A8D17975AA13BA4EED1A83EB4DEFF1B576EBF2DF02F1494B8C35D6DED65DB87` | NotSigned |
| `out/ZRdpSvnAgent.exe` | 423,424 byte | `0.7.0.0` | `F706D54539E5D50CD95A03E7978B5317BF2700790D7DD806E53442506F84030B` | NotSigned |

両EXEはx64で、Dynamic Base、NX Compatible、Control Flow Guardが有効である。
依存先はWindows system DLLだけであり、製品用の外部runtime DLLを追加していない。
未署名は想定どおりであり、Authenticode署名前のRelease Gateは未通過として扱う。

## 5. Compatibility

- application frame、message type、feature bit、SVC名`ESVNBRG`を変更していない。
- Version `0.7.0.0` ClientはVersion `0.6.0.0` Agentとwire互換である。
- 通常配布と実機確認では、取り違えを避けるため同じ`0.7.0.0` buildを使用する。
- 一つのClient process内だけでport slotを管理する。複数Client process間では
  portを協調しない。

## 6. 残るユーザー実機Gate

- 一つ目が`18080`、二つ目が`18081`として別Windowへ表示される。
- 各RDP session内でAgentを起動し、両Bridgeが独立してReadyになる。
- 各loopback Proxy portが対応するRDP sessionだけを経由する。
- 一方のsession終了後も他方が継続する。
- 解放したport slotを次の新規sessionが再利用する。
- 通常利用する一件を最初に開始すれば、従来の`18080`設定で利用できる。
