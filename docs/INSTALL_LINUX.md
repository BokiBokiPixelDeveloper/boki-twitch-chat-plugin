# Install on Linux

This package is for **native OBS Studio 32.x on Linux x86_64**. Releases are built
on rolling Arch Linux with Qt 6.9+ and libwebp. They require compatible system
libraries; a successful dependency check does not guarantee compatibility with
every distribution or custom OBS build. Flatpak, Snap, portable OBS and system-wide
installation are not supported by this installer. No administrator access is needed.

## First installation

1. Download the `bokis-twitch-chat-plugin-<version>-linux-x86_64.zip` asset and its
   matching `.zip.sha256` file from the project's GitHub Release. Use the release
   ZIP, not GitHub's **Source code** download or the standalone updater binaries.
2. In the download directory, verify the ZIP (substitute the downloaded version):

   ```bash
   sha256sum --check bokis-twitch-chat-plugin-<version>-linux-x86_64.zip.sha256
   ```

3. Extract the ZIP, open a terminal in its extracted folder, and close OBS completely.
4. Run:

   ```bash
   bash install.sh
   ```

5. Start OBS and add **Bokis Twitch Chat Plugin** as a source. Connect your Twitch account
   from the source properties.

Do not use `sudo`. To check the package and runtime without installing anything:

```bash
bash install.sh --check
```

The installer verifies every installed file against the included `SHA256SUMS`,
checks Linux/CPU and native OBS version, and uses `ldd` to detect missing libraries
or symbol versions in both binaries. It requires Bash, GNU coreutils, `flock`
(util-linux), `pgrep` (procps/procps-ng), and `ldd` (glibc). These are normally
available on a native Arch installation. If dependencies are unavailable, use a
build for your environment; the installer does not install system dependencies.
Checksums detect corruption; publisher authenticity relies on downloading the
archive and checksum from the trusted HTTPS release page. These are not signatures.

## Location and updates

The default location is:

```text
~/.config/obs-studio/plugins/bokis-twitch-chat-plugin/
├── bin/64bit/bokis-twitch-chat-plugin.so
├── bin/64bit/bokis-twitch-chat-updater
├── data/licenses/NotoColorEmoji-OFL.txt
└── uninstall.sh
```

An absolute `XDG_CONFIG_HOME` replaces `~/.config`. Relative XDG paths fall back
to the defaults. The bundled emoji font is embedded in the plugin binary.
OBS scenes, source settings and Twitch credentials are left in place.

After this first installation, use **Updates → Install update** in the source
properties. Fully close OBS and wait for the updater to finish before restarting.
Alternatively, extract a newer release and run its `install.sh` to replace the
complete package. Re-running an installer replaces the binaries with that archive's
version, even if the archive is older than the installed version.

The installer shares the updater's persistent locks. It refuses to proceed while
OBS or an updater is active, or while pending update metadata/journal exists. Finish
or recover that update first; the installer never silently discards it. An absolute
`XDG_CACHE_HOME` replaces the default `~/.cache` for the updater lock/state directory.
Keep OBS closed throughout installation; process checks cannot prevent a new OBS
process from starting between checks. Do not use this installer to overwrite
package-manager-owned files.

## Uninstall

Close OBS, then run the installed uninstaller:

```bash
bash "${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/bokis-twitch-chat-plugin/uninstall.sh"
```

Alternatively, run `bash install.sh --uninstall` from the extracted release.
Uninstallation does not require OBS or the release binaries to remain available.
It removes only the known plugin files. It preserves OBS settings, credentials,
updater backups/cache/state, unknown files and the persistent lock files. Empty
directories may remain. Use the same XDG environment used for installation.

## Interrupted installation

New files are staged and verified before any installed file is changed. Existing
files are hardlinked into a local rollback directory; each replacement uses a
rename. Ordinary errors and handled signals restore changed files automatically.
This is not a power-loss-safe transaction across all files.

If the process is killed with SIGKILL, power is lost, or rollback fails, keep OBS
closed. A remaining `.installer-transaction` directory inside the plugin directory
blocks another install/uninstall. Its `changed` file lists files whose replacement
or removal was attempted; `old/` contains the original versions of files that
existed before installation. With no OBS/updater running, restore each listed file
from `old/` to the same relative plugin path; if that file has no `old/` copy,
remove the newly installed file. Leave unlisted files untouched. After checking
the restored installation, remove only `.installer-transaction` and rerun the
installer. Never remove the persistent `.use.lock` or `update.lock` files.

For a pending built-in update, follow `docs/POST_EXIT_UPDATER.md` in the project
repository instead; its recovery journal is separate from this installer.
