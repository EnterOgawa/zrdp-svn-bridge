# Agent起動の統合と未署名配置テスト

日付: 2026-10-10

ユーザーの明示指示によりセッション操作を「Agent起動」一つへ統合し、
未署名ビルドで実Agentの配置・起動を確認する開発テストを追加する。
キー単体の受付確認では、実ファイルの配置・実行の受入を代替できない。

## 統一起動

固定の配布専用RDP driveからAgent setupを起動する。接続先のsetupは固定配置先の
二fileを確認し、未配置ならcreate-new、完全に同じAgentとINIなら再利用して起動する。
異なる既存fileや片方だけの配置は保全して停止する。更新・INI上書き・既存Agent終了を自動化しない。
Agent応答済みでは重複起動しない。ExplorerでのEXE選択は不要とする。

## 未署名の実配置テスト

Agent設定画面のチェックは既定OFF、保存しない、一回の要求後にOFFへ戻す。
専用の既定「いいえ」の警告確認で未署名の真正性未確認・転送と実行・clipboard置換・
誤入力リスクを説明し、承認した今回だけworkerへmodeを所有移転する。
成功と失敗のどちらでも次の通常要求へmodeを持ち越さない。

Clientは正式x64 PE、製品名、OriginalFilename、同一版、AgentUnsignedSetupProtocol=1、
双方のcertificate table不在とWindows署名なし結果を確認する。
署名の不在は信頼成功ではなく、発行者を証明できない。信頼できる自分のビルドだけを使う。
署名があるEXE、壊れた署名、未知の失敗は許可しない。
Client更新の署名検証は変更しない。

専用driveの固定commandは通常の--rdp-setupと、明示テストの--rdp-setup-unsigned-testだけ。
Agentはactive RDP、tsclient直下の単一drive、固定basename、LocalAppData固定配置先を必須にする。
Agent側も正式metadata・対応resource・署名不在・allowlistを検証し、コピー後にも再検証する。
通常argumentは署名必須のままとする。任意引数、shell、service、policy変更、通常drive転送は追加しない。
Windows・組織による実行制限と警告の利用者確認は維持する。

## 状態と失敗

UIの確認mode、worker結果、sealed driveの固定commandのmodeが一致しない場合は起動を予約しない。
固定SendInputの認証世代、clipboard世代、前景・focus、期限、取消、部分受付停止はADR 0017を維持する。
入力のAPI受付やCreateProcess成功だけを完了にせず、同じsessionのAgent応答を確認する。
失敗ではRDPと既存二fileを維持する。送信済み入力、配置、起動は取消操作で元へ戻さない。

## 検証と受入

ローカル試験は署名必須と未署名限定の分離、初回配置、同一二file再利用、異なるINIの保全、
壊れたcertificate tableの拒否、rollback、mode受渡しとUI既定OFF・一回限り・単一入口を確認する。
実RDPでは利用者が未配置時と同一二file配置済み時の起動、Windows警告、Agent応答を確認する。
特定配置の暗号学的な受領証明や実SVN操作の成功までをAgent応答から推定しない。

## API根拠

[WinVerifyTrust](https://learn.microsoft.com/en-us/windows/win32/api/wintrust/nf-wintrust-winverifytrust)の固定署名なし結果と
[PE Format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)のcertificate tableを併用する。
一般の署名検証失敗を署名不在の代替判定にしない。
