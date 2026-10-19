# Copyright (c) 2026 EnterSystem CO.,LTD.
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Inno Setupのcompile境界と、workspace内だけの配置・除去を検証する。
.DESCRIPTION
実Client／Agentを起動せず、署名・実設定・OSのshortcut／uninstall登録を変更しない。
InstallerTestModeの生成物はwork/tmpに限定する。検査に使ったfileとlogは保全する。
.PARAMETER RunInstallation
専用test modeをwork/tmpへsilent install・再install・uninstallする。
.PARAMETER OlderExecutableDirectory
任意の同版旧Client／Agentをcopyして、混在版とdowngrade拒否を検証する。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $ExecutableDirectory,
    [string] $IsccPath = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
    [string] $OlderExecutableDirectory,
    [switch] $RunInstallation
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Windows上で実行してください。' }
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
$compiler = (Get-Item -LiteralPath $IsccPath).FullName
$source = (Get-Item -LiteralPath $ExecutableDirectory).FullName
$base = Join-Path $repository ('work/tmp/installer-test-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $base
$scriptPath = Join-Path $repository 'installer/app_installer_setup.iss'
$images = @('ZRdpSvnBridge.exe', 'ZRdpSvnAgent.exe')
$before = @($images | ForEach-Object { (Get-FileHash -LiteralPath (Join-Path $source $_)).Hash })
$version = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $source $images[0])).FileVersion
$checks = [Collections.Generic.List[string]]::new()
$invocation = 0

function Invoke-IsolatedProcess([string] $program, [string[]] $arguments) {
    # 呼び出し先はcompilerまたは今回生成したtest Setup／uninstallerだけ。
    $script:invocation++
    $prefix = Join-Path $base ('process-' + $script:invocation)
    $argumentLine = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $process = Start-Process -FilePath $program -ArgumentList $argumentLine -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput ($prefix + '.stdout.log') -RedirectStandardError ($prefix + '.stderr.log')
    try {
        if (-not $process.WaitForExit(30000)) { throw 'Isolated installer process did not finish within 30 seconds.' }
        $process.Refresh()
        return $process.ExitCode
    } finally { $process.Dispose() }
}

function Compile-TestSetup([string] $inputDirectory, [string] $outputDirectory) {
    $null = New-Item -ItemType Directory -Path $outputDirectory -Force
    return Invoke-IsolatedProcess $compiler @('/Q', '/DInstallerTestMode',
        ('/DExecutableDirectory=' + $inputDirectory),
        ('/DInstallerOutputDirectory=' + $outputDirectory), $scriptPath)
}

function Assert-SourcePairUnchanged {
    $after = @($images | ForEach-Object { (Get-FileHash -LiteralPath (Join-Path $source $_)).Hash })
    if (@(Compare-Object $before $after).Count -ne 0) { throw 'Source Client/Agent was modified.' }
}

function Assert-InstalledPayload([string] $destination) {
    # 配布物は固定六file。EXEの署名を変えずbyte単位で同じものを配置する。
    $expected = @{
        'ZRdpSvnBridge.exe' = Join-Path $source 'ZRdpSvnBridge.exe'
        'ZRdpSvnAgent.exe' = Join-Path $source 'ZRdpSvnAgent.exe'
        'LICENSE' = Join-Path $repository 'LICENSE'
        'NOTICE' = Join-Path $repository 'NOTICE'
        'DEPENDENCIES.md' = Join-Path $repository 'docs/dependencies.md'
        'README.txt' = Join-Path $repository 'installer/README.txt'
    }
    foreach ($name in $expected.Keys) {
        $actual = Join-Path $destination $name
        if (-not (Test-Path -LiteralPath $actual) -or
            (Get-FileHash -LiteralPath $actual).Hash -cne (Get-FileHash -LiteralPath $expected[$name]).Hash) {
            throw ('Installed payload mismatch: ' + $name)
        }
    }
    $allowed = @($expected.Keys) + @('unins000.exe', 'unins000.dat', 'client.ini', 'preserve.txt')
    foreach ($entry in (Get-ChildItem -LiteralPath $destination -File)) {
        if ($entry.Name -notin $allowed) { throw ('Unexpected installed file: ' + $entry.Name) }
    }
    if (Test-Path -LiteralPath (Join-Path $destination 'agent.ini')) { throw 'Agent INI must not be installed.' }
}

function Assert-UninstalledPayload([string] $destination) {
    # 標準uninstallerの自己削除はprocess終了直後に遅延する場合がある。
    $uninstaller = Join-Path $destination 'unins000.exe'
    $selfDeleteDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while ((Test-Path -LiteralPath $uninstaller) -and [DateTime]::UtcNow -lt $selfDeleteDeadline) {
        Start-Sleep -Milliseconds 50
    }
    foreach ($name in @($images) + @('LICENSE', 'NOTICE', 'DEPENDENCIES.md', 'README.txt', 'unins000.exe', 'unins000.dat')) {
        if (Test-Path -LiteralPath (Join-Path $destination $name)) { throw 'Owned installed file was not removed.' }
    }
}

