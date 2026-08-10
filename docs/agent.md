# ZRdp SVN Agent

更新日: 2026-08-10

## 1. 配布と起動条件

`ZRdpSvnAgent.exe`はWindows x64用のportable console EXEである。installer、
Windows service、driver、管理者権限、registry登録、inbound portを必要としない。

Agentは、`ZRdpSvnBridge.exe`から接続した支給PCの同じRDPユーザーセッション内で
通常ユーザーとして起動する。開発PCのconsole session、Session 0、切断済みまたは
idle sessionでは起動条件を満たさず、安全に終了する。

## 2. Version 0.7の起動手順

1. 開発PCでZRdp ClientからRDP接続を完了する。
2. 組織で許可された方法により`ZRdpSvnAgent.exe`を支給PCへ配置する。
3. SVN／承認済みTracのHTTP streamを使用する場合は、許可済みendpointだけを
   記載した`agent.ini`を
   EXEと同じdirectoryへ配置する。形式は`docs/configuration.md`に従う。
4. 接続中のRDP画面内で`ZRdpSvnAgent.exe`を起動する。
5. 有効設定時はconsoleに`allowlist限定SVN streamを有効にしました。`と表示される。
   このVersion `0.7.15.0`の表示は、allowlist限定HTTP stream全体を表す。
   設定欠落／不正時はEcho-onlyで継続する旨が表示される。
6. consoleに`Clientと接続しました。`と表示されるまで待つ。
7. 必要に応じてClientの`Echoテスト`を実行する。Echo中は既存Proxy streamを
   閉じ、成功後にProxyを再度readyへ戻す。
8. 有効なallowlistでstream capabilityが成立すると、session Window titleの
   Proxy状態が`利用可`になる。
9. Agentを通常停止する場合はconsoleでCtrl+Cを押す。

Agentを先に常駐させる運用やservice化は行わない。起動後に一時的なnetwork link断が
発生し、Microsoft RDP Controlが同じuser sessionへ自動再接続する場合、Agentは
終了せず`ESVNBRG`を再openする。RDP sessionがlogoff等で終了した場合は停止する。

Ctrl+Cでは、Agentがconnection-level `CLOSE`をClientへ送ってからchannelを閉じる。
Clientは同じRDP接続を維持したままAgent待機へ戻るため、そのままAgentを再起動
できる。handshake成立後の周期HELLOはactive streamをresetするため送信しない。
終了通知を送れない異常終了はClientの5秒間隔health PINGと15秒deadlineで
自動検出する。Clientは旧Proxy streamを全破棄して新HELLOを開始するため、
同じRDP session内でAgentを再起動できる。途中のHTTP requestは再送または
resumeしない。

Agentの診断ログは、第一候補としてEXE隣接の
`logs\ZRdpSvnAgent.log`へ出力する。書込み不能な場合は
`%LOCALAPPDATA%\ZRdpSvnBridge\logs\ZRdpSvnAgent.log`へfallbackする。
Clientの`ZRdpSvnBridge.log`とはcurrent logおよび`.1`～`.3`のbackup世代を
共有しない。

## 3. Sessionとchannelの検証

Agentは現在processのSession IDを取得し、WTSから現在sessionのstateと
client protocol typeだけを取得する。ユーザー名、接続元address、接続先networkを
列挙しない。条件成立後に次と同等の呼出しだけでSVCを開く。

```cpp
WTSVirtualChannelOpenEx(
    WTS_CURRENT_SESSION,
    "ESVNBRG",
    0
);
```

flags `0`はStatic Virtual Channelを表す。初回channelを開けない場合にSocket、DNS、
OS Proxy、別channelへfallbackしない。成立後のchannel read／writeが失敗した場合は、
旧channel、protocol state、全upstream Socketを破棄する。current processのsessionが
`WTSDisconnected`等の一時状態なら待機し、`WTSActive`へ戻った後だけ同名channelを
250 msから最大2秒のbackoffで再openする。別sessionは列挙しない。

## 4. I/O model

同一handleに対するWTS Virtual Channel APIはthread-safeと仮定しない。一つの
I/O loopだけが100 ms timeoutのreadを行い、受信frameを`AgentBridge`へ渡し、
生成されたwriter queueを同じthreadで順にwriteする。Phase 3では同じowner loopが
non-blocking Socketのconnect／send／receiveも有限回ずつ進める。

