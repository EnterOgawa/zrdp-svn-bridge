# Copyright (c) 2026 EnterSystem CO.,LTD.
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
未署名の同版二EXEでlocal packageの同梱・hash・拒否経路を検証する。EXEを実行しない。
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string] $ExecutableDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$base = Join-Path $repository ('work/tmp/package-test-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $base
$generator = Join-Path $PSScriptRoot 'New-PortablePackage.ps1'
$source = (Get-Item -LiteralPath $ExecutableDirectory).FullName
$before = @('ZRdpSvnBridge.exe','ZRdpSvnAgent.exe') | ForEach-Object { (Get-FileHash -LiteralPath (Join-Path $source $_)).Hash }
$metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $source 'ZRdpSvnBridge.exe'))
$version = @($metadata.FileMajorPart,$metadata.FileMinorPart,$metadata.FileBuildPart,$metadata.FilePrivatePart) -join '.'

$rejected = $false
try { & $generator -ExecutableDirectory $source -OutputDirectory (Join-Path $base 'signed') -Tag ('v' + $version) | Out-Null }
catch { $rejected = $_.Exception.Message.StartsWith('正式配布準備には署名・timestamp') }
if (-not $rejected) { throw 'Unsigned pair must not produce an official package.' }

$output = Join-Path $base 'development'
$zipPath = & $generator -ExecutableDirectory $source -OutputDirectory $output -Development
$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $names = @($archive.Entries | ForEach-Object { [IO.Path]::GetFileName($_.FullName) } | Where-Object { $_ })
    $expected = @('ZRdpSvnBridge.exe','ZRdpSvnAgent.exe','LICENSE','NOTICE','DEPENDENCIES.md','README.txt','package-files.json')
    if (@(Compare-Object $expected $names).Count -ne 0) { throw 'Unexpected package contents.' }
    if ('zrdp-client-update.ini' -in $names -or 'agent.ini' -in $names -or 'client.ini' -in $names) { throw 'Private/update content in development package.' }
    $entry = $archive.Entries | Where-Object { $_.Name -ceq 'package-files.json' }
    $reader = [IO.StreamReader]::new($entry.Open())
    try { $receipt = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    if ($receipt.Purpose -cne 'development-not-for-release' -or $receipt.Files.Count -ne 6) { throw 'Package purpose mismatch.' }
    foreach ($file in $receipt.Files) {
        $entry = $archive.Entries | Where-Object { $_.Name -ceq $file.Name }
        $stream = $entry.Open()
        try { $hash = (Get-FileHash -InputStream $stream).Hash.ToLowerInvariant() } finally { $stream.Dispose() }
        if ($entry.Length -ne $file.Size -or $hash -cne $file.Sha256) { throw 'Receipt hash/size mismatch.' }
    }
} finally { $archive.Dispose() }
$originalHash = (Get-FileHash -LiteralPath $zipPath).Hash
$rejected = $false
try { & $generator -ExecutableDirectory $source -OutputDirectory $output -Development | Out-Null }
catch { $rejected = $true }
if (-not $rejected -or (Get-FileHash -LiteralPath $zipPath).Hash -cne $originalHash) { throw 'Existing archive was overwritten.' }

$incomplete = Join-Path $base 'incomplete'
$null = New-Item -ItemType Directory -Path $incomplete
Copy-Item -LiteralPath (Join-Path $source 'ZRdpSvnBridge.exe') -Destination $incomplete
$rejected = $false
try { & $generator -ExecutableDirectory $incomplete -OutputDirectory (Join-Path $base 'missing-agent') -Development | Out-Null }
catch { $rejected = $true }
if (-not $rejected) { throw 'Missing Agent was accepted.' }

# 別種EXEをAgent名へ変更しても、単なる二file存在確認では受け入れない。
$mislabeled = Join-Path $base 'mislabeled'
$null = New-Item -ItemType Directory -Path $mislabeled
Copy-Item -LiteralPath (Join-Path $source 'ZRdpSvnBridge.exe') -Destination $mislabeled
Copy-Item -LiteralPath (Join-Path $source 'ZRdpSvnBridge.exe') -Destination (Join-Path $mislabeled 'ZRdpSvnAgent.exe')
$rejected = $false
try { & $generator -ExecutableDirectory $mislabeled -OutputDirectory (Join-Path $base 'wrong-agent') -Development | Out-Null }
catch { $rejected = $_.Exception.Message -ceq 'EXEの種類が不正です。' }
if (-not $rejected) { throw 'Mislabeled Agent was accepted.' }

$rejected = $false
try { & $generator -ExecutableDirectory $source -OutputDirectory (Join-Path $base 'wrong-tag') -Tag v0.0.0.0 | Out-Null }
catch { $rejected = $_.Exception.Message.StartsWith('正式配布準備では同Version') }
if (-not $rejected) { throw 'Unrelated tag was accepted.' }

$rejected = $false
try { & $generator -ExecutableDirectory $source -OutputDirectory $repository -Development | Out-Null }
catch { $rejected = $true }
if (-not $rejected) { throw 'Output outside isolated workspace was accepted.' }
$after = @('ZRdpSvnBridge.exe','ZRdpSvnAgent.exe') | ForEach-Object { (Get-FileHash -LiteralPath (Join-Path $source $_)).Hash }
if (@(Compare-Object $before $after).Count -ne 0) { throw 'Input EXE was modified.' }
Write-Output 'Portable package: license/notice, exact files, receipt, unsigned rejection, existing-file, missing/mislabeled-agent, tag and output guards passed.'
