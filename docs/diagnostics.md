# ZRdp SVN Bridge 診断ログ

更新日: 2026-07-31

## 1. 目的

診断ログは、RDP接続要求、Bridge、Client Proxy、Agent設定がどの段階まで進んだかを、
接続先や認証情報を残さず確認するために使用する。

記録するのは固定名のstate transition、`HRESULT`、ActiveX event code、
再接続回数、Echo完了時の合計byte数等の数値だけである。Virtual Channelの
fragmentごとのbyte数やpayloadは記録しない。任意文字列を渡すlog APIは提供しない。

## 2. 保存場所

Clientの第一候補:

```text
実行EXEと同じdirectory\logs\ZRdpSvnBridge.log
```

Agentの第一候補:

```text
実行EXEと同じdirectory\logs\ZRdpSvnAgent.log
```

現在のworkspace buildでは次になる。

```text
C:\develop\zrdp\out\logs\ZRdpSvnBridge.log
C:\develop\zrdp\out\logs\ZRdpSvnAgent.log
```

EXEと同じdirectoryへ書き込めない場合のfallback:

```text
Client: %LOCALAPPDATA%\ZRdpSvnBridge\logs\ZRdpSvnBridge.log
Agent:  %LOCALAPPDATA%\ZRdpSvnBridge\logs\ZRdpSvnAgent.log
```

installerは不要であり、registry、Windows Event Log、OS全体の設定は変更しない。
ClientとAgentは別PCで動作するため、それぞれの実行場所またはLocalAppDataへ
役割別の固定file名でlogを作成する。同じdirectoryに両EXEを配置した場合も、
current logとbackup世代を相互に上書きしない。

## 3. 保持上限

- current log: 最大1 MiB
- backup: `.1`、`.2`、`.3`の3世代
- UI／COM thread: file I/Oを行わず、最大512 recordのbounded queueへ投入
- queue上限超過: payloadを保存せず、後続の`log.records_dropped records=<count>`で件数だけを記録
- 各record: worker threadで書込み後にflush

## 4. 記録形式

各行はUTC timestampを持つkey-value形式である。

```text
timestamp=2026-07-31T00:00:00.000Z level=INFO role=client version=0.7.14 session=00000000-0000-0000-0000-000000000000 event=ui.connect_button_clicked
timestamp=2026-07-31T00:00:00.001Z level=INFO role=client version=0.7.14 session=00000000-0000-0000-0000-000000000000 event=rdp.connect_call_returned hresult=0x00000000
```

`session`は一回のprocess起動を表す匿名IDであり、実RDP接続先ではない。
複数RDP sessionのProxy eventは`port`で区別できる。RDP接続先、domain、
ユーザー名、Window titleはlogへ追加しないため、複数sessionの調査では
session Windowに表示されたportとevent時刻を対応させる。

接続操作では、原則として次の順序を確認できる。

1. `ui.connect_button_clicked`
2. `rdp.connection_settings_apply_started`
3. `rdp.credssp_configuration_returned hresult=0x00000000 enabled=1`
4. `rdp.connection_settings_apply_returned`
5. `rdp.connect_call_started`
6. `rdp.connect_call_returned`
7. `rdp.event.connecting`
8. `rdp.event.connected`
9. `rdp.event.login_complete`

途中で失敗した場合は、最後のeventと`hresult`／`code`を確認する。`Connect()`は非同期接続の開始結果だけを返すため、接続完了は`rdp.event.*`で判断する。

予期しない切断は次のように記録する。

```text
event=rdp.event.disconnected code=2825 extended_reason=0 locally_requested=0 credential_prompt_cancelled=0 port=18081
```

- `code`: ActiveXのdisconnect reason
- `extended_reason`: ActiveXから取得した拡張切断理由
- `locally_requested`: ZRdpの切断操作または終了処理による切断なら`1`、予期しない切断なら`0`
- `credential_prompt_cancelled`: ログイン前のMicrosoft標準credential UIを利用者が閉じた完全一致条件なら`1`
- `port`: 対応sessionへ割り当てたloopback Proxy port。接続先ではない

Windowsが返す説明文はUIのerror dialogにだけ表示し、接続先等が混入する可能性を排除するためlogには保存しない。

credential UIの利用者取消はInformationとして次の数値だけを記録する。

```text
event=rdp.event.disconnected code=7943 extended_reason=0 locally_requested=0 credential_prompt_cancelled=1
```

