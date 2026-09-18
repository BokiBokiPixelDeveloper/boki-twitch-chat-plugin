[CmdletBinding()]
param([string]$ReleaseDirectory = "$PSScriptRoot/../release",
      [string]$BuildDirectory = "$PSScriptRoot/../build/windows-x86_64")
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot
$version = (Get-Content "$root/VERSION" -Raw).Trim()
$ReleaseDirectory = [IO.Path]::GetFullPath($ReleaseDirectory)
$temporary = Join-Path ([IO.Path]::GetTempPath()) ("Bokis tests ü 日本語 " + [Guid]::NewGuid().ToString('N'))
$destination = "$temporary/installed/bokis-twitch-chat-plugin"
$state = "$temporary/state"
$binary = "$destination/bin/64bit/bokis-twitch-chat-plugin.dll"
$helper = "$destination/bin/64bit/bokis-twitch-chat-updater.exe"
$setup = "$ReleaseDirectory/bokis-twitch-chat-plugin-setup-$version-windows-x86_64.exe"
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{D40BB76C-E0EB-48AD-94BD-A6C55D883D64}_is1'
if (Test-Path $uninstallKey) { throw 'Refusing to touch an existing installation. Use a clean CI runner.' }
$environmentBefore = @{}
foreach ($name in @('OBS_PLUGINS_PATH', 'OBS_PLUGINS_DATA_PATH')) {
    $environmentBefore[$name] = [Environment]::GetEnvironmentVariable($name, 'User')
}
function Assert([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Run-Installer([string]$Executable, [bool]$Success = $true) {
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/RegisterEnv=0',
        ('/DIR="' + $destination + '"'), ('/StateRoot="' + $state + '"'), ('/LOG="' + $temporary + '/setup.log"'))
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -Wait -PassThru
    if ($Success) { Assert ($process.ExitCode -eq 0) "Installer failed ($($process.ExitCode)); see $temporary/setup.log" }
    else { Assert ($process.ExitCode -ne 0) 'Installer incorrectly accepted a locked or pending installation' }
}
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class TestImageLoader {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr module);
}
'@
New-Item -ItemType Directory -Force $temporary | Out-Null
$installed = $false
try {
    foreach ($name in @("bokis-twitch-chat-plugin-$version-windows-x86_64.dll",
        "bokis-twitch-chat-updater-$version-windows-x86_64.exe",
        "bokis-twitch-chat-plugin-$version-windows-x86_64.zip",
        "bokis-twitch-chat-plugin-setup-$version-windows-x86_64.exe")) {
        $expected = (Get-Content "$ReleaseDirectory/$name.sha256" -Raw).Split(' ')[0]
        Assert ((Get-FileHash "$ReleaseDirectory/$name" -Algorithm SHA256).Hash.ToLowerInvariant() -eq $expected) "Bad checksum: $name"
    }
    Assert ((Get-Item $setup).VersionInfo.ProductVersion.Trim() -eq $version) 'Installer product version disagrees with VERSION'
    foreach ($name in @("bokis-twitch-chat-plugin-$version-windows-x86_64.dll",
        "bokis-twitch-chat-updater-$version-windows-x86_64.exe")) {
        $bytes = [IO.File]::ReadAllBytes("$ReleaseDirectory/$name")
        Assert ($bytes.Length -ge 64 -and [BitConverter]::ToUInt16($bytes, 0) -eq 0x5a4d) "Invalid MZ header: $name"
        $offset = [BitConverter]::ToInt32($bytes, 0x3c)
        Assert ($offset -ge 64 -and $offset -le $bytes.Length - 26) "Invalid PE offset: $name"
        Assert ([BitConverter]::ToUInt32($bytes, $offset) -eq 0x00004550) "Invalid PE signature: $name"
        Assert ([BitConverter]::ToUInt16($bytes, $offset + 4) -eq 0x8664) "Not an AMD64 binary: $name"
        Assert ([BitConverter]::ToUInt16($bytes, $offset + 24) -eq 0x20b) "Not PE32+: $name"
    }
    Expand-Archive "$ReleaseDirectory/bokis-twitch-chat-plugin-$version-windows-x86_64.zip" "$temporary/extracted"
    $package = "$temporary/extracted/bokis-twitch-chat-plugin-$version-windows-x86_64"
    foreach ($name in @('README.md', 'LICENSE', 'VERSION', 'INSTALL_WINDOWS.md', 'SHA256SUMS', 'install-windows.ps1',
        'plugin/bin/64bit/bokis-twitch-chat-plugin.dll', 'plugin/bin/64bit/bokis-twitch-chat-updater.exe',
        'plugin/bin/64bit/Qt6WebSockets.dll', 'licenses/DEPENDENCIES.md')) {
        Assert (Test-Path "$package/$name") "Missing ZIP entry: $name"
    }
    Assert ((Get-Content "$package/VERSION" -Raw).Trim() -eq $version) 'ZIP version mismatch'
    New-Item -ItemType Directory -Force "$state/backups", "$state/cache", "$state/state" | Out-Null
    foreach ($part in @('backups', 'cache', 'state')) { Set-Content "$state/$part/preserve.txt" 'keep me' }
    Run-Installer $setup
    $installed = $true
    Assert (Test-Path $binary) 'Plugin missing after installation'
    Assert (Test-Path $helper) 'Helper missing after installation'
    Assert (Test-Path $uninstallKey) 'Installed Apps registration missing'
    Assert ((Get-Content "$destination/VERSION" -Raw).Trim() -eq $version) 'Installed version mismatch'
    $originalHash = (Get-FileHash $binary).Hash
    $originalHelperHash = (Get-FileHash $helper).Hash
    Set-Content "$destination/user-owned.txt" 'preserve unknown files'
    Run-Installer $setup # Upgrade/reinstall at the same destination.
    Assert ((Get-FileHash $binary).Hash -eq $originalHash) 'Reinstallation changed the payload'

    $lock = [IO.File]::Open("$state/cache/pending/update.lock", 'OpenOrCreate', 'ReadWrite', 'None')
    try { Run-Installer $setup $false } finally { $lock.Dispose() }
    Set-Content "$state/cache/pending/pending.json" '{}'
    try { Run-Installer $setup $false } finally { Remove-Item "$state/cache/pending/pending.json" }
    Set-Content "$state/cache/pending/transaction.json" '{}'
    try { Run-Installer $setup $false } finally { Remove-Item "$state/cache/pending/transaction.json" }

    # Map a real DLL at the installation target. Even a non-OBS process must block replacement.
    Copy-Item "$BuildDirectory/tests/RelWithDebInfo/updater-test-image.dll" $binary -Force
    $imageHash = (Get-FileHash $binary).Hash
    $module = [TestImageLoader]::LoadLibraryW([IO.Path]::GetFullPath($binary))
    Assert ($module -ne [IntPtr]::Zero) 'Fixture DLL did not load'
    try {
        Run-Installer $setup $false
        Assert ((Get-FileHash $binary).Hash -eq $imageHash) 'A loaded DLL was overwritten'
        Assert ((Get-FileHash $helper).Hash -eq $originalHelperHash) 'Failed preflight modified helper'
    } finally { [TestImageLoader]::FreeLibrary($module) | Out-Null }
    Run-Installer $setup
    Assert ((Get-FileHash $binary).Hash -eq $originalHash) 'Payload not restored by reinstall'
    Run-Installer "$destination/unins000.exe"
    $installed = $false
    Assert (!(Test-Path $binary) -and !(Test-Path $helper)) 'Uninstall left plugin binaries'
    Assert (!(Test-Path $uninstallKey)) 'Uninstall left Installed Apps registration'
    Assert (Test-Path "$destination/user-owned.txt") 'Uninstall removed an unknown user file'
    foreach ($part in @('backups', 'cache', 'state')) {
        Assert ((Get-Content "$state/$part/preserve.txt" -Raw).Trim() -eq 'keep me') "Uninstall modified $part"
    }
    # The same developer script supports both a verified ZIP and a local CMake build.
    & "$package/install-windows.ps1" -Destination $destination -StateRoot $state -SkipRegistration
    Assert ((Get-FileHash $binary).Hash -eq $originalHash) 'ZIP installation mismatch'
    Add-Content "$package/plugin/bin/64bit/bokis-twitch-chat-plugin.dll" 'corrupt'
    $rejected = $false
    try { & "$package/install-windows.ps1" -Destination $destination -StateRoot $state -SkipRegistration }
    catch { $rejected = $true }
    Assert $rejected 'ZIP installer accepted a corrupt payload'
    Assert ((Get-FileHash $binary).Hash -eq $originalHash) 'Corrupt ZIP changed installation'
    & "$root/scripts/install-windows.ps1" -BuildDirectory $BuildDirectory -Destination $destination -StateRoot $state -SkipRegistration
    Assert ((Get-FileHash $binary).Hash -eq $originalHash) 'Developer installation mismatch'
    foreach ($name in $environmentBefore.Keys) {
        Assert ([Environment]::GetEnvironmentVariable($name, 'User') -eq $environmentBefore[$name]) 'Smoke test changed OBS discovery'
    }
    Write-Host 'Windows packaging, install, reinstall, refusal, preservation and uninstall tests passed.'
} finally {
    if ($installed -and (Test-Path "$destination/unins000.exe")) { Run-Installer "$destination/unins000.exe" }
    Remove-Item $temporary -Recurse -Force
}
