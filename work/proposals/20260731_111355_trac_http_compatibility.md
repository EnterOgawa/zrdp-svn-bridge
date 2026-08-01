# 改善 I-001: 許可済み Trac HTTP 接続の互換性確認

## 1. 目的

既存の ZRdp localhost HTTP Forward Proxy を、ユーザーが明示した HTTP 方式の
Trac にも利用できることを正式に確認する。対象は、Agent の allowlist に
`IPv4 address + TCP port` の完全一致で登録済み、または運用承認を得て追加する
Trac endpoint に限定する。

汎用 HTTP Proxy 化、任意 endpoint への接続、HTTP `CONNECT`、HTTPS、
hostname 解決、OS 全体 Proxy 変更は行わない。

## 2. 事前監査

開発環境へ導入済みの次の plugin を read-only で確認した。

- Mylyn Trac Connector Core `3.26.0`
- Mylyn Trac Connector UI `3.26.0`
- Mylyn Commons Net `3.26.0`
- Mylyn Tasks UI／Core `3.26.0`

確認結果は次のとおりである。

- Trac repository 設定は repository 固有の proxy host／port を保持できる。
- 既定の system proxy を使わず、明示した HTTP Proxy を repository 単位で
  選択できる。
- Web fallback は HTTP GET を使用する。
- XML-RPC は HTTP/1.1 POST を使用し、repository URL 配下の `/xmlrpc` へ
  XML body を送る。
- repository 認証、HTTP challenge、Cookie、Content-Type、Content-Length、
  response の Content-Encoding は end-to-end HTTP 情報として扱われる。
- attachment の read／write も XML-RPC payload として同じ POST 経路を利用する。

現行 `HttpRequestTransformer` は GET／POST を拒否せず、absolute-form を
origin-form へ変換し、end-to-end header と body を変更せず中継する。
Agent の security boundary は path ではなく `IPv4:port` の allowlist 完全一致
であるため、同一 endpoint の Trac path を利用しても接続可能な network endpoint
は増加しない。

## 3. 変更対象

### 3.1 方針・仕様

- `AGENTS.md`
  - HTTP SVN に加え、ユーザーが明示承認した HTTP Trac を対象に追加する。
  - Trac は allowlist 完全一致 endpoint に限定し、汎用 Proxy 禁止を維持する。
  - Mylyn の repository 固有 proxy を使用し、OS／Eclipse 全体 proxy の変更を
    必須にしない。
- `docs/http-proxy.md`
  - Trac XML-RPC／Web fallback の互換性境界を追記する。
- `docs/configuration.md`
  - Mylyn Trac の repository 固有 proxy 設定を追記する。
- `docs/security.md`
  - Trac credential／ticket／attachment payload も記録しないことを明記する。
- `docs/test-plan.md`
  - Mock Trac の受入項目を追記する。
- `docs/roadmap.md`
  - Phase 9 改善 I-001 として進捗を記録する。

### 3.2 テスト

- `tests/proxy/trac_http_compatibility_test.cpp`
  - repository 固有 proxy が生成する absolute-form GET／POST を検証する。
  - Cookie、XML content type、Content-Length、認証 header を保持し、
    proxy 固有 header だけを除去する。
  - endpoint変更、HTTPS、CONNECT、hostname が従来どおり拒否されることを確認する。
- `tests/integration/phase5_http_svn_compatibility_test.cpp`
  - production Proxy、Client bridge、fake SVC、Agent relay の既存 test transcript に
    Mock Trac Web／XML-RPC sequence を追加する。
  - 401 challenge 後も同一 connection で認証付き XML-RPC POST を中継する。
  - Cookie、XML body、gzip response header、redirect response を byte 単位で
    透過する。
  - endpoint allowlist 完全一致と loopback listener を既存境界のまま使用する。
- `tests/CMakeLists.txt`
  - `proxy.trac_http_compatibility` を追加する。

production code に不足が見つからない限り、`src` は変更しない。テストで互換性不足
が判明した場合だけ、失敗条件を固定した後に最小修正を行う。

## 4. 影響範囲

### ActiveX／COM

変更なし。RDP 接続、認証、画面、入力、clipboard、event sink に影響しない。

### Virtual Channel

wire protocol、frame type、capability、queue、flow control を変更しない。
既存の HTTP byte stream を同じ `ESVNBRG` stream で中継する。

### HTTP Proxy

新しい protocol や method の特別処理は追加しない。GET／POST、HTTP challenge、
Cookie、XML body、opaque response が既存規則で透過することを回帰テストへ固定する。

### Agent Socket

接続先判定は従来どおり `IPv4:port` の allowlist 完全一致である。Trac path を理由に
別 endpoint、DNS、OS Proxy へ fallback しない。

### Security boundary

許可対象はユーザー承認済み HTTP Trac endpoint に限定する。`CONNECT`、HTTPS、
hostname、IPv6、任意 endpoint、network 探索は引き続き拒否する。
Trac の URL path、query、header、ticket、comment、attachment、credential を
ZRdp log へ記録しない。

## 5. Protocol compatibility

`ESVNBRG` application protocol の変更はない。Client／Agent version の組合せ、
HELLO negotiation、stream state machine は Version `0.6.0.0` と互換である。
production binary の変更が不要な場合は version を変更しない。

## 6. 失敗時動作

- RDP または Agent が未接続なら、Trac 接続も既存 SVN と同様に短時間で失敗する。
- allowlist 不一致なら Agent が OPEN を拒否する。
- redirect 先が別 endpoint、hostname、HTTPS の場合は、接続先制限により拒否する。
- Trac 接続失敗で RDP 画面接続を切断しない。
- ZRdp は Trac credential を保存せず、認証結果は Mylyn の画面で確認する。

## 7. 検証

1. Debug／Release を build する。
2. 両構成で全 CTest を実行する。
3. 新規 Mock Trac test で GET、401、認証付き XML-RPC POST、Cookie、
   gzip response、redirect response の透過を確認する。
4. loopback-only bind、allowlist 完全一致、CONNECT／HTTPS 拒否の既存 test が
   継続して通ることを確認する。
5. ユーザー環境では repository 固有 proxy を `127.0.0.1:18080` に設定し、
   「設定の検証」を実行する。
6. 検証後、ticket query、ticket open、更新の順にユーザーが確認する。

## 8. Rollback

新規 test と Trac に関する方針・文書追記だけを戻す。production code、wire protocol、
Agent allowlist 形式、既存 SVN 経路には変更を加えないため、Version `0.6.0.0` の
SVN 利用へそのまま戻せる。

## 9. ローカル結果

2026-07-31に次を確認した。

- Debug／Releaseとも`/W4`／`/WX` build成功
- Debug全25 CTest成功
- Release全25 CTest成功
- `proxy.trac_http_compatibility`成功
- Mock Tracを追加した`integration.phase5_http_svn_compatibility`成功
- production code、`ESVNBRG` wire protocol、Agent allowlist形式、version変更なし

改善I-001の残作業は、ユーザー環境でのMylyn「設定の検証」と段階的なTrac操作確認
だけである。
