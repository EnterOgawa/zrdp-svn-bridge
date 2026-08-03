# ZRdp SVN Bridge 既知の制約と残存risk

更新日: 2026-08-03

## 1. Version 0.7.14の検証境界

Version `0.7.14.0`は、Version `0.6.0.0`で通過したPhase 6のローカル／実機回帰と
Phase 7／8の実環境Gateを維持し、複数RDP session Window、title集約UI、
session内のコンパクトな表示mode／全画面操作一行、名前付き接続プロファイル、接続時の
マイク／リモート音声設定、通常drag時のRDP表示領域アスペクト比制約、
ログイン完了後の初回動的解像度適用、成立済みsessionの外部切断時に
error dialogを表示せずWindowを閉じる動作、外周tileを持たない明色系の共通
Client／Agent iconを持つ。
Repository Explorer、Log、Diff、Checkout、Update、Commit、Add、Delete、Move、
conflict、Lock、Unlockは、Version `0.6.0.0`でユーザーから問題なしとの受入結果を
得ている。

- 実環境の操作method、URL、repository、pathは通常logへ記録しないため、操作結果は
  ユーザー受入、通信経路の健全性は固定診断logをそれぞれ根拠とする。
- 実機受入はユーザーが許可したendpointとrepositoryの範囲である。複数endpointの
  同時中継能力はMock結合testで確認しているが、未承認の実endpointへ拡大しない。
- 問題が後日判明した場合は、合格済みPhase全体を暗黙に無効化せず、該当操作を
  改善項目として再オープンする。
- 実環境でStatic Virtual Channelの帯域、遅延、長時間安定性が不足する場合は、
  実測値とADRなしにDynamic Virtual Channelへ変更しない。
- Agentと従来Clientは2026-08-01、R-008 Clientは2026-08-03に署名され、Windows trust
  `Valid`、SHA-256、RFC 3161 timestamp、Version維持、既存Agentとの署名者一致を
  確認した。R-008署名後Clientの起動、RDP、Bridge、Proxy smoke test、最終文書受入、
  配布確認が未完のため、Release Gate全体は未通過である。
- Mylyn Trac `3.26.0`相当のWeb GET／XML-RPC POSTはMock Gateを通過し、
  実Tracの設定検証と同期もユーザー環境で成立した。ticket更新やattachmentは、
  必要になった安全な対象だけを個別確認する。
- 複数RDP sessionとtitle集約UIはlocal ActiveX／port割当Gateを通過したが、
  二つの実RDPのうちAgent接続済みsessionを閉じると、別sessionも応答不能になる
  実機問題が判明した。Version `0.7.10.0`では各RDP Window／ActiveXを専用COM STAへ
  分離し、一方の非接続ActiveX破棄後も他方のSTAが応答するlocal Gateを通過した。
  2026-07-31、異なる二台へ接続して一方だけでAgentを成立させ、そのWindow終了後も
  別RDPとmanagerが応答し、表示mode変更とProxy port再利用ができることをユーザーが
  確認した。
- Version `0.7.13.0`の長時間接続後、Session Windowは消えたがClient processが
  残留する事象を一度確認した。Version `0.7.14.0`は閉じる操作を通常の
  `Disconnect()`／`OnDisconnected`経路へ統一し、Debug／Release各30 CTestを
  通過した。2026-07-31、実RDPでの終了後にWindowとClient processが残留しないことを
  ユーザー環境と固定診断logで確認し、署名前Gateを通過した。
- Version 1／2のDPAPI履歴は安全な音声既定値で読込めるが、Version `0.7.14.0`が
  保存するVersion 3履歴を旧Clientは読めない。rollback時は履歴fileだけを退避または
  削除すればRDP／Bridge機能を継続できる。
- マイクとリモート音声の設定は接続時に確定し、接続中には変更できない。
  設定変更後は新しいsessionを開始するか対象sessionを再接続する必要がある。
- 接続先GPO、Windows privacy、音声deviceの状態がリダイレクトを禁止する場合、
  ZRdp側の選択だけでは利用できない。ZRdpはpolicy変更やdevice列挙を行わない。
