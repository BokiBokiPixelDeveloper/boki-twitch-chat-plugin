[CmdletBinding()]
param(
    [string]$BuildDirectory = "$PSScriptRoot/../build/windows-x86_64",
    [string]$Configuration = 'RelWithDebInfo',
    [string]$Destination = (Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'obs-studio/plugins/bokis-twitch-chat-plugin'),
    [switch]$SkipRegistration,
    [string]$StateRoot = (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'BokisTwitchChatPlugin')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. "$PSScriptRoot/windows-common.ps1"
$Destination = [IO.Path]::GetFullPath($Destination)
$discovery = @{}
if (!$SkipRegistration) { $discovery = Get-ObsDiscovery $Destination; Test-ObsDiscovery $discovery }
if (Get-Process -Name obs64,obs32,obs -ErrorAction SilentlyContinue) {
    throw 'Close all OBS instances completely before installing.'
}
$temporary = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temporary | Out-Null
$locks = [Collections.Generic.List[IDisposable]]::new()
$changed = [Collections.Generic.List[string]]::new()
$backups = @{}
$transaction = Join-Path $Destination '.developer-install'
$ownsTransaction = $false
try {
    if (Test-Path "$PSScriptRoot/plugin") {
        $payload = "$PSScriptRoot/plugin"
        $listed = @()
        foreach ($line in Get-Content "$PSScriptRoot/SHA256SUMS") {
            if ($line -notmatch '^([0-9a-f]{64})  ([A-Za-z0-9_./-]+)$') { throw 'Invalid package manifest' }
            $hash, $name = $Matches[1], $Matches[2]
            if ($name.StartsWith('/') -or $name.Split('/') -contains '..') { throw 'Unsafe package path' }
            $listed += $name
            if ((Get-FileHash (Join-Path $payload $name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) {
                throw "Package checksum mismatch: $name"
            }
        }
        $actual = @(Get-ChildItem $payload -File -Recurse | ForEach-Object { [IO.Path]::GetRelativePath($payload, $_.FullName).Replace('\','/') })
        if (Compare-Object ($listed | Sort-Object) ($actual | Sort-Object)) { throw 'Unverified package files' }
    } else {
        $payload = "$temporary/plugin"
        & cmake --install $BuildDirectory --config $Configuration --prefix $payload
        if ($LASTEXITCODE -ne 0) { throw 'Build and install staging failed' }
    }
    foreach ($required in @('bokis-twitch-chat-plugin.dll', 'bokis-twitch-chat-updater.exe', 'Qt6WebSockets.dll')) {
        if (!(Test-Path "$payload/bin/64bit/$required")) { throw "Missing $required" }
    }
    $pending = Join-Path $StateRoot 'cache/pending'
    New-Item -ItemType Directory -Force $pending, "$Destination/bin/64bit" | Out-Null
    foreach ($path in @("$pending/update.lock", "$Destination/bin/64bit/bokis-twitch-chat-plugin.dll.use.lock")) {
        $locks.Add([IO.File]::Open($path, 'OpenOrCreate', 'ReadWrite', 'None'))
    }
    if ((Test-Path "$pending/pending.json") -or (Test-Path "$pending/transaction.json") -or (Test-Path $transaction)) {
        throw 'An update or interrupted development installation needs recovery first.'
    }
    New-Item -ItemType Directory $transaction | Out-Null
    $ownsTransaction = $true
    $files = @(Get-ChildItem $payload -File -Recurse)
    foreach ($file in $files) {
        $relative = [IO.Path]::GetRelativePath($payload, $file.FullName)
        $target = Join-Path $Destination $relative
        $staged = Join-Path "$transaction/new" $relative
        New-Item -ItemType Directory -Force (Split-Path $target), (Split-Path $staged) | Out-Null
        if (Test-Path $target) {
            $probe = [IO.File]::Open($target, 'Open', 'ReadWrite', 'None')
            $probe.Dispose()
            $backup = Join-Path "$transaction/old" $relative
            New-Item -ItemType Directory -Force (Split-Path $backup) | Out-Null
            Copy-Item $target $backup
            $backups[$target] = $backup
        }
        Copy-Item $file.FullName $staged
    }
    foreach ($file in $files) {
        $relative = [IO.Path]::GetRelativePath($payload, $file.FullName)
        $target = Join-Path $Destination $relative
        Add-Content "$transaction/changed.txt" $relative -Encoding utf8
        $changed.Add($target)
        [IO.File]::Move((Join-Path "$transaction/new" $relative), $target, $true)
    }
    if (!$SkipRegistration) { Register-ObsDiscovery $discovery $Destination }
    Remove-Item $transaction -Recurse -Force
    Write-Host "Installed to $Destination. Start OBS from a new session; sign out/in if it does not see the new plugin path."
} catch {
    $originalError = $_
    $rollbackFailed = $false
    for ($i = $changed.Count - 1; $i -ge 0; $i--) {
        $target = $changed[$i]
        try {
            if ($backups.ContainsKey($target)) { Copy-Item $backups[$target] $target -Force }
            else { Remove-Item $target -Force -ErrorAction SilentlyContinue }
        } catch { $rollbackFailed = $true }
    }
    if ($rollbackFailed) { Write-Warning "Keep OBS closed. Recover originals from $transaction/old." }
    elseif ($ownsTransaction) { Remove-Item $transaction -Recurse -Force }
    throw $originalError
} finally {
    foreach ($lock in $locks) { $lock.Dispose() }
    Remove-Item $temporary -Recurse -Force
}
