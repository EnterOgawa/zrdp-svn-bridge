# Phase 4 Client localhost HTTP Proxy 実装提案

作成日: 2026-07-30

## 1. 目的

`ZRdpSvnBridge.exe` に `127.0.0.1:18080` 限定の HTTP Forward Proxy を追加し、
開発 PC の HTTP SVN client から受けた request を Phase 2 の stream protocol と
Phase 3 の Agent TCP relay へ接続する。Phase 4 では Mock HTTP Server までを対象とし、
実 SVN、実取引先 IP、Zscaler 設定にはアクセスしない。

## 2. 採用する所有境界

Client は次の二つの thread 境界へ分ける。

- UI／COM STA thread
  - Remote Desktop ActiveX Control と `ClientBridge` を所有する。
  - `OnChannelReceivedData` の BSTR を所有 byte 列へ copy し、application frame を
    decode する。
  - worker が生成した frame を既存 bounded queue へ移し、
    `SendOnVirtualChannel` を Window Message から呼ぶ。
- Proxy worker thread
  - Winsock runtime、loopback listener、受理済み local Socket、
    HTTP request parser、Client role の `StreamMultiplexer` を所有する。
  - HTTP header 解析、Socket read／write、flow control、stream lifecycle を処理する。
  - ActiveX、COM interface、UI control を呼ばない。

両 thread 間は所有 `Frame` の bounded queue と Window Message だけで接続する。
worker から `SendOnVirtualChannel` を直接呼ばず、UI thread から Socket I/O を
呼ばない。

## 3. HTTP Phase 4 境界

- bind address は設定値にせず、実装内で IPv4 loopback `127.0.0.1` に固定する。
- port だけを有限な `uint16_t` 設定とし、既定値を `18080` とする。
- IPv6、`0.0.0.0`、LAN／Wi-Fi／VPN NIC の bind 経路を API と設定へ持たせない。
- request header は CRLF 境界を incremental に探索し、64 KiB を上限とする。
- absolute-form `http://IPv4[:port]/...` を origin-form へ変換する。
- origin-form と `*` は `Host` header の IPv4 literal と port を宛先に使用する。
- URL authority と `Host` は IPv4 octet と既定 port 80 を正規化して完全一致させる。
- `CONNECT`、`https://`、userinfo、fragment、hostname、IPv6、`Upgrade`、
  `Transfer-Encoding` を拒否する。
- `Proxy-Connection`、`Proxy-Authorization`、標準 hop-by-hop header と
  `Connection` が列挙する header を upstream request から除く。
- `Authorization`、Cookie、その他 end-to-end header は内容を解釈せず保持する。
- `Content-Length` body は header と別に有限 chunk で転送し、全量保持しない。
- 一つの local TCP connection は最初の `IPv4:port` へ固定し、Keep-Alive 中の
  宛先変更を拒否する。
- Phase 4 は Agent の単一 Socket Gate に合わせて local active connection を
  一件に制限する。複数同時 connection、chunked、完全な HTTP/1.1／SVN 互換性は
  Phase 5 で拡張する。

## 4. Bridge session と HELLO

現在の Client は handshake 成立後も一秒ごとに HELLO を送る。Agent は HELLO を
新 session 境界として全 Socket を閉じるため、active HTTP stream 中の周期 HELLO は
禁止する。

- HELLO retry は `WaitingForAgent` の間だけ実行する。
- 正常な Agent `CLOSE` 後は従来どおり直ちに新 HELLO を開始する。
- Echo test 開始時は Client Proxy stream session を先に停止する。
- Echo 用 HELLO／PING／PONG 完了後に Client multiplexer を新 session として開始する。
- RDP 切断、再接続、Bridge failure、Agent `CLOSE` では Proxy worker の全 local
  connection と stream state を破棄する。

異常終了で Agent の connection-level `CLOSE` 自体が失われた場合の health check と
timeout は Phase 6 で扱う。Phase 4 では request 受付時の Bridge 未 ready を固定
HTTP error で即時終了し、無期限 hang を作らない。

## 5. 失敗時の挙動

- Proxy listener の起動失敗は RDP Client を終了させず、Proxy 状態を失敗として UI と
  固定診断 event に表示する。
- Agent 未起動、RDP 未接続、Bridge 未 ready の local connection は
  `503 Service Unavailable` を返して閉じる。
- malformed HTTP、Host 不一致、禁止方式は payload を log せず固定 HTTP status を
  返して閉じる。
- `OPEN_ERROR` は固定 error code だけで `403`、`502`、`503`、`504` に分類する。
- queue 上限、protocol failure、RDP 切断では local Socket と stream を閉じるが、
  RDP 画面接続は切断しない。

## 6. セキュリティ影響

- 新しい待受は開発 PC の IPv4 loopback 一つだけであり、支給 PC や対外 NIC に
  inbound port を追加しない。
- Proxy は HTTP IPv4 literal だけを stream `OPEN` に変換する。最終接続は Agent の
  allowlist 完全一致を必須とし、Client の検証だけを trust boundary にしない。
- DNS、OS Proxy、HTTP `CONNECT`、SOCKS、汎用 TCP forward、network discovery を
  追加しない。
- URL path、query、header、body、credential、実 IP を診断 log へ渡さない。

## 7. 検証

- parser unit test:
  - fragmentation、absolute-form 変換、origin-form
  - malformed request、Host 欠落／不一致、宛先変更
  - CONNECT、Upgrade、hostname、IPv6、Transfer-Encoding の拒否
  - hop-by-hop header 除去、Content-Length body の chunk streaming
- Proxy worker test:
  - `127.0.0.1` bind と `0.0.0.0` 非使用
  - Bridge 未 ready の即時 `503`
  - RDP disconnect 相当 cleanup
- integration test:
  - Client Proxy -> fake UI transport -> Agent relay -> loopback Mock HTTP Server
  - request line 変換、response byte 完全一致、大きい body の bounded streaming

Debug／Release の全 CTest を warning なしで完了し、portable EXE の version、
依存 DLL、hash を再確認する。

## 8. Rollback

新規 `src/client/proxy` target、Client stream routing、UI status 連携を単位として
取り外せる。Phase 1 Echo、RDP ActiveX host、Phase 3 Agent relay、共通 protocol の
wire formatは変更しない。問題時は Client capability を Echo-only に戻せば、
既存 RDP と Echo Gate を維持したまま Phase 4 機能だけを停止できる。
