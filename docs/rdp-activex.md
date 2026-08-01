# ZRdp Remote Desktop ActiveX設計

更新日: 2026-07-31

## 1. 責務

`RdpControlHost`はWindowsにインストール済みのMicrosoft Remote Desktop
ActiveX ControlをATL `CAxWindow`へ収容し、次の版依存処理をUIから隔離する。

- 利用可能なControlを新しい版から選択する
- `IMsTscAxEvents`を購読・解除する
- 接続前設定、接続、切断、全画面、表示modeを適用する
- callback引数を検証し、BSTRとVirtual Channel payloadを所有copy化する
- COM interface、event sink、購読cookie、child windowを順序付きで解放する

ActiveXの作成から破棄までを、`CoInitializeEx`でSTA化したUI threadだけで行う。
worker threadからCOM interfaceを直接呼び出さない。

Version `0.7.14.0`では、接続管理Windowをmanager COM STA、各`MainWindow`と
`RdpControlHost`をsession専用COM STAで所有する。Control instance、ATL child
window、event sink、Connection Point cookie、`ESVNBRG` transport、message loopを
session間で共有しない。一つのsessionでMicrosoft Controlの切断または破棄が待機
しても、別sessionとmanagerのWindow messageを処理できる。最大16組という有限上限は
維持する。詳細はADR 0005に記録する。

## 2. 認証とsecurity

- passwordはZRdpの設定modelへ受け取らず、Microsoft標準credential UIへ委譲する。
- ドメインとユーザー名は別入力とし、接続前に`IMsTscAx::put_Domain`と
  `put_UserName`へそれぞれ設定する。
- 空ドメインも接続ごとに設定し、同じActiveX instanceに前回domainを残さない。
- ドメイン欄が空の旧`DOMAIN\user`入力だけをClient UIで分離し、
  `RdpConnectionSettings`とActiveX境界では結合済み値を拒否する。
- UPN形式のユーザー名からdomainを推測しない。
- CredSSPを有効化してreadbackし、NLAを弱めない。
- server authentication failureは警告表示とし、自動承認しない。
- credential保存を許可しない。
- `mstsc.exe`、system DLL、RDP protocolをコピーまたは再実装しない。

## 3. 表示mode

接続管理Windowの3つのラジオボタンを`RdpDisplayMode`へ対応させる。
接続後のセッションWindowでは同じ3 modeに加え、既存ActiveX全画面状態を
切り替える「全画面」buttonを同じ表示操作欄へ配置する。

| `RdpDisplayMode` | UI | ActiveX設定 |
|---|---|---|
| `NativeScroll` | 等倍表示（スクロール） | `SmartSizing=FALSE` |
| `SmartSizing` | Smart Sizing（縮小表示） | `SmartSizing=TRUE` |
| `DynamicResolution` | 動的解像度（Window追従） | `SmartSizing=FALSE`と`UpdateSessionDisplaySettings` |

`NativeScroll`と`SmartSizing`は現在monitor全体を初期desktop寸法に使用する。
`DynamicResolution`は現在のRDP child window client領域を初期寸法に使用する。

動的解像度では、ActiveX child windowを`WM_SIZE`で即時resizeした後、最後の
変更から250 ms後にUI threadで次を呼ぶ。

```cpp
IMsRdpClient9::UpdateSessionDisplaySettings(
    width,
    height,
    0,
    0,
    0,
    100,
    100
);
```

物理寸法は未指定、orientationは既定、desktop／device scaleは100%とする。
寸法は200～8192 pixelへ制限する。通常WindowではRDP childのclient領域、
ActiveX全画面ではセッションWindowが属するmonitor全体をtargetとする。
全画面移行とWindow復帰の両方でtimerを再予約し、timer実行時点の状態から
targetを選ぶ。寸法変更のために`Reconnect`は使用しない。

初回接続の`OnConnected`はtransport成立だけを示すため、ここではtimerを予約せず、
`OnLoginComplete`後に初回の動的解像度を適用する。表示modeの実行時切替も
ログイン完了まで無効にする。自動再接続完了は既存のログイン済みsessionへの復帰
として扱い、動的解像度を再予約する。

session Windowには3つのラジオボタンと全画面buttonを一行で表示し、
ログイン完了後の切替を可能にする。`OnLoginComplete`直後でもMicrosoft Controlが
表示更新準備中の場合は、
`UpdateSessionDisplaySettings`が一時的に`E_UNEXPECTED`を返す。この値だけを
500 ms、1,000 ms、1,500 msの有限delayで最大3回再試行する。再試行中はdialogを
表示せず、成功すれば動的解像度を維持する。ほかのHRESULTまたは再試行上限後は
RDP接続を維持したまま`NativeScroll`へ縮退し、Smart Sizingを暗黙に有効化しない。

