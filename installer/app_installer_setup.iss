; Copyright (c) 2026 EnterSystem CO.,LTD.
; SPDX-License-Identifier: Apache-2.0
; ZRdp SVN Bridge: 開発PCへClientとAgentを同時に配置するユーザー別Setup。
; 接続先PCの設定・起動、サービス登録、policy変更、実設定の同梱は行わない。

; 【入力と製品情報】-----------------------------------------------------------------------
#define AppName "ZRdp SVN Bridge"
#define ClientExeName "ZRdpSvnBridge.exe"
#define AgentExeName "ZRdpSvnAgent.exe"
#define RepositoryDirectory AddBackslash(SourcePath) + ".."
#ifndef ExecutableDirectory
  #define ExecutableDirectory RepositoryDirectory + "\out"
#endif
#ifndef InstallerOutputDirectory
  #define InstallerOutputDirectory RepositoryDirectory + "\out"
#endif
#define ClientImage AddBackslash(ExecutableDirectory) + ClientExeName
#define AgentImage AddBackslash(ExecutableDirectory) + AgentExeName

#if !FileExists(ClientImage) || !FileExists(AgentImage)
  #error "Client and Agent must both be present. Build the Release pair first."
#endif
#define AppVersion GetVersionNumbersString(ClientImage)
#if AppVersion == "" || AppVersion != GetVersionNumbersString(AgentImage)
  #error "Client and Agent must have the same numeric file version."
#endif
#if GetStringFileInfo(ClientImage, "ProductName") != AppName || GetStringFileInfo(AgentImage, "ProductName") != AppName
  #error "Both images must be ZRdp SVN Bridge products."
#endif
#if GetStringFileInfo(ClientImage, "OriginalFilename") != ClientExeName || GetStringFileInfo(AgentImage, "OriginalFilename") != AgentExeName
  #error "Client and Agent original filenames must match their fixed roles."
#endif
#if GetStringFileInfo(ClientImage, "ProductVersion") != AppVersion || GetStringFileInfo(AgentImage, "ProductVersion") != AppVersion
  #error "FileVersion and ProductVersion must agree in both images."
#endif
#define AppVersionMS 0
#define AppVersionLS 0
#expr GetVersionNumbers(ClientImage, AppVersionMS, AppVersionLS)

; 自前の未署名buildも作れるが、未署名Setupは名前で区別する。
#ifdef EnableEvSigning
  #define BuildSuffix ""
#else
  #define BuildSuffix "_unsigned"
#endif
#ifdef InstallerTestMode
  #ifdef EnableEvSigning
    #error "Isolated installer tests must not use the signing token."
  #endif
  #define BuildSuffix "_test_unsigned"
#endif
#define SetupBaseName "ZRdp_SVN_Bridge_Setup_" + AppVersion + BuildSuffix
#if FileExists(AddBackslash(InstallerOutputDirectory) + SetupBaseName + ".exe")
  #error "Existing Setup will not be overwritten. Back it up or choose an empty output directory."
#endif

