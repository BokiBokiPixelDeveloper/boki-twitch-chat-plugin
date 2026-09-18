# Windows installation

Use native **64-bit OBS 32.2.2 or newer within OBS 32**, on Windows 10
version 2004 or newer, or Windows 11. The release uses OBS's Qt 6.11.1
runtime. Older OBS/Qt builds, ARM64-native OBS and sandboxed installations
are not supported by this x86_64 package.

## Graphical installer

1. Download **bokis-twitch-chat-plugin-setup-<VERSION>-windows-x86_64.exe**
   and its matching **.exe.sha256** from the project's GitHub release.
   Compare the checksum with PowerShell's Get-FileHash -Algorithm SHA256.
2. Close OBS completely, including its tray icon. Run the setup EXE as your
   normal user. It requires no administrator privileges.
3. Start OBS from a fresh desktop session. If the source does not appear,
   sign out of Windows and back in so OBS receives the new environment settings.
4. Add the **Bokis Twitch Chat** source. Existing scenes, source IDs and settings
   remain compatible.

The EXE, DLL and installer are currently **unsigned**. SmartScreen may warn
about an unknown publisher. Verify the release origin and checksum before
choosing to proceed. No certificate, signing token or fake signature is included.

Default destination:

    %APPDATA%\obs-studio\plugins\bokis-twitch-chat-plugin\
      bin\64bit\bokis-twitch-chat-plugin.dll
      bin\64bit\bokis-twitch-chat-updater.exe
      bin\64bit\Qt6WebSockets.dll
      data\licenses\

The helper contains its own Qt Core worker and MSVC runtime. The plugin uses
the Qt runtime already provided by the supported OBS release; WebSockets is
included because OBS's dependency package does not provide that module.
An existing VC runtime supplied with OBS remains a plugin prerequisite.
Run the same installer to upgrade. It refuses active updater locks, pending
updates and loaded/locked binaries. It neither closes nor restarts OBS.
Do not open OBS during installation.

## Per-user plugin discovery

OBS 32.2.2 normally searches **ProgramData**, not AppData, for Windows plugins.
The installer registers these supported OBS overrides in your user environment:

| Setting | Default value |
| --- | --- |
| OBS_PLUGINS_PATH | %APPDATA%\obs-studio\plugins\%module%\bin\64bit |
| OBS_PLUGINS_DATA_PATH | %APPDATA%\obs-studio\plugins |

The actual values contain the resolved absolute path, not a literal APPDATA
variable. The %module% token is OBS's own placeholder. OBS adds the module
name to the data root; this plugin's rendering resources are embedded.
OBS's existing standard search paths are retained.

Existing conflicting user **or machine** overrides cause installation to
stop with an explanation. They are never overwritten. To use your own
discovery configuration, choose a destination covered by it and run setup
with /RegisterEnv=0 /DIR="absolute plugin directory". The destination's final
directory should be bokis-twitch-chat-plugin. Do not add multiple roots with
semicolons: OBS interprets this setting as one path pattern.

**Unicode discovery limitation in OBS:** its Windows environment reader uses
getenv, whereas its module loader expects UTF-8. Installation, state, backups
and updates use Unicode APIs, but discovery through this OBS override cannot
reliably accept every Unicode username on every Windows code page. For such
paths the installer uses an existing ASCII Windows short-path alias. If no
ASCII alias exists (for example, 8.3 names are disabled), it refuses to register
an unusable path. Choose an ASCII-only directory writable by your account with
/DIR, or supply a working custom OBS discovery configuration and
/RegisterEnv=0. It does not change your system code page, create a shared
system installation, or grant broader permissions.

