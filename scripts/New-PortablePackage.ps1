# Copyright (c) 2026 EnterSystem CO.,LTD.
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
固定二EXE・LICENSE・NOTICE・inventoryだけからlocal配布archiveをcreate-newで作る。
.DESCRIPTION
署名・GitHub・実設定・入力EXEを変更しない。通常は署名済み同版セットを必須にする。
-Developmentは署名前の同梱検査専用で、公式Releaseや自動更新manifestを生成しない。
出力はrepositoryのwork/tmp以下に限定し、既存archiveを上書きしない。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $ExecutableDirectory,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [string] $Tag,
    [switch] $Development
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Windows上で実行してください。' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
$allowedOutput = $repository + '\work\tmp\'
$outputPath = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
if (-not $outputPath.StartsWith($allowedOutput, [StringComparison]::OrdinalIgnoreCase)) {
    throw '出力先はrepositoryのwork/tmp以下にしてください。'
}
# 既存ancestorのreparseを追って、別directoryへarchiveを書かない。
$ancestor = $outputPath
while ($ancestor.Length -ge $repository.Length) {
    if (Test-Path -LiteralPath $ancestor) {
        $entry = Get-Item -LiteralPath $ancestor
        if (-not $entry.PSIsContainer -or ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw '出力先と親は通常directoryが必要です。'
        }
    }
    $ancestor = [IO.Path]::GetDirectoryName($ancestor)
}
$sourceDirectory = Get-Item -LiteralPath $ExecutableDirectory
if (-not $sourceDirectory.PSIsContainer -or ($sourceDirectory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw '通常の入力directoryが必要です。'
}
$project = Get-Content -LiteralPath (Join-Path $repository 'CMakeLists.txt') -Raw -Encoding UTF8
$versionMatch = [regex]::Match($project, '\bVERSION\s+(\d+\.\d+\.\d+)')
if (-not $versionMatch.Success) { throw '製品Versionが読み取れません。' }
$expectedVersion = $versionMatch.Groups[1].Value + '.0'
if (-not $Development -and $Tag -cne ('v' + $expectedVersion)) {
    throw '正式配布準備では同Versionのv<four-part-version> tagが必要です。'
}
$purpose = if ($Development) { 'development-not-for-release' } else { 'signed-release-candidate' }
$suffix = if ($Development) { '_development' } else { '' }
$stem = 'ZRdp_SVN_Bridge_' + $expectedVersion + $suffix
$zipPath = Join-Path $outputPath ($stem + '.zip')
if (Test-Path -LiteralPath $zipPath) { throw '既存archiveは上書きしません。' }

function Open-PackageImage([string] $name, [bool] $agent) {
    $image = Get-Item -LiteralPath (Join-Path $sourceDirectory.FullName $name)
    $limit = if ($agent) { 16MB } else { 64MB }
    if ($image.PSIsContainer -or ($image.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $image.Length -lt 128 -or $image.Length -gt $limit) { throw '通常の有限EXEが必要です。' }
    $stream = [IO.File]::Open($image.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        # 検査前のsize取得とopenの間に変更された入力を、上限検査済みと扱わない。
        if ($stream.Length -ne $image.Length -or $stream.Length -lt 128 -or $stream.Length -gt $limit) {
            throw 'Open時のEXE sizeが一致しません。'
        }
        $reader = [IO.BinaryReader]::new($stream, [Text.Encoding]::ASCII, $true)
        try {
            if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'PEではありません。' }
            $stream.Position = 0x3C
            $offset = $reader.ReadUInt32()
            if ($offset -lt 64 -or $offset -gt $stream.Length - 96) { throw 'PE offsetが不正です。' }
            $stream.Position = $offset
            if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'x64 EXEが必要です。' }
            $stream.Position = $offset + 22
            if (($reader.ReadUInt16() -band 0x2000) -ne 0) { throw 'DLLは配布できません。' }
            $stream.Position = $offset + 24
            if ($reader.ReadUInt16() -ne 0x20B) { throw 'PE32+が必要です。' }
            $stream.Position = $offset + 24 + 68
            $subsystem = if ($agent) { 3 } else { 2 }
            if ($reader.ReadUInt16() -ne $subsystem) { throw 'EXEの種類が不正です。' }
        } finally { $reader.Dispose() }
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($image.FullName)
        $fileVersion = @($info.FileMajorPart,$info.FileMinorPart,$info.FileBuildPart,$info.FilePrivatePart) -join '.'
        $productVersion = @($info.ProductMajorPart,$info.ProductMinorPart,$info.ProductBuildPart,$info.ProductPrivatePart) -join '.'
        if ($info.ProductName -cne 'ZRdp SVN Bridge' -or $info.OriginalFilename -cne $name -or
            $fileVersion -cne $expectedVersion -or $productVersion -cne $expectedVersion) {
            throw '固定製品情報・同一の現行Versionが必要です。'
        }
        $signature = Get-AuthenticodeSignature -LiteralPath $image.FullName
        $publisher = ''
        if (-not $Development) {
            if ($info.IsDebug -or $signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
                $null -eq $signature.SignerCertificate -or $null -eq $signature.TimeStamperCertificate) {
                throw '正式配布準備には署名・timestampのあるRelease二EXEが必要です。'
            }
            $key = $signature.SignerCertificate.PublicKey
            $publisher = $key.Oid.Value + ':' + [Convert]::ToBase64String($key.EncodedKeyValue.RawData) + ':' +
                [Convert]::ToBase64String($key.EncodedParameters.RawData)
        }
        $stream.Position = 0
        $hash = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash.ToLowerInvariant()
        return [PSCustomObject]@{Name=$name;Stream=$stream;Hash=$hash;Size=$image.Length;Publisher=$publisher;SignatureStatus=$signature.Status.ToString()}
    } catch { $stream.Dispose(); throw }
}