### 3.1 手動Window変更のアスペクト比

`MainWindow`は`Connect()`が受理された時点で、そのsessionがActiveXへ要求した
初期desktop幅／高さを保持する。通常Windowの辺または角を利用者がdragした場合、
`WM_SIZING`の候補矩形を補正し、実際のRDP child window領域を同じアスペクト比に
維持する。top-level Windowのtitle bar、frame、表示mode欄は比率計算から除外する。

一回のdrag開始時に`WM_ENTERSIZEMOVE`で基準寸法を固定する。左右辺では幅、上下辺
では高さを主軸とし、角では基準寸法からの変化率が大きい軸を主軸とする。
左または上を動かした場合は反対側をanchorする。`WM_EXITSIZEMOVE`で一時的な
基準寸法を破棄し、session間では共有しない。

この制約は通常の手動resizeだけに適用する。最大化、Windowsスナップ、
ActiveX全画面はWindows／Microsoft Controlの標準矩形を維持する。
`NativeScroll`と`SmartSizing`ではremote desktop寸法を変更しない。
`DynamicResolution`だけが制約後のRDP領域を既存の250 ms debounce経路で
接続先へ適用する。いずれもWindow変更を理由に`Reconnect`を呼び出さない。

接続管理WindowはWindows標準ダイアログ色を親背景に使用し、ラベルと
ラジオボタンの背景矩形を同色へそろえる。任意の接続名を履歴から選択した場合は、
対応する接続先、ドメイン、ユーザー名を一組で復元する。

session Windowはtitle bar、表示mode一行、RDP表示で構成する。通常titleは
接続名があれば`接続名 (接続先)`、名前がなければ接続先を表示し、
RDP／Agent／Proxy状態、session専用Proxy port、stream数、方向別転送量を表示する。
表示mode一行の全画面buttonと、Windows標準RDP互換の`Ctrl+Alt+Break`要求は
同じActiveX全画面APIとevent経路を使用する。eventで実際の状態を受けた後に
button表示を「全画面」または「ウィンドウ」へ更新する。

## 4. 全画面での接続先識別

全画面connection barと全画面window titleへ、次の形式で現在の接続先を表示する。

```text
ZRdp SVN Bridge - <接続先PC名またはIPアドレス>
```

`RdpConnectionSettings::server`として検証済みの値だけを使用し、ユーザー名や
passwordは含めない。hostname入力時はDNS解決して実IPを探索せず、入力値を
そのまま表示する。

表示用BSTRは`FullScreenTitle`の上限である`MAX_PATH - 1`文字以内で構築する。
通常のIPアドレスとhostnameは完全表示し、上限を超える入力は接続先設定を
変更せず、表示末尾だけを`...`として省略する。同じ有限長BSTRを
`FullScreenTitle`と`ConnectionBarText`へ設定する。

どちらかを設定できない場合、または`ConnectionBarText`のreadbackが設定した
有限長BSTRと完全一致しない場合は接続前設定を失敗させる。接続先を診断logや
resourceへ出力しない。

## 5. Clipboard

Phase 0必須機能として、接続前に
`IMsRdpClientAdvancedSettings5::RedirectClipboard=TRUE`を設定してreadbackする。
clipboard dataをZRdp自身で取得、保存、log出力しない。双方向転送はMicrosoft
Controlへ委譲し、GPO等で拒否された場合にpolicy変更や回避を行わない。

## 6. 音声リダイレクト

接続管理Windowで、接続開始前に次を選択する。

- `マイクを接続先で使用する`: 既定OFF
- `リモート音声`: 既定は`このPCで再生する`

リモート音声は次の固定値だけを`RdpAudioPlaybackMode`として扱う。

| UI | ActiveX値 |
|---|---:|
| このPCで再生する | 0 |
| 接続先PCで再生する | 1 |
| 再生しない | 2 |

`RdpControlHost::ApplyConnectionSettings`は未接続状態で
`IMsRdpClientAdvancedSettings5::AudioRedirectionMode`と
`IMsRdpClientAdvancedSettings7::AudioCaptureRedirectionMode`を設定する。
setter後に両値をreadbackし、選択値と完全一致しない場合は`Connect`を開始しない。

これらはRDP接続時に確定する設定である。session Windowへ音声設定controlは
表示せず、確立済みsessionへ途中反映しない。変更後は新しいsessionを開始するか、
対象sessionを切断して再接続する。接続先GPOやWindowsのprivacy設定が転送を
禁止する場合、ZRdpはpolicy変更や別経路へのfallbackを行わない。音声data、
device名、音量、録音内容は保存またはlog出力しない。

## 7. Event

最低限、次のeventを`RdpConnectionState`とUIへ反映する。

