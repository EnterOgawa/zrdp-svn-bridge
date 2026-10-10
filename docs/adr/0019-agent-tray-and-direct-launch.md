# 保存済み設定からの直接起動、トレイ常駐、バックアップ付き更新

> Status: Historical / amended. tray常駐・backup付き更新の境界は現行。署名条件とbutton起動は [ADR 0021](0021-optional-agent-signing.md)／[0022](0022-connection-agent-lifecycle.md) により変更済み。

日付: 2026-10-10

## 決定

ユーザーが現行版の実Agent起動・Client応答成功を確認した。
次の明示指示として設定編集を起動導線から外し、起動後のAgentをタスクトレイに常駐させる。
自動起動のタイミングはClientの「Agent起動」を押した後とする。
追加指示により、旧二fileを保全して検証済み新版へ上書き更新する。
この決定はADR 0016／0018の「異なる既存fileは上書きしない」方針を変更する。

## 起動と設定

起動要求は押したsession IDと認証世代を固定し、workerで保存一覧をLoadする。
破損・復号失敗は設定なしと扱わず停止する。空／未承認draftは起動候補にしない。
一件なら選択済み、複数なら明示選択する独立確認を表示する。
既定キャンセル、未署名テストは既定OFF・非永続・今回だけ。
確認画面の全文警告は署名modeに一致させ、明示起動後にだけ準備する。

設定なしでは設定画面を案内し、保存後も勝手に起動せずbuttonの再操作を必要とする。
設定編集画面から起動するcallbackとcheckboxを除く。
接続管理menuと起動確認の「設定...」から編集できる。
RDP接続履歴とAgent設定のnetwork値を使った推定関連付けは行わない。

## Agent常駐

既存console PE形式を維持し、setupは固定childをCREATE_NO_WINDOWで起動する。
通常Agentは専用UI threadでhidden HWNDとnotification iconを所有し、自身のconsoleからdetachする。
共有Terminalのwindowを非表示にする操作は行わない。
Client待ち／SVN Bridge接続済み／RDP復帰待ち／利用不能を固定日本語tooltipとmenuで表示する。
右clickまたは選択で状態行と「Agentを終了」を表示し、終了はI/O ownerの協調停止へ渡す。
WTS read／serialized write／Socket I/OをUI threadへ移動しない。
Explorer再起動では自分のiconだけを再登録し、登録・維持不能なら停止する。
RDP session終了では従来どおり全streamとiconを回収する。
Windowsログオン登録、service、scheduled task、常時network待受は追加しない。

## 更新と復旧

対象は接続先userの固定folder内ZRdpSvnAgent.exeとagent.iniだけ。
incomingの署名／metadata／allowlist、reparse・hard link・有限サイズを再検証する。
同一二fileはそのまま再利用する。異なる二file、片方のみ、空の通常fileは更新前backupへ保全する。
backupは一意のbackup-{GUID} directoryとcurrent-user限定ACLで作り、上書きしない。
新二fileをstage検証してから同一volumeのrenameでcommitする。
二fileの置換は単一atomic transactionではないため、部分commitを追跡して逆順復旧する。
検証・置換失敗は起動せず復旧し、復旧不能は分類35で停止、backupを利用者へ案内する。
backupを自動削除せず、INIが接続先設定を含むことを運用文書に明示する。

稼働Agentまたは競合setupはsession singletonで拒否する。強制終了や自動killは行わない。
利用者がtray終了（旧console版はCtrl+C）し、新しいRDP接続から更新を開始する。
同日ユーザーが実トレイ起動成功を確認した後、tray終了後のClient buttonが無反応になる
回帰を報告した。前回の完了phaseが残る受付拒否と、一回専用公開処理の再使用を修正する。
同じ認証世代で応答確認済みBridgeがAgent待機へ戻った場合だけ起動phaseをidleへ戻す。
毎回の新しい明示確認とworker再検証により、EXE・INI byte列・署名modeまで完全一致する
readonly配布driveを再許可する。成功画面だけを同じ対象・世代の再操作時に閉じる。
途中・失敗・世代失効は再試行せず、異なるpackageへの配布drive差替えも行わない。
同一packageの再起動にはRDPの作り直しを必要としないが、更新では新しいRDP接続を維持する。
前回の完了phaseだけを戻した修正ではFixedInputSequenceのCompleteが残り、
二回目のBeginを拒否していた。Cancelは受付済み操作を巻き戻さない契約であり、
CompleteをIdleへ戻さない。今回、同じAgent終了guardとsequenceのCompleteを一つの
境界で確認して両状態を戻す。未開始・入力途中・Stoppedのsequenceは初期化しない。
初期化は入力予約を伴わず、次の利用者確認と従来の全guard・待機・Agent応答を必要とする。
setupのprocess生成やSendInput受付だけでは成功表示せず、同じsessionのAgent応答を待つ。
Agent応答は接続確認であり、特定binary hashの暗号学的受領証明ではない。

## 根拠と検証境界

[Windows通知領域](https://learn.microsoft.com/en-us/windows/win32/shell/notification-area)と
[NOTIFYICONDATAW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw)に従い、
追加ごとにVERSION_4を指定する。
[FreeConsole](https://learn.microsoft.com/windows/console/freeconsole)は自processだけをdetachする。
外部codeの転用、build／dependency／license変更は行わない。

ローカルCTestは実RDP、実宛先、実clipboardに接続しない。
保存候補・世代失効・取消・未署名OFF復元、fake Shell lifecycleと終了、
backup・上書き・使用中file・検証後復旧・復旧不能・link拒否を隔離fixtureで確認する。
実トレイ初回起動はユーザー確認済み。同sessionのClient button再起動、上書き更新と
実SVN／Trac操作はユーザー環境で確認する。
