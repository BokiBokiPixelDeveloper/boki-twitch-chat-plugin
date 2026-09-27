# Updater UI stabilization — 0.1.0-alpha.15

## Behavior and state

The existing `UpdateChecker` remains the only updater. Its states map directly to
the UI: Idle, Checking, Current (up to date), Available, Downloading, Ready, Error.
The same persistent status field shows failures and the previous installation
result. Ready means preparation succeeded and OBS must exit before installation;
an in-process Installing/Completed state would misrepresent the post-exit helper.
The existing persisted helper result reports success/failure on the next launch.
Version comparison, manifest selection, verification, staging, locks, helper
launch and restart behavior are unchanged.

| State | Check | Install |
| --- | --- | --- |
| Initializing UI ownership | Disabled | Disabled |
| Idle / Current | Enabled | Disabled |
| Available | Enabled | Enabled |
| Checking / Downloading | Disabled | Disabled |
| Ready (close OBS to install) | Disabled | Disabled |
| Error | Enabled | Enabled only if a retryable package remains available |

`setStatus` synchronously notifies the observer, including automatic checks and
operation starts. Both UI enablement and backend entry guards use `canCheck` and
`canInstall`. Network failures are visible in the status field; diagnostics are
also logged. Status text is HTML-escaped before display.

## Why the properties page jumped

Both button callbacks returned true, requesting OBS `RefreshProperties`. Network
completion then called `obs_source_update_properties`, requesting a reload.
OBS destroys/replaces its properties container during refresh and restores scroll
position relative to the new content size. Changing content and focus can move
the viewport. Updating `obs_property_set_enabled` or a description alone does
not repaint the existing Qt widgets.

Reference: [OBS properties-view implementation](https://github.com/obsproject/obs-studio/blob/master/shared/properties-view/properties-view.cpp),
particularly `RefreshProperties`, `AddText`, `AddGroup`, and `ButtonClicked`.

Updater callbacks now return false and updater notifications never request a
properties rebuild. `UpdateUi` updates the existing QLabel and QPushButtons.
No scroll coordinates or focus are changed.

OBS exposes no public widget handle for individual properties. The isolated Qt
adapter identifies our status QLabel using a per-source invisible HTML span ID,
then identifies the two sibling buttons by the English labels supplied by this
plugin. It observes Show events to attach to newly opened/rebuilt views. A single
initial scan covers views shown before queued initialization; there is no polling.

This adapter depends on OBS rendering an info property as a QLabel and group
buttons as sibling QPushButtons. It uses no private OBS C++ headers or ABI, but
changes to that widget layout require integration validation. Unit tests mirror
that layout; they do not establish real OBS scroll behavior.

## Thread and lifecycle review

`UpdateUi` is a source-facing handle with a mutex-protected status snapshot.
Its presenter, checker, network manager, timer and widget access all belong to the
Qt application thread. Calls from another thread are queued onto that thread;
UI button calls run directly for immediate feedback.

Queued requests capture shared state, never a ChatSource or OBS property pointer.
Source destruction marks the state closed before queuing presenter deletion.
Closed requests/notifications do nothing. QObject context ownership cancels the
automatic-check timer and network handlers when the presenter/checker dies.
The presenter is also an application child for shutdown cleanup.

Open views are held only as QPointers. Closing properties invalidates those
pointers without canceling the update. Reopening reads the latest snapshot and
reattaches on Show. An unrelated properties rebuild is handled the same way.
Multiple views of one source share its state; different sources have distinct IDs.
No asynchronous operation captures a destroyed properties object.

## Validation (Linux)

- Configure: `cmake --preset linux-x86_64 -DENABLE_TESTS=ON`.
- Full build: `cmake --build --preset linux-x86_64 -j 4`.
- Focused updater CTest: 2/2 suites passed.
- Full CTest: 12/12 suites passed (14.27 seconds).
- Updater QtTest: 23 passed; UI QtTest: 9 passed; post-exit QtTest: 39 passed.
  These counts include QtTest initialization/cleanup entries.
- ASan/UBSan: all three updater/UI/post-exit suites passed, no findings.
- `git diff --check`: passed.

Sanitizer commands:

```sh
cmake -S . -B /tmp/bokis-alpha15-updater-asan -G Ninja -DENABLE_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic'
cmake --build /tmp/bokis-alpha15-updater-asan \
  --target updater-tests update-ui-tests post-exit-tests -j 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/bokis-alpha15-updater-asan \
  -R '^(updater|update-ui|post-exit)-tests$' --output-on-failure
```

Leak detection was disabled for this run. Configuration reports the optional
missing Vulkan headers. The final build has no compiler warnings. Tests emit
expected simulated-failure diagnostics and Qt offscreen `propagateSizeHints`
warnings. Windows and real OBS interactions were not executed here.

## Manual release validation still needed

1. Scroll to Updates and check for updates. Confirm immediate busy text, disabled
   actions, unchanged focus/scroll, and usable unrelated settings.
2. Rapidly click/keyboard-activate Check and Install. Confirm one request and no
   full properties rebuild on start, failure or completion.
3. Exercise current-version, available-update and offline/error responses.
4. Start a check/download, close properties, then reopen both before and after
   completion. Confirm current state, no crash and no stale controls.
5. Trigger the automatic startup check with properties open and closed.
6. Exercise another setting that rebuilds properties while a check is active.
   Confirm updater state restores correctly afterward.
7. Open properties for different sources and confirm status isolation.
8. Install a verified release package, confirm the close-OBS instruction, exit
   fully, restart and confirm the persisted installation result.
9. Remove a source and shut down OBS during a pending request. Repeat the UI
   checks on supported Windows/Linux OBS release builds, with long error text
   and narrow properties windows to check wrapping and scroll behavior.