- `OnConnecting`
- `OnConnected`
- `OnLoginComplete`
- `OnDisconnected`
- `OnEnterFullScreen`
- `OnLeaveFullScreen`
- `OnFatalError`
- `OnWarning`
- `OnAutoReconnecting2`
- `OnAutoReconnected`
- `OnChannelReceivedData`
- `OnConfirmClose`

COM callbackからC++例外を漏らさない。切断説明はUI dialogだけに使用し、
診断logには数値reasonとHRESULTだけを記録する。

Microsoft標準credential UIを利用者がcloseまたはCancelした場合、現在の
ActiveXは`OnDisconnected`を通知する。ログイン完了前の`Connecting`または
`Connected`、reason `7943 (0x1F07)`、extended reason
`exDiscReasonNoInfo`、非local切断がすべて一致した場合だけcredential promptの
利用者取消と分類する。この場合は追加error dialogを表示せず、固定の未接続表示へ
戻す。

reason単独で分類せず、ログイン前の別reasonまたは拡張reasonありの切断は従来の
error表示とする。`LoginComplete`後に接続先再起動、通信断、remote logoff等で
最終切断された場合は、利用不能なsession Windowを残さず、追加error dialogを
表示せずにWindowを閉じる。ZRdp自身の切断要求とcredential取消はこの外部切断
判定から除外し、それぞれ既存の終了経路と未接続復帰へ委ねる。

ただしVersion `0.7.14.0`は、ClientとWindows event logで観測したログイン前の
`discReason=263`、`ExtendedDisconnectReason=0`完全一致だけを一時失敗候補とする。
local切断、credential取消、Window終了中、login完了済み、一回予約済みのいずれでも
ない場合に限り、COM callback終了後のWindow timerから2,000 ms後に同じ検証済み
設定を再適用し、Microsoft ActiveXの通常`Connect()`を一度だけ呼ぶ。二度目の失敗、
timer確保失敗、設定失敗、同期的`Connect()`失敗では追加再試行しない。
`mstsc.exe`を起動せず、passwordを取得せず、NLA、CredSSP、server certificate
検証、認証警告を変更または自動承認しない。詳細はADR 0007に記録する。

公開仕様上の`OnDisconnected` reasonと`ExtendedDisconnectReason`は診断logへ
数値だけを残し、切断説明やcredential内容をlogへ出力しない。自動再接続中は
Windowを維持し、`OnAutoReconnected`で復帰できず`OnDisconnected`へ至った場合だけ
外部切断終了を適用する。

Version `0.7.14.0`では、接続中Session Windowの閉じる操作も通常の切断buttonと同じ
`Disconnect()`へ統一する。`OnDisconnected`を受信してControlが切断済みになった後だけ
Windowを破棄する。既に`Disconnecting`の場合は二重呼出しを行わず同eventを待ち、
未接続時は直ちに破棄する。これは長時間接続後に`OnConfirmClose`直後のATL OLE破棄が
停止し、画面消失後もprocessが残留した実機事象への対策である。`OnConfirmClose`の
購読と許可応答はMicrosoft Control互換境界として維持する。詳細はADR 0008に記録する。

## 8. 検証境界

自動testは実RDP接続を開始せず、非表示のWin32 parent上で次を確認する。

- ActiveX生成と破棄
- 不正な接続前設定の拒否
- 分離したDomain／UserNameの設定と、結合済み・上限超過・埋込みNULの拒否
- 3つの表示modeの接続前設定
- マイクON／OFFと3つのリモート音声再生先の接続前設定とreadback
- 未知の音声再生先の拒否
- 文書用IPv4 literalを使った全画面識別表示の接続前設定とreadback
- 接続先の空、上限超過、埋め込みNULの拒否
- 未接続時のSmart Sizing／動的解像度API拒否
- credential promptキャンセルreasonのログイン前限定分類
- ログイン完了有無とlocal／credential起点による外部切断Window終了policy
- 未接続、接続中、切断中ごとのWindow終了開始policy
- `263/0`完全一致だけの2秒後・最大一回ログイン前接続再試行policy
- 3つのラジオボタンresourceと既定等倍mode
- 8方向の手動Window変更、反対辺anchor、最小寸法、無効値の比率計算
- 同一COM STA上の二つのActiveX同時生成とinstance所有の独立性
- 異なる二つのCOM STA上でActiveXを生成し、一方の破棄完了後も他方のSTAが
  messageへ応答できること
- `E_UNEXPECTED`だけを500 ms、1,000 ms、1,500 msで有限再試行し、interface不在、
  不正引数、成功値を再試行しないこと

接続済みsessionでの画面、入力、clipboard、3 mode、手動Window変更、最大化、
Windowsスナップ、全画面、自動再接続、
マイク入力、リモート音声出力は、ZscalerをONにしたユーザー環境で確認する。
