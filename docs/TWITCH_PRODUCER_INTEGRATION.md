# V1 Twitch producer integration

Implemented on `feature/v1-event-foundation`. This continues the existing event
foundation and preserves its central dispatcher and normalized model. There is
one plugin-owned Twitch client, not one client per source. No commits, pushes,
merges, tags, branch switches, or destructive Git operations were performed.

## Connected events

“Tested” means offline fixtures passed through the production `TwitchClient`,
normalizer, enrichment pipeline and dispatcher to two independent mailboxes.
It does not claim a live Twitch broadcast or OBS GUI test.

| Event | Producer connected | Tested | EventSub producer |
| --- | --- | --- | --- |
| ChatMessage | Yes | Yes | `channel.chat.message` v1 |
| MessageDeleted | Yes | Yes | `channel.chat.message_delete` v1 |
| ChatCleared | Yes | Yes | `channel.chat.clear` / `channel.chat.clear_user_messages` v1 |
| Follow | Yes | Yes | `channel.follow` v2 |
| Subscription | Yes | Yes | `channel.chat.notification`: `sub` |
| Resubscription | Yes | Yes | `channel.chat.notification`: `resub` |
| GiftSubscription | Yes | Yes | `channel.chat.notification`: `sub_gift` |
| CommunityGiftSubscription | Yes | Yes | `channel.chat.notification`: `community_sub_gift` |
| Cheer | Yes | Yes | `channel.cheer` v1 |
| Raid | Yes | Yes | `channel.raid` v1, incoming channel condition |

The five chat subscriptions require `user:read:chat`. Follow uses
`moderator:read:followers`; Twitch also checks whether the authenticated user can
moderate that broadcaster. Cheer requires `bits:read` and the broadcaster's own
user token. Incoming raid needs no extra scope. Existing tokens without the new
scopes continue to support their authorized events; Connect requests updated
Device Flow authorization. Missing scopes, rejected subscriptions and revocations
are visible in status. Transient subscription failures have bounded retries.
These conditions follow the [Twitch subscription reference](https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/).

Subscription notices are the canonical chat-visible subscription producer. The
client does not also subscribe to `channel.subscribe`, `channel.subscription.gift`
or `channel.subscription.message`. Community summaries and individual gifts are
distinct events, correlated by community gift ID; they are not duplicates to drop.
Raid notices are ignored in favor of the dedicated raid producer. Shared-chat
subscription variants remain covered by normalizer tests.

## Actual flow and ownership

```text
PluginRuntime (plugin lifetime; one selected account/channel)
  -> TwitchClient (Qt application thread)
       Device Flow / token validation / Helix subscriptions / EventSub socket
       -> normalizeTwitchEvent(raw envelope)
       -> ordered ticket: timestamp, sequence, generation, transport ID
       -> deduplicate transport ID; enrich emotes/media; release FIFO
       -> freeze immutable PluginEvent
       -> EventDispatcher
            +-> source A mailbox -> NativeEventAdapter -> CPU layout/queues
            |                                           -> OBS tick/render
            +-> source B mailbox -> independent layout/queues -> OBS tick/render
            +-> future consumer mailbox; no socket or authentication of its own
```

`PluginRuntime::subscribe` exposes the existing dispatcher for future consumers;
it never starts another backend. A subscription is passive; a future standalone
consumer also holds a configuration attachment to keep the selected feed alive
when no OBS source needs it. The source attachment API carries copied settings,
status and token updates. Sources do not receive raw payloads or own a Twitch
client. Network callbacks never call source methods or OBS graphics functions.
The native adapter consumes only normalized events and retains no source pointer.

All client, socket, HTTP, catalog, image decode and runtime drain work runs on the
Qt application thread. Sources can attach, configure and close their handles from
OBS threads. CPU layout reads copied settings and enqueues prepared messages.
OBS tick handles lane scheduling and animation; graphics resource creation,
updates and destruction remain on the OBS graphics path. Source settings/draw
state are serialized, and CPU adapter queues have a separate short-held mutex.
The updater's existing independent lifecycle is unchanged.

The first configured attachment selects the account/channel. Matching attachments
share it. Empty tokens and recognized previous tokens can join the same selected
configuration; refreshed credentials cannot be rolled back by a stale source
snapshot. Token aliases are stored as hashes. Unknown nonempty tokens, different
client IDs and different channels are conservatively treated as conflicts. Saved
settings are not overwritten to resolve a conflict. With no compatible attachment,
a subsequent settings change or Connect request can select a new configuration.
A previously rejected source does not silently take over when another closes.
The last compatible attachment stops the client; removing all attachments resets
selection. This V1 runtime supports one account/channel at a time.

