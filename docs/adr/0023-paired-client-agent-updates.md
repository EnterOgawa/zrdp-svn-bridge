# ADR 0023 Client／Agentの同意付きセット更新

状態: 採用、実装済み。Debug／Release各47 CTest合格。署名済みReleaseからの実機受入は未実施。

日付: 2026-10-10

## 決定

ユーザー指示によりADR 0013のClientだけの取得・置換を変更し、同じRelease tagの
`ZRdpSvnBridge.exe`と`ZRdpSvnAgent.exe`を一セットとして取得・検証・適用する。
更新manifest format 2にAgentの固定asset名、size、SHA-256を追加し、旧format 1は拒否する。
旧updaterもformat 2を受け付けないため、この機能を含む最初の署名済み二EXEは手動配布する。

起動時・24時間周期のmanifest自動確認、EXE取得前の明示同意、全RDP／設定画面終了と
設定保存完了待ち、現行Clientと同じ署名者・timestamp・HTTPS・許可hostを維持する。
Clientの`PairedUpdateProtocol=1`、Agentの`AgentSetupProtocol=1`と`BridgeProtocol=1`を確認し、
候補二EXEは同じFileVersion／ProductVersionと同じBridge protocolを要求する。
通常起動の任意署名方針は変更しないが、自動更新は旧・新二EXEとも署名済みセットを前提とする。

## 置換と復旧

固定隣接stageへ二候補を有限streamingし、片方が失敗すれば適用しない。
同じ自作Clientのcopyをhelperにし、継承した親process終了後に双方を再検証する。
元Clientと元AgentのSHA-256をstageへ保存し、同一配置mutex下でAgent→Clientの順に
backup付きで置換する。双方の差替え後検証が完了するまで新版Clientを実行しない。

通常の置換・検証・起動失敗では、旧hash・metadata・署名を確認した変更済みfileだけを
Agent→Clientの順に復元し、両方が元の値になった場合だけ旧Clientを再起動する。
新Clientが生存中なら強制停止・復元は行わず、stageと旧backupを保持する。
正常更新後はClientとAgentの旧版をそれぞれ一世代保全する。

二file同時のOS-level atomic性は保証しない。電源断・helper強制終了・復元不能時は
stageの旧二fileを保全し、利用者が停止状態でセットを確認・手動復旧する。
file lock、reparse、複数hard link、未知backupを安全と推測せず、昇格や別pathへfallbackしない。

接続設定・DPAPI履歴・INI・logを更新対象にしない。接続先Agentへの反映は、次回のRDP認証後に
登録済み宛先がある場合だけ、既存の専用drive／固定setup／協調停止の経路で行う。
接続中のAgent EXE差替えや強制RDP再接続は行わない。SVCとHTTP data planeは変更しない。

## 参照

単一fileの置換と部分失敗は[ReplaceFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)、
同一volumeでの復元・保全は[MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)を使用する。
複数fileのtransaction性はAPIから推定せず、元hash照合とrollbackで扱う。