$client = Open-PackageImage 'ZRdpSvnBridge.exe' $false
try {
    $agent = Open-PackageImage 'ZRdpSvnAgent.exe' $true
    try {
        if (-not $Development -and $client.Publisher -cne $agent.Publisher) { throw '署名者を一致させてください。' }
        # 検証が完了する前に入力fileや出力directoryを変更しない。
        $null = New-Item -ItemType Directory -Path $outputPath -Force
        $stage = Join-Path $outputPath ($stem + '-stage-' + [Guid]::NewGuid().ToString('N'))
        $null = New-Item -ItemType Directory -Path $stage
        foreach ($image in @($client,$agent)) {
            $target = Join-Path $stage $image.Name
            $destination = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try { $image.Stream.Position=0; $image.Stream.CopyTo($destination); $destination.Flush($true) }
            finally { $destination.Dispose() }
            if ((Get-FileHash -LiteralPath $target).Hash.ToLowerInvariant() -cne $image.Hash) { throw 'Copy hashが不一致です。' }
        }
        foreach ($name in @('LICENSE','NOTICE')) {
            Copy-Item -LiteralPath (Join-Path $repository $name) -Destination (Join-Path $stage $name) -ErrorAction Stop
        }
        Copy-Item -LiteralPath (Join-Path $repository 'docs/dependencies.md') -Destination (Join-Path $stage 'DEPENDENCIES.md')
        $readme = @('ZRdp SVN Bridge', "Version: $expectedVersion", "Purpose: $purpose",
            'Place ZRdpSvnBridge.exe and ZRdpSvnAgent.exe together. Start the Client.',
            'Register approved HTTP endpoints in Agent settings. Agent starts after RDP authentication.',
            'Respect Windows and organizational policies. No policy bypass is provided.',
            'See LICENSE, NOTICE and DEPENDENCIES.md.',
            'Documentation: https://github.com/EnterOgawa/zrdp-svn-bridge',
            'Development archives are not official Releases and cannot be used as signed auto-update assets.') -join "`n"
        [IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $readme + "`n", [Text.UTF8Encoding]::new($false))
        if (-not $Development) {
            # 既存の厳格なmanifest生成器を再利用する。署名のskip経路は追加しない。
            & (Join-Path $PSScriptRoot 'New-ClientUpdateManifest.ps1') -ClientExecutable (Join-Path $stage $client.Name) -AgentExecutable (Join-Path $stage $agent.Name) -Tag $Tag -OutputDirectory $stage | Out-Null
        }
        $entries = @(Get-ChildItem -LiteralPath $stage -File | Sort-Object Name | ForEach-Object {
            [PSCustomObject]@{Name=$_.Name;Size=$_.Length;Sha256=(Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()}
        })
        $receipt = [PSCustomObject]@{Product='ZRdp SVN Bridge';Version=$expectedVersion;Purpose=$purpose;Files=$entries}
        [IO.File]::WriteAllText((Join-Path $stage 'package-files.json'), ($receipt | ConvertTo-Json -Depth 5) + "`n", [Text.UTF8Encoding]::new($false))
        [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $true)
        Write-Output $zipPath
    } finally { $agent.Stream.Dispose() }
} finally { $client.Stream.Dispose() }

# 入力を実行しない。失敗時のstageも削除せず、署名・remote操作・履歴変更は行わない。
