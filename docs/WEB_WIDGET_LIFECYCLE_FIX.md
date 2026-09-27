# V3 Web Widget lifecycle regression fix

Scope: the 0.1.0-alpha.15 lineage. No V4 work, source ID/settings migration,
release, or change to the native renderer architecture.

## Concrete defects

The previous `State` destructor called `socket->close()` and then dereferenced
`socket` again. Its context-less `disconnected` callback captured `State`, cleared
that same pointer, and modified `status`/`pendingDelivery`. A synchronous disconnect
could therefore cause a null dereference. A later disconnect during server/child
destruction could access already-destroyed members: those strings were declared
after the server and were destroyed first. Queued deletion did not cancel these
context-less callbacks. This is a concrete refresh/source teardown defect; no
crash dump was available to identify which path the reported OBS crash took.

`ChatSource` also synchronously invoked runtime construction/destruction on the Qt
application thread. During source destruction OBS can wait for the destruction
thread while that thread waits for the UI invocation. `closingDown()` does not
establish whether the UI can service the call. The fallback also deleted Qt state
on the calling thread. Construction under the source mutex could likewise wait
for UI work that reentered the source. These are concrete deadlock/thread-affinity
hazards, independent of the last socket warning in the log.

Status strings and dimensions were also shared between Qt callbacks and OBS
methods without common synchronization. HTTP clients were retained as server
children after disconnect until the entire runtime was destroyed.

## Ownership and teardown

The OBS facade is now a plain C++ object. ChatSource's existing source mutex
serializes creation, refresh, mode changes, resize, enumeration and rendering.
OBS source destruction is the terminal owner path. Browser operations remain on
the OBS-facing side, not on network callbacks. Enumeration takes its own source
reference; refresh moves the owned runtime out before detachment so reentrant
child enumeration cannot see a retiring child.

Each runtime has a private Qt bridge thread. HTTP/WebSocket servers, clients,
timers and the subscription are created, serviced, stopped and destroyed there.
The QThread control object has no affinity/event handlers and may be retired by
an OBS thread after joining. Status reads use a mutex; resize uses context-bound
queued delivery. There are no UI-thread invocations in this lifecycle.

Shutdown is idempotent:

1. Set the facade shutdown flag and atomic bridge stop flag; stop new delivery.
2. Quit the owned event loop. On its thread, close the event subscription and stop
   the pump timer. Event delivery is subscription polling, not callbacks that
   capture the browser/source.
3. Disconnect runtime socket handlers before aborting and deleting WebSocket
   clients; close the WebSocket server.
4. Close HTTP listening, disconnect/abort/delete HTTP clients. Their timeout
   callbacks are canceled with their QObject contexts.
5. Destroy bridge state on its owner thread, cancel remaining context-bound
   queued work, and join the thread.
6. Remove the browser active child exactly once when registration succeeded;
   release the browser reference exactly once.

The join does not depend on the OBS UI event loop: the bridge does no blocking
network I/O, OBS source operations, or parent-mutex acquisition. No wait for peer
close acknowledgement is required. Qt documents that
[`QWebSocket::abort`](https://doc.qt.io/qt-6/qwebsocket.html#abort) immediately
closes the connection. Child removal/reference balancing follows the
[OBS source API](https://docs.obsproject.com/reference-sources).

Refresh completes teardown before creating a new bridge/browser. A mode switch
and source deletion use the same teardown path. No Web Widget service survives
source destruction into module unload. If PluginRuntime stops first through
`aboutToQuit`, subscriptions report runtime stopped and later bridge teardown
still uses its own live event loop. The existing core runtime's application-thread
startup and shutdown contract is unchanged.

## Socket warning and CSP

`QIODevice::read (QSslSocket): device not open` is not established as causal.
There is no stack trace identifying the transport. The widget bridge uses local
HTTP/ws; Twitch uses TLS elsewhere. Twitch stop already invalidates generations,
disconnects reply/socket callbacks before aborting, and cancels timers. Its reply
completion path can read an aborted reply, so socket warnings can also be
secondary diagnostics. No unrelated Twitch transport change was made.

The private browser previously injected an inline CSS string despite
`style-src 'self'`. The string is now empty; the bundled external CSS already
provides transparent backgrounds, margins and overflow rules. The widget also
assigned `name.style.color`; it now validates hex colors and chooses a nearby
color from eight fixed external CSS classes. Exact arbitrary username RGB values
are approximated by this palette. CSP directives are unchanged; no `unsafe-inline`
was added. Actual CEF console validation remains manual.

## Diagnostics

All logs use `[WebWidget][Runtime]` and contain no capability URL or credentials:

- Refresh requested / Previous runtime stopped / Runtime recreated
- Created independent browser runtime
- Shutdown requested / Event subscription released / Timers stopped
- WebSocket clients closed / WebSocket server stopped / HTTP server stopped
- Browser child detached / Browser source released / Shutdown complete
- Actionable browser creation/attachment failures

## Automated validation

- CMake configured with `cmake --preset linux-x86_64 -DENABLE_TESTS=ON`.
- Full plugin build succeeded with `cmake --build --preset linux-x86_64 -j 4`.
- Full CTest: 13/13 suites passed, including Web Widget, Web event bridge,
  synthetic events, dispatcher concurrency, native chat/producer and updater tests.
- Separate Debug ASan/UBSan build: event-dispatcher, synthetic-event, web-event
  and web-runtime suites passed (4/4); leak detection remained enabled.
- No compiler or sanitizer warnings were emitted. Configure reports optional
  Vulkan headers missing (`WrapVulkanHeaders`); configuration/build still succeed.
- `git diff --check` passed.

The new Linux runtime test wraps the OBS browser boundary while exercising real
Qt threads and HTTP/WebSocket clients. It checks repeated replacement/shutdown,
connected-client teardown, partial HTTP requests, queued resize, event arrival
around shutdown, independent consumers, missing browser/creation/attachment
failures, exactly-once detach/release/server-stop logs, and destruction without UI
event processing. The existing dispatcher tests cover concurrent publication
and subscription closure, including no delivery after release. JavaScript tests
reject inline DOM styling. These do not execute actual ChatSource mode switching,
OBS graphics, or CEF. Runtime tests require loopback access: their initial run in
the restricted sandbox failed to listen; the authorized run outside that network
restriction passed. No tests were skipped to obtain the passing result.

## Manual OBS validation still required

1. Load this build in OBS with the plugin enabled; confirm Web Widget rendering.
2. Refresh repeatedly while connected and while synthetic events are arriving.
   Check old shutdown completes before the next runtime is created.
3. Trigger chat, follow, subscription, cheer, raid and moderation events. Verify
   badges, animated emotes, native rendering and another consumer continue working.
4. Switch Native → Web → Native; repeat with a visible source.
5. Delete/recreate the source; also try two Web Widget source instances.
6. Check widget developer output for CSP violations and visible username colors.
7. Close OBS normally with Web Widget active, including shortly after refresh and
   during synthetic event delivery. Repeat with the widget inactive/hidden.
8. Verify the OBS process exits without a manual kill and logs
   `[WebWidget][Runtime] Shutdown complete`, followed by normal OBS shutdown
   completion (`All scene data cleared`, context freeing, profiler/leak output).
9. Smoke-test updater UX and persisted renderer/settings after restarting OBS.

No interactive OBS validation was performed. The concrete unsafe paths are fixed;
the original crash and hang are not claimed manually confirmed resolved.
