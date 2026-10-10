# ZRdp SVN Bridge 診断ログ

更新日: 2026-10-10

現行の自動Agent管理は [ADR 0022](adr/0022-connection-agent-lifecycle.md) に従う。
自動起動の失敗はsession titleのAgent異常と既存の固定段階・分類・HRESULTで確認する。
以下の開発入力テストに関する記述は過去の診断履歴であり、現行製品のUI・送信APIは削除済み。

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
D:\develop\zrdp\out\logs\ZRdpSvnBridge.log
D:\develop\zrdp\out\logs\ZRdpSvnAgent.log
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
Window終了中、Microsoft認証dialog表示済みのいずれでもない場合だけ、一回目を
2,000 ms後、同じsignatureが続いた二回目を5,000 ms後に再試行する。固定eventは
次の順になる。

```text
event=rdp.pre_login_retry_scheduled attempt=1 max_attempts=2 reason=263 extended_reason=0 port=18080
event=rdp.pre_login_retry_started attempt=1 port=18080
event=rdp.pre_login_retry_scheduled attempt=2 max_attempts=2 reason=263 extended_reason=0 port=18080
event=rdp.pre_login_retry_started attempt=2 port=18080
event=rdp.pre_login_retry_completed attempt=2 port=18080
```

`completed`は二回目または三回目の接続で`OnLoginComplete`へ到達した場合だけ
記録する。三回目も同じ数値組で切断された場合は、
`rdp.pre_login_retry_exhausted attempt=2 max_attempts=2`を記録して従来のerror
dialogを表示する。異なるreason、認証UI表示後、credential取消、成立済みsessionの
切断は自動再試行しない。固定field以外の説明、接続先、domain、ユーザー名、
credential、passwordは記録しない。

Windowsのconnection security dialog互換設定とserver認証levelは、接続先や
certificate情報を含めず次の固定数値だけを記録する。

```text
event=rdp.security_dialog_configuration_returned hresult=0x00000000 dialog_version=1 enabled=1
event=rdp.authentication_level_configuration_returned hresult=0x00000000 authentication_level=2
```

`dialog_version=1`はMicrosoftが提供する更新前dialog互換値である。property追加前の
旧ActiveXでは`enabled=0`と既知の未対応HRESULTをWarningで記録し、そのsystemの
既定dialogを維持する。その他の失敗は接続前設定を停止する。
`authentication_level=2`以外のreadbackではConnectを開始しない。certificate名、
issuer、接続先、利用者の選択内容は記録しない。

ActiveXの認証dialog親をControl Windowの既定処理へ委譲した候補は、一instance生成時に
次の固定eventを記録する。

```text
event=rdp.activex_default_dialog_parent_selected
```

このeventは`UIParentWindowHandle`をsession top-level Windowへ明示設定していないことを
示す。親HWND、Window title、接続先、certificate情報、利用者の選択は記録しない。

Microsoft認証warningを同一試行で二回以上表示した後、login未完了、非local、
credential取消以外、`discReason=263`、`ExtendedDisconnectReason=0`が完全一致した
切断は、certificate警告の取消候補として次の固定fieldを記録する。

```text
event=rdp.event.disconnected disconnect_reason=263 extended_disconnect_reason=0 security_warning_cancelled=1
```

`security_warning_cancelled=1`では追加のRDP接続errorを表示せず、対象session Windowだけを
閉じる。このfieldはActiveXが公開しないbutton選択値そのものではなく、warning表示段階と
切断値の限定分類である。certificate名、接続先、利用者の選択文字列は記録しない。

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

## 初回Agent配置の停止診断

初回配置の画面は、停止段階、固定分類、HRESULTを表示する。入力開始時の失敗は
即時停止とする。操作開始から45秒経過した場合は、Enter送信済みのAgent応答待ちと、
入力が完了する前の期限到達を区別する。
最初の停止理由は後続取消で上書きしない。キー送信APIの成功は起動や配置の完了を意味しない。