- アスペクト比制約は通常Windowの手動dragだけに適用する。最大化、Windows
  スナップ、ActiveX全画面はWindows標準矩形を優先するため、RDP表示領域が
  接続時desktopと異なる比率になる場合がある。
- 辺／角drag、3つの表示mode、最大化、スナップ、全画面の組合せは計算moduleの
  ローカルGateを通過したが、実RDP描画を使ったユーザー実機Gateは未通過である。
- 初回から動的解像度を選んだ接続は一度ユーザー確認を通過したが、別の実接続で
  `OnLoginComplete`の約0.24秒後にも`E_UNEXPECTED`が再発した。Version
  `0.7.10.0`はこのHRESULTだけを有限delayで最大3回再試行する。恒久errorは隠さず
  等倍表示へ縮退する。2026-07-31、再発環境でerror dialogなしに動的解像度が
  機能することをユーザーが確認した。
- ログイン完了後の外部切断を無通知終了する判定はローカルGateを通過したが、
  接続先再起動または通信断による実RDP切断のユーザー実機Gateは未通過である。
  ログイン前の接続失敗は原則としてerror dialogを表示する。署名済みVersion
  `0.7.14.0`は観測済みの`discReason=263`、`ExtendedDisconnectReason=0`完全一致を
  2秒後に一度再試行する。2026-08-03のR-005 Client候補は、Microsoft RDP Clientが
  三回目に成功した実測に合わせ、同じ完全一致だけを2秒後と5秒後に最大二回
  再試行した。その後、新しい接続先ではcredential入力後のcertificate警告を
  操作可能にならず、三回とも同じ失敗を反復することが判明した。R-006候補は
  Microsoft認証dialogを表示した試行を自動retryから除外し、認証UIへ到達する前の
  完全一致だけにR-005を残す。このsignatureは公開仕様で原因が定義されておらず、
  Windows更新等で別reasonになった失敗は自動回復しない。Version `0.7.14.0`の通常起動とRDP
  ログイン完了はVersion
  `0.7.13.0`のユーザー実機結果を基礎とし、終了経路変更後の実RDP終了も再確認済みで
  ある。R-006初回候補は`RedirectionWarningDialogVersion=1`、`AuthenticationLevel=2`
  readback、認証dialog中のfocus保護を実装した。実機では両設定に成功したが二回目の
  warning eventが即時dismissされた。`ShowRedirectionWarningDialog=TRUE`候補は既存接続先も
  `Connect()`の`S_FALSE`、`disconnect code=1`で失敗させたため撤回済みである。
  回帰撤回版では従来接続先が再び接続できることをユーザーが確認した。次のR-007候補は
  `UIParentWindowHandle`のsession top-level Window指定を外し、ActiveX Control Windowの
  既定親へcredential UIとcertificate UIを一貫して委譲する。実機ではcertificate警告が
  操作可能となり、利用者の明示継続後にdesktopへ到達した。従来接続先もR-007候補で
  回帰なく接続できた。「いいえ」ではControlの`263/0`後にZRdpが重複errorを表示したため、
  R-008候補はwarning二回以上かつlogin前の完全一致だけを利用者取消として扱い、対象
  session Windowを閉じる。ActiveXはbutton選択値を公開しないため、「はい」の直後に
  同じ`263/0`で別障害が起きた場合も取消分類へ一致し得る。R-008は実機で「いいえ」後に
  重複errorなしで対象sessionだけが閉じることを確認し、受入後Clientを再署名した。
- 枠なしのpopカラーiconは実機feedbackを受け、subject幅約96%／高さ約92%へ
  拡大し、黄色endpointをroyal purpleへ変更した。16、24、32、48pxの明暗背景と
  Debug／Release EXE埋め込みはローカル確認済みである。Explorer／taskbar／
  title barはWindows icon cacheやDPIの影響を受けるため、Version `0.7.14.0`の
  ユーザー表示確認は未通過である。
- セッションWindowの全画面button表示とMicrosoft全画面への移行はユーザー実機で
  成立した。Version `0.7.11.0`では動的解像度を選択してもremote desktop寸法が
  通常Window時のまま残る問題が判明した。Version `0.7.12.0`は全画面monitorへの
  追従とWindow復帰時の再追従を実装し、Debug／Release全30 CTestを通過したが、
  実RDPでの追従確認は未通過である。

