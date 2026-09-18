[CmdletBinding()]
param(
    [string]$BuildDirectory = "$PSScriptRoot/../build/windows-x86_64",
    [string]$OutputDirectory = "$PSScriptRoot/../release",
    [string]$Configuration = 'RelWithDebInfo',
    [string]$Iscc = "$PSScriptRoot/../.deps/windows/inno/ISCC.exe"
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot
$version = (Get-Content "$root/VERSION" -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$') { throw 'Invalid VERSION' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$package = "$OutputDirectory/bokis-twitch-chat-plugin-$version-windows-x86_64"
if (Test-Path $package) { Remove-Item $package -Recurse -Force }
New-Item -ItemType Directory -Force "$package/plugin", "$package/licenses" | Out-Null
& cmake --install $BuildDirectory --config $Configuration --prefix "$package/plugin"
if ($LASTEXITCODE -ne 0) { throw 'CMake installation failed' }
Copy-Item "$root/README.md", "$root/LICENSE", "$root/VERSION", "$root/docs/INSTALL_WINDOWS.md" $package
Copy-Item "$root/resources/fonts/OFL.txt" "$package/licenses/NotoColorEmoji-OFL.txt"
Copy-Item "$root/.deps/windows/licenses/*" "$package/licenses" -Recurse -Force
Copy-Item "$root/docs/WINDOWS_DEPENDENCIES.md" "$package/licenses/DEPENDENCIES.md"
Copy-Item "$package/licenses/*" "$package/plugin/data/licenses" -Recurse -Force
Copy-Item "$PSScriptRoot/install-windows.ps1", "$PSScriptRoot/windows-common.ps1" $package
foreach ($file in @('bokis-twitch-chat-plugin.dll','bokis-twitch-chat-updater.exe','Qt6WebSockets.dll')) {
    if (!(Test-Path "$package/plugin/bin/64bit/$file")) { throw "Missing package file: $file" }
}
# Future Authenticode signing belongs here, before checksums/ZIP/installer.
Copy-Item "$package/plugin/bin/64bit/bokis-twitch-chat-plugin.dll" "$OutputDirectory/bokis-twitch-chat-plugin-$version-windows-x86_64.dll"
Copy-Item "$package/plugin/bin/64bit/bokis-twitch-chat-updater.exe" "$OutputDirectory/bokis-twitch-chat-updater-$version-windows-x86_64.exe"
$records = foreach ($file in Get-ChildItem "$package/plugin" -File -Recurse | Sort-Object FullName) {
    $relative = [IO.Path]::GetRelativePath("$package/plugin", $file.FullName).Replace('\','/')
    "$((Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $relative"
}
[IO.File]::WriteAllLines("$package/SHA256SUMS", [string[]]$records, [Text.UTF8Encoding]::new($false))
Compress-Archive -Path $package -DestinationPath "$package.zip" -Force
& $Iscc "/DAppVersion=$version" "/DPackageDir=$package" "/DOutputDir=$OutputDirectory" "$root/installer/windows.iss"
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed' }
# A future installer signature must be applied before this checksum loop.
$names = @("bokis-twitch-chat-plugin-$version-windows-x86_64.dll",
    "bokis-twitch-chat-updater-$version-windows-x86_64.exe",
    "bokis-twitch-chat-plugin-$version-windows-x86_64.zip",
    "bokis-twitch-chat-plugin-setup-$version-windows-x86_64.exe")
foreach ($name in $names) {
    $file = Join-Path $OutputDirectory $name
    if (!(Test-Path $file)) { throw "Missing artifact: $name" }
    $hash = (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant()
    [IO.File]::WriteAllText("$file.sha256", "$hash  $name" + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
}
Write-Host "Windows release payload: $OutputDirectory"
