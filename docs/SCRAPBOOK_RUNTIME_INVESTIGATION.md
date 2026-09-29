# Scrapbook runtime investigation — 0.1.0-alpha.18

## Evidence and conclusions

The starting tree was clean on `main` at `1ad3085`, with `VERSION` equal to
`0.1.0-alpha.18`. Work is on `fix/v4-scrapbook-runtime`. No version bump, commit,
merge, push, tag, release, or installation into the user's OBS profile was performed.

The September 28 OBS logs identify alpha.18. In `2026-09-28 21-05-28.txt`, the
package imports at 21:07:43.567 and reload begins at 21:07:59.475. Both teardown
sequences finish, and replacement browser sources are created. In
`2026-09-28 21-08-03.txt`, import/replacement completes at 21:08:34.934 immediately
before another crash. The September 29 log also confirms alpha.18 and orderly
runtime shutdown. No historical per-resource/startup milestone logs were available.

`coredumpctl` found OBS SIGSEGV dumps for PIDs 248239 and 248999. The readable
248239 has the same Qt/OBS properties-handler stack; the 248999 core was examined in GDB. Its crashing main-thread stack starts with
`QMetaObject::invokeMethodImpl`, called from the OBS properties button handler.
Disassembly resolves the invoked method string to `RefreshProperties`.

### Reload crash

`refreshWebWidget()` called `obs_source_update_properties()` synchronously inside
an OBS properties button callback. That rebuild destroys the active button's
`WidgetInfo`. The callback then returned `true`, and OBS attempted to queue
`RefreshProperties` through the destroyed handler/view state. This is properties
callback use-after-free, not a browser double release. The original core and
completed teardown logs agree with this mechanism.

Reload now returns `false` and does not rebuild properties. Import updates the
saved package setting and returns `true` for one OBS-managed deferred rebuild of
the package list. Source update applies selection changes and replaces the runtime
once. Runtime status is updated in the existing QLabel without rebuilding the
properties sheet or resetting its scroll position.

### Apparent startup hang

The displayed Loading text was a snapshot taken while properties were built.
Neither readiness nor the existing ten-second failure timeout updated that label.
It could therefore say Loading indefinitely even after the runtime became ready
or failed. The old bridge also reported ready immediately on WebSocket welcome,
before StreamElements initialization, so its internal Ready state did not prove
widget initialization either.

Before changing bootstrap/CSP, Chromium executed the actual imported Scrapbook
through the production resource server: one `onWidgetLoad`, jQuery available,
and a synthetic message row. This rules out a deterministic fixture startup loop
or universally rejected Scrapbook resource in that baseline. Historical logs do
not establish whether the original user run also had an external dependency
failure. That uncertainty is not presented as a proven CDN/CSP deadlock.

The investigation also found that missing compatibility settings defaulted to
Generic despite the UI listing Auto first, and changing package/compatibility
settings did not recreate an existing runtime. New/missing settings now default
to Auto; explicit saved Generic/StreamElements choices remain intact. Package,
compatibility, and channel changes recreate the runtime.

## Final lifecycle and ownership

The bridge tracks Starting → Running or Failed → Stopping → Stopped. A startup
failure is terminal for that runtime; a late ready frame cannot undo it. The
source stays alive and can reload. Failure reasons distinguish bridge timeout,
document initialization timeout, script/stylesheet load failure, JavaScript
initialization failure, disconnect, and missing event acknowledgement. Only
bounded categories are accepted from widget diagnostics, never exception text,
resource URLs, secrets, or viewer messages.

Reload retires the old runtime under ChatSource's existing serialization before
creating another. Startup is cancelled atomically. The owning worker closes its
subscription, stops timers, disconnects callbacks, aborts sockets, and destroys
Qt state on its own thread. The caller joins that worker, detaches the active
browser child once, and releases the child once. Queued resize calls have the
State QObject as context; HTTP timeout callbacks have the socket as context.
Tests cover early cancellation, queued resize, partial HTTP, connected bridges,
repeated shutdown, creation/attachment failures, and late startup completion.

The UI mailbox contains strings only and no runtime/source pointer. Its Qt timer
is application-owned and observes the mailbox's closed flag. Labels distinguish
Loading, Reloading, Ready, and Failed. Existing widget settings and OBS source ID
are unchanged. Native rendering remains on the native graphics path.

## Bootstrap, resources, and CSP

The wrapper installs diagnostic/document readiness support, the bridge,
StreamElements configuration, and the adapter/SE_API before original widget code.
Deferred jQuery executes before the original external widget script.
`DOMContentLoaded` and bridge welcome must both occur before `onWidgetLoad` is
dispatched. The adapter completes imported-widget readiness after dispatch;
rejected async initialization handlers get a turn to report failure first.
Generic imports use document readiness without requiring original code to call
BokiChat. This is document/dispatch readiness, not a promise that every arbitrary
third-party background task has completed.

Scrapbook's HTML is a `.main-container` fragment. Its script registers
`onEventReceived` and an async `onWidgetLoad` handler. That handler reads saved
field values, channel/currency, creates font style elements using jQuery, and
sets message alignment. It has no startup polling loop or awaited SE service.
Synthetic chat invokes its original message creation code. Google font imports
are authored by the fixture's dynamically created style text; successful font
fetch/rendering was not independently established. No dependency was vendored.