## 2. timeoutと再開

- Agent死活確認はReady後5秒間隔、応答期限15秒である。異常終了の検出には
  最大約20秒かかり、その間に開始された新しいProxy接続は成功しない可能性がある。
- health timeout、RDP切断、Client／Agent再起動では旧HTTP streamとSocketを
  すべて破棄する。途中requestのresumeや自動再送は行わない。
- timeout直前の旧session Frameは新HELLO_ACKまで破棄する。新sessionへ旧DATAを
  引き継がないことを優先する。
- 最初のHTTP headerは30秒、OPEN後のlocal／upstream完全無通信は30分で終了する。
  長時間無通信を意図するHTTP Keep-Aliveも切断対象となる。
- RDPそのものを切断・再接続した場合、支給PCの新しいRDPユーザーセッション内で
  Agentを起動し直す。

## 3. 設定とnetwork境界

- `agent.ini`はAgent起動時に一度だけ読み込む。変更はAgent再起動後に反映し、
  live reloadは行わない。
- Client Proxyは認証なしのloopback listenerである。同じ開発PC上の別processも
  接続できるが、Agent側の完全一致allowlist外へは接続できない。
- 対象はIPv4 literalの`http://`方式SVNと承認済みTracだけである。hostname、
  HTTPS、HTTP `CONNECT`、
  `svn://`、`svn+ssh://`、SOCKS、任意TCP forwardには対応しない。
- Proxy base port以外のtimeout、bind address、endpointはClient設定から変更できない。
- SVN clientへloopback HTTP Forward Proxyを設定する必要がある。既存repository
  URLを変更せず、JavaHLのSubversion `servers`設定からloopback Proxyを使用する
  運用を実機確認済みである。Eclipse全体のnetwork設定を自動変更しない。
- Mylyn Tracではrepository固有Proxyを`127.0.0.1:<port>`へ設定する。redirect先が
  別endpoint、hostname、HTTPSの場合は追従できず、security境界として拒否する。

## 4. 複数RDP session

- 複数接続は`ZRdpSvnBridge.exe`を複数起動する方式ではない。一つの接続管理
  Windowから「新しい接続」を繰り返す。別process間ではport slotを共有しないため、
  二つ目のprocessはbase port競合でProxyだけが`BindFailed`となり得る。
- `client.ini`のportはbaseである。最初に開始した通常利用sessionがbase `18080`、
  後続が`18081`以降を使う。session Window titleに表示された実portを
  SVN／Trac clientへ設定する。
- 外部processが割当予定portを使用していても、ZRdpはport scanや自動fallbackを
  行わない。対象sessionのRDP画面は維持するがProxyは利用できない。
- 各RDP session内で対応する`ZRdpSvnAgent.exe`を起動する必要がある。一つのAgentを
  複数RDP sessionから共有しない。
- JavaHLのSubversion `servers`等、一つのProxy portを共有する設定は一つの
  RDP sessionへ対応する。複数sessionを同時に使い分ける場合は、client側で
  repositoryまたはworkspaceごとに対応portを指定できる運用が必要である。
- 一つのClient processは最大16 RDP session、一sessionは最大8 HTTP streamである。
  複数session全体を一つの128-stream Proxyとして扱う機能はない。
- sessionごとのCOM STAによりWindow応答性は分離するが、同じClient process、
  診断log、ATL moduleを共有する。process全体のcrash isolationは提供しない。
  専用STAでもMicrosoft ActiveX自身の終了が長時間待機する可能性はあるが、その間も
  managerと別sessionのmessage loopが応答することを設計上の保証範囲とする。

## 5. 機密情報

- Application protocolは独自暗号化を追加せず、Windowsが確立したRDP transportを
  利用する。HTTP SVN／Trac認証情報はpayloadとして透過中継するが、ZRdpは
  保存・解析・記録しない。
- 同じWindowsユーザーで動くprocessや管理者に対する防御はDPAPI接続履歴の
  security boundary外である。
- crash dumpにはpayloadが残る可能性があるため、既定で収集せずGit管理対象外とする。
- 実環境logを外部共有する場合は、固定field以外が混入していないことを利用者が
  再確認する。