この場合は追加のRDP接続error dialogを表示しない。`LoginComplete`後の切断、
異なるreason、拡張reasonありの場合は`credential_prompt_cancelled=0`となり、
通常のerror表示を維持する。

ログイン完了前の`code=263 extended_reason=0`が、local切断、credential取消、
Window終了中のいずれでもなく、現在の接続意図でまだ再試行していない場合だけ、
2,000 ms後に一度再試行する。固定eventは次の順になる。

```text
event=rdp.pre_login_retry_scheduled attempt=1 max_attempts=1 reason=263 extended_reason=0 port=18080
event=rdp.pre_login_retry_started attempt=1 port=18080
event=rdp.pre_login_retry_completed attempt=1 port=18080
```

`completed`は二回目の接続で`OnLoginComplete`へ到達した場合だけ記録する。
二回目も同じ数値組で切断された場合は、`rdp.pre_login_retry_exhausted`を記録して
従来のerror dialogを表示する。異なるreason、credential取消、成立済みsessionの
切断は自動再試行しない。固定field以外の説明、接続先、domain、ユーザー名、
credential、passwordは記録しない。

接続設定開始時の`display_mode`は、排他的な表示modeの選択を表す。

```text
event=rdp.connection_settings_apply_started display_mode=0
```

- `display_mode=0`: 等倍表示。remote desktop寸法を維持し、必要時にActiveX標準scroll barを使用する。
- `display_mode=1`: Smart Sizing。remote desktop寸法を維持し、Client側表示だけをwindowへ縮小する。
- `display_mode=2`: 動的解像度。remote desktop寸法をZRdpのRDP表示領域へ追従させる。

接続中に表示modeを切り替える際のSmart Sizing property反映結果は、remote desktop寸法を含めず次のように記録する。

```text
event=rdp.smart_sizing_call_returned hresult=0x00000000 enabled=1
```

`enabled=1`はSmart Sizing、`enabled=0`は等倍表示または動的解像度への切替を表す。`0x00000000`以外の場合もRDP接続は切断せず、ラジオボタンを直前のmodeへ戻す。

動的解像度のsession更新結果は、画面寸法を含めず次のように記録する。

```text
event=rdp.dynamic_resolution_call_returned hresult=0x00000000 attempt=1 port=18080
```

`attempt`は同じ更新要求に対する1から始まる試行回数、`port`はsession識別用の
loopback Proxy portである。`E_UNEXPECTED (0x8000FFFF)`だけは、Microsoft Controlの
ログイン直後の一時状態として500 ms、1,000 ms、1,500 msの有限delayで最大3回
再試行する。後続行が`hresult=0x00000000`なら自動回復している。ほかの失敗または
上限後はRDP接続を維持し、追加の寸法変更を停止するため等倍表示へ縮退する。
Smart Sizingを暗黙に有効化せず、`Reconnect`も行わない。

session Windowの終了要求にも対応portを記録する。

```text
event=ui.close_requested port=18081
```

複数sessionの終了問題では、同じportの`ui.close_requested`、
`rdp.event.confirm_close`、`rdp.event.disconnected`、Proxy停止を時刻順に確認する。
RDP接続先、domain、ユーザー名は引き続き記録しない。

接続履歴の読込と保存は、内容を含めず結果と件数だけを記録する。

```text
event=ui.connection_history_load_returned hresult=0x00000000 records=2
event=ui.connection_history_save_returned hresult=0x00000000 records=2
```

履歴がまだない場合の読込結果は`S_FALSE`であり、
`hresult=0x00000001 records=0`となる。接続先、ドメイン、ユーザー名は
このeventにも、ほかのeventにも記録しない。

`ui.connect_input_rejected`の`reason`は次の固定値である。

| reason | 意味 |
|---:|---|
| 1 | 接続先が空 |
| 2 | 選択modeに必要な初期remote desktop寸法をmonitorまたはRDP表示領域から取得できない |
| 3 | ドメインとユーザー名の組合せ、区切り、文字数、埋込みNULが不正 |

## 5. BridgeとClient Proxyの確認

ClientはActiveX instanceの最初の接続設定時に、登録結果を一度記録する。

```text
event=rdp.virtual_channel_registration_returned hresult=0x00000000
```

`hresult`が成功でなければZRdpはRDP専用modeへ暗黙に縮退せず、その接続要求を
開始しない。登録成功後、RDPの`login_complete`からAgent待機を開始する。

正常なClient側のPhase 1 event順序は次となる。

