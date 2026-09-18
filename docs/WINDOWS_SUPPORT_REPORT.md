# Windows support implementation report

Implementation is on **feat/windows-support**. VERSION remains
**0.1.0-alpha.11**. No commit, push, tag or release was created.
Linux validation passes. **The native MSVC Windows build and Windows regression
suite have not been run in this Linux workspace.** Their first CI execution is
still required before claiming Windows release readiness.

## 1. Changed and new files

The complete inventory is at the end of this report. Linux installer, packaging,
release helper, Twitch ingestion, rendering, source ID and source property keys
were not edited.

## 2. Cross-platform architecture

The existing transaction code is extracted into post-exit-shared.cpp. It retains
pending schemas 1/2, validation, backups, journal, paired replacement, rollback
and result handling. post-exit.cpp retains Linux process/filesystem calls;
post-exit-windows.cpp supplies Windows equivalents through a small internal
interface. The update checker uses platform-selected names, paths, validation,
locks and launch functions.

## 3. Windows build

The windows-x86_64 preset uses Visual Studio 2022 x64 and RelWithDebInfo.
Bootstrap verifies pinned OBS 32.2.2 source/dependencies, Qt 6.11.1, WebSockets,
QtTest source, libwebp and Inno Setup. OBS's binary Qt omits WebSockets and Test,
so these modules are built against the supplied Qt without rebuilding Core/GUI.
libwebp is static. The output includes the native plugin DLL, Qt worker, and
resource-containing helper EXE.

## 4. Installer

Inno Setup provides a per-user, no-admin GUI installer, upgrade/reinstall,
Installed Apps registration and uninstaller. It installs under AppData by
default, includes documentation/licenses, and refuses updater locks, pending
transactions and loaded/unwritable binaries. Normal uninstall preserves OBS
configuration and LocalAppData state/backups/cache.

OBS does not discover Windows AppData plugins by default. The installer registers
OBS's two supported user environment overrides, preserving conflicting custom
settings. Non-ASCII paths use an ASCII short-path alias where possible; otherwise
registration fails clearly because OBS's narrow environment reader cannot
reliably represent that path. An ASCII writable destination or existing custom
discovery is the supported alternative. Details: [installation guide](INSTALL_WINDOWS.md).

The PowerShell 7 installer supports verified ZIPs and local CMake builds,
serializes with updater locks, stages locally and rolls back ordinary failures.
It never terminates OBS.

## 5. Updater lifecycle

Check the platform manifest, verify all downloads, flush staged files and publish
pending metadata. Copy/start the helper, acknowledge readiness and wait for the
originating OBS process. After exit, re-read/reverify state, block other users of
the DLL, back up both targets, prepare same-volume candidates, replace helper
then DLL, write the result and clean pending state. Restart remains manual.

## 6. Exact process wait

OpenProcess captures the origin with SYNCHRONIZE and
PROCESS_QUERY_LIMITED_INFORMATION. The helper inherits that kernel handle plus
the PID and creation FILETIME from GetProcessTimes. It validates both values and
waits with WaitForSingleObject. A reused PID cannot change the referenced process.
CreateProcessW uses a handle whitelist and explicit argument quoting.

Process/module enumeration compares module paths and volume/file IDs, including
hardlinks. An identified live OBS candidate with unreadable modules blocks the
update; unrelated access denial does not. A kernel write-open probe adds a check
for loaded/locked target images.

## 7. Helper self-update

The installed helper is copied, flushed and hash-compared into a UUID runner
directory under LocalAppData. Only that copy executes. Its static-runtime
bootstrap extracts the embedded worker, QtCore and MSVC runtime into that private
directory and starts the worker with the same inherited handle whitelist.
The installed EXE is therefore available for replacement. No shell, service,
delete trick or automatic OBS restart is used.

## 8. Backup, rollback and journal

Both originals get persistent timestamp/UUID backups and local hardlink rollback
files. Candidates are copied beside the targets, verified and flushed. Windows
uses MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH), without cross-volume copying
inside the final rename. The helper is replaced first, DLL last.

Ordinary failures restore both originals in reverse order. Prepared journals
restore before retrying; committed journals verify installed files and complete
cleanup. Failed recovery retains evidence and reports rollbackFailed.
Two renames are not one atomic transaction. Power loss can expose a mixed pair
until recovery; Windows also lacks a portable directory-fsync equivalent.
The existing documented orphan-journal cleanup limitation remains.

## 9. Manifest

The public schema remains 1, with linux-x86_64 and windows-x86_64 entries.
Plugin/helper size and SHA-256 are generated from the actual release payloads
and checked against sidecars. Helper entries remain optional for legacy feeds.
Missing Windows entries report an unavailable platform cleanly. Windows declares
OBS minVersion 32.2.2 and maxMajorVersion 32; optional per-platform OBS constraints
are checked before offering an update.

## 10. Release artifacts

The Windows packaging script produces these names, each with a .sha256:

- bokis-twitch-chat-plugin-<VERSION>-windows-x86_64.dll
- bokis-twitch-chat-updater-<VERSION>-windows-x86_64.exe
- bokis-twitch-chat-plugin-<VERSION>-windows-x86_64.zip
- bokis-twitch-chat-plugin-setup-<VERSION>-windows-x86_64.exe

The shared update-manifest.json is generated after merging both platform outputs.
Production Windows artifacts have not been built locally. The Inno validation
artifact used dummy payloads and is not a release.

## 11. CI

Both existing workflows call the reusable windows-build.yml job on windows-2022.
It bootstraps, configures, builds, runs CTest, packages, compiles Inno, validates
the Windows manifest and runs installer smoke tests before uploading artifacts.
Linux keeps its complete Arch build/test/package flow. Publishing requires both
platform jobs and a verified combined manifest. scripts/release.sh is unchanged.

Signing insertion points are documented before payload checksums and after
installer compilation. Current builds need no secrets and are unsigned.

## 12. Tests

The native Windows suite covers Known Folders, spaces/Unicode, pending state,
PE/AMD64 validation, hashes/sizes, exact process handles/creation times, waiting
while the origin lives, parallel helper exclusion, loaded DLLs through hardlinks,
denied-process classification, self-update, backups, both swap failures, crash
journal recovery, committed cleanup recovery and results.

Shared update-checker tests run on both platforms, including old manifests,
optional helper entries and incompatible OBS versions. Python tests verify the
combined manifest and corrupt/missing artifacts.

Installer smoke tests use temporary destinations/state with discovery registration
disabled. They check ZIP/checksums/version, silent install/reinstall/uninstall,
locks/pending state/loaded DLL refusal, state and unknown-file preservation,
and ZIP/developer installers. An existing Installed Apps entry aborts the test.

## 13. Locally executed validation

- CMake configure and plugin/helper build on Linux: **passed**.
- CTest: **5/5 suites passed** — chat, update checker, Linux post-exit,
  manifest, and Linux installer.
- Existing Linux packaging plus generated Linux manifest verification: **passed**.
- Inno Setup 6.5.4 compilation under Wine with dummy payloads: **passed**.
- Silent fixture install/uninstall and pending-update refusal at a Unicode Wine
  destination: **passed**.
- PowerShell 7 parser checks for Windows scripts/tests: **passed**.
- PowerShell ZIP installer with temporary Linux fixtures: install/reinstall,
  unknown-file preservation and corrupt-payload rejection **passed**.
- actionlint 1.7.7 for all three workflows: **passed**.
- YAML/JSON parsing, shell syntax, dependency archive hashes and git diff checks:
  **passed**.
- Isolated QtTest adapter configure/build/install against local Linux Qt:
  **passed**. This verifies the adapter, not a Windows ABI/runtime.

Remaining local warnings: optional Vulkan headers are absent; OBS headers emit
anonymous-struct pedantic warnings in the test build. Wine emits graphics-driver
warnings. None failed these checks.

## 14. Known limitations

- Native Windows compilation/runtime validation remains pending.
- OBS's environment discovery imposes the Unicode/short-path restriction above.
- Supported runtime is OBS 32.2.2+ within major 32 with compatible Qt, on x64
  Windows 10 2004+/11. No ARM64-native or sandbox-specific package is included.
- Installer/binaries are unsigned; SmartScreen behavior depends on local policy.
- The updater requires writable local NTFS-style hardlink support. Network,
  cloud-synced and FAT/exFAT target installations are unsupported.
- External dependency changes require a full package installation. The updater
  replaces the DLL and self-contained helper only.
- Old runners/backups are retained. Prepared/committed recovery does not eliminate
  power-loss windows, hardware failure or possible manual repair.
- Keep OBS closed during replacement. Access controls/antivirus can deny it.
- The developer installer retains an interrupted local transaction for manual
  recovery; it does not run an automatic crash-recovery service.

## 15. Windows-runner verification still required

Run the new workflow without skipping failing tests. Confirm MSVC compilation,
Windows SDK imports/resource embedding, QtTest/WebSockets bootstrap, PE loader
behavior, inherited-handle lifetime, helper execution without developer PATH,
real Windows sharing violations, transaction recovery and the full packaging/
installer smoke test. Wine and Linux checks do not substitute for this run.
No CI run was triggered because this session must not commit or push.

## 16. Manual acceptance on a real Windows machine

1. Use a disposable standard Windows account and native OBS 32.2.2. Save existing
   scenes/settings and install a known release A. Record both installed hashes.
2. Verify setup's checksum, observe the unsigned-publisher/SmartScreen experience,
   install without elevation, and confirm Installed Apps/uninstaller registration.
3. Start OBS from a fresh session and verify discovery, source ID, existing scene
   settings, Twitch Device Flow/EventSub, Floating rendering, emotes and emoji.
4. Repeat installation under paths with spaces and a non-ASCII username. Verify
   short-path discovery, or the explicit refusal/fallback when no ASCII alias
   exists. Confirm custom environment settings are preserved.