Token updates use an OBS weak-source handle promoted only for the settings update;
the expected client/channel/old-token snapshot is checked before persistence. The
saved property keys and source ID `bokis_twitch_chat_plugin` are unchanged. Status
changes refresh source properties on the application thread. User authorization
codes are UI-only, and neither tokens nor raw external payloads are logged.

## Ordering, duplicates, moderation and teardown

- One production publication call exists, in `OrderedEventPipeline`. The old
  `MessageCallback`/`GifCallback` route and per-source client ownership were removed.
  The legacy GIF extension travels inside `ChatMessage::media`, so it cannot
  bypass dispatch or duplicate a separate GIF delivery.
- Transport IDs are remembered for ten minutes, up to 16,384 entries. Deduplication
  survives socket handoff and ordinary reconnect, and resets on account/channel
  generation changes. It is a bounded replay filter, not durable exactly-once
  storage. New consumers receive future events only.
- Ingress sequence is assigned before asynchronous enrichment. Every event type
  shares the FIFO, so a fast deletion/follow cannot overtake earlier chat waiting
  for an image. Tickets have a 2.5-second deadline, 128-event and 64-MiB accounting
  budgets; pressure/deadline fallback preserves text and available metadata/URLs.
  Oversized decoded assets are removed before dispatcher publication.
- Twitch, 7TV, BTTV and FFZ provider identity, UTF-16 occurrence ranges, asset URLs,
  immutable decoded images, overlays and existing catalog precedence are retained.
  Badges retain type/version/info/provider; badge image lookup and presentation
  remain unimplemented as before. No HTML is generated.
- Native message/GIF queues and active objects retain message/user/channel IDs.
  Deletion, whole-channel clear and per-user clear remove matching native objects;
  textures are retired on the existing graphics path. A bounded 256-control
  history prevents recently deleted/cleared messages from reappearing after late
  delivery. Clear prefers server timestamps over arrival order, including across
  reauthorization generations, and does not erase newer messages.
- Each source has a separate mailbox and renderer state. Mailbox overflow clears
  that source's presentation and resubscribes for new events. Configuration changes
  close old mailboxes and reset the native presentation. Non-chat event payloads
  are available to consumers but do not invent new Floating alert presentation.
