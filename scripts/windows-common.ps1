# Shared by the developer/ZIP installer. Never changes machine settings.
function Get-ObsDiscovery([string]$Destination) {
    $parent = Split-Path $Destination
    # OBS 32's getenv() paths pass through the ANSI CRT before its UTF-8 loader.
    # Use an ASCII 8.3 alias when available; never register a lossy Unicode path.
    if ($parent -match '[^\x00-\x7f]') {
        New-Item -ItemType Directory -Force $parent | Out-Null
        if (!('BokisShortPaths' -as [type])) {
            Add-Type @'
using System.Text;
using System.Runtime.InteropServices;
public static class BokisShortPaths {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)]
    public static extern uint GetShortPathNameW(string path, StringBuilder output, uint size);
}
'@
        }
        $buffer = [Text.StringBuilder]::new(32768)
        $length = [BokisShortPaths]::GetShortPathNameW($parent, $buffer, 32768)
        if (!$length -or $length -ge 32768 -or $buffer.ToString() -match '[^\x00-\x7f]') {
            throw 'OBS cannot safely discover this Unicode path. Choose an ASCII-only writable -Destination, or configure discovery yourself and use -SkipRegistration. See INSTALL_WINDOWS.md.'
        }
        $parent = $buffer.ToString()
    }
    return @{ OBS_PLUGINS_PATH = "$parent\%module%\bin\64bit"; OBS_PLUGINS_DATA_PATH = $parent }
}
function Test-ObsDiscovery([hashtable]$Discovery) {
    foreach ($name in $Discovery.Keys) {
        foreach ($scope in @('User', 'Machine')) {
            $existing = [Environment]::GetEnvironmentVariable($name, $scope)
            if ($existing -and $existing -ne $Discovery[$name]) {
                throw "Existing $scope $name conflicts with this installation. See INSTALL_WINDOWS.md; existing settings were preserved."
            }
        }
    }
}
function Register-ObsDiscovery([hashtable]$Discovery, [string]$Destination) {
    $record = Join-Path $Destination 'environment.ini'
    foreach ($name in $Discovery.Keys) {
        if (![Environment]::GetEnvironmentVariable($name, 'User')) {
            if (!(Test-Path $record)) { Set-Content $record '[Environment]' -Encoding unicode }
            Add-Content $record "$name=$($Discovery[$name])" -Encoding unicode
            [Environment]::SetEnvironmentVariable($name, $Discovery[$name], 'User')
        }
    }
}