; 【基本設定】-----------------------------------------------------------------------------
[Setup]
#ifdef InstallerTestMode
AppId={{06CCA40D-2D6A-4385-B54D-75BF18149C24}
#else
AppId={{42F6975D-9B06-4937-97ED-D32781B71254}
#endif
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=EnterSystem CO.,LTD.
AppPublisherURL=https://github.com/EnterOgawa/zrdp-svn-bridge
AppSupportURL=https://github.com/EnterOgawa/zrdp-svn-bridge/issues
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
VersionInfoDescription={#AppName} Setup
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
#ifdef InstallerTestMode
UsePreviousAppDir=no
#else
UsePreviousAppDir=yes
#endif
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
AllowRootDirectory=no
AllowNetworkDrive=no
AllowUNCPath=no
ArchitecturesAllowed=x64compatible and not arm64
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
OutputDir={#InstallerOutputDirectory}
OutputBaseFilename={#SetupBaseName}
SetupIconFile={#RepositoryDirectory}\assets\icons\zrdp.ico
UninstallDisplayIcon={app}\{#ClientExeName}
LicenseFile={#RepositoryDirectory}\LICENSE
; Silentでも接続中のClientを閉じたり、勝手に起動し直したりしない。
CloseApplications=no
RestartApplications=no

; 外部で設定したInno Sign Toolを使う。PIN・鍵・証明書識別値はここに書かない。
#ifdef EnableEvSigning
SignTool=ZRdpSvnBridgeEV
SignedUninstaller=yes
SignToolRetryCount=0
#endif

; 回帰検査用Setupはworkspace内だけに展開し、OSのアプリ一覧やshortcutを変更しない。
#ifdef InstallerTestMode
CreateUninstallRegKey=no
UsePreviousGroup=no
UsePreviousTasks=no
#endif

[Languages]
Name: "Japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "English"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
Japanese.DesktopIcon=デスクトップにショートカットを作成する
Japanese.LaunchClient=ZRdp SVN Bridgeを起動する
Japanese.CloseClient=ZRdp SVN Bridgeの接続画面と接続マネージャーをすべて終了してから、もう一度実行してください。
Japanese.ImageBusy=ClientまたはAgentのファイルを使用中、またはアクセスできません。アプリを終了し、保存先を確認してから、もう一度実行してください。
Japanese.InvalidTarget=インストール先に版情報を確認できない同名EXEがあります。別のフォルダーを選んでください。
Japanese.NewerInstalled=インストール先に新しいバージョンがあります。古いインストーラーでは上書きできません。
Japanese.UninstallOptions=ZRdp SVN Bridge - アンインストール設定
Japanese.ConnectionDataChoice=接続履歴・Agent設定も削除する
Japanese.ConnectionDataIntro=接続情報を残すと、再インストール後も引き継げます。
Japanese.ConnectionDataNote=削除は、このWindowsユーザーの全接続分が対象です。元に戻せません。ログ・バックアップ・手動のINI・接続先PCのAgentは残します。
Japanese.ContinueUninstall=続ける
Japanese.CancelUninstall=キャンセル
Japanese.ConnectionDataUnsafe=接続情報を安全に削除できません。保存先がリンク、読み取り専用、使用中、または確認できない状態です。接続情報を保持するか、保存先を確認してからやり直してください。
Japanese.ConnectionDataDeleteFailed=接続情報の削除を完了できませんでした。接続履歴またはAgent設定が残っている可能性があります。保存先を確認してください。
English.DesktopIcon=Create a desktop shortcut
English.LaunchClient=Launch ZRdp SVN Bridge
English.CloseClient=Close all ZRdp SVN Bridge session and connection manager windows, then try again.
English.ImageBusy=The Client or Agent image is in use or inaccessible. Close the application and check the destination, then try again.
English.InvalidTarget=An existing image at the destination has no readable version. Choose another directory.
English.NewerInstalled=A newer version is installed. This older installer cannot overwrite it.
English.UninstallOptions=ZRdp SVN Bridge - Uninstall options
English.ConnectionDataChoice=Also delete connection history and Agent settings
English.ConnectionDataIntro=Keep connection data to use it again after reinstalling.
English.ConnectionDataNote=Deletion affects all connections for this Windows user and cannot be undone. Logs, backups, manual INI files and the Agent on remote PCs are kept.
English.ContinueUninstall=Continue
English.CancelUninstall=Cancel
English.ConnectionDataUnsafe=Connection data cannot be safely deleted. It is linked, read-only, in use or inaccessible. Keep the connection data, or check its location before trying again.
English.ConnectionDataDeleteFailed=Connection data deletion could not be completed. Connection history or Agent settings may remain. Check their location.

#ifndef InstallerTestMode
[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcon}"; Flags: unchecked
#endif

; 【固定fileだけを配布】-------------------------------------------------------------------
; out全体をwildcardで取り込まない。実INI・ログ・PDB・鍵は対象外。
[Files]
Source: "{#ClientImage}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#AgentImage}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepositoryDirectory}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepositoryDirectory}\NOTICE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RepositoryDirectory}\docs\dependencies.md"; DestDir: "{app}"; DestName: "DEPENDENCIES.md"; Flags: ignoreversion
Source: "{#SourcePath}\README.txt"; DestDir: "{app}"; Flags: ignoreversion

#ifndef InstallerTestMode
[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#ClientExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#ClientExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#ClientExeName}"; Description: "{cm:LaunchClient}"; Flags: nowait postinstall skipifsilent unchecked
#endif

; InstallDelete／UninstallDeleteは置かない。標準logでは配布fileとshortcutだけを削除する。
; 接続情報の二fileだけ、利用者が選択した場合にCode側で削除する。再帰削除は行わない。

[Code]
var
  { 今回のuninstallだけの選択。保持が既定で、前回の選択は保存しない。 }
  DeleteConnectionData: Boolean;

{ 既存EXEの使用中確認だけにWindows標準APIを使う。Handleは確認後すぐに閉じる。 }
function OpenImageForExclusiveRead(FileName: String; DesiredAccess, ShareMode,
  SecurityAttributes, CreationDisposition, FlagsAndAttributes: Cardinal;
  TemplateFile: THandle): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseImageHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

{ file内容やpathをlogへ出さず、通常file・link・不在だけを確認する。 }
function GetConnectionAttributes(FileName: String): Cardinal;
  external 'GetFileAttributesW@kernel32.dll stdcall';
function DeleteConnectionFile(FileName: String): Boolean;
  external 'DeleteFileW@kernel32.dll stdcall';

{ ClientのWindowまたは対象の二EXEが使用中なら、変更前に停止する。強制終了はしない。 }
function BusyReason(): String;
var
  Index: Integer;
  Name, Path: String;
  ImageHandle: THandle;
begin
  Result := '';
  if (FindWindowByClassName('ZRdpSvnBridge.ConnectionManagerWindow') <> 0) or
     (FindWindowByClassName('ZRdpSvnBridge.MainWindow') <> 0) then
  begin
    Result := CustomMessage('CloseClient');
    Exit;
  end;

  for Index := 0 to 1 do
  begin
    if Index = 0 then Name := '{#ClientExeName}' else Name := '{#AgentExeName}';
    Path := AddBackslash(ExpandConstant('{app}')) + Name;
    if FileExists(Path) then
    begin
      ImageHandle := OpenImageForExclusiveRead(Path, $80000000, 0, 0, 3, $80, 0);
      if ImageHandle = THandle($FFFFFFFF) then
      begin
        Result := CustomMessage('ImageBusy');
        Exit;
      end;
      CloseImageHandle(ImageHandle);
    end;
  end;
end;

{ 既存の新しいEXEを、古いinstallerで一部だけ巻き戻さない。 }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Index: Integer;
  Name, Path: String;
  VersionMS, VersionLS: Cardinal;
begin
  Result := BusyReason();
  if Result <> '' then Exit;
  for Index := 0 to 1 do
  begin
    if Index = 0 then Name := '{#ClientExeName}' else Name := '{#AgentExeName}';
    Path := AddBackslash(ExpandConstant('{app}')) + Name;
    if FileExists(Path) then
    begin
      if not GetVersionNumbers(Path, VersionMS, VersionLS) then
      begin
        Result := CustomMessage('InvalidTarget');
        Exit;
      end;
      if (VersionMS > {#AppVersionMS}) or
         ((VersionMS = {#AppVersionMS}) and (VersionLS > {#AppVersionLS})) then
      begin
        Result := CustomMessage('NewerInstalled');
        Exit;
      end;
    end;
  end;
end;

{ 本製品のcurrent-user保存先だけ。test buildはworkspace内の専用fixtureへ固定する。 }
function ConnectionDataDirectory(): String;
begin
#ifdef InstallerTestMode
  Result := '{#InstallerOutputDirectory}\connection-data';
#else
  Result := ExpandConstant('{localappdata}\ZRdpSvnBridge');
#endif
end;

{ 任意名やwildcardを受け付けず、削除対象を現在の保存形式の二fileへ限定する。 }
function ConnectionDataPath(Index: Integer): String;
begin
  if Index = 0 then
    Result := AddBackslash(ConnectionDataDirectory()) + 'connection-history.bin'
  else
    Result := AddBackslash(ConnectionDataDirectory()) + 'agent-settings.bin';
end;

{ 確認不能を不在と見なさず、directoryやlinkをfile削除の対象にしない。 }
function ConnectionDataSafetyReason(): String;
var
  Index: Integer;
  Attributes, ErrorCode: Cardinal;
  Path: String;
  ImageHandle: THandle;
begin
  Result := '';
  Attributes := GetConnectionAttributes(ConnectionDataDirectory());
  if Attributes = $FFFFFFFF then
  begin
    ErrorCode := DLLGetLastError();
    { 保存先がない場合は削除不要。それ以外の確認失敗は拒否する。 }
    if (ErrorCode <> 2) and (ErrorCode <> 3) then
      Result := CustomMessage('ConnectionDataUnsafe');
    Exit;
  end;
  { 親directoryのjunction／symlinkを通って別の保存先へ削除しない。 }
  if ((Attributes and $10) = 0) or ((Attributes and $400) <> 0) then
  begin
    Result := CustomMessage('ConnectionDataUnsafe');
    Exit;
  end;

  { 二fileを両方検査してから削除へ進み、既知のlock等で片方だけ消さない。 }
  for Index := 0 to 1 do
  begin
    Path := ConnectionDataPath(Index);
    Attributes := GetConnectionAttributes(Path);
    if Attributes = $FFFFFFFF then
    begin
      ErrorCode := DLLGetLastError();
      if (ErrorCode <> 2) and (ErrorCode <> 3) then
      begin
        Result := CustomMessage('ConnectionDataUnsafe');
        Exit;
      end;
    end
    else
    begin
      { read-only、directory、reparse pointを通常の接続情報と推測しない。 }
      if (Attributes and ($1 or $10 or $400)) <> 0 then
      begin
        Result := CustomMessage('ConnectionDataUnsafe');
        Exit;
      end;
      ImageHandle := OpenImageForExclusiveRead(Path, $80000000, 0, 0, 3, $80, 0);
      if ImageHandle = THandle($FFFFFFFF) then
      begin
        Result := CustomMessage('ConnectionDataUnsafe');
        Exit;
      end;
      CloseImageHandle(ImageHandle);
    end;
  end;
end;

{ 選択していてもcancelなら削除を許可しない。UIとsilentで同じ契約を使う。 }
function RecordUninstallChoice(Choice: Integer; Requested: Boolean): Boolean;
begin
  DeleteConnectionData := (Choice = mrOk) and Requested;
  Result := Choice = mrOk;
end;

{ 未選択を既定とする一回限りの選択画面。閉じる／Escapeもcancelになる。 }
function ChooseUninstallOptions(): Boolean;
var
  Form: TSetupForm;
  Intro, Note: TNewStaticText;
  Option: TNewCheckBox;
  ContinueButton, CancelButton: TNewButton;
  Choice, ButtonWidth: Integer;
begin
  Form := CreateCustomForm(ScaleX(480), ScaleY(248), False, True);
  try
    Form.Caption := CustomMessage('UninstallOptions');
    Intro := TNewStaticText.Create(Form);
    Intro.Parent := Form;
    Intro.SetBounds(ScaleX(16), ScaleY(16), Form.ClientWidth - ScaleX(32), ScaleY(48));
    Intro.AutoSize := False;
    Intro.WordWrap := True;
    Intro.Caption := CustomMessage('ConnectionDataIntro');

    Option := TNewCheckBox.Create(Form);
    Option.Parent := Form;
    Option.SetBounds(ScaleX(16), ScaleY(76), Form.ClientWidth - ScaleX(32), ScaleY(24));
    Option.Caption := CustomMessage('ConnectionDataChoice');
    Option.Checked := False;

    Note := TNewStaticText.Create(Form);
    Note.Parent := Form;
    Note.SetBounds(ScaleX(16), ScaleY(116), Form.ClientWidth - ScaleX(32), ScaleY(76));
    Note.AutoSize := False;
    Note.WordWrap := True;
    Note.Caption := CustomMessage('ConnectionDataNote');

    ContinueButton := TNewButton.Create(Form);
    ContinueButton.Parent := Form;
    ContinueButton.Caption := CustomMessage('ContinueUninstall');
    ContinueButton.ModalResult := mrOk;
    ContinueButton.Default := True;
    CancelButton := TNewButton.Create(Form);
    CancelButton.Parent := Form;
    CancelButton.Caption := CustomMessage('CancelUninstall');
    CancelButton.ModalResult := mrCancel;
    CancelButton.Cancel := True;
    ButtonWidth := Form.CalculateButtonWidth([ContinueButton.Caption, CancelButton.Caption]);
    ContinueButton.SetBounds(Form.ClientWidth - ScaleX(24) - 2 * ButtonWidth,
      Form.ClientHeight - ScaleY(40), ButtonWidth, ScaleY(24));
    CancelButton.SetBounds(Form.ClientWidth - ScaleX(16) - ButtonWidth,
      Form.ClientHeight - ScaleY(40), ButtonWidth, ScaleY(24));
    Form.ActiveControl := Option;
    Choice := Form.ShowModal();
    Result := RecordUninstallChoice(Choice, Option.Checked);
  finally
    Form.Free();
  end;
end;

{ Client使用中と不正保存先を、uninstallによる変更前に拒否する。 }
function InitializeUninstall(): Boolean;
var
  Reason: String;
begin
  DeleteConnectionData := False;
  Reason := BusyReason();
  Result := False;
  if Reason <> '' then
  begin
    SuppressibleMsgBox(Reason, mbError, MB_OK, IDOK);
    Exit;
  end;
#ifdef InstallerTestMode
  { 自動回帰でもUIと同じcancel契約を通し、実profileへはアクセスしない。 }
  if ExpandConstant('{param:TESTCANCELCHOICE|0}') = '1' then
  begin
    Result := RecordUninstallChoice(mrCancel, True);
    Exit;
  end;
#endif
  { Silentは保持が既定。明示的な一値だけを削除の同意として扱う。 }
  if UninstallSilent() then
    Result := RecordUninstallChoice(mrOk, ExpandConstant('{param:DELETECONNECTIONDATA|0}') = '1')
  else
    Result := ChooseUninstallOptions();
  if not Result then Exit;
  if DeleteConnectionData then
  begin
    Reason := ConnectionDataSafetyReason();
    if Reason <> '' then
    begin
      Result := False;
      SuppressibleMsgBox(Reason, mbError, MB_OK, IDOK);
    end;
  end;
end;

{ 標準uninstall実行後だけ、選択済みの固定二fileを除去する。取消段階では消さない。 }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Index: Integer;
  Attributes, ErrorCode: Cardinal;
  Reason, Path: String;
begin
  if (CurUninstallStep <> usPostUninstall) or not DeleteConnectionData then Exit;
  { 選択画面の後でClient起動やfileの状態変化があっても、未確認の削除をしない。 }
  Reason := BusyReason();
  if Reason = '' then Reason := ConnectionDataSafetyReason();
  if Reason <> '' then RaiseException(Reason);
  for Index := 0 to 1 do
  begin
    Path := ConnectionDataPath(Index);
    Attributes := GetConnectionAttributes(Path);
    if Attributes = $FFFFFFFF then
    begin
      ErrorCode := DLLGetLastError();
      if (ErrorCode <> 2) and (ErrorCode <> 3) then
        RaiseException(CustomMessage('ConnectionDataDeleteFailed'));
    end
    else
    begin
      { 再確認後に不正種別へ変化した場合も、そのfileを削除しない。 }
      if (Attributes and ($1 or $10 or $400)) <> 0 then
        RaiseException(CustomMessage('ConnectionDataDeleteFailed'));
      if not DeleteConnectionFile(Path) then
        RaiseException(CustomMessage('ConnectionDataDeleteFailed'));
    end;
  end;
end;
