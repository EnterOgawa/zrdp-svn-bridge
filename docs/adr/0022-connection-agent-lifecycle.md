# ADR 0022 接続マネージャーによるAgent自動管理

状態: 採用、実装済み。実RDP受入は利用者確認待ち。

日付: 2026-10-10

## 決定

Agent設定は接続マネージャーで編集し、RDP接続先・domain・userの組ごとに保存する。
接続名は識別keyにせず、改名で設定を失わない。入力の長さを含む有限keyをWindows ordinalで照合し、
DNS探索やrepository IPからの推定を行わない。独立保存形式2をDPAPIで保護し、形式1は未関連付けで保全する。

設定画面はIP・HTTPポート・用途の登録と削除に限定し、それぞれ自動保存する。
登録は明示した宛先の業務利用承認を兼ね、その意味を画面へ表示する。
一覧は現在の接続keyに一致した登録済み宛先だけを表示する。旧未関連付け設定と未承認候補は
保存snapshotで保全し、表示・起動・他接続への推定継承に使わない。
自動起動も登録済みprojectionからINIを生成し、非表示の未承認候補で登録済み宛先まで停止させない。
表示rowには元entry indexを保持し、選択・削除前に範囲と登録状態を照合する。
managerの接続入力変更は開いている設定画面だけへ通知し、当該接続の一覧を再読込みする。
未登録接続は空一覧、未選択は編集不可。Load中は最新変更一件だけ、Save中は保存先を維持したまま
最新変更一件だけを保留する。保存失敗では元接続と入力を保全し、再保存成功後に切り替える。
設定名編集、独立した保存、承認checkbox、INI入出力は画面から除去する。

認証後に固定Agentを自動配置・更新・起動する。session専用STAは固定入力とCOMだけを所有し、
managerは上限16sessionのepoch付き有限queueと一秒周期観測で判断する。
自動管理では確認・進捗・失敗dialogを表示しない。未設定・未承認でも設定画面を自動表示せず、
認証後のtitleに `Agent:要設定` を表示する。設定はmanagerのAgent設定buttonから手動編集する。
Agentのsetup・通常起動の失敗は既存終了codeと非機密の固定診断へ記録する。Clientは従来の
無応答期限と状態表示で判断し、setup終了codeを即時返す追加protocolは導入しない。
session起動button、開発用入力テストのUIと送信APIは製品から除去する。
署名はADR 0021に従い必須にしない。配布元の信頼とWindows／組織policyは前提とする。

専用driveの初回EXE／INIはreadonlyで維持する。合意feature 0x4の限定control 13／14／15により
現在のINIだけを渡し、最新INIのACK 1前はProxyとAgent relayの両方を閉じる。
設定差分は旧Socketを閉じ、INIだけをbackup付きatomic renameで更新する。
ACK 2後にchannel・tray・singletonを解放し、固定自身を再起動する。削除・承認撤回は協調停止とする。
frame header、protocol version 1、既存HTTP data plane、RDP認証設定を変更しない。

## 復旧と制約

Agent終了・死活断は同じ認証世代へ限定し、5秒から60秒のbackoffを使う。
起動応答は45秒、安定確認は30秒。連続三回の失敗で自動復旧を停止する。
部分入力・設定拒否・protocol異常は即停止し、明示再登録または新しい認証世代でだけ再準備する。
RDPは維持し、HTTP要求の再送やstream resumeは行わない。

固定入力は一度に一sessionだけで、対象前面、非最小化、認証警告なし、入力休止を待つ。
開始後の画面切替、clipboard変更、認証世代失効では未送信入力を停止し、focusを奪い返さない。
SendInputの直前確認後の競合を完全に防ぐ仕組みではない。
全画面も対象前面なら扱い、独自buttonをoverlayしない。Windowsの実行画面は固定起動方式で
一時表示される。Windows警告は隠さず利用者が確認し、自動承認しない。

setupは同じユーザーsessionの停止eventへだけ協調停止を要求し、singleton消滅を最大15秒待つ。
旧版や応答不能Agentは強制killせず、使用中・不安全な二fileも上書きしない。
Windows startup、service、任意command、任意path／PID、追加受信port、policy変更を追加しない。
EXE変更は新しいClientとRDP接続から反映し、同じRDP中のINI変更は限定SVCで反映する。

## API境界

停止待機は [OpenMutexW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-openmutexw) の
object消滅確認を使い、保有するprobe handleを各回で閉じる。
[CreateEventW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw) の既存objectは
新しいACLが適用されないため再利用せず、重複を拒否する。
[GetLastInputInfo](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getlastinputinfo) は
呼出しsessionだけの入力休止の目安とし、実際の入力先の証明には使わない。

接続候補の選択通知では [CBN_SELCHANGE](https://learn.microsoft.com/en-us/windows/win32/controls/cbn-selchange) に
対応する選択indexとlist文字列を取得してから接続組を復元する。edit部分の直接入力は
[CBN_EDITCHANGE](https://learn.microsoft.com/en-us/windows/win32/controls/cbn-editchange) 後に読む。
[MFC CComboBoxの通知契約](https://learn.microsoft.com/en-us/cpp/mfc/reference/ccombobox-class?view=msvc-170) にも、
選択通知中はeditのGetWindowTextではなくlistの文字列を読む制約が記載されている。

## 受入

ローカル試験は実RDPと実repositoryへ接続しない。接続keyの独立性・形式互換、INI更新とbackup、
controlの方向・重複・ACK、Proxy gate、停止event、期限とbackoff、診断UI削除を隔離fixtureで確認する。

隔離EXEを非転送pathから固定setup引数で起動し、sessionまたはpathの拒否でmodalなしに
終了することと診断codeを確認する。通常のSVC・実INI・実配置先には進まない。

実RDPでは独自dialogなしの自動起動と未設定時の手動設定入口、未配置の初回、既配置更新、tray終了からの自動復旧、設定変更再起動、承認撤回、
同時複数接続の対象分離、最小化・他画面からの復帰、入力途中切替、Windows警告、転送拒否を確認する。
旧Agent稼働時は初回のみ手動終了し、同一版Client／Agentを同時に更新する。
rollbackは今回の保全snapshotと接続先backupを使い、他の作業差分は維持する。
