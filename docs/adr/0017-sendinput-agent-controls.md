# Agent固定入力のSendInput方式

> Status: Historical / amended. 固定SendInputと安全停止・残存リスクの境界は現行。旧button／開発入力テストは [ADR 0022](0022-connection-agent-lifecycle.md) で廃止している。

日付: 2026-10-10

利用者がR単独とWin＋RのSendInput反映を接続先で確認し、配置・起動の固定入力切替を明示承認した。
自動初期配置のWin＋R、Ctrl＋V、Enterと、Explorer選択確認付き起動のEnterをSendInputへ切り替える。
SendKeys内部の不反映原因が確定したという判断ではない。署名、承認済みINI、配置先、専用driveの制限は維持する。

## 固定入力と所有境界

`RdpControlHost`はキーやcommandを受け取らず、同期のowner承認callbackだけを受ける。
callbackはCOM照会後にも認証世代、固定phase、clipboard世代、取消条件を確認し、保存しない。
所有STA、警告のない認証、実ControlのIOleWindow、前景・active Window、可視・有効・非最小化、
関連キーと修飾キーの解放を確認する。承認不在や失効では送信しない。

一回のSendInputへ次の固定列だけを渡す。

- Win＋Rは左Win押下、R押下、R解放、左Win解放。scan 0x5Bはextended、Rは0x13。
- Ctrl＋Vは左Ctrl押下、V押下、V解放、左Ctrl解放。scanは0x1Dと0x2F。
- Enterは主Enter押下、解放。scanは0x1C。keypad Enterではない。

任意INPUT、文字列、shell、任意実行path・argument、入力captureは追加しない。
旧SendKeys診断三種類とR／Win＋RのSendInput比較は独立して保持する。

## 待機と停止

初回だけ同じClient processから対象sessionへfocusを準備し、350 ms以上待つ。
Run後のPasteとPaste後のEnterはそれぞれ700 ms以上待ち、予定時刻から3秒以上遅れた入力は捨てる。
固定sequenceは早着、終了後の残留通知、同段階のtimer再入を無処理にする。
後続段階はfocusを奪い返さずreadbackし、認証世代・専用drive・Agent待機・clipboard条件を再確認する。
手動起動は確認後のfocus準備と350 ms後のEnterだけを分離する。

別Window切替、最小化、警告、切断・再接続、close、取消では未送信入力を失効させ、戻っても再開しない。
全要素受付だけをS_OKとし、不完全受付では元Win32 errorが0でも停止する。
部分受付の押下残留は利用者が該当キーを押して離し、追加解放・再送・SendKeys fallbackは行わない。
起動済みAgentや配置済みfileは停止で巻き戻さず、全Enter受付も実Agent応答までは成功と表示しない。

## 残るリスクと受入条件

SendInputはremote sessionを直接指定せずローカルWindows入力streamへ挿入する。
直前確認後の切替競合で別画面へ貼付けやEnterが届き、誤実行するリスクは残る。
開始確認と手動起動画面へ明記し、入力中の操作・Window切替を控えるよう案内する。
Clientはremote Run画面やExplorer選択内容を自動検証しない。

同一版・同一署名者の署名済みClient／Agent、初期配置対応metadata、全宛先承認、固定tsclient source、
固定ユーザー配置先、異なる既存file非上書き、45秒のAgent応答期限は変更しない。
未署名EXEの自動配置を許可せず、NLA、policy、権限、Zscalerも変更しない。
HTTP Proxy、allowlist、Agent relay/socket、wire format、ESVNBRGへの影響はない。

実入力なしのfakeと未接続ActiveX、専用UI fixtureで検証する。配置・Ctrl＋V・Enterの接続先反映と
Agent応答は署名後の利用者による実機受入条件である。
復帰は本変更のsourceだけを戻し、旧EXEへ差し戻す。remote fileや起動済みAgentは自動削除しない。

2026-10-10の実装検証ではDebug／Release各47件のCTest、138 fileの書式check、16件のformatter self-testが合格した。
専用fixtureの確認・起動・部分受付画面を96 DPIで検証した。実RDPと実SendInputは実行しておらず、
outのClient／Agentは未署名である。自動配置の実機受入には双方の再署名が必要である。

根拠は[SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)と
[KEYBDINPUT](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-keybdinput)とする。

2026-10-10追記: 起動入口の統合と明示未署名開発テストは[ADR 0018](0018-unified-agent-start.md)を適用する。通常署名要件と固定入力の安全境界は維持する。