Client logの`agent_deployment.input_returned`は各入力段階の結果、
`agent_deployment.stopped`は画面側の最初の停止を記録する。
`code`は段階、`reason`は分類、`hresult`は失敗したAPIの元HRESULTまたは固定分類値である。
入力段階のlogには識別用のloopback Proxy `port`も含める。

| code | 段階 |
|---:|---|
| 0 | 段階未確定 |
| 1 | owner STAへの要求受付 |
| 2 | 認証世代、専用drive、Bridge条件の確認 |
| 3 | 固定起動文字列のclipboard公開 |
| 4 | Win+R送信 |
| 5 | 入力timer開始 |
| 6 | Ctrl+V直前の確認と送信 |
| 7 | Enter直前の確認と送信 |
| 8 | Agent応答待ち |
| 9 | 入力focus・Windowsキー転送の準備 |

分類は`0=失敗なし`、`1=認証世代失効`、`2=開始済み／不正段階`、
`3=専用drive未準備／取消済み`、`4=Bridge状態変化`、`5=clipboard世代不一致／取得不能`、
`6=Win32処理失敗`、`7=キー送信API失敗`、`8=queue拒否`、`9=timer生成失敗`、
`10=45秒応答なし`、`11=例外`、`12=詳細状態取得不能`、`13=入力完了前の期限到達`、
`14=入力focus／キー転送の準備失敗`、`15=固定入力の部分受付`である。

SendInput方式では初回準備後350 ms、後続入力のAPI終了後700 ms以上を空ける。
予定時刻から3秒遅れた入力は`段階5／分類13`で停止する。途中の別Window切替や最小化は
未送信入力を停止し、前景復帰でも再開しない。部分受付では元HRESULTを保持したまま分類15で止める。

### 製品の固定入力受付

`rdp.deployment_run_input_returned`、`rdp.deployment_paste_input_returned`、
`rdp.agent_start_input_returned`はそれぞれWin＋R、Ctrl＋V、EnterのSendInput境界結果である。
`code`は受付event数で、Win＋R／Ctrl＋Vは4、Enterは2だけが全件受付である。
`reason`は`0=全件受付`、`1=非所有STA`、`2=未認証・警告・切断`、`3=実Control focus／前景不一致`、
`4=関連キーまたは修飾キー押下中`、`5=全拒否`、`6=部分受付`、`7=不正件数・例外`、
`9=COM照会後のowner承認失効・不在`である。`8`は画面側で専用method未到達を表す。

`win32_error`は不完全受付直後の元GetLastError。0でも全拒否ならE_FAIL、部分受付なら
ERROR_PARTIAL_COPY相当に分類し、非0の元errorは対応するHRESULTとして保持する。
全件受付ではerrorを0とする。この値でUIPI等の原因を確定しない。
受付数は貼付け・Run表示・起動の成功ではなく、Agent応答も別途確認する。
部分受付でも追加解放・再送はせず、該当するWin／Ctrl／R／V／Enterを利用者が手動で押して離す。
clipboard内容、起動文字列、入力先、画面や選択内容は記録しない。

`rdp.deployment_keyboard_configuration_returned`は接続前設定と入力直前のreadbackを
記録する。`code`は標準`KeyboardHookMode`の観測値（未取得は-1）、`enabled`は
`EnableWindowsKey`の観測値を0／1へ変換した値、`hresult`は設定または確認の結果である。
`code=1 enabled=1 hresult=0x00000000`だけを自動入力の準備条件成立として扱う。

`rdp.deployment_input_focus_returned`は入力準備の結果であり、キーを送った証明ではない。
`enabled=1`は準備成功、`enabled=0`は停止、`hresult`は元APIまたは固定分類値である。
このeventの`reason`は画面の分類番号とは別の次の固定値である。

| reason | 入力準備の結果 |
|---:|---|
| 0 | 前景、active Window、Control focusを確認済み |
| 1 | 別アプリが前景、または前景が確認中に変化 |
| 2 | 対象が非準備・最小化・無効、認証警告中、または接続状態が変化 |
| 3 | 対象sessionの前景化を確認できない |
| 4 | 対象sessionのactive状態／Control focusを確認できない |
| 5 | Windowsキー転送設定を確認できない |