function Assert-PreservedManualFiles([string] $destination) {
    foreach ($entry in @(@('client.ini','[fixture] preserve=true'), @('preserve.txt','unrelated file must remain'),
            @('logs/fixture.txt','local fixture only'))) {
        if ([IO.File]::ReadAllText((Join-Path $destination $entry[0])) -cne $entry[1]) {
            throw 'Existing setting, unrelated file or log was removed or changed.'
        }
    }
}

function Assert-ConnectionData([string] $directory, [bool] $deleted) {
    # 実設定ではなく専用test rootの既知二fileと無関係fileだけを検査する。
    foreach ($name in @('connection-history.bin', 'agent-settings.bin')) {
        $path = Join-Path $directory $name
        if ($deleted) {
            if (Test-Path -LiteralPath $path) { throw 'Selected connection data was not removed.' }
        } elseif (-not (Test-Path -LiteralPath $path) -or [IO.File]::ReadAllText($path) -cne ('fixture:' + $name)) {
            throw 'Connection data was removed or changed without consent.'
        }
    }
    foreach ($name in @('keep.txt', 'agent-settings.bin.bak', 'logs/fixture.txt')) {
        if ([IO.File]::ReadAllText((Join-Path $directory $name)) -cne 'fixture:keep') {
            throw 'Cleanup exceeded the two fixed connection data files.'
        }
    }
}

$output = Join-Path $base 'compiler output'
if ((Compile-TestSetup $source $output) -ne 0) { throw 'Test-mode Setup compile failed; see retained process logs.' }
$setup = Join-Path $output ('ZRdp_SVN_Bridge_Setup_' + $version + '_test_unsigned.exe')
if (-not (Test-Path -LiteralPath $setup)) { throw 'Expected test Setup was not generated.' }
$checks.Add('compile')
$setupHash = (Get-FileHash -LiteralPath $setup).Hash
if ((Compile-TestSetup $source $output) -eq 0 -or (Get-FileHash -LiteralPath $setup).Hash -cne $setupHash) {
    throw 'Existing Setup was overwritten.'
}
$checks.Add('existing-output-rejection')

foreach ($missing in $images) {
    $fixture = Join-Path $base ('missing-' + $missing)
    $null = New-Item -ItemType Directory -Path $fixture
    foreach ($name in $images) {
        if ($name -ne $missing) { Copy-Item -LiteralPath (Join-Path $source $name) -Destination $fixture }
    }
    if ((Compile-TestSetup $fixture (Join-Path $fixture 'output')) -eq 0) { throw 'Incomplete pair was accepted.' }
}
$checks.Add('missing-client-and-agent-rejection')
$mislabeled = Join-Path $base 'wrong-agent'
$null = New-Item -ItemType Directory -Path $mislabeled
Copy-Item -LiteralPath (Join-Path $source $images[0]) -Destination $mislabeled
Copy-Item -LiteralPath (Join-Path $source $images[0]) -Destination (Join-Path $mislabeled $images[1])
if ((Compile-TestSetup $mislabeled (Join-Path $mislabeled 'output')) -eq 0) { throw 'Mislabeled Agent was accepted.' }
$checks.Add('mislabeled-agent-rejection')

$olderSource = ''
if ($OlderExecutableDirectory) {
    $olderSource = (Get-Item -LiteralPath $OlderExecutableDirectory).FullName
    $olderVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $olderSource $images[0])).FileVersion
    if ([version]$olderVersion -ge [version]$version) { throw 'The downgrade fixture must be older than the current pair.' }
    $mixed = Join-Path $base 'mixed-version'
    $null = New-Item -ItemType Directory -Path $mixed
    Copy-Item -LiteralPath (Join-Path $source $images[0]) -Destination $mixed
    Copy-Item -LiteralPath (Join-Path $olderSource $images[1]) -Destination $mixed
    if ((Compile-TestSetup $mixed (Join-Path $mixed 'output')) -eq 0) { throw 'Mixed versions were accepted.' }
    $checks.Add('mixed-version-rejection')
}

