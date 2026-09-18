[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot
$deps = Join-Path $root '.deps/windows'
$pins = Get-Content "$PSScriptRoot/windows-dependencies.json" -Raw | ConvertFrom-Json
$spec = Get-Content "$root/buildspec.json" -Raw | ConvertFrom-Json
if ($pins.obsSource.url -notlike "*/$($spec.dependencies.'obs-studio'.version)") {
    throw 'OBS source pin and buildspec.json disagree'
}
New-Item -ItemType Directory -Force "$deps/downloads", "$deps/licenses" | Out-Null
function Invoke-Build([string[]]$Arguments) {
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) { throw "CMake failed: $Arguments" }
}
function Get-Verified([string]$Name, [string]$Extension) {
    $pin = $pins.$Name
    $file = "$deps/downloads/$Name.$Extension"
    if (!(Test-Path $file)) { Invoke-WebRequest $pin.url -OutFile $file }
    if ((Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.sha256) {
        throw "Checksum mismatch: $file. Remove this download and retry."
    }
    return $file
}
foreach ($item in @(@('obsSource', 'source'), @('obsDeps', 'prebuilt'), @('qt', 'qt'),
    @('qtwebsockets', 'qtwebsockets-source'), @('qtbase', 'qtbase-source'))) {
    $archive = Get-Verified $item[0] 'zip'
    $target = "$deps/$($item[1])"
    $expected = $pins.($item[0]).sha256
    if ((Test-Path "$target/.extracted") -and (Get-Content "$target/.extracted" -Raw).Trim() -ne $expected) {
        throw "Dependency pins changed. Remove .deps/windows and bootstrap again."
    }
    if (!(Test-Path "$target/.extracted")) {
        New-Item -ItemType Directory -Force $target | Out-Null
        Expand-Archive $archive -DestinationPath $target -Force
        Set-Content "$target/.extracted" $pins.($item[0]).sha256
    }
}
# OBS's Qt distribution omits WebSockets. Build the matching upstream module,
# dynamically linked to exactly the Qt used by the supported OBS release.
$env:PATH = "$deps/qt/bin;$deps/prebuilt/bin;" + $env:PATH
Invoke-Build @('-S', "$root/cmake/QtTest", '-B', "$deps/qttest-build",
    '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DCMAKE_PREFIX_PATH=$deps/qt",
    "-DQT_BASE_SOURCE=$deps/qtbase-source/qtbase-6.11.1", "-DCMAKE_INSTALL_PREFIX=$deps/qt",
    '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_EXAMPLES=OFF', '-DCMAKE_CONFIGURATION_TYPES=RelWithDebInfo')
Invoke-Build @('--build', "$deps/qttest-build", '--config', 'RelWithDebInfo', '--parallel')
Invoke-Build @('--install', "$deps/qttest-build", '--config', 'RelWithDebInfo')
Invoke-Build @('-S', "$deps/qtwebsockets-source/qtwebsockets-6.11.1", '-B', "$deps/qtwebsockets-build",
    '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DCMAKE_PREFIX_PATH=$deps/qt",
    "-DCMAKE_INSTALL_PREFIX=$deps/qt", '-DQT_BUILD_TESTS=OFF', '-DQT_BUILD_EXAMPLES=OFF',
    '-DQT_BUILD_EXAMPLES_BY_DEFAULT=OFF', '-DCMAKE_CONFIGURATION_TYPES=RelWithDebInfo')
Invoke-Build @('--build', "$deps/qtwebsockets-build", '--config', 'RelWithDebInfo', '--parallel')
Invoke-Build @('--install', "$deps/qtwebsockets-build", '--config', 'RelWithDebInfo')
# The OBS dependency ZIPs have a flat prefix containing lib/cmake and bin.
$obs = "$deps/source/obs-studio-$($spec.dependencies.'obs-studio'.version)"
$prefixes = "$deps/prebuilt;$deps/qt"
Invoke-Build @('-S', $obs, '-B', "$deps/obs-build", '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-DENABLE_FRONTEND=OFF', '-DENABLE_PLUGINS=OFF', '-DENABLE_BROWSER=OFF', '-DENABLE_SCRIPTING=OFF',
    "-DOBS_VERSION_OVERRIDE=$($spec.dependencies.'obs-studio'.version)", "-DCMAKE_PREFIX_PATH=$prefixes")
Invoke-Build @('--build', "$deps/obs-build", '--config', 'RelWithDebInfo', '--target', 'obs-frontend-api', '--parallel')
Invoke-Build @('--install', "$deps/obs-build", '--config', 'RelWithDebInfo', '--component', 'Development', '--prefix', "$deps/obs")

$webpArchive = Get-Verified 'webp' 'tar.gz'
New-Item -ItemType Directory -Force "$deps/webp-source" | Out-Null
& tar -xf $webpArchive -C "$deps/webp-source"
if ($LASTEXITCODE -ne 0) { throw 'libwebp extraction failed' }
Invoke-Build @('-S', "$deps/webp-source/libwebp-1.6.0", '-B', "$deps/webp-build", '-G', 'Visual Studio 17 2022', '-A', 'x64',
    '-DBUILD_SHARED_LIBS=OFF', '-DWEBP_BUILD_ANIM_UTILS=OFF', '-DWEBP_BUILD_CWEBP=OFF', '-DWEBP_BUILD_DWEBP=OFF',
    '-DWEBP_BUILD_GIF2WEBP=OFF', '-DWEBP_BUILD_IMG2WEBP=OFF', '-DWEBP_BUILD_VWEBP=OFF', '-DWEBP_BUILD_WEBPINFO=OFF',
    '-DWEBP_BUILD_WEBPMUX=OFF', '-DWEBP_BUILD_EXTRAS=OFF', "-DCMAKE_INSTALL_PREFIX=$deps/webp")
Invoke-Build @('--build', "$deps/webp-build", '--config', 'RelWithDebInfo', '--parallel')
Invoke-Build @('--install', "$deps/webp-build", '--config', 'RelWithDebInfo')
Copy-Item "$deps/webp-source/libwebp-1.6.0/COPYING" "$deps/licenses/libwebp-COPYING.txt"
# Ship the actual Qt license files from the pinned distribution.
$qtLicenses = @(Get-ChildItem "$deps/qt" -Directory -Recurse | Where-Object { $_.Name -eq 'licenses' })
if (!$qtLicenses.Count) { throw 'Qt distribution is missing its licenses directory' }
Copy-Item $qtLicenses[0].FullName "$deps/licenses/Qt" -Recurse -Force
Copy-Item "$deps/qtbase-source/qtbase-6.11.1/LICENSES" "$deps/licenses/QtBase" -Recurse -Force
Copy-Item "$deps/qtwebsockets-source/qtwebsockets-6.11.1/LICENSES" "$deps/licenses/QtWebSockets" -Recurse -Force

$inno = Get-Verified 'inno' 'exe'
$installed = Start-Process -FilePath $inno -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
    '/CURRENTUSER', ('/DIR="' + "$deps/inno" + '"')) -Wait -PassThru
if ($installed.ExitCode -ne 0 -or !(Test-Path "$deps/inno/ISCC.exe")) { throw 'Inno Setup installation failed' }
$runtime = @("$deps/qt/bin", "$deps/prebuilt/bin",
    "$deps/obs-build/rundir/RelWithDebInfo/bin/64bit")
$env:PATH = ($runtime -join ';') + ';' + $env:PATH
if ($env:GITHUB_PATH) {
    $runtime | Out-File $env:GITHUB_PATH -Append -Encoding utf8
}
Write-Host "Dependencies ready in $deps"