5. Stage a trusted test release B with DLL and helper entries. Keep OBS open:
   installed hashes must stay A; one temporary runner waits and a second update
   cannot acquire the lock. Do not publish a production test release for this check.
6. Close the originating OBS. Wait for the result file before reopening. Check
   success/helperUpdated, both B hashes, both A backups, pending cleanup and the
   next startup's update message.
7. Repeat with a second OBS instance loading the same DLL. Close only the origin:
   replacement must fail safely. Close the second instance, retry, then verify
   successful installation. Unrelated protected Windows processes must not block it.
8. Repeat with a plugin-only legacy manifest, then a Linux-only manifest. Confirm
   plugin-only success and a clear unavailable-platform result respectively.
9. While OBS is still open, corrupt/truncate each staged payload in turn. Close OBS:
   verification must fail without changing either installed target.
10. In a disposable copy, exercise the regression suite's first/second swap
    failures and forced crash after helper replacement. Verify restoration/retry
    and retained evidence if recovery is deliberately made impossible.
11. Put pending state and installed files on distinct local NTFS volumes and repeat
    a paired update. Check same-volume candidates and successful backups.
12. Test setup/reinstall while OBS or the updater holds the plugin/lock. It must
    refuse and must not terminate OBS or schedule replacement at reboot.
13. Uninstall with OBS closed. Verify binaries/owned discovery are removed while
    scenes, configuration, backups, cache, state and unknown files remain.
14. Verify ZIP and local-build PowerShell installation, then repeat a normal OBS
    startup and update after reinstalling both binaries.

## File inventory

<!-- Generated from the task's Git working-tree changes. -->

| File | Status |
| --- | --- |
| [.github/workflows/ci.yml](../.github/workflows/ci.yml) | Modified |
| [.github/workflows/release.yml](../.github/workflows/release.yml) | Modified |
| [.github/workflows/windows-build.yml](../.github/workflows/windows-build.yml) | New |
| [.gitignore](../.gitignore) | Modified |
| [CMakeLists.txt](../CMakeLists.txt) | Modified |
| [CMakePresets.json](../CMakePresets.json) | Modified |
| [README.md](../README.md) | Modified |
| [buildspec.json](../buildspec.json) | Modified |
| [cmake/QtTest/CMakeLists.txt](../cmake/QtTest/CMakeLists.txt) | New |
| [cmake/WindowsHelper.cmake](../cmake/WindowsHelper.cmake) | New |
| [docs/INSTALL_WINDOWS.md](../docs/INSTALL_WINDOWS.md) | New |
| [docs/POST_EXIT_UPDATER.md](../docs/POST_EXIT_UPDATER.md) | Modified |
| [docs/WINDOWS_DEPENDENCIES.md](../docs/WINDOWS_DEPENDENCIES.md) | New |
| [docs/WINDOWS_SUPPORT_REPORT.md](../docs/WINDOWS_SUPPORT_REPORT.md) | New |
| [installer/windows.iss](../installer/windows.iss) | New |
| [scripts/bootstrap-windows.ps1](../scripts/bootstrap-windows.ps1) | New |
| [scripts/generate-manifest.py](../scripts/generate-manifest.py) | New |
| [scripts/install-windows.ps1](../scripts/install-windows.ps1) | New |
| [scripts/package-windows.ps1](../scripts/package-windows.ps1) | New |
| [scripts/windows-common.ps1](../scripts/windows-common.ps1) | New |
| [scripts/windows-dependencies.json](../scripts/windows-dependencies.json) | New |
| [src/updater/helper-bootstrap-windows.cpp](../src/updater/helper-bootstrap-windows.cpp) | New |
| [src/updater/helper-windows-main.cpp](../src/updater/helper-windows-main.cpp) | New |
| [src/updater/post-exit-internal.hpp](../src/updater/post-exit-internal.hpp) | New |
| [src/updater/post-exit-shared.cpp](../src/updater/post-exit-shared.cpp) | New |
| [src/updater/post-exit-windows.cpp](../src/updater/post-exit-windows.cpp) | New |
| [src/updater/post-exit.cpp](../src/updater/post-exit.cpp) | Modified |
| [src/updater/post-exit.hpp](../src/updater/post-exit.hpp) | Modified |
| [src/updater/update-checker.cpp](../src/updater/update-checker.cpp) | Modified |
| [src/updater/windows-platform.hpp](../src/updater/windows-platform.hpp) | New |
| [tests/CMakeLists.txt](../tests/CMakeLists.txt) | Modified |
| [tests/README.md](../tests/README.md) | Modified |
| [tests/manifest-tests.py](../tests/manifest-tests.py) | New |
| [tests/post-exit-windows-tests.cpp](../tests/post-exit-windows-tests.cpp) | New |
| [tests/updater-tests.cpp](../tests/updater-tests.cpp) | Modified |
| [tests/windows-installer-tests.ps1](../tests/windows-installer-tests.ps1) | New |
| [tests/windows-test-image.cpp](../tests/windows-test-image.cpp) | New |