```text
event=bridge.session_started attempt=1
event=bridge.ready
event=bridge.echo_started
event=bridge.echo_passed bytes=11600897
```

HELLOはAgent起動まで1秒間隔で再送するが、retryごとのeventは記録しない。
PING／PONG、fragment、payload、途中progressも記録せず、全targetの完全一致後に
合計byte数だけを一度記録する。

Agent側では次の順序を確認する。

```text
event=agent.session_validation_returned
event=agent.channel_open_returned
event=agent.allowlist_load_returned records=3
event=agent.winsock_initialization_returned reason=0
event=agent.stream_relay_enabled enabled=1
event=bridge.ready
```

設定欠落／不正時は`agent.allowlist_load_returned reason=<固定分類>
extended_reason=<行番号>`をWarningで記録し、streamを有効化せずEcho-onlyで
継続する。`records`は採用件数であり、実IP、port、匿名endpoint ID、設定path、
入力行は記録しない。Winsock失敗時も数値error codeだけを`reason`へ記録する。

Ctrl+CでAgentを正常終了すると、Clientはconnection-level `CLOSE`を受信して
同じRDP接続のまま新しいBridge sessionを開始する。Client logには次の
`bridge.session_started attempt=1`が追加され、再起動AgentのHELLO_ACK後に
`bridge.ready`へ戻る。CLOSE自体やHELLO retryごとのeventは、通常logを増やさない
ため個別記録しない。handshake成立後はactive streamをresetしないよう周期HELLOを
送らない。

AgentがCLOSEを送れず異常終了した場合はhealth PINGの無応答を検出し、次の固定
eventを一度記録して新HELLO待機へ戻る。

```text
event=bridge.peer_timeout
event=bridge.session_started attempt=1
```

health payload、sequence、旧streamのendpointは記録しない。検出までの上限は、
最初の5秒intervalと15秒deadlineを合わせた約20秒である。

Client Proxyは起動とBridge利用可否の変化を、固定数値だけで記録する。

```text
event=proxy.client_state_changed proxy_state=1 proxy_error=0 port=0 enabled=0
event=proxy.client_state_changed proxy_state=2 proxy_error=0 port=18080 enabled=0
event=proxy.client_state_changed proxy_state=2 proxy_error=0 port=18080 enabled=1
```

`proxy_state`は`0=Stopped`、`1=Starting`、`2=Listening`、`3=Failed`である。
`enabled=1`はcompatible HELLOが成立し、新しいlocal connectionからBridge streamを
開始できる状態を表す。listener作成またはworker relayに失敗した場合は
`proxy.client_start_failed`と固定`proxy_error`、`reason`のWinsock数値codeを
記録する。

active stream数と方向別合計byte数はUI状態表示にだけ使用し、通常logへrequest単位で
記録しない。local peer、HTTP method、URL、Host、header、body、Agent endpointは
記録しない。

異常時の固定eventは次である。

| Event | 数値field | 意味 |
|---|---|---|
| `bridge.failed` | `bridge_state`, `bridge_error` | protocol、queue、transport、Echo timeout |
| `bridge.peer_timeout` | なし | Agent health PINGが15秒無応答。旧stream破棄後に再HELLO |
| `agent.session_validation_returned` | `code`, `reason` | current active RDP session条件を満たさない |
| `agent.channel_open_returned` | `reason` | `ESVNBRG` open失敗 |
| `agent.channel_read_failed` | `reason` | RDP切断またはWTS read失敗 |
| `agent.channel_write_failed` | `reason` | WTS writeまたはpartial write失敗 |

Clientの`bridge_error=9`は手動Echo中の未応答PINGが15秒timeoutしたことを表す。
この場合もRDP画面を切断せず、Bridgeだけを停止する。通常運用中のAgent無応答は
`bridge.peer_timeout`で区別し、旧Proxy streamを破棄して自動的に新HELLOを開始する。

## 6. 記録しない情報

次はDebug buildを含め、診断ログへ渡さない。

- RDP接続先PC名／IPアドレス
- RDPドメイン／ユーザー名／password／credential
- URL、path、query
- HTTP header、Cookie、Authorization
- HTTP／SVN body、source、diff
- clipboard内容
- Virtual Channel名およびpayload
- 画面capture、Windows event log、packet capture

実機確認で問題が起きた場合、まず同一`session`の行だけを確認する。外部共有前には、固定field以外の情報が混入していないことを念のため目視確認する。
