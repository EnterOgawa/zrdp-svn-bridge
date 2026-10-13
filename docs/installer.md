# ZRdp SVN Bridge インストーラー

更新日: 2026-10-13。対象: 正式版 `0.7.21.0`。

## インストールする方へ

Setupを実行すると、ClientとAgentが同じフォルダーに入ります。
既定の配置先は `%LOCALAPPDATA%\Programs\ZRdp SVN Bridge` です。管理者権限は要求しません。
スタートメニューからClientを起動できます。デスクトップのショートカットは任意です。

Agentを個別に起動する必要はありません。接続マネージャーの「Agent設定」で宛先を登録すると、
RDP認証後にClientが接続先へAgentを配置・起動します。使い方は [README](../README.md) をご覧ください。

更新やアンインストールの前に、すべてのRDP画面と接続マネージャーを終了してください。
稼働中のClientや使用中のEXEを検出した場合は、処理を止めます。強制終了・自動再起動はしません。
新しい版のEXEがある場所を、古いSetupで上書きすることも拒否します。
確認後に別のprocessが起動する競合や、二EXEの更新中の電源断まで完全に防ぐものではありません。

アンインストールで削除するのは、配置したプログラム・文書・ショートカットと標準のuninstall登録です。
接続履歴とAgent設定は、最初の選択画面の「接続履歴・Agent設定も削除する」で選べます。
既定はチェックなし（保持）です。選択すると、このWindowsユーザーの全接続分を削除します。
同じユーザーが使う別の配置先のZRdpも接続情報を共有しているため、その設定も対象です。
削除は元に戻せません。キャンセル、または画面を閉じた場合は、プログラムも接続情報も変更しません。
ログ・既存backup・一時file・手動で置いた `client.ini` は、選択にかかわらず残ります。
接続先PCに配置済みのAgentも、開発PCのアンインストーラーでは削除しません。

削除する接続情報は、次の固定二fileだけです。フォルダー全体は削除しません。

```text
%LOCALAPPDATA%\ZRdpSvnBridge\connection-history.bin
%LOCALAPPDATA%\ZRdpSvnBridge\agent-settings.bin
```

Windowsが管理するRDPの保存済み資格情報は削除しません。
リンク、読み取り専用、使用中、確認不能の保存先は、削除を選んだ場合に処理を止めます。
削除時にも再確認し、失敗はエラーとして通知します。二fileの削除のatomic性は保証しません。

silent uninstallは保持が既定です。接続情報も削除する場合だけ、uninstallerへ
`/DELETECONNECTIONDATA=1` を明示します。この指定をSetupへ渡しても接続情報は削除しません。

## 同梱するもの

- `ZRdpSvnBridge.exe`、`ZRdpSvnAgent.exe`
- `LICENSE`、`NOTICE`、`DEPENDENCIES.md`、`README.txt`
- Inno Setupの標準アンインストーラー

固定fileだけを指定し、`out` 全体を取り込みません。実INI、履歴、ログ、鍵、PDB、SDK、RDP DLLは同梱しません。

## `.iss` から作る

Windows上のInno Setup 6.3以上を使います。自作scriptは
[installer/app_installer_setup.iss](../installer/app_installer_setup.iss) です。
同じ版のRelease Client／Agentを `out` へ用意し、リポジトリのルートで次を実行します。

```powershell
& 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe' installer/app_installer_setup.iss
```

版はClientのVersion resourceから読み取り、AgentとのFileVersion／ProductVersion、
正式製品名、固定EXE名を確認します。既存Setupは上書きしません。必要なら保全してから再生成してください。
出力は `out/ZRdp_SVN_Bridge_Setup_0.7.21.0_unsigned.exe` です。
Client／Agentが署名済みでも、通常compileだけではSetupとアンインストーラーは署名されません。
自前の未署名アプリからも作れますが、`_unsigned` のSetupは正式公開候補として扱わないでください。

