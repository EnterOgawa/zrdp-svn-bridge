# Copyright (c) 2026 EnterSystem CO.,LTD.
# SPDX-License-Identifier: Apache-2.0
# manifest生成の実入力検査とserializationを検証する。署名・network・EXE実行はしない。
param([string] $RepositoryRoot, [string] $ClientExecutable, [string] $WorkingDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repository = (Get-Item -LiteralPath $RepositoryRoot).FullName.TrimEnd('\')
$work = (Get-Item -LiteralPath $WorkingDirectory).FullName
if (-not $work.StartsWith($repository + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Fixture must stay inside the repository.'
}
$fixture = Join-Path $work ('manifest-fixture-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $fixture
$manifestPath = Join-Path $fixture 'zrdp-client-update.ini'
$generator = Join-Path $repository 'scripts/New-ClientUpdateManifest.ps1'
$originalHash = (Get-FileHash -LiteralPath $ClientExecutable -Algorithm SHA256).Hash
$agentExecutable = Join-Path (Split-Path -Parent $ClientExecutable) 'ZRdpSvnAgent.exe'
# Debug等では既存CMakeがClient／Agentを別target directoryへ出力する。
if (-not (Test-Path -LiteralPath $agentExecutable)) {
    $clientDirectory = Get-Item -LiteralPath (Split-Path -Parent $ClientExecutable)
    if ($clientDirectory.Parent.Name -cne 'client') { throw 'Unexpected test build layout.' }
    $agentExecutable = Join-Path $clientDirectory.Parent.Parent.FullName ('agent\' + $clientDirectory.Name + '\ZRdpSvnAgent.exe')
}
$agentHash = (Get-FileHash -LiteralPath $agentExecutable -Algorithm SHA256).Hash

try {
    $rejected = $false
    try { & $generator -ClientExecutable $ClientExecutable -AgentExecutable $agentExecutable -Tag v0.8.0 -OutputDirectory $fixture }
    catch { $rejected = $true }
    if (-not $rejected -or (Test-Path -LiteralPath $manifestPath)) {
        throw 'Unsigned input must not produce a manifest.'
    }

    # このtest内だけで署名I/Oをfakeへ差替える。production scriptを書き換えない。
    $source = Get-Content -LiteralPath $generator -Raw -Encoding utf8
    $nativeVerification = '$signature = Get-AuthenticodeSignature -LiteralPath $image.FullName'
    if ($source.IndexOf($nativeVerification, [StringComparison]::Ordinal) -lt 0) {
        throw 'Signature seam changed; review the test.'
    }
    $fakeVerification = '$signature = [pscustomobject]@{ Status = [Management.Automation.SignatureStatus]::Valid; SignerCertificate = [pscustomobject]@{ PublicKey = [pscustomobject]@{ Oid=[pscustomobject]@{Value="fixture"}; EncodedKeyValue=[pscustomobject]@{RawData=[byte[]]@(1,2)}; EncodedParameters=[pscustomobject]@{RawData=[byte[]]@(3)} } }; TimeStamperCertificate = 1 }'
    $serializationOnly = [scriptblock]::Create($source.Replace($nativeVerification, $fakeVerification))
    # 片方未署名・署名者違い・Agent欠落・混在版ではmanifestを作らない。
    $agentUnsigned = [scriptblock]::Create($source.Replace($nativeVerification,
        $fakeVerification + '; if ($agent) { $signature.Status = [Management.Automation.SignatureStatus]::NotSigned }'))
    $differentPublisher = [scriptblock]::Create($source.Replace($nativeVerification,
        $fakeVerification + '; if ($agent) { $signature.SignerCertificate.PublicKey.EncodedKeyValue.RawData = [byte[]]@(9) }'))
    $mixedVersion = [scriptblock]::Create($source.Replace($nativeVerification, $fakeVerification).Replace(
        '$version = $parts -join ''.''', '$version = $parts -join ''.''; if ($agent) { $version = "0.0.0.1" }'))
    foreach ($negative in @($agentUnsigned, $differentPublisher, $mixedVersion)) {
        $rejected = $false
        try { & $negative -ClientExecutable $ClientExecutable -AgentExecutable $agentExecutable -Tag v0.8.0 -OutputDirectory $fixture | Out-Null }
        catch { $rejected = $true }
        if (-not $rejected -or (Test-Path -LiteralPath $manifestPath)) {
            throw 'Incomplete, mixed-version or different-publisher set must be rejected.'
        }
    }
    $rejected = $false
    try { & $serializationOnly -ClientExecutable $ClientExecutable -AgentExecutable (Join-Path $fixture 'missing-agent.exe') -Tag v0.8.0 -OutputDirectory $fixture | Out-Null }
    catch { $rejected = $true }
    if (-not $rejected -or (Test-Path -LiteralPath $manifestPath)) { throw 'Missing Agent must be rejected.' }
    & $serializationOnly -ClientExecutable $ClientExecutable -AgentExecutable $agentExecutable -Tag v0.8.0 -OutputDirectory $fixture | Out-Null
    $lines = @(Get-Content -LiteralPath $manifestPath -Encoding ascii)
    $versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($ClientExecutable)
    $version = @($versionInfo.FileMajorPart, $versionInfo.FileMinorPart,
        $versionInfo.FileBuildPart, $versionInfo.FilePrivatePart) -join '.'
    $expected = @('[update]', 'format=2', "version=$version", 'tag=v0.8.0',
        'asset=ZRdpSvnBridge.exe', "size=$((Get-Item -LiteralPath $ClientExecutable).Length)",
        "sha256=$($originalHash.ToLowerInvariant())", 'bridge_protocol=1',
        'agent_asset=ZRdpSvnAgent.exe', "agent_size=$((Get-Item -LiteralPath $agentExecutable).Length)",
        "agent_sha256=$($agentHash.ToLowerInvariant())")
    if (($lines -join "`n") -cne ($expected -join "`n")) {
        throw 'Manifest does not match the runtime parser contract.'
    }
    $manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
    $rejected = $false
    try { & $serializationOnly -ClientExecutable $ClientExecutable -AgentExecutable $agentExecutable -Tag v0.8.0 -OutputDirectory $fixture }
    catch { $rejected = $true }
    if (-not $rejected -or (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash -cne $manifestHash) {
        throw 'Existing manifest must not be overwritten.'
    }
    $rejected = $false
    try { & $serializationOnly -ClientExecutable $ClientExecutable -AgentExecutable $agentExecutable -Tag '../outside' -OutputDirectory $fixture }
    catch { $rejected = $true }
    if (-not $rejected -or (Get-FileHash -LiteralPath $ClientExecutable -Algorithm SHA256).Hash -cne $originalHash) {
        throw 'Unsafe tag must be rejected without changing the input.'
    }
    if ((Get-FileHash -LiteralPath $agentExecutable -Algorithm SHA256).Hash -cne $agentHash) { throw 'Agent input changed.' }
} finally {
    # Guidで今回作成した固定fileだけを削除し、directoryは非再帰で空の場合だけ除去する。
    if (Test-Path -LiteralPath $manifestPath) { Remove-Item -LiteralPath $manifestPath }
    [IO.Directory]::Delete($fixture, $false)
}
Write-Output 'Manifest script checks passed.'
