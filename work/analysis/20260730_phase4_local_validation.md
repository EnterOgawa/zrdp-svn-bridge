# Phase 4 localhost HTTP Proxy ローカル検証記録

- 実施日: 2026-07-30
- 対象version: `0.4.0.0`
- 対象platform: Windows x64

## 1. 検証範囲

Phase 4のGateとして、実RDP、Zscaler、支給PC、実SVNを使用せず、次のproduction
componentをloopback Mock HTTP Serverまで接続した。

```text
local TCP client
  -> ClientProxyWorker
  -> ClientProxySession
  -> ClientBridge encode／SVC fragmentation
  -> AgentBridge decode
  -> AgentStreamRelay
  -> production Agent Winsock
  -> loopback Mock HTTP Server
```

Mock ServerのOS割当`127.0.0.1` portだけを検証済みallowlistへ登録した。
実接続先、実URL、credential、HTTP payloadをlogまたは本記録へ保存していない。

## 2. Build／CTest

- [x] `cmake --build --preset debug -- /m`
- [x] DebugをC++20、x64、`/W4`、`/WX`でwarningなしに完了
- [x] `ctest --preset debug --output-on-failure`: 21/21成功
- [x] `cmake --build --preset release -- /m`
- [x] ReleaseをC++20、x64、`/W4`、`/WX`でwarningなしに完了
- [x] `ctest --preset release --output-on-failure`: 21/21成功
- [x] Phase 4結合test Debug反復: 20/20成功
- [x] Phase 4結合test Release反復: 20/20成功

## 3. HTTP／relay検証

- [x] Windows TCP table上でproduction listenerがIPv4
  `127.0.0.1`だけに存在する
- [x] Bridge未ready時に固定`503 Service Unavailable`を返す
- [x] absolute-formからorigin-formへ変換する
- [x] authorityと`Host`のIPv4／portを一致検証する
- [x] proxy固有headerと`Connection`指定headerを除去する
- [x] `CONNECT`、HTTPS、Upgrade、Transfer-Encoding／chunkedを拒否する
- [x] 512 KiB超request bodyを256 KiB window越しにbyte完全一致で送る
- [x] 384 KiB超response bodyを256 KiB window越しにbyte完全一致で受ける
- [x] downstream消費後だけ`WINDOW_UPDATE`でcreditを戻す
- [x] local FIN、server FIN、normal CLOSE、ackを完了する
- [x] 終了後のactive stream数が0となる
- [x] Stop時にlocal／upstream Socketと有限queueを破棄する

## 4. Portable成果物

| File | Size | Version | SHA-256 | 署名 |
|---|---:|---:|---|---|
| `out/ZRdpSvnBridge.exe` | 615,936 byte | 0.4.0.0 | `AEF8E9B07B1F7362E080EA9EED04BD4F6F14F8324326BFB3E7B59C6527D19BD9` | 未署名 |
| `out/ZRdpSvnAgent.exe` | 421,376 byte | 0.4.0.0 | `C6827E499F7FB881B876AE785D7AF900E641977B74ACC8D9E5DD7125D5DB44EE` | 未署名 |

両EXEはPE x64で、Dynamic Base、NX compatible、Control Flow Guardが有効である。
importはWindows system DLLだけであり、外部runtime DLLを追加していない。
未署名はRelease Gate前の想定状態であり、ユーザーから署名完了連絡を受けるまで
Release公開やremote operationを行わない。

## 5. Gate判定と残課題

Phase 4のローカルGateは通過とする。次はPhase 5で次を扱う。

- [ ] HTTP/1.1 chunked transfer encoding
- [ ] `Expect: 100-continue`
- [ ] 長時間Keep-Aliveとpipelining
- [ ] Basic／Digest／NTLM等の認証challenge／response透過
- [ ] 複数同時server／port／repository
- [ ] Mock／test SVNでのWebDAV／DeltaV sequence
- [ ] Proxy portを変更可能にする安全なlocal設定境界

実支給PCまたは実SVNでの追加テストはこのGateでは不要である。実環境read-onlyは
Phase 7、write操作はPhase 8で、ユーザーの明示指示を受けた安全な対象だけに限る。

Agentがconnection-level CLOSEを送れない異常終了では、active streamをresetしない
安全なhealth-checkと同一RDP session内の再開が未実装である。これはPhase 6の
Hardening項目として扱う。
