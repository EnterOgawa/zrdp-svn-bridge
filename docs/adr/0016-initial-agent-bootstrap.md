# 初回Agentの自動配置と起動

> Status: Historical / amended. 専用drive・固定setupの境界は現行にも使用する。署名必須・起動確認・上書き制限の記述は [ADR 0019](0019-agent-tray-and-direct-launch.md)／[0021](0021-optional-agent-signing.md)／[0022](0022-connection-agent-lifecycle.md) により変更済み。

日付: 2026-10-09

固定入力方式は2026-10-10の[SendInput方式](0017-sendinput-agent-controls.md)で更新する。
専用drive、署名、INI、固定配置先とAgent応答の境界は本ADRを維持する。

ユーザーの明示承認に基づき、初回配置に限って専用driveのRDP転送と固定Agentの起動入力を追加する。接続先でのfolder作成、二fileの貼付け、EXE選択を不要にする。実RDPでの転送・起動と署名後の検証は受入Gateとして残す。

## 配布範囲

drive letter割当は同じWindows sessionのClient process間でもmutexで直列化し、最大2秒で停止する。Windowsの全体転送設定と個別選択を適用した後にdynamic driveを再度無効化し、専用driveだけが有効のままかを最後にreadbackする。

session開始前のstartup workerで、current-user専用directoryを生成し、未使用のdrive letterを割り当てる。`DefineDosDevice`のtargetはそのdirectoryの完全なNT pathだけとする。既存driveは上書きしない。RDP ActiveXで通常driveとdynamic driveを無効にし、専用driveだけを選択する。全driveの状態をreadbackできなければ接続を開始しない。空directoryもreadonlyとし、一般のC/D drive、workspace、user folder全体を転送しない。

明示承認後、設定workerが同一版・同一署名者の正式Client/Agentと全宛先承認済みINIを検証する。Agentは16 MiB、INIは64 KiB以下に限定する。初期配置対応は署名対象resourceの`AgentSetupProtocol=1`で確認する。旧版Agentへ未知argumentを送らない。新規二fileだけを専用directoryへ出力し、DACLとreadonly HANDLEでwrite/deleteを制限する。sourceを保持する間はdirectoryと親のreparse・renameを拒否する。

INIは `%LOCALAPPDATA%\ZRdpDeployment` の専用directoryに一時的な平文fileとして置く。この公開とclipboard置換を事前確認に明記する。正常session終了時は自分のmappingと固定二fileだけを回収し、recursive deleteは行わない。クラッシュやfile使用中で残留したfileはcurrent-userと管理者が扱う。暗号化保存の代替ではなく、同じuserまたは管理者に対する完全な防御でもない。

## 起動と配置

manager STAから最大一件の要求をsession所有STAへmarshalする。Win+R、固定文字列のCtrl+V、Enterをtimerで分ける。文字列は `\\tsclient\<専用drive>\ZRdpSvnAgent.exe --rdp-setup` のみとし、cmd、PowerShell、任意argument、任意path、remote shellは受け付けない。各段階で認証世代、clipboard世代、取消とAgent待機状態を再確認する。実行警告やcredentialを自動承認しない。キー受付を起動成功と表示せず、45秒以内のAgent応答を別に確認する。

Agentはactive RDP session、固定tsclient source、x64正式metadata、Windows trust、SHA-256署名、timestamp、INIを検証する。固定の `%LOCALAPPDATA%\ZRdp` へ二fileをcreate-newで配置し、コピー後のEXEを再検証してからINIを完成させる。既存二fileがbyte完全一致なら再利用し、異なるfile、片方だけの既存file、linkは変更しない。コピー失敗ではこの処理が新規作成したfileだけを回収する。`CreateProcess`のapplication名を固定し、同じuser/sessionでAgentを起動する。通常Agentのnamed mutexでsession内の二重起動を防止する。

## 制約と互換性

RDP標準入力にはfocusとclipboard同期の制約がある。操作中は利用者が接続先へ入力せず、Windowsの警告は利用者が判断する。禁止された転送・実行では停止し、別経路、policy変更、サービス、task登録へfallbackしない。失敗後は新しいRDP接続で準備し直す。中止は未送信入力だけを停止し、起動済みAgentやremoteの配置済みfileは強制削除しない。

HTTP Proxy、allowlistの宛先制限、upstream Socket、wire format、SVC名`ESVNBRG`は変更しない。HELLO_ACKはAgentの接続応答であり、特定の配置処理の暗号学的な受領証明ではない。従来の手動配置controllerと選択確認付き起動は保持する。

## 検証

Debug／Release各47 CTest、132 source fileの書式確認、16 formatter self-testを通過した。自動配置画面はfake入力・応答の96 DPI fixtureで確認した。

workspace以下のfixtureだけで、専用driveの生成・公開範囲・readonly・回収、未署名の実Agent EXE拒否、metadata検証、INI拒否、同一file再利用、既存file保全、コピー後検証失敗のrollbackを確認する。実Windows ActiveXを接続せず生成し、専用driveだけの選択とreadback、未接続入力拒否を確認する。UIはfake起動queueとfake応答で、folder操作の非表示、同意前の拒否、入力受付前後の成功表示禁止を確認する。

Microsoft API根拠は[driveの個別選択](https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpdrive-redirectionstate)、[drive collection](https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpclientnonscriptable3-drivecollection)、[標準入力](https://learn.microsoft.com/en-us/windows/win32/termserv/imsrdpclientnonscriptable-sendkeys)とする。転送先でのWindows trust、tsclient file metadata、clipboard同期、実行警告は実機で確認する。

2026-10-10追記: 起動入口の統合と明示未署名開発テストは[ADR 0018](0018-unified-agent-start.md)を適用する。通常署名要件と固定入力の安全境界は維持する。