The baseline browser capture found a CSP violation in jQuery `parseHTML` when it
sets a same-origin base URL. It was nonfatal for this fixture. Imported widgets
now have an explicit trusted-code policy: HTTPS scripts/styles and inline
scripts/styles, same-origin base URLs, local/HTTPS fonts, and HTTPS/data/blob
images. Development keeps its strict self-only script/style policy. Eval,
frames, objects, and form submission remain disallowed. Widget trust is still
required before import. No Twitch credentials or arbitrary filesystem bridge
is provided.

The wrapper's base URL points to the original HTML directory. Runtime script
URLs are absolute under the per-instance capability; the bridge resolves
bootstrap relative to its own script URL. Package paths are percent-encoded on
output and decoded exactly once on input; queries do not become manifest
filenames. Malformed encoding, invalid UTF-8, encoded path separators, NUL,
backslash, and traversal remain rejected. MIME handling includes fonts and JSON,
and extension checks are case-insensitive. Manifest membership stays
case-sensitive. Directory listing/implicit index lookup is not provided; explicit
manifest file paths are required. Browser fragments are not sent in HTTP requests.

Canonical containment, symlink exclusion, file size, manifest membership, and
SHA-256 checks remain in place. The original ZIP and installed originals remain
unchanged. Field expansion happens only on served copies. The test's intentional
integrity mutation is confined to its disposable imported copy and restored.

## Validation and limitations

CMake configuration and full Linux build pass without compiler warnings.
The complete 16-suite CTest run passes with `RUN_WIDGET_BROWSER_TESTS=1`:
package/archive security, adapter/bootstrap, web runtime/events, synthetic events,
native chat/layout, Twitch producer, updater/installer, and status UI tests.
The optional browser cases include Generic, Development, Scrapbook, blocked
jQuery, and successful replacement after failure. The adapter test has seven
passing assertions/tests, including terminal bootstrap failures.

The full isolated OBS CEF test reaches Ready at six checkpoints, displays the
actual Scrapbook synthetic message in a captured source screenshot, performs
immediate repeated reload, switches Native/Web, runs two widget instances,
deletes sources, and exits with status 0. It uses OBS 32.2.2 and CEF 151 on this
machine. The screenshot and log are local ignored build artifacts under
`build/scrapbook-smoke-final/`.

An initial fresh-profile OBS run stalled in Chromium's Additional Terms of
Service first-run dialog, before requesting a widget document. Window metadata
and a debugger-child run identified that test-environment issue. The successful
smoke runner uses Chromium's standard `--no-first-run` automation option; it does
not accept terms or modify the user's profile. CEF's remote debugging endpoint
was not available in this installation, so visual confirmation uses OBS's own
source screenshot. A first screenshot with empty account settings contained no
events: the shared backend had not accepted that source. Dummy identifiers in
the isolated test activate synthetic routing without credentials.

OBS reports unrelated missing DeckLink/v4l2loopback components and portal/frontend
shutdown warnings. It also reports one outstanding OBS allocation at exit; this
counter is not a stack trace and is not attributed to the widget without evidence.
See the final sanitizer results below. No historical or test browser failure is
claimed fixed merely because a status label changed.

No manual debugging action required before the next build test.

Final sanitizer command:

```sh
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
RUN_WIDGET_BROWSER_TESTS=1 ctest --test-dir build/v4-asan --output-on-failure -j 4
```

Result: 16/16 suites passed, including browser execution, with no ASan, UBSan,
or LeakSanitizer reports. The first sanitizer pass used `detect_leaks=0`; the
final pass above explicitly enabled it. OBS's own allocation counter is a
separate remaining observation. `git diff --check` passes.

The OBS browser ownership audit also consulted upstream
[BrowserSource lifecycle code](https://github.com/obsproject/obs-browser/blob/master/obs-browser-source.cpp)
and [libobs source activation](https://github.com/obsproject/obs-studio/blob/master/libobs/obs-source.c).
Browser creation/rendering remains within the existing OBS child-source boundary.

## Files changed

- `CMakeLists.txt`
- `src/plugin-main.cpp`
- `src/renderer/chat-source.cpp`
- `src/renderer/chat-source.hpp`
- `src/web/web-widget-runtime.cpp`
- `src/web/widget-resource-request.cpp`
- `src/web/widget-status-ui.cpp` (new)
- `src/web/widget-status-ui.hpp` (new)
- `resources/web-runtime/imported-bootstrap.js` (new)
- `resources/web-runtime/streamelements-adapter.js`
- `resources/web-themes/development/bridge.js`
- `resources/web-widget.qrc`
- `tests/CMakeLists.txt`
- `tests/README.md`
- `tests/streamelements-adapter-tests.js`
- `tests/web-event-tests.cpp`
- `tests/web-runtime-tests.cpp`
- `tests/widget-browser-probe.js` (new)
- `tests/widget-status-ui-tests.cpp` (new)
- `tests/obs-widget-smoke.lua` (new)
- `tests/obs-widget-smoke.py` (new)
- `docs/SCRAPBOOK_RUNTIME_INVESTIGATION.md` (new)
