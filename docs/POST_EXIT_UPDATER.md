# Linux post-exit updater

## Installation and packaging

Build with `cmake --preset linux-x86_64 -DENABLE_TESTS=ON`, then
`cmake --build --preset linux-x86_64`. Run
`ctest --test-dir build/linux-x86_64 --output-on-failure`.
The helper uses Qt Core for JSON, file handling, and SHA-256, plus POSIX/Linux APIs.
It links neither OBS nor Qt GUI/Network. This reuses the plugin's existing dependency
rather than adding a JSON or crypto dependency. CMake installs both binaries into
`lib/obs-plugins`; the existing package and user-install scripts put them together:

```text
$XDG_CONFIG_HOME/obs-studio/plugins/bokis-twitch-chat-plugin/
└── bin/64bit/
    ├── bokis-twitch-chat-plugin.so
    └── bokis-twitch-chat-updater
```

`XDG_CONFIG_HOME` defaults to `~/.config`. The ZIP contains:

```text
bokis-twitch-chat-plugin-<VERSION>-linux-x86_64/
├── plugin/bin/64bit/bokis-twitch-chat-plugin.so
├── plugin/bin/64bit/bokis-twitch-chat-updater
├── VERSION
├── README.md
└── licenses/NotoColorEmoji-OFL.txt
```

The release workflow runs tests before packaging. In addition to the existing
standalone `.so`, packaging exports `bokis-twitch-chat-updater-<VERSION>-linux-x86_64.bin`
and its SHA-256 file. The ZIP still installs the helper under its unversioned name.

### Optional public manifest entry (schema 1)

```json
{
  "schema": 1,
  "version": "B",
  "platforms": {
    "linux-x86_64": {
      "url": "https://example.org/plugin.so",
      "sha256": "<plugin SHA-256>",
      "size": 123456,
      "helper": {
        "url": "https://example.org/updater.bin",
        "sha256": "<helper SHA-256>",
        "size": 23456
      }
    }
  }
}
```

`helper` is optional: omitting it preserves plugin-only updates. When present, all
three helper fields are mandatory (HTTPS URL, 64 hex SHA-256 characters, positive
integer size). Invalid helper objects reject the entire manifest; they never
silently downgrade to plugin-only updates. The release workflow emits this entry
and uploads the standalone helper. Existing manifest readers can ignore it.

The installed helper must understand local pending schema 2 before it can process
a paired update. Deploy this implementation once via the complete package if the
installed helper only understands schema 1. Old helpers explicitly reject schema 2,
which prevents them from silently installing only half of the pending update.
No version number is changed by this feature.

## Lifecycle and process identity

1. A nonblocking exclusive `flock` on `pending/update.lock` serializes downloads,
   staging, and the complete helper lifetime across sources and processes.
2. The asynchronous network download verifies mandatory manifest size and SHA-256,
   plus ELF magic. With a helper entry, it downloads and verifies the helper next,
   retaining the same lock. Neither pending metadata nor a helper process is created
   until **both** payloads have passed verification. A failed helper download cannot
   start a plugin-only installation.
3. The payloads are saved and fsynced. `pending.json` is published last as their
   common commit marker, then its directory is fsynced. No installed file is touched.
4. OBS opens a `pidfd` for **itself**, before forking. The helper inherits this exact
   process handle and the same lock file description. PID and `/proc/PID/stat`
   field 22 (start ticks) are also passed as separate arguments.
5. `fork` / `setsid` / second `fork` / `execv` detach the currently installed helper.
   A close-on-exec pipe reports exec errors; the intermediate child is reaped.
   No shell is used. Standard streams go to `/dev/null`, the working directory
   becomes `/`, and unrelated descriptors are marked close-on-exec.
6. The helper blocks on `poll(pidfd, POLLIN)`. If pidfd is unavailable or denied,
   it checks PID **and start ticks** every 200 ms. PID reuse proves the original
   process exited; unreadable procfs without ESRCH fails closed. OBS is never killed.
7. After exit, acquire the exclusive `.so.use.lock` (held shared by cooperating
   plugin processes), reread metadata, and verify **all** payloads again. Scan
   `/proc/*/maps` initially and immediately before each swap. Compare device and
   inode to the target `.so`, including aliases/hardlinks, rather than process names.
   Any matching mapping aborts with the PID in the error. Unreadable maps for a live
   same-user process abort conservatively; inaccessible other-user processes are skipped.
8. Prepare **all** backups, candidates, and local rollback hardlinks before any swap.
   Backups get unique UTC timestamp/UUID names under the XDG data directory.
   Candidates are copied beside each target, chmod 0755, fsynced, and reverified.
   Backup and target directories are fsynced. Cache may reside on another filesystem.
9. Persist `transaction.json` with phase `prepared`. Rename the helper candidate
   over the installed helper first, then the plugin candidate over the `.so`.
   Fsync the target directory after each swap. The running helper continues from
   its original image; it never truncates or writes its executing inode.
10. Persist phase `committed` only after every planned swap succeeds. Write a result
    with `success: true` only for a committed transaction. Clean pending files and
    metadata, journal, and local rollback links; retain the XDG backups.

The installed target is resolved from the loaded module using `dladdr` and its
canonical path. The helper is resolved beside it. Pending state for a different
installation is refused. Missing helpers leave the verified pending state intact;
checking again or creating a source on the next OBS start retries launching it.
Legacy staged binaries without metadata are not installed automatically. Persistent
lock files are never unlinked, which prevents competing locks on different inodes.

## Local state schema

Absolute XDG variables are honored; absent/relative variables use HOME defaults.

| File | Default location |
| --- | --- |
| Binary and `pending.json`, `update.lock` | `~/.cache/bokis-twitch-chat-plugin/pending/` |
| Backups | `~/.local/share/bokis-twitch-chat-plugin/backups/` |
| `last-update-result.json` | `~/.local/state/bokis-twitch-chat-plugin/` |