- read／write bufferは最大1600 byte
- writeは全byte書込みだけを成功とする
- partial writeは失敗
- writer queueは最大256 chunkかつ256 KiB
- Agent streamは同時8件、connect timeoutは5秒
- 接続済みupstream Socketの完全無通信timeoutは30分
- Socket書込み待ちは最大256 KiB、Socket読込み待ちは最大16 KiB
- 一PumpのSocket operationは最大8回
- malformed frame、順序違反、queue上限超過でfail-closed
- Ctrl+C、RDP切断、channel errorで旧channelと全SocketをRAII解放
- 一時RDP切断ではAgent processを維持し、新channelの`HELLO`から再開
- Ctrl+C正常終了時は空payloadのconnection-level `CLOSE`を最後に送信

100 ms polling方式が実RDP環境で欠落や過度な遅延を起こさないことは、Phase 1の
11,600,897-byte Echoで測定する。Raw file handleとOVERLAPPED I/Oへの変更は
実測結果と別の設計判断なしには行わない。

## 5. Consoleと終了code

Consoleにはcompile時に固定した状態文だけを表示し、session ID、接続先、
ユーザー名、payloadを表示しない。

| Exit code | 意味 |
|---:|---|
| 0 | Ctrl+Cまたはcurrent RDP session終了による正常停止 |
| 2 | current active RDP sessionではない |
| 3 | `ESVNBRG`を開けない |
| 5 | protocol異常 |
| 6 | Ctrl+C正常終了通知のchannel write失敗 |

## 6. Phase 3のnetwork境界

Agentは`agent.ini`全体の検証に成功し、Client `OPEN`のIPv4とportが一件の
allowlist entryへ完全一致した場合だけoutbound TCP接続を開始する。

- Socket作成前にallowlistを判定する
- DNS APIを使用せず、4 octetを直接`AF_INET`へ変換する
- connect完了後にだけ`OPEN_OK`を返す
- allowlist不一致は`RejectedByPolicy`
- 5秒timeoutは`Timeout`、refused等は`ConnectFailed`
- reset／I/O failureは固定`CLOSE` reasonへ変換する
- 別address、別port、OS Proxy、別channelへfallbackしない

設定の匿名ID、IP、port、HTTP data、credentialはconsoleへ表示しない。診断logは
allowlistの件数、固定error分類、行番号だけを記録する。

## 7. 現在のAgentで行わないこと

- inbound TCP listen
- localhost／汎用Proxy
- DNS探索、port scan、host列挙
- credentialまたはpayloadの保存
- HTTP request解析、URL変換、SVN／Trac認証処理

Phase 6 Agentは最大8 upstream Socketをstream ID別に所有し、一owner threadで
round-robin処理する。Version `0.6.0.0`でClientのlocalhost HTTP Proxyと
Client側stream workerを接続し、production Client／Agent経路でMock WebDAVの
複数server／port／repositoryまで確認した。Phase 9改善I-001では同じ
opaque byte中継とallowlist境界のままMock Trac Web／XML-RPCを確認した。

AgentはHTTPを解析せずopaque TCP byteとして中継する。chunked、
`Expect: 100-continue`、認証sequenceの構文境界はClientが担当し、Agentは
allowlist一致とstream／Socket対応だけを維持する。実取引先SVN／Tracへ接続して
Phase 5を完了判定していない。後続のPhase 7／8ではユーザーが許可した実環境に
限定してSVN read／write操作の受入を完了した。実Tracの設定検証と同期も
Phase 9改善I-001でユーザー受入済みである。

Version `0.7.15.0`で複数RDP sessionを使用する場合は、各sessionのRDP画面内で
Agentを一つずつ起動する。一つのAgent processやWTS channel handleを別RDP
sessionと共有しない。wire protocolはVersion `0.6.0.0`と互換である。

接続済みupstream Socketはsend／receive進捗が30分ない場合、対象streamだけを
`CLOSE(Timeout)`として終了する。allowlistは起動時に一度だけ全体検証するため、
設定変更はAgent再起動時に反映する。不正な新設定を部分採用しない。