- Reconnect handoff keeps the old socket until replacement welcome and reuses
  migrated subscriptions. Fresh reconnect creates subscriptions again. Welcome
  duplicates do not create extra subscriptions. These follow the
  [Twitch WebSocket lifecycle](https://dev.twitch.tv/docs/eventsub/handling-websocket-events/).
- Source destruction first invalidates the shared adapter, then closes its mailbox.
  A copied/drained callback can finish only against shared CPU state and a weak OBS
  handle. It cannot dereference a destroyed `ChatSource`.
- `aboutToQuit` and module unload stop the runtime on its owner thread, cancel HTTP,
  image/catalog work, timers and sockets, delete their QObject-owned deferred work,
  then shut down the dispatcher. Shutdown from another thread waits only as a
  lifecycle operation, never on tick/render. The quit hook also releases a pending
  off-thread shutdown if the application loop is exiting. Module unload is
  idempotent after that hook. The Qt application must exist when loading the plugin.

## Files and responsibilities

The inventory includes the earlier uncommitted event foundation, which was
preserved, plus this producer integration. The audit plan predates implementation.

| Files | Responsibility |
| --- | --- |
| `src/core/event-types.hpp/.cpp` | Existing ten-event envelope, kind mapping and retained-byte accounting. |
| `src/core/event-dispatcher.hpp/.cpp` | Existing bounded fan-out mailboxes and RAII lifecycle. |
| `src/core/ordered-event-pipeline.hpp/.cpp` | New ingress sequencing, replay filter, enrichment, FIFO/deadline/budget publication. |
| `src/core/plugin-runtime.hpp/.cpp` | New plugin-owned client, source attachment/configuration policy, mailbox draining and shutdown. |
| `src/twitch/twitch-client.hpp/.cpp` | Shared producer; authentication, capability-aware subscriptions, controls/reconnect, dispatcher pipeline entry. |
| `src/twitch/eventsub-socket.hpp/.cpp` | WebSocket transport wrapper and offline test seam. |
| `src/twitch/event-normalizer.hpp/.cpp` | Existing pure typed mapping of all ten events and shared subscription variants. |
| `src/twitch/chat-message-parser.hpp/.cpp` | Existing extracted chat/user parser, identity, fragments, badges, metadata and media. |
| `src/chat/chat-types.hpp` | Existing canonical renderer-independent users, chat, badges, emotes and assets. |
| `src/chat/emote-catalog.hpp/.cpp` | Existing provider/range preservation and catalog precedence. |
| `src/chat/emote-service.hpp/.cpp` | Per-job completions, normalized media enrichment and cancellation of catalogs/images. |
| `src/chat/image-cache.hpp/.cpp` | Cancel/reset outstanding work; own reply/timer lifetime. |
| `src/renderer/native-event-adapter.hpp/.cpp` | New CPU-only normalized consumer, copied layout settings, bounded queues and moderation identity. |
| `src/renderer/chat-source.hpp/.cpp` | Replace client with runtime attachment; native queue consumption, token/status handling and moderation. |
| `src/renderer/render-types.hpp` | Message/user/event identity for prepared/active messages and GIFs. |
| `src/renderer/message-layout.cpp` | Earlier change to canonical nested user fields. |
| `src/plugin-main.cpp` | Own runtime across sources; shut down before module resources; guard null properties context. |
| `CMakeLists.txt`, `tests/CMakeLists.txt` | Core/backend/native adapter targets and offline producer suite. |
| `tests/event-dispatcher-tests.cpp`, `tests/event-normalizer-tests.cpp` | Earlier foundation contracts and mapping tests. |
| `tests/chat-tests.hpp/.cpp` | Earlier structured emote/badge/range and catalog regression tests. |
| `tests/twitch-producer-tests.hpp/.cpp` | Production producer/runtime/adapter tests with fake HTTP/WebSockets. |
| `tests/fixtures/eventsub-events.json` | Twelve synthetic realistic envelopes spanning ten events, Unicode and anonymous/null data. |
| `docs/ARCHITECTURE.md`, `docs/EVENT_FOUNDATION.md`, `tests/README.md` | Current architecture, dispatcher contract and test coverage. |
| `docs/TWITCH_PRODUCER_INTEGRATION.md` | This report and exact validation/Git inventory. |
| `docs/V1_EVENT_FOUNDATION_PLAN.md` | Preserved historical audit. |

## Remaining validation and limitations

- Actual OBS source creation/destruction, property refresh/token persistence, native
  GPU behavior and plugin unload still require a manual Linux and Windows check.
  The offline suite tests runtime/adapter ownership, not OBS itself.
- Live Device Flow, real account permissions, Twitch delivery, remote provider
  catalogs and platform WebSocket behavior have not been exercised here. The
  fixture suite uses production logic with fake transports and synthetic tokens.
- Only one account/channel is active at a time. Multi-channel pooling is not
  implemented. Subscription notices represent chat-visible activity rather than
  complete subscription accounting. These are explicit V1 boundaries.
- Twitch does not replay all events missed during an ordinary disconnected period.
  Local deduplication and moderation history are bounded and are not persisted.
- Native badge drawing and presentations for Follow/subscription/Cheer/Raid alerts
  remain outside this producer connection step; normalized data is available.

No browser bridge, web themes, StreamElements mapping or dedicated V2 XSS suite
was added.

## Final review and stabilization (2026-09-26)

Architecture review confirmed one shared producer and one dispatcher publication
path, structured emotes/badges, consistent string IDs/UTC timestamps, and no new
German developer identifiers or accidental browser/StreamElements/widget code.
All ten events and existing consumer/lifetime/mapping suites remain covered.

Concrete fixes in this pass:

- Delayed clear events no longer remove newer messages merely because they arrived
  earlier; clears also cover older visible messages across reauthorization.
- Returning drained native work now enforces the 128-message/30-GIF limits and
  rechecks moderation. A queue revision rejects work invalidated by reset/close,
  including an in-flight layout or a batch already drained by OBS.
- Attachment close releases callback captures and mailbox ownership outside its
  mutex. Connection setup/log callbacks also run outside attachment locks, so
  status reads cannot deadlock. Focused regressions cover both reentrant paths.

Review edits: `src/core/plugin-runtime.cpp`,
`src/renderer/native-event-adapter.hpp/.cpp`, `src/renderer/chat-source.cpp`,
`tests/twitch-producer-tests.hpp/.cpp`, `docs/EVENT_FOUNDATION.md` and this report.
The [V2 insertion point](EVENT_FOUNDATION.md#version-2-security-boundary-not-implemented)
is documented only; no V2 implementation was added.

## Validation (2026-09-26)

All commands below passed against the final implementation:

```bash
cmake --preset linux-x86_64 -DENABLE_TESTS=ON
cmake --build --preset linux-x86_64 -j 4
ctest --test-dir build/linux-x86_64 --output-on-failure
```

- Plugin and all test targets built with `-Wall -Wextra -Wpedantic`.
- **8/8 suites passed**: chat, dispatcher, normalizer, updater, post-exit,
  manifest, Linux installer and Twitch producer.
- The producer suite has **18 test methods**, reported as **20 passed** including
  setup/cleanup. Tests cover the ten-event fixture matrix and shared runtime,
  reconnect/deduplication, failed/missing subscriptions, Unicode/assets/media,
  ordered fallback, token lifecycle, source conflicts/configuration replacement,
  independent overflow recovery, moderation, cancellation and off-thread shutdown.
- No compiler warnings or Qt runtime warnings appeared in the final build/test
  logs. CMake still reports the pre-existing optional missing
  `WrapVulkanHeaders` / `Vulkan_INCLUDE_DIR` dependency.
- `git diff --check` passed. Static call-site review found one production
  `TwitchClient` construction in the runtime and one production dispatcher
  publication in the ordered pipeline, with no old source callbacks remaining.

Additional ASan/UBSan validation:

```bash
cmake -S . -B /tmp/bokis-v1-event-asan -DENABLE_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wpedantic'
cmake --build /tmp/bokis-v1-event-asan --target chat-tests \
  event-dispatcher-tests event-normalizer-tests twitch-producer-tests -j 4
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/bokis-v1-event-asan \
  -R '^(chat|event-dispatcher|event-normalizer|twitch-producer)-tests$' \
  --output-on-failure
```

The final review sanitizer build used `-j 4`; **4/4 selected suites passed**
with no sanitizer findings. Leak detection was disabled because the earlier
sandbox LeakSanitizer exit check failed; this is not a leak-check pass. The earlier
foundation TSan run reported system QtTest teardown diagnostics and was not clean;
this step does not claim new TSan coverage. No live Twitch, OBS GUI/GPU, or Windows
validation was performed.

## Final Git inventory

`git branch --show-current` returned `feature/v1-event-foundation`.
`git status --porcelain=v1 -uall` returned the following; ` M` means an unstaged
tracked modification and `??` means an untracked file. Nothing is staged. This
includes preserved earlier foundation changes, not just files introduced in this
step.

```text
 M CMakeLists.txt
 M docs/ARCHITECTURE.md
 M src/chat/chat-types.hpp
 M src/chat/emote-catalog.cpp
 M src/chat/emote-catalog.hpp
 M src/chat/emote-service.cpp
 M src/chat/emote-service.hpp
 M src/chat/image-cache.cpp
 M src/chat/image-cache.hpp
 M src/plugin-main.cpp
 M src/renderer/chat-source.cpp
 M src/renderer/chat-source.hpp
 M src/renderer/message-layout.cpp
 M src/renderer/render-types.hpp
 M src/twitch/twitch-client.cpp
 M src/twitch/twitch-client.hpp
 M tests/CMakeLists.txt
 M tests/README.md
 M tests/chat-tests.cpp
 M tests/chat-tests.hpp
?? docs/EVENT_FOUNDATION.md
?? docs/TWITCH_PRODUCER_INTEGRATION.md
?? docs/V1_EVENT_FOUNDATION_PLAN.md
?? src/core/event-dispatcher.cpp
?? src/core/event-dispatcher.hpp
?? src/core/event-types.cpp
?? src/core/event-types.hpp
?? src/core/ordered-event-pipeline.cpp
?? src/core/ordered-event-pipeline.hpp
?? src/core/plugin-runtime.cpp
?? src/core/plugin-runtime.hpp
?? src/renderer/native-event-adapter.cpp
?? src/renderer/native-event-adapter.hpp
?? src/twitch/chat-message-parser.cpp
?? src/twitch/chat-message-parser.hpp
?? src/twitch/event-normalizer.cpp
?? src/twitch/event-normalizer.hpp
?? src/twitch/eventsub-socket.cpp
?? src/twitch/eventsub-socket.hpp
?? tests/event-dispatcher-tests.cpp
?? tests/event-normalizer-tests.cpp
?? tests/fixtures/eventsub-events.json
?? tests/twitch-producer-tests.cpp
?? tests/twitch-producer-tests.hpp
```