`pending.json` remains private schema 1 for plugin-only updates:

```json
{
  "schema": 1,
  "fromVersion": "A",
  "version": "B",
  "sha256": "64 hex characters",
  "size": 123456,
  "binary": "/absolute/cache/pending/bokis-twitch-chat-plugin.so",
  "target": "/absolute/installation/bokis-twitch-chat-plugin.so"
}
```

For a paired update, its schema is **2**, with the same plugin fields plus:

```json
{
  "helper": {
    "sha256": "64 hex characters",
    "size": 23456,
    "binary": "/absolute/cache/pending/bokis-twitch-chat-updater",
    "target": "/absolute/installation/bokis-twitch-chat-updater"
  }
}
```

The helper target must be the fixed filename beside the plugin target. Both
pending filenames are fixed; manifest URLs never determine installation paths.

The result contains `success`, `fromVersion`, `toVersion`, UTC ISO `timestamp`,
`error`, `helperUpdated`, and `rollbackFailed`. The first source to read it removes
the result file and retains the message alongside subsequent automatic checks.
A rollback failure explicitly requests manual recovery; the UI does not claim
that the old versions were retained in that case.

## Failure, rollback, and interruption semantics

Verification/preparation failures leave both installed files untouched. If a swap
or its fsync fails, restore original inodes with atomic renames from local rollback
hardlinks, in reverse order. These links are in the destination filesystem, so
rollback does not need to copy a backup across filesystems or overwrite an active
executable. Persistent XDG backups of **both** files are also available.

The journal records the transaction ID, target paths and hashes, release version,
and phase. On the next helper attempt (after OBS exits), a `prepared` transaction
is restored before retrying. A `committed` transaction verifies both installed
files and finishes pending cleanup without reinstalling or overwriting backups.
Malformed/mismatched journals are retained and installation aborts for diagnosis.

Two POSIX path renames are **not** a single atomic filesystem transaction. Ordinary
reported swap errors trigger rollback, but SIGKILL/power loss can interrupt the
interval between them. The durable journal enables subsequent recovery; it cannot
prevent another program from observing that intermediate state. Do not restart OBS
until a result is present. If the filesystem also prevents rollback, the helper
reports `rollbackFailed: true`, preserves recovery state and backups, and requires
manual repair with OBS fully closed. No implementation can guarantee successful
rollback on a failing or newly read-only filesystem.

Interrupted final cleanup may leave orphaned local rollback/candidate files. If
metadata has already been removed but a committed journal remains, retain the
backups and remove that orphaned journal only after verifying both installed files
against its recorded hashes. This situation blocks a later mismatched transaction
rather than silently discarding recovery evidence. Backups are not automatically
pruned. No automatic rollback based on OBS startup health is implemented.

## Limits

- Linux only; requires writable user installation, procfs for fallback, and Qt Core.
  No sudo, daemon, systemd unit, OBS termination, or automatic restart.
- Close all OBS instances and wait for the result before restarting. The use-lock
  protects cooperating instances after module initialization, and the maps scan
  also catches older/non-cooperating instances. OBS's loader does not participate
  in this lock, so a new mapping can still race the final check. Process namespaces,
  procfs restrictions, and inaccessible other-user processes limit visibility. The originating OBS
  process is always awaited, even if its source is deleted early.
- Session managers that kill every user process at logout can also kill the helper.
  Power loss between rename and state cleanup can leave a pending update for retry.
- ELF magic and hash checks do not prove ABI compatibility. Hash authenticity relies
  on the existing HTTPS release/manifest trust model; no signature scheme is added.
- One pending update per user. Local metadata is user-owned, not a privilege boundary.
  Root/system installations and separately sandboxed OBS packages may need manual installation.
- Size and hashing retain the existing in-memory download behavior.

## Manual Linux/OBS test

1. Fully close OBS. Build and run `./scripts/install-user.sh` to install **both**
   binaries. Keep a known working backup. Do not install over a running OBS process.
2. Start OBS with a chat source and select a newer existing test release in the
   configured feed. Click **Nach Updates suchen**, then **Update installieren**.
3. Confirm the full-close message, both pending files, and one helper process.
   Compare `sha256sum` of the installed `.so` before/after staging: it must match.
   Repeated buttons/another source must not start a second download/helper.
4. If the manifest includes a helper, verify both pending payload hashes against
   its entries and ensure a single helper waits for OBS. Fully exit OBS. Wait until `$XDG_STATE_HOME/bokis-twitch-chat-plugin/last-update-result.json`
   appears (default `~/.local/state/...`). Confirm `success: true`, `helperUpdated`
   matching the manifest, backups matching each original, both new target hashes,
   mode 0755, and removal of pending metadata and payloads.
5. Start OBS. Confirm version B and the success message; verify chat and renderer
   behavior and preserved settings.
6. In a disposable installation, stage again and alter one byte of the pending
   binary while OBS is still open. Close OBS. Expect a failure result and unchanged
   installed hash. Repeat with truncated/missing pending binary, or an unwritable
   backup directory. Repeat with a corrupted helper payload: neither target may
   change. Restore/remove invalid pending state before retrying; do not remove an
   interrupted transaction journal without checking its recovery state.
7. Repeat with XDG cache and installation on different filesystems. Repeat with a
   second cooperating OBS instance kept open: the helper must refuse replacement.

Automated tests use temporary directories, injected wait decisions for failure
cases, deterministic rename failures for rollback, journal recovery fixtures, a
separate process mapping the plugin through a hardlink, and a short-lived test
process for the real detached lifecycle including replacement of the executing
helper itself. No actual
OBS process or installation is modified by the tests.
