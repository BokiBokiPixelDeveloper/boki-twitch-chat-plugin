# Version 3 — Web Widget Runtime

Target release: `0.1.0-alpha.14`. Audit date: 2026-09-26.

This document began as the architecture audit and remains the implementation
specification. The runtime is implemented on this feature branch for
`0.1.0-alpha.14`. All identifiers, logs, tests, and developer documentation are
English.

Recommended architecture: each `ChatSource` owns an optional private OBS
`browser_source`, backed by the installed obs-browser module. A per-instance
loopback content server serves embedded first-party resources. A bounded,
authenticated loopback WebSocket carries only validated event DTOs. Native
Floating rendering continues through its existing OBS graphics path.

No additional Chromium/CEF distribution, user-created Browser Source, Twitch
connection, StreamElements adapter, widget importer, or persistence system is
needed for V3.

## A. Existing OBS/browser capabilities

### Evidence and target environment

The local Linux environment contains:

| Item | Observed value |
| --- | --- |
| OBS packages | `obs-studio 32.2.2-1`, `obs-studio-plugin-browser 32.2.2-1` |
| Browser module | `/usr/lib/obs-plugins/obs-browser.so` |
| Browser subprocess | `/usr/lib/obs-plugins/obs-browser-page` |
| CEF | `cef 151.3.24-1`, `/usr/lib/cef/libcef.so` |
| Qt | Base `6.11.2-3`, WebSockets `6.11.2-1` |
| Build tools | CMake `4.4.3`, Ninja `1.13.2` |
| OBS development headers | `/usr/include/obs/obs.h`, `obs-source.h`, `callback/proc.h`, `callback/calldata.h` |
| Existing project dependencies | `OBS::libobs`, Qt Core/Gui/Network/WebSockets; no CEF or Qt WebEngine linkage |

`ldd` found no missing browser-module dependencies. `nm -D` confirms the exports
`obs_browser_initialize`, `obs_browser_create_qcef`, and
`obs_browser_qcef_version_export`. The installed binary also contains the
procedure signature `void javascript_event(string eventName, string jsonString)`.
Browser C++ implementation headers are not part of the installed libobs API.

The Windows dependency manifest, `scripts/windows-dependencies.json`, also pins
OBS `32.2.2`. Windows runtime DLLs were not inspected or executed here. The
installed OBS distribution must supply `obs-browser.dll`, its matching CEF,
browser subprocess, and resource files; the plugin must not package substitutes.