HWND、process名、Window title、起動文字列、clipboard内容は追加しない。
旧SendKeys方式の`code=4 reason=7 hresult=0x80004005`はWin+RのSendKeysがE_FAILを返した結果であり、
focus、転送policy、Control内部状態のどれが原因かをこの値だけで断定しない。
修正版では上記の準備eventを先に確認する。接続先の警告やpolicyを変更せず、
同じE_FAILが続く場合は入力APIの結果と環境条件を踏まえて追加の設計判断を行う。

例として`code=6 reason=5 hresult=0x800704D5`は、Ctrl+V直前にclipboard世代の
一致を確認できず、貼付けを停止した状態である。どのアプリがclipboardを変更したかは
記録・推測しない。`code=8 reason=10 hresult=0x80070102`は応答待ち期限の到達であり、
配置失敗、Windows警告待ち、起動失敗のどれかをClientだけで断定しない。

## 開発用WinR入力テスト

署名なしの`入力テスト（開発用）▼`では、選択した固定種類ごとに結果eventを分ける。

- Win＋Rは`rdp.run_input_test_returned`。
- 左Windowsキー単独は`rdp.win_key_input_test_returned`。
- Rキー単独は`rdp.r_key_input_test_returned`。
- R単独SendInput比較は`rdp.r_key_emulation_test_returned`。
- Win＋RのSendInput比較は`rdp.win_r_key_emulation_test_returned`。
- Ctrl＋Vのメモ帳診断は`rdp.paste_key_emulation_test_returned`。
- Enterのメモ帳診断は`rdp.enter_key_emulation_test_returned`。

自動配置の結果とは別であり、このeventでAgent配置や起動の完了を判定しない。
`code`は`1=接続・競合条件の確認`、`2=利用者確認`、`3=入力focus・キー転送準備`、
`4=選択した固定キー列の送信`である。`reason`は`0=入力受付`、`1=認証世代失効`、`2=他操作等との競合`、
`3=非所有thread`、`4=利用者取消`、`5=入力準備・再確認失敗`、`6=選択した入力API失敗`、`7=例外`、
`8=timer予約失敗または入力前の待機期限到達`、`9=Pasteのclipboard世代取得不能／変更`である。
画面には同じ段階・分類・HRESULTを表示する。取消では追加入力も結果dialogも表示しない。
他Window前景、認証警告、最小化、終了・世代失効時は結果dialogを表示せず、ログだけに記録する。

`code=3 reason=5`では、先行する`rdp.deployment_keyboard_configuration_returned`と
`rdp.deployment_input_focus_returned`を確認する。
正常時は準備後250 ms以上を空けて、同じキー設定とfocusのreadback eventがもう一組出る。
`code=3 reason=8 hresult=0x80070102`は確認終了から送信までの3秒期限に到達した状態であり、
入力APIは呼ばない。timer予約失敗の場合は元Win32 HRESULTを保持する。
`code=4 reason=6 hresult=0x80004005`は、準備と送信直前の認証条件を確認した後も
選択した固定キー列の入力APIがE_FAILを返した状態である。元HRESULTを保持して停止し、再送しない。
`code=4 reason=0 hresult=0x00000000`は入力API受付であり、Run dialog、スタートメニュー等、文字入力は利用者が
接続先画面で確認する。clipboard内容、command、接続先、HWND、画面captureを記録しない。
通常の送信結果eventと結果dialogは、入力API終了から1000 ms以上待ってから記録・表示する。
Window破棄時は待機を終了して元結果をログに残し、dialogは表示しない。
送信済みの結果は終了処理の取消で上書きせず、未送信の取消だけを失効として扱う。
単独testのevent名は利用者が選択した固定診断の識別だけであり、実キー入力のcaptureではない。
メモ帳内容、IME状態、起動中application、実接続先を追加記録しない。

### R単独SendInputの受付数

`rdp.r_key_emulation_input_returned`は、専用methodの送信直前条件またはSendInputの
数値結果を記録する。controllerの結果eventとは別であり、`code`は入力eventの受付数である。
`2`だけが全二要素の受付、`0`は未入力または全拒否、`1`は不完全受付を示す。