if ($RunInstallation) {
    # 許可した隔離folderを明示する。silentでも実Client／Agentは起動しない。
    $destination = Join-Path $base 'インストール test'
    $null = New-Item -ItemType Directory -Path $destination
    [IO.File]::WriteAllText((Join-Path $destination 'client.ini'), '[fixture] preserve=true')
    [IO.File]::WriteAllText((Join-Path $destination 'preserve.txt'), 'unrelated file must remain')
    $null = New-Item -ItemType Directory -Path (Join-Path $destination 'logs')
    [IO.File]::WriteAllText((Join-Path $destination 'logs/fixture.txt'), 'local fixture only')
    $installArguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', '/LANG=Japanese', ('/DIR=' + $destination))
    if ((Invoke-IsolatedProcess $setup $installArguments) -ne 0) { throw 'Isolated install failed.' }
    Assert-InstalledPayload $destination
    if ((Invoke-IsolatedProcess $setup $installArguments) -ne 0) { throw 'Isolated reinstall failed.' }
    Assert-InstalledPayload $destination
    $checks.Add('install-and-reinstall-payload-hashes')
    $uninstaller = Join-Path $destination 'unins000.exe'
    $uninstallArguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')
    $deleteArguments = $uninstallArguments + @('/DELETECONNECTIONDATA=1')
    # 接続情報のroot不在は正常な未使用状態。実ユーザーprofileへfallbackしない。
    if ((Invoke-IsolatedProcess $uninstaller $deleteArguments) -ne 0) { throw 'Absent connection directory cleanup failed.' }
    Assert-UninstalledPayload $destination
    Assert-PreservedManualFiles $destination
    $checks.Add('missing-connection-directory')
    if ((Invoke-IsolatedProcess $setup $installArguments) -ne 0) { throw 'Reinstall for connection tests failed.' }
    Assert-InstalledPayload $destination

    $connectionDirectory = Join-Path $output 'connection-data'
    $null = New-Item -ItemType Directory -Path (Join-Path $connectionDirectory 'logs')
    foreach ($name in @('connection-history.bin', 'agent-settings.bin')) {
        [IO.File]::WriteAllText((Join-Path $connectionDirectory $name), ('fixture:' + $name))
    }
    foreach ($name in @('keep.txt', 'agent-settings.bin.bak', 'logs/fixture.txt')) {
        [IO.File]::WriteAllText((Join-Path $connectionDirectory $name), 'fixture:keep')
    }
    if ((Invoke-IsolatedProcess $uninstaller ($deleteArguments + @('/TESTCANCELCHOICE=1'))) -eq 0) {
        throw 'Cancelled uninstall was accepted.'
    }
    Assert-InstalledPayload $destination
    Assert-ConnectionData $connectionDirectory $false
    $checks.Add('cancel-choice-preserves-program-and-connection-data')

    # 既存EXEを保持したまま、installとuninstallの両方が変更前に拒否される。
    $locked = [IO.File]::Open((Join-Path $destination $images[1]), [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::None)
    try {
        if ((Invoke-IsolatedProcess $setup $installArguments) -eq 0) { throw 'Locked image install was accepted.' }
        if ((Invoke-IsolatedProcess $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')) -eq 0) {
            throw 'Locked image uninstall was accepted.'
        }
    } finally { $locked.Dispose() }
    Assert-InstalledPayload $destination
    $checks.Add('in-use-install-and-uninstall-rejection')

    # 片方だけのlockやread-onlyでも、もう片方を先に削除しない。
    $connectionFile = Join-Path $connectionDirectory 'agent-settings.bin'
    $locked = [IO.File]::Open($connectionFile, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try {
        if ((Invoke-IsolatedProcess $uninstaller $deleteArguments) -eq 0) { throw 'Locked connection cleanup was accepted.' }
    } finally { $locked.Dispose() }
    Assert-InstalledPayload $destination
    Assert-ConnectionData $connectionDirectory $false
    $checks.Add('locked-connection-data-rejection')
    $originalAttributes = [IO.File]::GetAttributes($connectionFile)
    try {
        [IO.File]::SetAttributes($connectionFile, $originalAttributes -bor [IO.FileAttributes]::ReadOnly)
        if ((Invoke-IsolatedProcess $uninstaller $deleteArguments) -eq 0) { throw 'Read-only connection cleanup was accepted.' }
    } finally { [IO.File]::SetAttributes($connectionFile, $originalAttributes) }
    Assert-InstalledPayload $destination
    Assert-ConnectionData $connectionDirectory $false
    $checks.Add('read-only-connection-data-rejection')

    # test専用の固定一fileを保全してdirectoryへ差し替え、再帰除去されないことを確認する。
    $heldFile = Join-Path $connectionDirectory 'held-agent-settings.bin'
    Move-Item -LiteralPath $connectionFile -Destination $heldFile
    $null = New-Item -ItemType Directory -Path $connectionFile
    try {
        if ((Invoke-IsolatedProcess $uninstaller $deleteArguments) -eq 0) { throw 'Directory-shaped connection data was accepted.' }
        if (-not (Test-Path -LiteralPath $connectionFile -PathType Container)) { throw 'Fixture directory was removed.' }
    } finally {
        # 空のfixture directoryだけを除去し、保全した一fileを戻す。再帰削除しない。
        Remove-Item -LiteralPath $connectionFile
        Move-Item -LiteralPath $heldFile -Destination $connectionFile
    }
    Assert-InstalledPayload $destination
    Assert-ConnectionData $connectionDirectory $false
    $checks.Add('directory-shaped-connection-data-rejection')

    # directory移動前に両絶対pathを今回のfixture配下と確認し、junction先も同scopeにする。
    $heldDirectory = [IO.Path]::GetFullPath((Join-Path $base 'held-connection-data'))
    $resolvedConnectionDirectory = [IO.Path]::GetFullPath($connectionDirectory)
    foreach ($candidate in @($heldDirectory, $resolvedConnectionDirectory)) {
        if (-not $candidate.StartsWith($base + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Connection fixture move would leave the isolated workspace.'
        }
    }
    Move-Item -LiteralPath $resolvedConnectionDirectory -Destination $heldDirectory
    try {
        $null = New-Item -ItemType Junction -Path $resolvedConnectionDirectory -Target $heldDirectory
        if ((Invoke-IsolatedProcess $uninstaller $deleteArguments) -eq 0) { throw 'Junction-shaped connection directory was accepted.' }
        Assert-ConnectionData $heldDirectory $false
    } finally {
        if (Test-Path -LiteralPath $resolvedConnectionDirectory) {
            $junction = Get-Item -LiteralPath $resolvedConnectionDirectory -Force
            if ($junction.LinkType -ne 'Junction') { throw 'Unexpected fixture type; refusing removal.' }
            # 明示したjunctionだけを消す。targetの内容は除去しない。
            Remove-Item -LiteralPath $resolvedConnectionDirectory -Force
        }
        Move-Item -LiteralPath $heldDirectory -Destination $resolvedConnectionDirectory
    }
    Assert-InstalledPayload $destination
    Assert-ConnectionData $connectionDirectory $false
    $checks.Add('junction-connection-directory-rejection')

    if ($olderSource) {
        $olderOutput = Join-Path $base 'older-setup'
        if ((Compile-TestSetup $olderSource $olderOutput) -ne 0) { throw 'Older test Setup compile failed.' }
        $olderSetup = Join-Path $olderOutput ('ZRdp_SVN_Bridge_Setup_' + $olderVersion + '_test_unsigned.exe')
        if ((Invoke-IsolatedProcess $olderSetup $installArguments) -eq 0) { throw 'Downgrade was accepted.' }
        Assert-InstalledPayload $destination
        $checks.Add('downgrade-rejection')
    }

    if ((Invoke-IsolatedProcess $uninstaller @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART')) -ne 0) {
        throw 'Isolated uninstall failed.'
    }
    Assert-UninstalledPayload $destination
    Assert-PreservedManualFiles $destination
    Assert-ConnectionData $connectionDirectory $false
    $checks.Add('uninstall-preserves-settings-logs-and-unrelated-files')

    # silentの不正値は同意にしない。既定保持を維持する。
    if ((Invoke-IsolatedProcess $setup $installArguments) -ne 0) { throw 'Reinstall before invalid-choice test failed.' }
    if ((Invoke-IsolatedProcess $uninstaller ($uninstallArguments + @('/DELETECONNECTIONDATA=invalid'))) -ne 0) {
        throw 'Invalid cleanup value failed instead of preserving settings.'
    }
    Assert-UninstalledPayload $destination
    Assert-ConnectionData $connectionDirectory $false
    $checks.Add('invalid-delete-value-preserves-connection-data')

    # 明示選択時は二fileだけを除去し、INI、log、backup、無関係fileを保持する。
    if ((Invoke-IsolatedProcess $setup $installArguments) -ne 0) { throw 'Reinstall before selected cleanup failed.' }
    if ((Invoke-IsolatedProcess $uninstaller $deleteArguments) -ne 0) { throw 'Selected connection cleanup failed.' }
    Assert-UninstalledPayload $destination
    Assert-PreservedManualFiles $destination
    Assert-ConnectionData $connectionDirectory $true
    $checks.Add('selected-cleanup-removes-only-two-connection-files')

    # 削除済みの場合も正常完了する。不在fileから別pathを推定しない。
    if ((Invoke-IsolatedProcess $setup $installArguments) -ne 0) { throw 'Reinstall before absent-file cleanup failed.' }
    if ((Invoke-IsolatedProcess $uninstaller $deleteArguments) -ne 0) { throw 'Absent connection files cleanup failed.' }
    Assert-UninstalledPayload $destination
    Assert-PreservedManualFiles $destination
    Assert-ConnectionData $connectionDirectory $true
    $checks.Add('missing-connection-files')
}

Assert-SourcePairUnchanged
$checks.Add('original-exe-hashes-unchanged')
Write-Output ('Installer checks passed: ' + ($checks -join ', '))
Write-Output ('Retained isolated fixture: ' + $base)
