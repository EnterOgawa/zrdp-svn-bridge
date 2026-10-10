# Copyright (c) 2026 EnterSystem CO.,LTD.
# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
同一版・同一署名者のClient／Agentからformat 2セットmanifestを作成する。
.DESCRIPTION
EXE、証明書、GitHub、設定を変更しない。署名またはtimestampのないEXE、
更新bootstrap非対応EXE、既存manifestへの上書きを拒否する。
Client側ではこの検査に加えSHA-256署名と現行Clientとの公開鍵一致を必須にする。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $ClientExecutable,
    [string] $AgentExecutable,
    [Parameter(Mandatory = $true)][string] $Tag,
    [Parameter(Mandatory = $true)][string] $OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Windows上で実行してください。' }
if ($Tag.Length -gt 128 -or $Tag -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or
    $Tag.Contains('..')) { throw 'Release tagの形式が不正です。' }

$clientImage = Get-Item -LiteralPath $ClientExecutable
if ([string]::IsNullOrEmpty($AgentExecutable)) {
    $AgentExecutable = Join-Path $clientImage.DirectoryName 'ZRdpSvnAgent.exe'
}
$agentImage = Get-Item -LiteralPath $AgentExecutable
$destination = Get-Item -LiteralPath $OutputDirectory
if ($clientImage.PSIsContainer -or $clientImage.Name -cne 'ZRdpSvnBridge.exe' -or
    $agentImage.PSIsContainer -or $agentImage.Name -cne 'ZRdpSvnAgent.exe' -or
    ($clientImage.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    ($agentImage.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    $clientImage.Length -lt 128 -or $clientImage.Length -gt 64MB -or
    $agentImage.Length -lt 128 -or $agentImage.Length -gt 16MB) {
    throw '固定名の通常Client／Agent EXEと有限sizeが必要です。'
}
if (-not $destination.PSIsContainer -or
    ($destination.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw '出力先は既存の通常directoryを指定してください。'
}
$manifestPath = Join-Path $destination.FullName 'zrdp-client-update.ini'
if (Test-Path -LiteralPath $manifestPath) { throw '既存manifestは上書きしません。' }

if (-not ('ZRdp.ManifestVersionResource' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace ZRdp {
    public static class ManifestVersionResource {
        [DllImport("version.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFileVersionInfoSizeW(string path, out uint ignored);
        [DllImport("version.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool GetFileVersionInfoW(string path, uint ignored, uint size, byte[] data);
        [DllImport("version.dll", CharSet = CharSet.Unicode)]
        private static extern bool VerQueryValueW(byte[] data, string key, out IntPtr value, out uint length);
        public static string Read(string path, string key) {
            uint ignored;
            uint size = GetFileVersionInfoSizeW(path, out ignored);
            if (size == 0 || size > 1024 * 1024) throw new InvalidOperationException("Invalid version resource.");
            byte[] data = new byte[size];
            if (!GetFileVersionInfoW(path, 0, size, data)) throw new InvalidOperationException("Version read failed.");
            IntPtr value;
            uint length;
            if (!VerQueryValueW(data, @"\VarFileInfo\Translation", out value, out length) || length < 4)
                throw new InvalidOperationException("Translation missing.");
            ushort language = unchecked((ushort)Marshal.ReadInt16(value));
            ushort codepage = unchecked((ushort)Marshal.ReadInt16(value, 2));
            string query = String.Format(@"\StringFileInfo\{0:x4}{1:x4}\{2}", language, codepage, key);
            if (!VerQueryValueW(data, query, out value, out length) || length == 0 || length > 256) return "";
            return Marshal.PtrToStringUni(value, (int)length - 1);
        }
    }
}
'@
}

# 検査中は二入力とも書込み・renameを共有せず、fileを一度も変更しない。
$clientSource = [IO.File]::Open($clientImage.FullName, [IO.FileMode]::Open,
    [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $agentSource = [IO.File]::Open($agentImage.FullName, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        function Read-UpdateImage($image, $source, [bool] $agent) {
            $reader = [IO.BinaryReader]::new($source, [Text.Encoding]::ASCII, $true)
            try {
                if ($reader.ReadUInt16() -ne 0x5A4D) { throw 'PEではありません。' }
                $source.Position = 0x3C
                $peOffset = $reader.ReadUInt32()
                if ($peOffset -lt 64 -or $peOffset -gt $source.Length - 96) { throw '不正PE offsetです。' }
                $source.Position = $peOffset
                if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
                    throw 'Windows x64 EXEが必要です。'
                }
                $source.Position = $peOffset + 24
                if ($reader.ReadUInt16() -ne 0x20B) { throw 'PE32+ EXEが必要です。' }
                $source.Position = $peOffset + 24 + 68
                $expectedSubsystem = if ($agent) { 3 } else { 2 }
                if ($reader.ReadUInt16() -ne $expectedSubsystem) { throw 'EXE subsystemが不正です。' }
            } finally { $reader.Dispose() }
            $metadata = [Diagnostics.FileVersionInfo]::GetVersionInfo($image.FullName)
            if ($metadata.ProductName -cne 'ZRdp SVN Bridge' -or
                $metadata.OriginalFilename -cne $image.Name) { throw '製品情報が不正です。' }
            $parts = @($metadata.FileMajorPart, $metadata.FileMinorPart, $metadata.FileBuildPart,
                $metadata.FilePrivatePart)
            $version = $parts -join '.'
            $product = @($metadata.ProductMajorPart, $metadata.ProductMinorPart,
                $metadata.ProductBuildPart, $metadata.ProductPrivatePart) -join '.'
            if ($version -cne $product -or @($parts | Where-Object { $_ -lt 0 -or $_ -gt 65535 }).Count -gt 0) {
                throw 'FileVersionとProductVersionを一致させてください。'
            }
            if ([ZRdp.ManifestVersionResource]::Read($image.FullName, 'BridgeProtocol') -cne '1') {
                throw 'Bridge protocol 1が必要です。'
            }
            if ($agent) {
                if ([ZRdp.ManifestVersionResource]::Read($image.FullName, 'AgentSetupProtocol') -cne '1') {
                    throw '固定Agent setup対応が必要です。'
                }
            } elseif ([ZRdp.ManifestVersionResource]::Read($image.FullName, 'UpdaterProtocol') -cne '1' -or
                [ZRdp.ManifestVersionResource]::Read($image.FullName, 'PairedUpdateProtocol') -cne '1') {
                throw 'Clientのセット更新対応が必要です。'
            }
            $signature = Get-AuthenticodeSignature -LiteralPath $image.FullName
            if ($signature.Status -ne [Management.Automation.SignatureStatus]::Valid -or
                $null -eq $signature.SignerCertificate -or $null -eq $signature.TimeStamperCertificate) {
                throw '二EXEともWindowsで有効な署名とtimestampが必要です。'
            }
            $key = $signature.SignerCertificate.PublicKey
            $publisher = $key.Oid.Value + ':' +
                [Convert]::ToBase64String($key.EncodedKeyValue.RawData) + ':' +
                [Convert]::ToBase64String($key.EncodedParameters.RawData)
            $hash = (Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            return [pscustomobject]@{ Version=$version; Publisher=$publisher; Hash=$hash; Size=$source.Length }
        }
        $client = Read-UpdateImage $clientImage $clientSource $false
        $agent = Read-UpdateImage $agentImage $agentSource $true
        if ($client.Version -cne $agent.Version -or $client.Publisher -cne $agent.Publisher) {
            throw 'ClientとAgentは同一版・同一署名者で配布してください。'
        }
        $manifest = @('[update]', 'format=2', "version=$($client.Version)", "tag=$Tag",
            'asset=ZRdpSvnBridge.exe', "size=$($client.Size)", "sha256=$($client.Hash)", 'bridge_protocol=1',
            'agent_asset=ZRdpSvnAgent.exe', "agent_size=$($agent.Size)", "agent_sha256=$($agent.Hash)") -join "`n"
        $bytes = [Text.Encoding]::ASCII.GetBytes($manifest + "`n")
        # 完全なセットだけの固定manifestをcreate-newで保存する。
        $output = [IO.File]::Open($manifestPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write,
            [IO.FileShare]::None)
        try { $output.Write($bytes, 0, $bytes.Length); $output.Flush($true) }
        finally { $output.Dispose() }
    } finally { $agentSource.Dispose() }
} finally { $clientSource.Dispose() }
Write-Output "manifestを作成しました: $manifestPath"