このeventの`reason`は、`0=全二要素受付`、`1=非所有STA`、`2=未認証・警告・切断`、
`3=当該ActiveX Windowまたは前景・focus不一致`、`4=Rまたは修飾キー押下中`、
`5=入力全拒否`、`6=一要素の部分受付`、`7=不正受付数・例外`、`9=owner承認失効`である。
画面の送信分類`8`は確認・準備等で専用methodへ到達していない状態を示す。

`win32_error`は不完全受付直後の元GetLastErrorを保持する。0もそのまま出力し、
0で全拒否ならHRESULTはE_FAIL、一要素ならERROR_PARTIAL_COPY相当へ分類する。
元errorが非0なら対応するHRESULTを保持する。受付二要素の成功ではerrorを0とする。
UIPI等が原因だったかはこの値だけで確定しない。

部分受付でも自動再送・追加解放入力・別経路への切替は行わない。Rの押下が残る可能性が
ある場合は、利用者がRを手動で押して離す。受付二要素も接続先の文字入力成功とは別である。
key状態そのもの、入力文字、HWND、接続先、画面captureはログへ追加しない。

### WinRのSendInput受付数

`rdp.win_r_key_emulation_input_returned`は固定Win＋R比較の専用method結果を記録する。
`code=4`だけが四要素の全受付、`0`は未入力または全拒否、`1〜3`は部分受付である。
controller結果の`code=4`は送信段階を表すため、二つのeventの意味を混同しない。

`reason`はR比較と同じ固定値を使い、`0=全件受付`、`1=非所有STA`、`2=未認証・警告・切断`、
`3=当該ActiveX Windowまたは前景・focus不一致`、`4=Rまたは修飾キー押下中`、
`5=全拒否`、`6=部分受付`、`7=不正件数・例外`、`9=owner承認失効`である。画面の`8`は専用method未到達を示す。
元Win32 errorを保持し、0の場合も全拒否はE_FAIL、部分受付はERROR_PARTIAL_COPY相当に分類する。

全四要素受付はRun画面の表示成功ではない。接続先とローカル側のどちらに開いたかは利用者が確認する。
部分受付でも再送・追加解放入力はしない。Win／Rの押下が残る場合は利用者が左WinとRを手動で押して離す。
診断は貼付け・Enter・command・Agent起動を追加せず、Run画面の内容や表示先も記録しない。

### 貼付けとEnterのSendInput受付数

`rdp.paste_key_emulation_input_returned`は固定Ctrl＋V、`rdp.enter_key_emulation_input_returned`は
固定主Enterの診断専用methodの結果である。controllerの段階結果と分け、`code`は受付数を表す。
Pasteは4、Enterは2だけが全件受付であり、0は未入力／全拒否、その間の値は部分受付である。

`reason`は既存のSendInput境界と共通で、0=全件受付、1=非所有STA、2=未認証・警告・切断、
3=実Control focus／前景不一致、4=関連キー・修飾キー押下、5=全拒否、6=部分受付、
7=不正件数・例外、9=COM照会後のowner承認不在／失効である。画面の8はmethod未到達を示す。
`win32_error`は不完全受付直後の元値であり、0でも全拒否はE_FAIL、部分受付はERROR_PARTIAL_COPY相当にする。
全件受付は接続先の貼付け・改行成功ではない。

Pasteの確認・待機中にclipboard世代が変わった場合はcontrollerの`code=1 reason=9`、
HRESULT 0x800704D5で停止する。送信callbackのCOM照会後にclipboard・世代・取消・期限が
失効した場合はnativeの`reason=9`、受付数0、HRESULT 0x800704C7を返す。
主分類9と送信分類9の意味を混同しない。同じclipboard番号も内容やRDP同期を保証しない。

clipboardの内容・世代番号、入力先、Window識別子、commandは記録しない。
部分受付ではCtrl／VまたはEnterを利用者が手動で押して離す。再送・追加解放・配置へのfallbackはしない。
入力済みEnterは取消できない。操作手順は[署名なしで貼付けとEnterを確認する](agent-settings.md#署名なしで貼付けとenterを確認する)を参照する。

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