入力・出力先を変える場合は、compilerへ `/DExecutableDirectory=<folder>` と
`/DInstallerOutputDirectory=<folder>` を渡します。署名済み二EXEを再build・再署名する必要はありません。

## Setupを署名する

InnoのCompiler IDE、またはcompilerの `/S` 引数で、承認された署名commandを
`ZRdpSvnBridgeEV` の名前で設定します。
明示的に `/DEnableEvSigning` を指定してcompileした場合だけ、Setupと埋込みアンインストーラーを署名します。
PINはmiddlewareの保護されたUIで入力し、script・command引数・chat・logには入れません。
署名command、証明書識別情報、鍵はrepositoryへ保存しません。自動再試行は0回です。

Setupを生成後に署名するだけでは、内部のアンインストーラーは署名されません。
正式配布では同梱二EXEに加え、Setupとアンインストーラーの署名・timestamp・署名者を確認してください。
Clientの既存自動更新方式は変更していません。
Clientによるセット更新は、アプリ一覧に記録したSetup版を更新しない点にも注意してください。

Innoの動作は公式の [SignTool](https://jrsoftware.org/is6help/topic_setup_signtool.htm) と
[SignedUninstaller](https://jrsoftware.org/is6help/topic_setup_signeduninstaller.htm) を参照してください。

## 検証

```powershell
./scripts/Test-Installer.ps1 -ExecutableDirectory ./out -RunInstallation
```

`-IsccPath` でcompilerの場所を指定できます。`-OlderExecutableDirectory` へ同版の旧二EXEを渡すと、
混在版とdowngrade拒否も検証します。test modeはuninstall登録、shortcut、Client起動を無効にし、
`work/tmp` の新規folderだけで実行します。test modeの接続情報保存先はcompiler出力先配下の
`connection-data` に固定します。実接続先や実ユーザー設定を使用しません。

2026-10-13に以下を確認しました。

- 通常modeと隔離test modeのcompile
- 欠落Client／Agent、役割の偽装、混在版、既存Setupの上書き拒否
- 新規配置、同版再インストール、二EXEと文書のhash一致
- 使用中EXEがある場合のinstall／uninstall拒否、旧Setupによるdowngrade拒否
- アンインストール後の設定・ログ・未知のfile保持、元の署名済み二EXEのhash保持
- 接続情報の保持・選択削除、取消、保存先とfileの不在、silent不正値の保持
- 接続情報の使用中・読み取り専用・directory偽装・junction拒否、無関係fileとbackup保持
- 削除選択を反映した署名済みSetupとcompilerの埋込み用アンインストーラーを検証
  （Windows trust `Valid`、SHA-256署名、RFC 3161 timestamp、同梱二EXEと同一署名者）

削除選択を反映した署名済み更新版は `out/ZRdp_SVN_Bridge_Setup_0.7.21.0.exe` です。
変更前の署名済みSetupはworkspace内のbackupへ保全し、配布先を更新しています。
未署名版 `out/ZRdp_SVN_Bridge_Setup_0.7.21.0_unsigned.exe` も保全しています。
Client／Agentは再build・再署名せず、元のhashを保持しています。

2026-10-13にユーザーが通常Setupでのインストールと、インストール後のAgent／SVN／Trac動作を確認し、正式版として承認しました。
マウス操作が進まなかった問題は、Setupを変更せずPC再起動後に解消しています。Windowsやドライバーのどちらが原因かは特定していません。

最終監査後の対応では、インストール済みClient／Agent／`unins000.exe` をread-onlyで検証し、
製品版 `0.7.21.0`、有効な署名・timestamp・同じ署名者を確認しました。
現行版の実機利用はユーザーが全て問題なしと確認しています。
shortcut・アプリ一覧・通常アンインストールの選択／取消などの項目別記録は、包括的な実機受入と分けて扱います。
接続情報の保持・削除と拒否条件は、上記の隔離回帰検証で確認しています。
確認手順は [公開前の実機確認](publication-validation.md#インストーラーの確認項目) を参照してください。