The OBS `32.2.2` tag pins obs-browser commit
`3f0a2cdf378939ebe3c6f9ab36d4ea100c25aac2`, verified through the GitHub contents
API. The audit used that revision, not a moving upstream branch:
[OBS submodule](https://github.com/obsproject/obs-studio/tree/32.2.2/plugins/obs-browser).
Local symbol/package inspection corroborates the integration points but does not
prove the distro binary is identical to unpatched upstream code. No actual OBS
browser rendering or storage experiment was performed during this audit.

### Integration mechanisms

| Mechanism | Concrete API | Assessment |
| --- | --- | --- |
| Plugin-owned browser source | `obs_source_create_private("browser_source", name, settings)` | Recommended. A separate child per parent, absent from the normal source list. |
| Rendering | `obs_source_video_render(child)` inside the parent's `video_render` | Recommended. OBS/obs-browser retain texture ownership. |
| Size | Child settings `width`, `height`; `obs_source_get_width/height`; `obs_source_update` | Use the parent's validated viewport dimensions. |
| Lifetime | `obs_source_get_ref`, `obs_source_release`, weak-source APIs | Wrap references in RAII; null alone does not prove source creation succeeded. |
| Source hierarchy | `enum_active_sources`, `enum_all_sources`, `obs_source_add_active_child`, `obs_source_remove_active_child` | Needed for visibility/activation and scene traversal. |
| Runtime detection | `obs_get_module("obs-browser")`, `obs_enum_source_types` | Check when entering Web mode, after normal module loading. |
| Per-source JS event | `obs_source_get_proc_handler`, `proc_handler_call(..., "javascript_event", ...)`; calldata fields `eventName`, `jsonString` | Real, usable mechanism, but optional implementation detail rather than a libobs browser contract. |
| Browser UI panel | `panel/browser-panel.hpp`, `QCef`, `QCefWidget`, `QCefCookieManager`; exported factory/version functions | QWidget/dock integration; not an offscreen OBS source texture API. Do not use for V3 rendering. |
| Raw CEF browser/texture | Internal `BrowserSource`, `BrowserClient`, `DispatchJSEvent`, `CefBrowser` | No supported exported reusable texture/CEF-object API found. Do not link private C++ internals. |

Private source creation and hierarchy semantics are exposed by
[libobs](https://github.com/obsproject/obs-studio/blob/32.2.2/libobs/obs-source.c).
`obs_source_add_active_child` updates activation counts; it does not store the
child, retain it, or implement enumeration for the parent. The wrapper must do
all three. Do not also manually increment showing/active counts for the same
relationship. Do not copy the browser's `OBS_SOURCE_DO_NOT_DUPLICATE` flag onto
the parent: duplicating the parent must create a new private child.

obs-browser creates a windowless browser, handles browser painting and GPU
resources, and draws its texture. Software painting uploads BGRA pixels;
accelerated paths use platform shared textures where supported. The plugin
should delegate the draw, with correct graphics-state restoration, rather than
copy pixels or obtain CEF texture pointers. Resize handling exists in the child.
See [browser rendering](https://github.com/obsproject/obs-browser/blob/3f0a2cdf378939ebe3c6f9ab36d4ea100c25aac2/browser-client.cpp#L304)
and [source implementation](https://github.com/obsproject/obs-browser/blob/3f0a2cdf378939ebe3c6f9ab36d4ea100c25aac2/obs-browser-source.cpp).

### Transport decision

The built-in `javascript_event` procedure targets one Browser Source and sends a
CEF process message. It does not supply a load-ready signal, delivery ACK, or a
bounded application mailbox. Its renderer-side handler distributes the event to
frames and builds a CustomEvent expression using CEF V8 evaluation. Payload JSON
is parsed and reserialized, but the event name is concatenated into code. If
ever used, the event name must be a compile-time constant, never viewer data.
See [upstream event handler](https://github.com/obsproject/obs-browser/blob/3f0a2cdf378939ebe3c6f9ab36d4ea100c25aac2/browser-app.cpp#L226).

Use the existing Qt WebSockets dependency for V3's generic transport. This gives
explicit readiness, frame limits, acknowledgement, and disconnect handling,
without injected JavaScript or dependence on obs-browser's internal procedure.
It also matches the repository's existing local-bridge direction. This is a
transport choice, not a different browser implementation. The installed OBS
browser remains the renderer. Missing `javascript_event` alone therefore does
not make the chosen Web runtime unavailable.

### Browser settings for the private child

Set an explicit allowlist of child settings, constructed from scratch:

```text
is_local_file = false
url = generated per-instance loopback entry URL
width / height = validated parent canvas dimensions
shutdown = false
restart_when_active = false
webpage_control_level = 0
reroute_audio = true
fps_custom = true
fps = 30
css = fixed transparent/reset stylesheet, with no event data
```

`webpage_control_level=0` means `ControlLevel::None`; the upstream default is
`ReadObs`, so relying on defaults gives unnecessary rights. `reroute_audio=true`
mutes browser host playback and routes browser audio through OBS; additionally
mute the private source and use no audio elements in the development widget.
V3 does not add parent audio output. Test that no audio reaches the desktop or
stream. These settings come from
[registration/defaults](https://github.com/obsproject/obs-browser/blob/3f0a2cdf378939ebe3c6f9ab36d4ea100c25aac2/obs-browser-plugin.cpp)
and [control levels](https://github.com/obsproject/obs-browser/blob/3f0a2cdf378939ebe3c6f9ab36d4ea100c25aac2/obs-browser-source.hpp#L30).

## B. Relevant repository files

| Existing path | Responsibility | Expected V3 work |
| --- | --- | --- |
| `src/plugin-main.cpp` | Source registration, defaults, shared runtime, unload | Generic description; renderer default; child enumeration; Web service shutdown before runtime/module teardown. Keep source ID. |
| `src/renderer/chat-source.hpp/.cpp` | Settings, backend attachment, native state, tick/draw | Own renderer selection and optional Web runtime; route draw; handle refresh and independent instances. Keep lane state native. |
| `src/renderer/native-event-adapter.hpp/.cpp` | Native event-to-layout adaptation | Preserve native behavior; deactivate/clear it while Web mode is selected. |
| `src/renderer/render-types.hpp`, `message-layout.hpp/.cpp` | Native layout/GPU data | No Web bridge responsibilities; preserve native rendering. |
| `src/core/plugin-runtime.hpp/.cpp` | Shared Twitch service, attachments, subscriptions | Add a credential-free attachment-status snapshot and control-only attachment option; retain account/channel conflict policy. |
| `src/core/event-dispatcher.hpp/.cpp` | RAII immutable event mailboxes | Reuse `subscribe`, `takeBatch`, queue limits and terminal states; no browser dependency. |
| `src/core/event-types.hpp`, `src/chat/chat-types.hpp` | Normalized event and asset model | Serialize existing fields explicitly; no lane/DOM/pointer additions. |
| `src/core/event-validation.hpp/.cpp`, `ordered-event-pipeline.hpp/.cpp` | Validation, enrichment, freeze/publication | Preserve the security boundary; reuse asset URL policy, add tests rather than Web-specific ingress. |
| `src/core/synthetic-event-producer.hpp/.cpp` | Synthetic production through the ordered pipeline | Reuse unchanged. No Web-specific test producer. |
| `src/twitch/*`, `src/chat/emote-*`, `image-cache.*` | Ingestion, enrichment and assets | No new Twitch client. Current badge URLs are unresolved; support text badge fallback. |
| `resources/web-themes/README.md`, `classic-default/README.md` | Future-theme placeholders | Document the development widget; do not represent the placeholder as implemented. |
| `CMakeLists.txt` | Targets and embedded resources | Add Web support and widget QRC; reuse Qt Network/WebSockets and libobs. No CEF SDK requirement. |
| `tests/CMakeLists.txt`, `tests/README.md` | Test registration/instructions | Register serializer, endpoint, lifecycle and widget tests; document real OBS checks. |
| `data/locale/en-US.ini` | English UI strings | Add renderer/runtime labels consistent with current property conventions. |
| `docs/ARCHITECTURE.md`, `docs/ROADMAP.md`, `README.md` | Product/architecture documentation | Describe implemented modes and dependencies after implementation, link this plan now. |
| `scripts/package.sh`, `package-windows.ps1`, `install-user.sh`, `install-windows.ps1`, `installer/windows.iss` | Distribution and installation | Verify widget is embedded and present in built binaries; do not bundle CEF. No separate asset installation needed with QRC. |
| `VERSION`, `.github/workflows/*` | Release version/build pipelines | Bump to alpha.14 only with the implementation; add checks without releasing. |

The existing native constructor consumes `BackendAttachment::Delivery`, which
also contains token updates. Do not serialize that structure. Web events must
come from their own `EventSubscription`; credential updates stay in the existing
native C++ settings path.

## C. Proposed Web runtime classes and types

| Proposed file/type | Responsibility |
| --- | --- |
| `src/renderer/renderer-mode.hpp`: `RendererMode` | `Native`, `WebWidget`; serialize setting values as stable strings. |
| `src/web/web-widget-runtime.hpp/.cpp`: `WebWidgetRuntime` | One source's controller, lifecycle, config revision and event subscription. |
| `src/web/obs-browser-source.hpp/.cpp`: `ObsBrowserSource` | RAII private OBS source, detection, child settings, hierarchy and render reference access. |
| `src/web/web-runtime-state.hpp`: `WebRuntimeState`, `WebRuntimeStatus`, `WebWidgetConfiguration` | Explicit lifecycle/status and credential-free configuration. |
| `src/web/web-event-serializer.hpp/.cpp`: `WebEventSerializer` | Pure immutable-event-to-JSON mapping; no OBS/CEF/network operations. |
| `src/web/widget-content-server.hpp/.cpp`: `WidgetContentServer` | Per-runtime bounded loopback HTTP endpoint serving an exact embedded resource map. |
| `src/web/web-event-bridge.hpp/.cpp`: `WebEventBridge` | Per-runtime WebSocket server/session, readiness, acknowledgements, bounded drain. |
| `src/web/web-runtime-services.hpp/.cpp`: `WebRuntimeServices` | Module-owned Qt-thread lifecycle registry and shutdown barrier; owns no singleton browser. |
| `resources/web-themes/development/*`, `resources/web-widget.qrc` | First-party widget, generic bridge library and embedded assets. |

Use QObject parents for servers/timers on their owner thread and RAII for OBS
references and subscriptions. A test seam around `ObsBrowserSource` allows
lifecycle tests without loading OBS. Avoid a broad renderer framework rewrite.

## D. Multi-instance ownership and isolation

```text
Plugin module
  shared PluginRuntime -> one Twitch/EventSub service + dispatcher
  WebRuntimeServices -> weak registry / Qt lifecycle coordinator

ChatSource A                    ChatSource B                  ChatSource C
  BackendAttachment              BackendAttachment             BackendAttachment
  WebWidgetRuntime A             WebWidgetRuntime B             Native adapter
    EventSubscription A            EventSubscription B          Floating state/GPU
    WidgetContentServer A          WidgetContentServer B
    WebEventBridge A               WebEventBridge B
    ObsBrowserSource A             ObsBrowserSource B
      private browser_source A       private browser_source B
```

Each Web runtime has separate DOM, JavaScript global object, timers, listeners,
configuration, delivery counters and lifecycle generation. Browsers may share
CEF processes; process separation is not a requirement or a guarantee. Same
widget assets can be shared as immutable compiled bytes, never as mutable widget
state. `instanceId` and `runtimeEpoch` are freshly generated runtime identifiers,
not persisted source settings. A real OBS source duplicate gets new identifiers,
servers, subscription and child. Adding another scene reference to the *same*
OBS source intentionally reuses its runtime.

### Storage findings

obs-browser supplies a null request-context argument during browser creation.
CEF documents that this uses its global context, and obs-browser configures a
shared cache location. Independent Browser Sources are not independent browser
profiles. This is an inference from upstream construction and
[CEF's request-context contract](https://cef-builds.spotifycdn.com/docs/132.2/classCefRequestContext.html),
to be verified on target binaries.

| State | Same-origin browser instances | V3 policy/limitation |
| --- | --- | --- |
| DOM/globals/listeners/timers | Separate document runtime | Recreate on explicit refresh. |
| `localStorage` | Shared by origin in the shared context | Distinct active loopback ports give different origins; widget does not use storage. |
| IndexedDB | Shared by origin/storage partition | Same limitation; no custom persistence or cleanup of global browser storage. |
| `sessionStorage` | Scoped by origin and top-level browsing context; survives ordinary reload | New private browser on refresh; no opener relationship between A and B. |
| Cookies | Domain/path/context rules; ports do not isolate cookies | Do not use cookies for identity, auth or configuration. No promise of isolated cookies. |

Browser storage rules are described in
[same-origin policy](https://developer.mozilla.org/en-US/docs/Web/Security/Defenses/Same-origin_policy)
and [sessionStorage](https://developer.mozilla.org/en-US/docs/Web/API/Window/sessionStorage).
Cookie port sharing is specified in
[RFC 6265 section 8.5](https://www.rfc-editor.org/rfc/rfc6265#section-8.5).
Different URL paths alone do not isolate localStorage or IndexedDB. Separate
ephemeral ports isolate active document origins, but port reuse can expose old
origin storage later; refresh is not a promise of secure persistent-data erasure.
Remote sites' cookies can also be shared across widgets. Do not clear OBS's
global cookie/cache directory or invent a per-source persistence scheme in V3.

## E. Browser lifecycle and threading

Proposed states:

```text
Disabled -> Checking -> Loading -> Ready
                         |          |
                         +-> Failed <-+
Ready/Loading/Failed -> Stopping -> Disabled or Checking
Any live state -> Stopping -> Stopped (module shutdown)
```

1. **Create:** read settings, default missing mode to Native. Web mode queues
   creation on the Qt application thread. Check browser registration, allocate
   an epoch and endpoints, subscribe, construct the private child and register
   the child relationship. Load the bundled entry URL. The bridge sends no
   events until the authenticated JS ready handshake succeeds.
2. **Ready:** a 20 ms Qt timer drains bounded batches, preserving mailbox order.
   OBS owns the child's ticking; do not manually call its `video_tick`. The
   parent calls `obs_source_video_render` only from its render callback. No
   socket wait, HTTP parsing or serialization runs on render/tick paths.
3. **Properties:** publish a validated settings snapshot. Size changes update
   the child and bridge configuration with a revision; they need not replace
   the DOM. Widget change or explicit refresh uses full replacement. Native
   settings remain saved while their controls are hidden in Web mode.
4. **Refresh:** revoke the epoch, stop draining, close the subscription and
   sockets, clear queues, remove the child relationship and release the child.
   Then create fresh identifiers/endpoints and a new private source. No replay
   of the old queue. Browser closure is asynchronous inside obs-browser; old
   work is harmless because its endpoint and epoch are invalidated. Do not use
   `RefreshNoCache` as a substitute for this lifecycle.
5. **Scene changes:** preserve the runtime while the source exists. Enumerate
   the child and balance add/remove-active-child calls on replacement. Keep
   `shutdown=false` and `restart_when_active=false`; hiding does not reset DOM.
   Hidden-browser throttling can cause bridge timeouts; recovery must report a
   reset/gap and resubscribe, never grow queues indefinitely.
6. **Destroy:** mark the control state stopping before accepting more tasks;
   close subscriptions, detach timers/socket handlers, revoke capabilities and
   child access, remove the active relationship and release refs. Queued work
   uses weak control state/epoch checks, not raw `ChatSource*` captures. A render
   callback holds a temporary strong child reference acquired under a short
   lock; release that lock before invoking OBS rendering.
7. **OBS exit/module unload:** stop Web services before `PluginRuntime::shutdown`.
   Stop Qt timers/endpoints while the Qt event loop still exists, detach every
   remaining child, and drain plugin-owned queued callbacks before unloading
   code. Use an idempotent coordinator shared by `aboutToQuit` and module unload.
   OBS normal module unload happens after source destruction; do not claim
   arbitrary hot-unloading a module with live sources is supported.

Serialize lifecycle transitions on one owner thread. OBS source hierarchy
changes and callback reads require an explicit lock/order policy tested against
scene traversal. Never wait for Qt while holding the source mutex or graphics
lock; never wait for browser readiness in a source callback. Destruction from
OBS's deferred destruction thread must invalidate synchronously and retire Qt
objects through the coordinator. Teardown barriers belong to shutdown, not
render/tick. Browser internals own CEF work and texture destruction; network
callbacks in this plugin perform no graphics operations.

OBS itself defers source destruction, and obs-browser queues browser deletion;
the source implementations linked in section A establish why releasing a
reference is not equivalent to immediate CEF closure.

## F. Exact event bridge and internal JavaScript API

```text
EventSub -> normalizer/enrichment --+
                                  +-> OrderedEventPipeline
SyntheticEventProducer -----------+   -> ingress/final validation
                                      -> immutable EventPtr publication
                                      -> PluginRuntime::subscribe(...)
                                      -> EventSubscription::takeBatch(32)
                                      -> WebEventSerializer
                                      -> WebEventBridge WebSocket text frame
                                      -> JSON.parse + envelope validation
                                      -> frozen DTO -> BokiChat.onEvent callbacks
                                      -> controlled DOM construction
```

Use all-kind subscriptions so synthetic events on `synthetic-test-channel` are
not accidentally filtered out by a real channel ID. Preserve the backend's
conflicting-account/channel policy: add a renderer-independent attachment status
snapshot (`productionAccepted`, resolved channel ID and reset revision) and a
control-only attachment that does not retain a redundant native event mailbox.
Forward production/local-transport events only while that source is accepted;
allow synthetic publication to attached Web consumers as documented by the
existing shared Event Test Mode. Reset the Web subscription on backend selection
changes. A raw unfiltered subscription must not bypass this acceptance check.
The snapshot must contain no tokens or full `TwitchConfiguration`.

Define `window.BokiChat` as a namespaced first-party facade, separate from
`window.obsstudio` and future third-party adapters:

```javascript
const unsubscribe = window.BokiChat.onEvent(event => {
  // event is a deeply frozen normalized DTO
});
const unsubscribeStatus = window.BokiChat.onStatus(status => {});
const configuration = await window.BokiChat.ready;
// unsubscribe() removes only this listener.
```

`ready` resolves after protocol negotiation; configuration is read-only and has
only widget ID, viewport, instance/epoch and revision. Install event listeners
before announcing ready. `onStatus` covers ready/reset/disconnected/unavailable,
not Twitch credentials. Listener exceptions are caught per listener. ACK after
synchronous dispatch; promises returned by widget listeners do not hold the
transport open. This API can later feed a StreamElements adapter without
changing the C++ transport. Do not add `onWidgetLoad`, `onEventReceived` or
`SE_API` in V3.

### Protocol and bounded delivery

Each runtime binds HTTP and WebSocket listeners to `127.0.0.1` with OS-assigned
ports. Using two listeners keeps Qt's existing WebSocket handshake parser and
avoids inventing a TCP-to-WebSocket handoff. No listener binds all interfaces.
Use `QWebSocketServer` origin authentication and message/frame limits, described
in [Qt's server API](https://doc.qt.io/qt-6/qwebsocketserver.html).

Use a cryptographically random, per-epoch local capability in the entry path.
Serve a same-origin bootstrap JSON resource containing the WebSocket port and
capability under that protected path. This capability authorizes only this
ephemeral event session; it is not an OAuth token or plugin credential. Never
log or persist it. Require exact HTTP Host, exact WebSocket Origin and the
capability in the first WebSocket `hello` message. Origin is a browser defense,
not authentication against a local native process. Allow one authenticated
client per runtime; reject competing clients and stale epochs.

Control frames have `protocolVersion: 1`, `instanceId`, `runtimeEpoch`, and a
`type` discriminator:

| Direction/type | Fields and behavior |
| --- | --- |
| JS `hello` | `capability`, `supportedSchemaVersions: [1]`; negotiate before event access. |
| C++ `welcome` | `schemaVersion: 1`, `configuration`; no source settings dump. |
| JS `ready` | Confirms bridge/widget handlers installed. Starts delivery. |
| C++ `events` | Decimal-string `deliveryId`, ordered `events: WebEvent[]`. |
| JS `ack` | Matching `deliveryId`; releases the in-flight batch. |
| C++ `configuration` | New read-only config/revision after resize. |
| C++ `reset` | Fixed reason enum, no event data; widget clears retained rows. |

Initial proposed limits: inherited subscription limits of 256 events/32 MiB;
32 events per drain; 512 KiB maximum event JSON; 1 MiB maximum encoded batch;
one unacknowledged batch; at most 8 KiB incoming control frames; 10 s
startup/ACK timeout. Also cap accepted/pending connections, partial HTTP bytes,
socket write buffers and idle request time. If a validated event exceeds the
wire cap, report oversize and reset the consumer rather than emitting partial
JSON. Limits require tests with maximum validated messages.

`ConsumerState::Overflowed` means the subscription is terminal: clear widget
state, close it, and create a fresh subscription. Do not silently lose a delete
while preserving the message it should remove. `RuntimeStopped` closes the
bridge; `Closed` ends that consumer. On disconnect invalidate in-flight state;
reconnect negotiates a reset and live-only subscription, with no history replay
or exactly-once claim across sessions. Slow or broken widgets cannot block
other consumers. `sequence` and `generation` belong to producers and may overlap
between synthetic and production producers; never sort or globally deduplicate
by them. Bridge `deliveryId` orders deliveries within an epoch.

## G. Event DTO schema

The following TypeScript notation specifies wire JSON, not an implementation or
a new TypeScript build dependency. Emit every listed field; represent absent
optionals as `null`, absent collections as `[]`. Empty optional C++ identifier
strings become `null`; preserve required empty semantic text as `""`. Ignore
unknown additive fields within schema version 1; breaking changes increment it.
Unknown event types are ignored with a bounded diagnostic. No raw enum integers.

```typescript
type DecimalU64 = string; // canonical unsigned decimal; avoids JS precision loss
type UtcTimestamp = string; // ISO 8601 UTC, exactly millisecond precision
type Color = string; // canonical #RRGGBB; null if QColor is invalid
type AssetUrl = string; // V2-approved fully encoded HTTPS URL only

interface User {
  id: string | null;
  login: string;
  displayName: string;
  color: Color | null;
}
type EmoteProvider = "twitch" | "frankerfacez" | "betterttv" | "seventv";
interface TextRange { offset: number; length: number } // UTF-16, half-open
interface TwitchEmote {
  setId: string | null;
  ownerId: string | null;
  supportsStatic: boolean;
  supportsAnimated: boolean;
}
interface Cheermote { prefix: string; bits: number; tier: number }
interface Fragment {
  type: "text" | "emote";
  text: string;
  emoteId: string | null;
  provider: EmoteProvider | null;
  imageUrl: AssetUrl | null;
  fallbackUrl: AssetUrl | null;
  zeroWidth: boolean;
  sourceRange: TextRange | null;
  twitch: TwitchEmote | null;
  mention: User | null;
  cheermote: Cheermote | null;
}
interface Badge {
  provider: "twitch";
  type: string;
  version: string;
  info: string;
  imageUrl: AssetUrl | null;
}
interface Media { imageUrl: AssetUrl }
interface ChatMetadata {
  messageType: string;
  systemText: string;
  reply: { parentMessageId: string | null; threadMessageId: string | null } | null;
  cheerBits: number | null;
  rewardId: string | null;
}
interface ChatMessageData {
  messageId: string;
  user: User;
  text: string;
  fragments: Fragment[];
  badges: Badge[];
  media: Media[];
  metadata: ChatMetadata;
}
type SubscriptionTier = "unknown" | "tier1" | "tier2" | "tier3";
interface SubscriptionTerms {
  tier: SubscriptionTier;
  isPrime: boolean | null;
  durationMonths: number;
}
interface GiftActor { anonymous: boolean; user: User | null }

interface Header {
  eventId: string;
  timestamp: UtcTimestamp;
  receivedAt: UtcTimestamp;
  channelId: string;
  originChannelId: string | null;
  originMessageId: string | null;
  sequence: DecimalU64;
  generation: DecimalU64;
  origin: "production" | "localTransportTest" | "syntheticTest";
}
type EventBody =
  | { type: "ChatMessage"; data: ChatMessageData }
  | { type: "MessageDeleted"; data: { messageId: string; user: User } }
  | { type: "ChatCleared"; data: { user: User | null } }
  | { type: "Follow"; data: { user: User; followedAt: UtcTimestamp } }
  | { type: "Subscription"; data: {
      notice: ChatMessageData; terms: SubscriptionTerms } }
  | { type: "Resubscription"; data: {
      notice: ChatMessageData; terms: SubscriptionTerms;
      cumulativeMonths: number; streakMonths: number | null;
      isGift: boolean; gifter: GiftActor | null } }
  | { type: "GiftSubscription"; data: {
      notice: ChatMessageData; gifter: GiftActor; recipient: User;
      terms: SubscriptionTerms; communityGiftId: string | null;
      cumulativeTotal: number | null } }
  | { type: "CommunityGiftSubscription"; data: {
      notice: ChatMessageData; gifter: GiftActor; tier: SubscriptionTier;
      communityGiftId: string | null; count: number;
      cumulativeTotal: number | null } }
  | { type: "Cheer"; data: { user: User | null; bits: number; text: string } }
  | { type: "Raid"; data: { from: User; to: User; viewers: number } };

type WebEvent = {
  schemaVersion: 1;
  header: Header;
} & EventBody;
```

All numeric counts/ranges are validated nonnegative JSON integers within the
existing C++ policy limits. Preserve zero and false; never substitute unknown
with zero/false. Anonymous gifts and anonymous cheers stay explicitly anonymous.
Message deletion uses `messageId`, not transport `eventId`. A null clear user
means clear all messages for the envelope's channel; a user means clear that
user's messages in that channel. Preserve shared-chat origin IDs for consumers.

Construct JSON with `QJsonObject/QJsonArray/QJsonDocument`, never interpolated
JavaScript or HTML. Serialize only allowlisted fields. No `QImage`, decoded
pixels, CPU/GPU pointers, QObject addresses, tokens, source settings, raw Twitch
payloads or local paths cross the bridge. Do not pass the entire `Delivery`.

Fragments already cover semantic message text in order. Render fragments
directly; JavaScript string offsets use the same UTF-16 units as QString. Keep
fallback text, asset provider and zero-width metadata; no Twitch parser state
reconstruction is needed. Badge `imageUrl` is currently normally empty: render
a text badge derived from `type/version` until centralized enrichment supplies
a validated URL. Do not invent a Twitch badge CDN URL or give JS credentials to
resolve badges. Media image URLs remain structured, not HTML strings.

## H. Development widget

Planned files under `resources/web-themes/development/`:

| File | Behavior |
| --- | --- |
| `index.html` | Transparent document, chat list, compact event list/status; loads fixed external scripts/styles. |
| `style.css` | Responsive chat/event rows; configurable viewport, no fixed orientation. |
| `bridge.js` | Generic `BokiChat` facade, bootstrap/handshake, JSON validation/freezing, bounded dispatch and ACKs. |
| `widget.js` | First-party safe DOM rendering for all ten event types. |
| `README.md` | Contract, synthetic test procedure, badge/media limitations. |

Embed these through `resources/web-widget.qrc`. Serving QRC resources avoids
filesystem traversal and ensures standalone binary updates include the widget.
Keep `classic-default` as a separate future theme; no package manifest/importer
is required for the development widget.

Chat rows display user color/name, text badges or validated badge images,
Unicode text and structured emotes. Use `createElement("img")`, assign validated
URLs, set text alternatives, attempt one validated fallback URL, then replace
with text on failure. Implement zero-width emotes with a controlled overlay
container and text fallback when no base exists. Key rows in Maps by channel
and message ID, not viewer-derived CSS selectors. Delete and both clear scopes
remove the corresponding rows. Embedded media can be a bounded image or safe
text fallback; it is not necessary to reproduce native bouncing GIF physics.

Each event kind has a short visible card: follower, subscription tier/Prime,
resub months, gift actor/recipient, community gift count, cheer bits/text, raid
source/target/viewers. Preserve anonymous labels. Subscription notice text,
badges and fragments use the same safe chat renderer. Keep community and
individual gift events distinct; do not invent deduplication rules.

Bound retained DOM to 100 chat rows and 50 event cards, cancel per-row timers
when rows are removed, and clear all retained UI state on bridge reset. A
synthetic badge marks `syntheticTest` visibly. This marker does not change event
routing or create a special rendering path. The old native emoji/GIF buttons
are renderer diagnostics and should be hidden in Web mode; use the existing
Event Testing buttons for end-to-end Web testing.

## I. Source properties and compatibility

| Key | UI/default | Rules |
| --- | --- | --- |
| `renderer_mode` (new) | Renderer Mode: Native / Web Widget; default `native` | Stored values `native`, `web_widget`; missing means existing Native behavior. Preserve unknown values and show unsupported-mode status rather than overwrite them. |
| `canvas_width` (existing) | Width; default 1920 | Reuse for both modes; native bounds stay 320–7680, proposed Web bounds 64–7680. |
| `canvas_height` (existing) | Height; default 1080 | Reuse for both modes; native bounds stay 240–4320, proposed Web bounds 64–4320. |
| `web_widget_id` (new) | Widget; default `development` | V3 exposes bundled development widget only; future allowlisted IDs can select other content. |
| `web_refresh` (button) | Refresh Widget | Full per-instance replacement, not a persisted value. |
| `web_status_info` (info) | Runtime/loading/error status | Separate from Twitch status; no capabilities or full URLs in text. |

Validate signed dimension values before converting to `uint32_t`; the current
cast-before-clamp pattern can turn negative settings into the maximum. Apply
mode-specific effective bounds without overwriting saved dimensions during mode
switches. Width and height are independent: test 400×900 and 1200×120 viewports.
The parent and child report the same effective dimensions. Larger viewports
remain subject to normal OBS/GPU limits and explicit failure reporting.

Native's UI may describe the current style as Floating, but its stored mode is
generic. Keep source ID `bokis_twitch_chat_plugin`, all existing property keys,
Twitch credentials/settings and native tuning. Switching modes does not modify
OAuth configuration, account selection or Event Test Mode. Separate widget
configuration belongs to each source; do not use browser storage for it.

## J. Missing-browser detection and errors

Check the loaded module and registered `browser_source` type when creating Web
mode, not only at this plugin's `obs_module_load` (module order can differ).
Create the child, then require the JS handshake. A non-null `obs_source_t*`
alone is insufficient: libobs can retain a source object even when its internal
creation failed. Readiness timeout also detects failed CEF initialization,
subprocess/resource problems or content/bridge failures.

Use stable internal error codes: `BrowserModuleMissing`,
`BrowserSourceUnavailable`, `BrowserCreationFailed`, `ContentServerFailed`,
`BridgeProtocolMismatch`, `WidgetLoadTimeout`, `WidgetDisconnected`,
`ConsumerOverflow`, `RuntimeStopped`. Supply actionable English status without
logging event text, credentials, or entry capabilities.

For a missing module display:

> Web Widget Runtime unavailable. The required OBS browser component could not be loaded.

Keep the saved selection and dimensions; show error status in properties and a
fixed diagnostic placeholder in the source preview/output using native drawing
on the graphics path. Native remains selectable. No silent renderer switch and
no attempt to download/load another Chromium. Refresh retries after dependency
repair; native creation must never depend on browser availability.

## K. Security analysis

### DOM and JavaScript

Semantic event text remains semantic. The widget uses `textContent`,
`createTextNode`, fixed tag names and explicitly assigned attributes. Audit
plugin-owned bridge/widget code for `innerHTML`, `outerHTML`,
`insertAdjacentHTML`, `document.write`, `eval`, `new Function`, and string-based
timers. None may accept viewer-controlled data. Do not construct inline scripts,
event-handler attributes, CSS rules or selector strings from messages. Apply
colors only from normalized color values. Avoid object merging with untrusted
keys; parse into explicit schema fields and freeze before delivery.

Test JSON quoting, backslashes, U+2028/U+2029, `</script>`, HTML tags, prototype
keys and malformed envelopes. WebSocket JSON is data and never executable code.
The selected transport avoids obs-browser's CustomEvent evaluation path for
plugin events. OBS may still use that path for its own notifications.

Future user-installed widget JavaScript is executable trusted code, not viewer
text. Do not claim a global ban on every HTML API in such widgets. V3's safe
first-party code and CSP do not constitute a hostile-widget sandbox.

### URLs, local content and networking

Reuse V2's provider-specific HTTPS URL policy: Twitch
`static-cdn.jtvnw.net`, 7TV `cdn.7tv.app`, BTTV `cdn.betterttv.net`, FFZ
`cdn.frankerfacez.com`; badge URLs are Twitch-only. URLs may not have userinfo,
fragments, arbitrary ports or unsafe schemes. The serializer fails closed if
its asset assumptions are violated. Browser fetches can follow redirects;
first-party `img-src` restricts destinations to the same approved hosts so a
provider URL does not become a filesystem/arbitrary-host escape through a
redirect. A blocked image becomes fallback text.

Do not use OBS local-file mode: it rewrites files through `http://absolute/`,
whose handler opens a decoded filesystem path without a widget-package root.
See [local scheme handler](https://github.com/obsproject/obs-browser/blob/3f0a2cdf378939ebe3c6f9ab36d4ea100c25aac2/browser-scheme.cpp).
Never supply `file://` or `http://absolute/` entry URLs.

`WidgetContentServer` serves only exact resource IDs and a generated bootstrap
JSON document. No arbitrary `QFile` path derived from an HTTP request, directory
listing, proxy, upload, token endpoint, command endpoint or settings endpoint.
Reject traversal (including percent/double encoding), backslashes, malformed
requests, duplicate Host, unsupported methods/bodies and unknown routes. Bound
headers/connections/timeouts; respond with fixed MIME types, `nosniff`,
`Cache-Control: no-store`, and `Referrer-Policy: no-referrer`. Use a small audited
HTTP parser/strict subset with connection-close responses; do not turn this into
a general-purpose web server.

For the first-party widget send CSP with: `default-src 'none'`, scripts/styles
from self without unsafe-inline/unsafe-eval; images from self and the four
approved HTTPS CDNs; fonts from self; connections to its exact WebSocket endpoint
plus HTTPS/WSS; `object-src 'none'`, `frame-src 'none'`,
`frame-ancestors 'none'`, `base-uri 'none'`, `form-action 'none'`. No third-party
scripts or service-worker registration in V3. Test CSP on real OBS CEF. The
generic bridge must not modify global CEF networking or disable normal CORS/TLS.
Future trusted widget networking can have a separate documented policy without
weakening viewer-data handling.

OBS's existing browser is not a complete filesystem/OS sandbox: upstream sets
`no_sandbox=true`, registers its local scheme globally, exposes an `obsstudio`
object, and blocks popups in the source client. Browser same-origin/CORS checks
are not a promise that arbitrary trusted scripts cannot navigate or load local
resources. V3 avoids exposing paths or callable file APIs and restricts its own
document; broader guarantees for imported code require another audit. See
[CEF initialization](https://github.com/obsproject/obs-browser/blob/3f0a2cdf378939ebe3c6f9ab36d4ea100c25aac2/obs-browser-plugin.cpp#L289)
and [browser request/client behavior](https://github.com/obsproject/obs-browser/blob/3f0a2cdf378939ebe3c6f9ab36d4ea100c25aac2/browser-client.cpp#L79).

### Credentials and cross-instance access

Only immutable DTOs and minimal widget configuration cross the boundary. The
Web runtime never receives `BackendAttachment::Delivery::tokens` or full OBS
settings. Its local session capability is short-lived, scoped to one runtime,
rotated on refresh, and never placed in logs or persistent OBS settings. HTTP
and WS authorization do not rely on cookies. Reject missing/null/unexpected
origins, enforce the exact loopback Host, and do not enable permissive CORS.
One source's capability must not authenticate another source's listener.

Private Browser Sources are not a security boundary against other native OBS
plugins or same-user processes. Shared CEF storage/cookies and browser-level
OBS notifications remain limitations. Browser control level None reduces API
permissions but is not a guarantee that all OBS-generated notifications vanish.
No security claim should exceed the first-party-widget/viewer-input threat model.

## L. Tests and acceptance criteria

### Automated tests to add during implementation

1. **Serializer:** all ten variants and every optional field; unknown tier,
   anonymous actors, zero/false/null, shared-chat IDs, UTF-16 ranges, Unicode,
   emote fallbacks/overlays, unresolved badges, 64-bit values above 2^53;
   malicious text survives as data. Assert exact schemas and absence of secrets,
   pixels, settings and pointers.
2. **Content server:** exact bundled assets, MIME/CSP, loopback binding, Host,
   origin/capability rejection, traversal/encoding, unknown route, header/body
   limits, partial reads, idle peers, shutdown during requests. Only test-owned
   sockets/resources; never expose a real profile or home directory.
3. **Bridge:** hello/ready ordering, ACKs, FIFO, byte/count/frame bounds, slow
   client, oversize event, invalid controls, foreign source/old epoch, reconnect,
   runtime stop and terminal overflow. Verify moderation is never silently lost
   while old rows remain. Two clients cannot claim one runtime.
4. **Lifecycle with fake browser seam:** create, resize, refresh, switch modes,
   destroy before ready, destroy during drain, stale queued tasks, repeated
   shutdown, source duplicate, missing browser and module-load order. Check
   child reference and active-count balance; queued callbacks cannot retain a
   destroyed parent or touch GPU state.
5. **Pipeline integration:** existing `SyntheticEventProducer` -> real ordered
   pipeline/validation/dispatcher -> two Web consumers and one Native consumer.
   Exercise all kinds, moderation, a stalled Web consumer, conflicting saved
   account/channel settings and backend reset. No shortcut directly to JS.
6. **Widget DOM:** actual DOM harness with recorded DTOs checks text insertion,
   image URL/fallback behavior, zero-width overlays, badge labels, all event
   cards, scoped moderation, listener disposal, reset and retained-node limits.
   Static dangerous-sink searches supplement these tests, not replace them.
   A test-only headless browser may be used in CI, never shipped as runtime.

### Real OBS integration gate (Linux and Windows)

Create A and B in Web mode with different dimensions and widget configuration,
and C in Native. Inject every Event Testing kind and confirm both Web instances
and the appropriate Native behavior. Refresh A while B continues; duplicate A
and prove new IDs/state. Exercise preview/program, scene references, transitions,
hidden sources, filters, source removal, mode changes and OBS exit under event
load. Check transparency, color space, sizing, smooth native motion, muted audio,
no orphan child in the source list and no growth after repeated refreshes.

Test with browser module missing, CEF failing, endpoint bind failure, page crash,
blocked assets and handshake mismatch. Native must still work. Verify no event
appears in an unrelated Browser Source.

Run a controlled storage probe with two same-origin browsers and two proposed
different-port browsers: DOM counters, localStorage, sessionStorage, IndexedDB
and cookies before/after reload, source replacement and OBS restart. Record the
shared-cookie limitation instead of claiming full profile isolation. Confirm
first-party CSP blocks file/absolute-origin loads, redirects outside image
allowlists, frames and script injection. These tests require real obs-browser;
passing Qt unit tests alone is insufficient.

### Baseline validation performed for this audit

```bash
cmake --preset linux-x86_64 -DENABLE_TESTS=ON
cmake --build --preset linux-x86_64
ctest --test-dir build/linux-x86_64 --output-on-failure
```

Configure and build succeeded. All nine CTest suites passed: chat, dispatcher,
normalizer, synthetic events, updater, post-exit, manifest, Linux installer and
Twitch producer. CTest time: 13.71 s. No compiler warnings were emitted in this
build; CMake reported missing optional `WrapVulkanHeaders`/`Vulkan_INCLUDE_DIR`.
This is baseline validation, not proof of the proposed V3 runtime. Windows and
real OBS graphics/browser/lifecycle testing remain unperformed.

## M. Implementation sequence for the next task

1. Preserve this feature branch and current work. Read this plan and repository
   instructions; keep alpha.14 as the implementation target, with no release.
2. Build a narrow private-browser integration slice: capability detection,
   RAII child, enumeration/active relationship, dimensions and safe bundled
   entry loading. Test two parents in real OBS on Linux and Windows before
   expanding the runtime. Verify lifecycle/thread assumptions and audio policy.
3. Implement `WebEventSerializer` and contract fixtures/tests for all ten types.
   Keep OBS dependencies outside the serializer target.
4. Implement the constrained QRC content server and authenticated WebSocket
   session with explicit readiness, bounds, ACK/reset and epoch checks. Test
   endpoint restrictions independently of OBS.
5. Add credential-free backend acceptance/reset status and control-only
   attachments. Preserve native delivery/token persistence behavior and shared
   Twitch ownership; prove conflicts cannot leak another channel into Web.
6. Implement `WebWidgetRuntime`, mailbox draining, renderer switching and
   shutdown coordination. Keep network/serialization work off graphics paths.
7. Add the first-party development widget and DOM tests. Connect only the
   generic API; all Event Testing buttons use the existing pipeline.
8. Add renderer properties, mode-specific dimensions, refresh and actionable
   failure placeholders. Verify old saved scenes/settings still render Native.
9. Run all automated and real OBS acceptance checks, especially independent
   refresh/duplication, source destruction under load, shutdown, storage and
   missing-browser behavior. Address failures before claiming completion.
10. Update architecture/user/test documentation, embedded-resource packaging
    checks and `VERSION` to `0.1.0-alpha.14` once the implementation qualifies.
    Configure, build and run tests on supported platforms; report remaining
    warnings and untested platform checks. Do not commit, push, tag or publish
    without a subsequent explicit request.

Out of scope throughout: StreamElements compatibility or its named callbacks,
`SE_API`, ZIP/widget-package import/library, `fields.txt`, `data.txt`, donation
providers, downloaded widget installation and custom persistent browser profiles.

## N. Git and audit changes

Original branch: `main`, clean working tree, at `30fc8bf` (V2 merge), matching
the locally recorded `origin/main` and tag `v0.1.0-alpha.13`. The V2 branch commit
`65e7aac` is an ancestor of this base. No remote fetch or hosted-release audit
was necessary; this does not claim live remote synchronization.

The required branch did not already exist. Created and switched to
`feature/v3-web-widget-runtime` from that base before any repository file edits.
The sandbox initially prevented writing `.git`; the authorized branch creation
succeeded through the escalation mechanism. No existing work was discarded.

Audit changes only:

- `docs/V3_WEB_WIDGET_RUNTIME_PLAN.md` — this audit and concrete plan.
- `docs/ARCHITECTURE.md` — link to the plan.

Result: feature branch at the same base commit, with the two documentation files
uncommitted. Build outputs remain ignored. No implementation, version change,
commit, push, tag, release, reset or history rewrite was performed.