This behavior follows the pinned
[OBS module-path implementation](https://github.com/obsproject/obs-studio/blob/32.2.2/frontend/widgets/OBSBasic.cpp).
Real OBS discovery, especially on non-ASCII accounts, is part of the manual
Windows acceptance test.

## ZIP installation

Download the Windows ZIP and its .zip.sha256, verify the outer checksum,
and extract it. Keep the whole package together:

    bokis-twitch-chat-plugin-<VERSION>-windows-x86_64/
      plugin/bin/64bit/
      plugin/data/licenses/
      install-windows.ps1
      windows-common.ps1
      SHA256SUMS
      README.md
      INSTALL_WINDOWS.md
      LICENSE
      VERSION
      licenses/

With **PowerShell 7**, close OBS and run .\install-windows.ps1 from the
extracted directory. It verifies every payload against SHA256SUMS and registers
the same discovery settings. -Destination "absolute path" changes the target;
-SkipRegistration preserves custom discovery. This script also preserves
unknown files and refuses pending updater transactions.

For a fully manual install, close OBS, copy the complete contents of plugin
into the destination, retain the package's documentation/licenses, and configure
the two user environment settings above through Windows Environment Variables.
Avoid copying only the DLL: the helper and WebSockets library are required.
ZIP/script installation does not add an Installed Apps entry.

## Uninstall

Close OBS and let any pending update finish. Use **Settings → Apps → Installed
apps → Bokis Twitch Chat Plugin → Uninstall**, or unins000.exe in the installation.
Uninstall removes installed binaries and only the discovery settings this
installer created, provided their values have not changed. Unknown files are
preserved. OBS scenes, OBS configuration, backups, cache and updater state
are not removed. Persistent lock files may keep otherwise empty directories present.

For ZIP/developer installations, close OBS and remove the plugin's installed
files manually. Before removing environment.ini, use it to identify settings
created by the script; remove those user environment variables only if their
current values still match. Preserve your OBS configuration.

## Automatic updates and recovery

Use **Check for updates**, then **Install update** in source properties.
Downloads are checked using HTTPS, manifest size, SHA-256 and x86_64 PE headers.
The plugin and optional helper are staged together. Keep OBS open as long as
needed; the installed files stay unchanged until that exact OBS process exits.
Close all OBS instances and wait for the result before starting OBS again.
The updater never stops or starts OBS itself.

| Purpose | Location |
| --- | --- |
| Pending payloads, pending.json, transaction.json, update.lock | %LOCALAPPDATA%\BokisTwitchChatPlugin\cache\pending |
| Temporary helper runners | %LOCALAPPDATA%\BokisTwitchChatPlugin\cache\runners |
| Timestamped plugin and helper backups | %LOCALAPPDATA%\BokisTwitchChatPlugin\backups |
| last-update-result.json | %LOCALAPPDATA%\BokisTwitchChatPlugin\state |

The next source startup reads the result into its status message. Backups and
old runner directories are retained; remove old runners only when no update
helper is running. No persistent service is installed.

Ordinary errors roll back both binaries. An interrupted transaction is recovered
on the next helper attempt. If rollbackFailed is true, keep OBS closed and
preserve the pending directory, journal and backups. Restore both binaries from
the same timestamped backup pair if manual repair is required. Never remove a
journal without first determining whether it describes a partial installation.
The graphical installer deliberately refuses unresolved update state.

Two file replacements cannot be one atomic Windows filesystem transaction.
Power loss can leave a mixed pair until journal recovery runs. The updater uses
local candidates, flushed writes, rollback hardlinks and native renames; it
requires a writable filesystem supporting hardlinks (normally local NTFS).
Cloud-synced, network or FAT/exFAT installation directories are not supported.
The download cache may be on another volume.

Automatic updates replace the plugin DLL and helper EXE. Changes to external
runtime dependencies such as QtWebSockets require the full installer/ZIP.
Release maintainers must keep this ABI stable or raise the manifest's OBS
compatibility requirement and require package reinstallation.
See [the updater design](POST_EXIT_UPDATER.md) for journal semantics.

## Developer build

Install Visual Studio 2022's Desktop development with C++ workload and a Windows
SDK >= 10.0.20348, CMake >= 3.28, Python 3, and PowerShell 7. From the repository:

    ./scripts/bootstrap-windows.ps1
    cmake --preset windows-x86_64
    cmake --build --preset windows-x86_64
    ctest --test-dir build/windows-x86_64 -C RelWithDebInfo --output-on-failure
    ./scripts/install-windows.ps1

Bootstrap downloads SHA-256-pinned OBS source/dependencies, matching Qt
WebSockets source, libwebp and Inno Setup into .deps/windows. It builds OBS's
development SDK without its UI/plugins, QtTest, QtWebSockets, and static libwebp.
It does not install OBS or modify a real OBS plugin directory.
Run it again in a new PowerShell session to restore the test runtime PATH,
or prepend .deps/windows/qt/bin and .deps/windows/prebuilt/bin yourself.
If changing dependency pins, remove .deps/windows and the Windows build tree
before rebuilding.

./scripts/package-windows.ps1 creates all release artifacts using VERSION:

- bokis-twitch-chat-plugin-<VERSION>-windows-x86_64.dll
- bokis-twitch-chat-updater-<VERSION>-windows-x86_64.exe
- bokis-twitch-chat-plugin-<VERSION>-windows-x86_64.zip
- bokis-twitch-chat-plugin-setup-<VERSION>-windows-x86_64.exe
- A .sha256 beside each artifact.

CI validates a Windows-only manifest; the release publish job creates the shared
update-manifest.json after both Linux and Windows jobs pass. Local
scripts/release.sh continues to build Linux and use the same version/tag flow.
No Windows signing secrets are required.
