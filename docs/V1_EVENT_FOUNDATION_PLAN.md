# V1 event foundation: repository audit and implementation plan

Audit date: 2026-09-26. Baseline: `82ec873`.
This document is the deliverable for the architecture step. All proposed types,
files, APIs, and behavior below are **designs, not implemented features**.

## Scope and repository state

- Original branch: `fix/windows-emoji-rendering`.
- Initial workspace: clean, including staged and untracked files.
- Base: the original branch tip, `82ec873`, which includes local `main` at
  `c3222f5` plus three Windows dependency/updater/emoji fixes. Branching from
  `main` would have omitted those fixes. This conclusion uses local Git refs;
  no remote fetch was necessary or performed.
- Created and switched to `feature/v1-event-foundation`; it did not already
  exist among local branches or locally recorded remote branches.
- No commits, pushes, resets, tags, releases, or history changes were performed.
- Only this plan is added. No production code, settings, or build definitions
  are changed. Build outputs remain ignored.

The next implementation should establish one plugin-owned Twitch backend,
normalized events, independent consumers, and safe source teardown. It should
retain native Floating rendering and existing settings. V1 supports multiple
sources observing the same backend/channel; simultaneous distinct Twitch
accounts/channels require a later configuration design. That limitation must be
visible when loading conflicting legacy source settings, never silently resolved
by overwriting them.

Exclude Browser/CEF rendering, OBS Browser integration, StreamElements APIs,
`onWidgetLoad`, `onEventReceived`, `SE_API`, widget instances/packages, ZIP import,
HTML/DOM/sanitization, URL-policy work, `fields.txt`, and `data.txt`. Existing
theme placeholders do not authorize implementing any of these. No version bump
or release work belongs in this step.

## Current event flow

```text
obs_module_load -> register source type
  sourceCreate -> ChatSource (one per OBS source)
    -> TwitchClient + UpdateChecker (one of each per source)
    -> source settings -> configure -> token validation/refresh or Device Flow
    -> broadcaster lookup -> QWebSocket -> EventSub welcome
    -> POST channel.chat.message subscription
    -> parseTwitchMessage -> EmoteService
       -> channel/global FFZ, BTTV, 7TV catalogs
       -> ImageCache -> CPU image decoding -> ordered ChatMessage callback
    -> ChatSource::enqueueMessage -> layoutMessage (CPU Qt text shaping)
    -> mutex-protected PreparedMessage queue
    -> OBS tick: consume, place, move, advance animation
    -> OBS render: create/update/destroy textures and draw

Separate legacy path:
  fragment named "gif" -> image loading -> DecodedGif callback -> bouncing GIF
```

There is no IRC client, central backend, event bus, general observer registration,
or worker `QThread` in the inspected source. Qt objects acquire the thread of
construction; the current Twitch code does not assert or marshal its ownership
thread. Network callbacks perform CPU layout but no OBS graphics operations.
`pendingMutex_` protects prepared/GIF queues, and `activeMessageCount_` is atomic.
Those protections do not cover all mutable source settings or status.

`ChatMessage` currently retains only display name, color, text, and fragments.
The parser drops chat message ID, timestamp, user ID/login, badges, reply context,
and other message metadata. `ChatFragment` already preserves emote ID, fallback
text, primary/fallback URLs, overlay behavior, and immutable shared CPU images.
Provider identity exists while selecting catalogs but is lost from fragments.
Badges have no existing parser, catalog, renderer, or tests.

`EmoteService` preserves chat arrival order despite asynchronous assets. It uses
a 2.5-second message deadline, a 50 ms flush timer, a 64-message pending limit,
weak pending-job references, and a generation guard for catalog changes. At
capacity it delivers the oldest message early. `ImageCache` coalesces downloads,
limits concurrent requests to six, bounds downloads/decoded images/cache use,
and supports static fallback and negative caching. Decoding is synchronous CPU
work on its Qt owner thread. Images retained by messages can outlive cache
eviction; the cache's budget is not a global process-memory bound.

Source destruction currently resets the updater, disconnects/destroys its Twitch
client, and frees textures inside an OBS graphics context. Plugin unload only
removes the bundled font. There is no plugin-wide Twitch shutdown procedure.

## Relevant files and reuse decisions

Paths in paired rows are individually relevant; no changes to unrelated updater
or installer internals are proposed.

| Path | Current purpose | Relevance and action for V1 |
| --- | --- | --- |
| `AGENTS.md` | Repository constraints | Keep C++20, RAII, source ID/settings compatibility, native graphics ownership, and build gates. |
| `src/plugin-main.cpp` | Module load/unload, source registration and callbacks, defaults, font | Own runtime lifetime here; inject its handle at source creation. Preserve `bokis_twitch_chat_plugin` and defaults. |
| `src/twitch/twitch-client.hpp` | Per-source client, callbacks, Qt members, OAuth state | Change backend ownership and callback contract; add explicit thread/lifetime contract and capability state. |
| `src/twitch/twitch-client.cpp` | Device Flow, validation/refresh, broadcaster lookup, socket, single subscription | Reuse HTTP/auth code; isolate normalizer, expand subscriptions, make reconnect/cancellation explicit. |
| `src/chat/chat-types.hpp` | `ChatMessage`, `ChatFragment`, `DecodedImage`, `ImageAsset` | Extend the existing canonical model; do not add a competing chat-message representation. |
| `src/chat/emote-catalog.hpp` | Provider enum, parser declarations, precedence buckets | Move shared provider identity to chat types; keep explicit third-party bucket mapping. |
| `src/chat/emote-catalog.cpp` | Twitch fragments and provider catalogs/token expansion | Reuse normalization and precedence; retain providers, source ranges, and Twitch-specific asset metadata. |
| `src/chat/emote-service.hpp` | Pending jobs and async completion contract | Add ticketed completion/cancellation for an ordered event pipeline. |
| `src/chat/emote-service.cpp` | Catalog refresh, deadline, image enrichment | Reuse once per backend, not per consumer; preserve timeout/text-fallback behavior. |
| `src/chat/image-cache.hpp` | CPU asset ownership/cache API | Reuse immutable assets across consumers; keep GPU handles out. |
| `src/chat/image-cache.cpp` | Credential-free fetching, bounds, decoding, animation helper | Reuse; add explicit cancellation only as needed for runtime shutdown. |
| `src/renderer/chat-source.hpp` | Per-source network ownership, queues, settings and renderer state | Replace Twitch ownership with consumer/attachment handles; retain lane and animation state locally. |
| `src/renderer/chat-source.cpp` | Source lifecycle, UI buttons, token persistence, CPU preparation, OBS rendering | Small integration seam for shared backend and event consumption; preserve drawing and movement. |
| `src/renderer/render-types.hpp` | Prepared and GPU-backed renderer records | Carry message/user IDs for moderation without moving renderer state into core. |
| `src/renderer/message-layout.hpp` | CPU layout and inline asset records | Keep layout interface using canonical `ChatMessage`. |
| `src/renderer/message-layout.cpp` | Unicode shaping, inline emotes, diagnostic category | Retain layout; only change accesses if user identity becomes nested. No new badge layout required in V1. |
| `src/updater/update-checker.hpp` | QObject updater and callback lifetime | Reference only; updater remains separate. |
| `src/updater/update-checker.cpp` | Guarded queued OBS UI notifications | Reuse its weak-lifetime/queued-notification pattern conceptually; do not put event dispatch in the updater. |
| `tests/chat-tests.hpp` | Qt Test slots | Extend existing model/provider regression cases. |
| `tests/chat-tests.cpp` | Fake network, parsing, assets, layout, ordering/destruction | Reuse fake replies and offscreen tests; extract fake transport helpers if shared by new tests. |
| `tests/fixtures/animation.gif`, `tests/fixtures/animation.webp` | Generated animated fixtures | Preserve asset/animation regression coverage. |
| `tests/updater-tests.cpp` | Stubbed OBS UI queue and lifetime tests | Pattern for deterministic source/runtime adapter tests; preserve suite. |
| `tests/post-exit-tests.cpp`, `tests/post-exit-windows-tests.cpp` | Platform updater lifecycle coverage | Existing regression gates; no event-related edits expected. |
| `tests/manifest-tests.py`, `tests/linux-installer-tests.py`, `tests/windows-installer-tests.ps1` | Packaging/install verification | Preserve gates; avoid coupling new event targets to release code. |
| `CMakeLists.txt` | C++20 module, `chat-support`, updater targets, dependencies | Add OBS-independent event/runtime testable targets; preserve existing targets. |
| `tests/CMakeLists.txt` | Qt Test/CTest registration, platform setup | Register event, dispatcher, and backend lifecycle tests. |
| `CMakePresets.json` | Linux/Windows build presets | Reuse, with `ENABLE_TESTS=ON`; no new dependency needed for the design. |
| `cmake/QtTest/CMakeLists.txt` | Windows QtTest dependency build | Keep new suites compatible with existing Windows test setup. |
| `scripts/build-local.sh` | Local configure/build entry point | Existing path; does not enable tests itself. |
| `.github/workflows/ci.yml`, `.github/workflows/windows-build.yml` | Linux/Windows configure/build/test jobs | New CTest targets should run through existing jobs. |
| `tests/README.md` | Test commands, coverage and manual limitations | Document new lifecycle tests and two-source smoke checks. |
| `docs/ARCHITECTURE.md` | Intended shared architecture and current emote pipeline | Distinguish aspiration from implementation; update when runtime actually lands. |
| `docs/ROADMAP.md` | Later renderers/themes and shared core | Keep event foundation aligned without implementing future presentation work. |
| `README.md`, `data/locale/en-US.ini` | User setup/property text | Later explain shared connection conflicts/capabilities; preserve current property keys. |
| `resources/emoji.qrc`, `resources/emoji-windows.qrc`, `resources/fonts/README.md` | Platform font assets and rationale | Preserve Windows fixes inherited from the chosen base. |
| `resources/web-themes/README.md`, `resources/web-themes/classic-default/README.md` | Future theme placeholders | Explicitly outside implementation scope. |

## Architectural issues that affect this change

1. **Backend multiplicity:** two sources produce two auth flows, sockets,
   subscriptions, catalogs, and caches. Merely adding a static dispatcher would
   still duplicate ingestion. Backend ownership must move with distribution.
2. **Identity loss:** deletion cannot target a message, user clearing cannot
   target an author, and duplicate detection cannot use transport IDs today.
   Renderer preparation also discards semantic identity.
3. **Raw callback lifetime:** Twitch callbacks capture `this` without a QObject
   receiver context. Network/socket/timer members outlive some callback/string
   members during reverse member destruction. `disconnect()` does not cancel
   outstanding OAuth/lookup/subscription replies or invalidate their results.
   This is a teardown/reconfiguration hazard, not a reproduced crash in this audit.
4. **Implicit thread assumptions:** settings/status/raw source captures rely on
   callback scheduling that is not enforced. Adding central consumers must not
   introduce direct calls into sources from a network thread. Queue mutexes alone
   do not make `ChatSource` thread-safe.
5. **Incomplete session lifecycle:** the old socket is closed before reconnect
   completes; every welcome subscribes again. There is no keepalive watchdog,
   reconnect backoff, revocation state, request-generation guard, or duplicate
   transport-event cache. These directly affect a persistent shared backend.
6. **Assets affect ordering:** publishing moderation immediately but publishing
   chat only after image completion could resurrect deleted messages. A single
   ordering policy must cover all event types and delayed asset work.
7. **Per-source credentials:** token refresh currently calls `obs_source_update`
   from a source-capturing callback. A shared backend must not oscillate between
   stale tokens saved on different sources or re-enter auth on token persistence.
8. **Coverage boundary:** tests exercise fragment parsing and emote-service
   teardown, but not `TwitchClient`, EventSub sessions, source creation/destruction,
   concurrent consumers, or module shutdown. No live OBS/Twitch run was made.

Existing logs use `blog` with `[bokis-twitch-chat-plugin]`; layout has the optional
`bokis.render.emoji` Qt category. Keep that convention through an injected log
sink for OBS-independent core tests. Add bounded diagnostics for ignored event
types, dropped duplicates, overflow, capabilities, and lifecycle failures. Do
not log payloads, tokens, authorization headers, or OAuth codes. Existing Device
Flow status currently includes a code and is logged; separate the UI status from
the log record while touching that auth path.

## Event coverage and Twitch normalization boundary

The repository recognizes only `channel.chat.message`, plus the separate `gif`
fragment branch. It does not already distinguish follow/subscription/cheer/raid
variants. The subscription family below deliberately uses chat notifications as
its one authoritative feed: these are chat-visible occurrences, not a complete
subscription billing ledger or silent-renewal stream.

| Internal payload | Canonical input | V1 meaning |
| --- | --- | --- |
| `ChatMessage` | `channel.chat.message` v1 | One authored chat message. |
| `MessageDeleted` | `channel.chat.message_delete` v1 | Target message and author. |
| `ChatCleared` | `channel.chat.clear` / `channel.chat.clear_user_messages` v1 | Whole channel or one user's messages. |
| `Follow` | `channel.follow` v2 | Follower and occurrence time. |
| `Subscription` | `channel.chat.notification` v1: `sub` | New subscription notice. |
| `Resubscription` | Same: `resub` | Resubscription notice with optional text. |
| `GiftSubscription` | Same: `sub_gift` | Individual recipient; optional community correlation. |
| `CommunityGiftSubscription` | Same: `community_sub_gift` | Batch summary, not additional individual gifts. |
| `Cheer` | `channel.cheer` v1 | Bits alert, including anonymous cheering. |
| `Raid` | `channel.raid` v1 | Incoming raid; ignore duplicate raid chat notices. |

For WebSocket chat subscriptions use `user:read:chat`, broadcaster ID, and the
authenticated user ID. Follow needs `moderator:read:followers` and an eligible
moderator/broadcaster identity. Cheer needs broadcaster authorization with
`bits:read`. Incoming raids use `to_broadcaster_user_id` and need no additional
scope. A user token is still needed for WebSocket subscription creation.
The alternative `channel.subscribe`, `channel.subscription.message`, and
`channel.subscription.gift` require `channel:read:subscriptions`; do not subscribe
to these in parallel with the chosen notice feed.
Source: [Twitch subscription types](https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/).

Validate token scopes and identity; represent unsupported capabilities explicitly.
Keep existing chat-only tokens usable. Optional event authorization failure must
not stop chat. Offer Device Flow reauthorization when the user enables additional
capabilities; never assume a refreshed token gains new scopes. Track desired,
pending, enabled, failed, and revoked subscriptions separately, with bounded retry
for transient errors and no retry storm on authorization failures.

Preserve null identities for anonymous gifts/cheers, Prime information separately
from tier, months, community gift linkage, and structured notice text. Retain
shared-chat origin and source message identity where supplied. Mentions and
cheermotes are fragment metadata, not new event types. Badge set/version/info and
emote owner/set/format information remain structured.
Source: [Twitch event reference](https://dev.twitch.tv/docs/eventsub/eventsub-reference/).

Do not add types for every notice. Gift/Prime paid upgrades, pay-it-forward,
announcements, unraid, badge-tier notices, charity, streaks, and anniversaries are
deferred with an ignored-type counter; none is separately implemented today.
Shared-chat versions of the four selected subscription notices map to the same
payload types with origin metadata. Unknown variants produce no fabricated event.
User-targeted clear is a scope of `ChatCleared`, avoiding another top-level type.

The existing `gif` fragment path is not established by the documented chat
fragment schema inspected here. Preserve its current compatibility behavior as
an optional message media attachment handled by the native adapter; do not
invent a Twitch `GifEvent`, claim it is an official event, or remove the test GIF
button. Fixture-test that branch separately as legacy behavior.

## Proposed C++ model

Use existing global type names, Qt value types, `std::vector`, and explicit
ownership. The declarations are interface sketches; headers/includes and helper
constructors would be supplied during implementation.

### Extend `src/chat/chat-types.hpp`

```cpp
struct ChatUser {
    QString id;
    QString login;
    QString displayName;
    QColor color; // Invalid means unavailable; renderer supplies its fallback.
};

enum class EmoteProvider { Twitch, FrankerFaceZ, BetterTTV, SevenTV };
enum class BadgeProvider { Twitch };
struct TextRange { qsizetype offset = 0; qsizetype length = 0; };

struct ChatBadge {
    BadgeProvider provider = BadgeProvider::Twitch;
    QString type;
    QString version;
    QString info;
    QUrl imageUrl; // Empty until resolved; type/version are a usable asset key.
};

struct TwitchEmoteMetadata {
    QString setId;
    QString ownerId;
    bool supportsStatic = false;
    bool supportsAnimated = false;
};

struct CheermoteMetadata { QString prefix; int bits = 0; int tier = 0; };
struct ReplyMetadata { QString parentMessageId; QString threadMessageId; };
struct ChatMetadata {
    QString messageType;
    QString systemText; // Separate from user-authored text in a notice.
    std::optional<ReplyMetadata> reply;
    std::optional<int> cheerBits;
    QString rewardId;
};

struct ChatMedia {
    QUrl imageUrl;
    ImageAsset image;
};

struct ChatMessage {
    QString messageId;
    ChatUser user;
    QString text;
    std::vector<ChatFragment> fragments;
    std::vector<ChatBadge> badges;
    std::vector<ChatMedia> media;
    ChatMetadata metadata;
    // Keep a convenience constructor for synthetic messages/tests.
};
```

Retain the current `ChatFragment` fields and add `std::optional<EmoteProvider>
provider`, `std::optional<TextRange> sourceRange`, optional
`TwitchEmoteMetadata`, optional `ChatUser mention`, and optional
`CheermoteMetadata`. Existing Text/Emote classification and renderer fallback can
remain; do not reinterpret mentions or cheermotes as third-party token emotes.
Use existing `text` as the emote's displayed name, including channel aliases.
No duplicate `emotes[]` is needed: ordered emote fragments are that collection.

Ranges are half-open UTF-16 code-unit ranges into `ChatMessage::text`, matching
`QString`; assign occurrence ranges during token expansion, never in catalog
templates. Validate concatenated fragment text and retain the existing whole-text
fallback on mismatch. Test supplementary Unicode and combining sequences. Any
future serializer must explicitly convert range units for its target consumer.

Move `EmoteProvider` out of `emote-catalog.hpp` and replace its current ordinal
array indexing with an explicit FFZ/BTTV/7TV bucket function. Simply inserting
`Twitch` into the enum would corrupt the six-bucket indexing/precedence.
Provider-specific flags already interpreted as `zeroWidth`, aliases, selected
URLs, and fallback URLs remain intact; do not retain entire provider JSON objects.

Replace `userName`/`userColor` with `user.displayName`/`user.color` in the few
layout/test accesses. Do not keep two mutable copies of user identity. Retain
`DecodedImage`, `ImageAsset`, and `DecodedGif`; CPU image sharing is useful and
does not introduce GPU state. Badges may remain unresolved asset keys in V1;
badge catalog fetching and badge drawing are not required for this foundation.

### Add `src/core/event-types.hpp`

```cpp
struct EventHeader {
    QString eventId; // EventSub transport notification ID, not chat message ID.
    QDateTime timestamp; // Transport timestamp, UTC, millisecond precision.
    QDateTime receivedAt;
    QString channelId; // Delivery channel.
    QString originChannelId; // Empty when no different origin was supplied.
    QString originMessageId;
    std::uint64_t sequence = 0; // Local ingress order, assigned once.
    std::uint64_t generation = 0; // Configuration generation, not socket ID.
};

struct MessageDeleted { QString messageId; ChatUser user; };
struct ChatCleared {
    std::optional<ChatUser> user; // Absent: entire channel. Present: this user.
};
struct Follow { ChatUser user; QDateTime followedAt; };

enum class SubscriptionTier { Unknown, Tier1, Tier2, Tier3 };
struct SubscriptionTerms {
    SubscriptionTier tier = SubscriptionTier::Unknown;
    std::optional<bool> isPrime;
    int durationMonths = 0;
};
struct GiftActor {
    bool anonymous = false;
    std::optional<ChatUser> user;
};
struct Subscription { ChatMessage notice; SubscriptionTerms terms; };
struct Resubscription {
    ChatMessage notice;
    SubscriptionTerms terms;
    int cumulativeMonths = 0;
    std::optional<int> streakMonths;
    bool isGift = false;
    std::optional<GiftActor> gifter;
};
struct GiftSubscription {
    ChatMessage notice;
    GiftActor gifter;
    ChatUser recipient;
    SubscriptionTerms terms;
    QString communityGiftId;
    std::optional<int> cumulativeTotal;
};
struct CommunityGiftSubscription {
    ChatMessage notice;
    GiftActor gifter;
    SubscriptionTier tier = SubscriptionTier::Unknown;
    QString communityGiftId;
    int count = 0;
    std::optional<int> cumulativeTotal;
};
struct Cheer {
    std::optional<ChatUser> user; // Absent for anonymous cheering.
    int bits = 0;
    QString text;
};
struct Raid { ChatUser from; ChatUser to; int viewers = 0; };

using EventPayload = std::variant<ChatMessage, MessageDeleted, ChatCleared,
    Follow, Subscription, Resubscription, GiftSubscription,
    CommunityGiftSubscription, Cheer, Raid>;
struct PluginEvent { EventHeader header; EventPayload payload; };
using EventPtr = std::shared_ptr<const PluginEvent>;
```

The envelope carries the one timestamp/channel/sequence record for chat and
other events; no duplicate timestamp is required in `ChatMessage`. Synthetic
test messages receive locally generated IDs and an envelope when used as test
events. Keep the existing per-source test buttons source-local.

`notice` reuses structured chat content rather than introducing a second rich
text model. Its user is the notice author; a gift recipient has an independent
role. `ChatMetadata::systemText` preserves system-generated notice wording
separately from authored `text`. Missing
anonymous identities stay missing. Validate required IDs/counts and required
variant objects; missing required data is a parse failure, not a zero-filled
valid event. Unknown optional enum values may map to `Unknown`.

Raw `QJsonObject`/`QJsonDocument` belongs only in Twitch/provider normalization
and fixtures. The dispatcher and source adapters see `PluginEvent` and typed
backend status snapshots only. Authentication/session/status events are control
state, not additional chat-domain variant alternatives.

## Dispatcher, ownership, and delivery contract

Use a central dispatcher with **per-consumer mailboxes**, rather than invoking
arbitrary source callbacks. This makes unsubscribe independent of callback
execution and avoids a raw pointer from the backend to a renderer.

```cpp
enum class EventKind {
    ChatMessage, MessageDeleted, ChatCleared, Follow, Subscription,
    Resubscription, GiftSubscription, CommunityGiftSubscription, Cheer, Raid
};
struct EventFilter {
    QString channelId;
    std::vector<EventKind> kinds; // Empty means all payload types.
};
struct QueueLimits {
    size_t maxEvents = 256;
    size_t maxBytes = 32 * 1024 * 1024;
};
enum class ConsumerState { Active, Closed, Overflowed, RuntimeStopped };
struct EventBatch {
    std::vector<EventPtr> events;
    ConsumerState state = ConsumerState::Active;
};

class EventSubscription { // Move-only RAII handle to a shared mailbox.
public:
    EventSubscription() noexcept; // Empty, closed handle.
    EventSubscription(EventSubscription &&) noexcept;
    EventSubscription &operator=(EventSubscription &&) noexcept;
    EventSubscription(const EventSubscription &) = delete;
    EventSubscription &operator=(const EventSubscription &) = delete;
    ~EventSubscription();
    EventBatch takeBatch(size_t limit = 32);
    void close() noexcept;
};

class EventDispatcher {
public:
    EventSubscription subscribe(EventFilter filter, QueueLimits limits = {});
    void publish(EventPtr event); // Only the serialized producer thread calls this.
    void shutdown() noexcept;
};
```

`subscribe` and mailbox operations use short locks. Dispatcher registration holds
weak mailbox references; the subscription owns its mailbox. Handle destruction
marks it closed and clears the queue under its lock, without calling back into
the dispatcher. It remains safe after the dispatcher dies. No OBS pointer,
QObject pointer, or consumer callback is stored in the dispatcher.

Publication snapshots live mailbox references under the registry lock, releases
that lock, then appends under each mailbox lock. Every append rechecks closed
state. No user code, logging sink, or network operation runs under these locks.
Expired registrations are pruned on subscribe/publish. Serialize all publication
on the producer thread; callers from elsewhere must enqueue producer commands.
This avoids a different publication order in different consumers.

`takeBatch` moves a bounded batch out and releases its lock before processing.
Already taken batches are caller-owned immutable values: close cannot revoke
them. Consumer teardown must invalidate its own processing state before closing
the mailbox. Calling methods on a C++ handle concurrently with destruction of
that same object remains invalid; synchronize handle ownership or operate through
the shared adapter state, rather than claiming RAII makes that race safe.

New consumers receive only future publication, with no history replay. Each
healthy consumer receives every matching event once within the deduplication
window and queue limits. Filters are fixed for a subscription; channel changes
close and replace it. No acknowledgement, durable storage, or network exactly-once
guarantee is proposed.

On overflow, close and clear only the affected mailbox, set `Overflowed`, and
report the condition. The adapter clears its local presentation and may explicitly
resubscribe for future events. Do not silently lose a deletion while retaining
the deleted message. Slow consumers must not stall ingestion or other mailboxes.
Count semantic bytes and conservatively charge referenced decoded image bytes
per event, even when images are shared. An event larger than the byte limit also
causes explicit overflow; no unbounded oversized-event exception. A consumer may
hold only one drained batch at a time, bounded by the same 32 MiB limit. These
initial limits are policy defaults to measure with representative asset traffic.

## Backend and source lifecycle

### Runtime and configuration

Add a plugin-owned `PluginRuntime` and a `TwitchService`. The runtime owns the
dispatcher; the service owns one `TwitchClient`, its enrichment pipeline, and
shared catalogs/cache. OBS sources own consumer/attachment handles, never clients.
One steady-state EventSub connection serves all matching sources. A reconnect
handoff may temporarily require two sockets inside this one backend.

For V1, keep network, enrichment, and Qt CPU layout on the Qt application thread,
with async I/O. Create their QObjects there; assert thread affinity in service
entry points. Do not move the current value-member QObjects to a worker thread
by moving their enclosing C++ object. OBS callbacks on another thread submit
copied commands through a lifetime-gated UI executor and return promptly.

Choose one active configuration: the first complete legacy source configuration
attached at startup establishes it. Normalize channel login for comparison, and
validate account identity before treating another credential set as equivalent.
Equivalent sources join; stale copies cannot replace a current refreshed token.
Conflicting sources retain all saved keys and receive a connection-conflict
status, with no events from the wrong channel. They do not open another socket.
While other attachments use the active configuration, a conflicting Connect
request fails visibly; it does not retarget their connection. Once no matching
attachments remain, the next explicit Connect may select a different configuration.
Unconfigured sources can wait for configuration without initiating authorization.

Last attachment removal stops the connection/timers and cancels work, but the
runtime object survives until module unload. Source visibility does not change
backend lifetime. Attach/detach during rapid scene changes is serialized; a
generation token invalidates work from a stopped or replaced configuration.

Retain `client_id`, `channel`, `access_token`, `refresh_token`, and every renderer
property key. Queue token persistence through an OBS adapter using weak source
references; promote only for the duration of a UI update, then release. Update
only matching attachments. A closed attachment rejects persistence, even if the
underlying OBS source still exists. Tag internal persistence so `update()` cannot
reconfigure/restart auth recursively. No new credential storage is needed for V1.

### Safe native consumption

Add a small `SourceEventAdapter` on the Qt application thread. It drains its
mailbox in bounded timer batches and calls existing CPU layout logic using a
copied renderer-settings snapshot. It captures shared CPU preparation state,
never raw `ChatSource *`. The source and adapter share a mutex-protected pending
queue, an active flag, a settings snapshot, and one subscription handle. Access
to that handle (drain or close) is serialized through the shared state's lock;
the adapter never borrows a handle member from a destructible source. Finish
layout outside locks, then recheck active/generation before enqueueing. Status
is also read through a synchronized snapshot. Apply copied renderer-setting
updates on OBS tick so UI writes cannot race video-thread reads of render fields.

Source destruction first marks this shared state inactive and closes delivery,
then queues adapter removal on its owner thread. Outstanding CPU work may finish
against its retained shared state but cannot dereference or enqueue into a dead
source. The adapter has no GPU resources. Do not block OBS tick/render waiting
for Qt work, network replies, or callback completion.

The Floating renderer consumes chat/media as today; activity payloads can remain
unrendered in V1 while independent test consumers verify delivery. Add only the
IDs and ordered moderation commands necessary to remove pending/live messages.
Carry message ID, author ID, channel, and generation through prepared/rendered
records. Apply deletion/clear commands on OBS tick; defer texture destruction to
render, using existing graphics ownership. Do not add badge drawing, alert
animations, change lane scheduling, or implement another renderer.

### Shutdown contract

1. Close the runtime command gate so late source commands are rejected.
2. Mark all source-adapter state inactive; close mailboxes and detach token/status
   sinks. Cancel their timers on the Qt owner thread.
3. Mark Twitch stopping and increment configuration generation **before** aborting
   replies; abort may emit completion synchronously. Stop device, watchdog,
   reconnect, catalog, and flush timers. Disconnect completion contexts, cancel
   tracked HTTP/asset work, close both handoff sockets, and clear pending events.
4. Explicitly destroy client/service QObjects on their owning thread while its
   event loop is still available. Add a QObject lifetime context to Twitch
   connections, but do not rely only on base QObject destruction: disable
   callbacks at the start of teardown, before dependent members are destroyed.
5. Shut down the dispatcher; surviving subscription handles become inert. Drain
   or cancel posted integration tasks before releasing module code and fonts.

Do not leave `deleteLater` work or raw plugin function pointers queued past module
unload. Source teardown never waits for the UI executor; final module shutdown
must establish its completion barrier outside graphics locks. Implementation must
verify actual OBS load/unload/source callback threads on Linux and Windows and
test the owner-thread and off-thread cases. Current code does not establish this
contract. If OBS's unload point is too late for a UI barrier, install an earlier
supported shutdown hook; do not assume frontend API availability (`OFF` today).
This lifecycle check belongs in the first runtime integration work package.

## Ordering, duplicates, and asset completion

Parse complete EventSub envelopes in `src/twitch/event-normalizer.*`; return a
typed result distinguishing supported event, ignored type, and invalid payload.
Retain transport ID separately from chat ID. Deduplicate before sequence assignment
and before catalog/image work. Use a bounded ID cache (initial proposal: 10 minutes,
16,384 entries) keyed by active account/configuration and transport ID; preserve it
across socket reconnects, clear on account/channel replacement. This is a bounded
best-effort guarantee, not perpetual deduplication.

Twitch can resend notifications with the same transport ID. Handoff should keep
the old socket until the replacement welcome and retain migrated subscriptions;
a fresh session after a disconnect recreates subscriptions. Lost events during a
disconnect are not replayed. Add a keepalive watchdog, bounded reconnect backoff,
and revocation handling. These are backend responsibilities shared by all sources.
Sources: [WebSocket handling](https://dev.twitch.tv/docs/eventsub/handling-websocket-events/)
and [WebSocket message IDs](https://dev.twitch.tv/docs/eventsub/websocket-reference/).

Maintain one ordered pending-event deque in `OrderedEventPipeline`. Every accepted
event receives a sequence before enrichment. Non-chat events are immediately
ready; chat/notice/media events complete by ticket when assets resolve, or use
text/URL fallback at deadline. Release only the ready prefix. Adapt
`EmoteService` to complete tickets rather than publishing directly. A completion
may be synchronous on a cache hit, so allocate/insert its ticket first and use
the existing assembly guard pattern.

Preserve the 2.5-second asset deadline as a starting point. Bound the entire event
pipeline to 128 events and 64 MiB of retained semantic/decoded data, not just chat
jobs; on pressure, finalize the oldest asset-dependent event with fallback and
release the ready prefix. Keep URL/text data but drop decoded attachments when
retaining them would exceed the budget. Reject an oversized semantic envelope
with a bounded diagnostic before allocating its normalized structures. Late completions cannot mutate
published const events. Preserve provider tokenization/URLs even if image decoding
times out; when initial catalogs are unavailable, plain text remains the fallback.
Do not emit a second ChatMessage when an image eventually arrives.

This deliberately delays later moderation/activity behind earlier asset work by
at most the pipeline deadline plus scheduling delay. It is the simplest V1
ordering policy that retains current native readiness. Test and measure it. A
later asset-independent publication design could reduce latency, but should not
silently replace this contract during implementation.

Consumers observe local ingress order, not a claimed global Twitch occurrence
order. Do not sort by timestamps or wait indefinitely for missing messages.
Keep bounded deletion tombstones and channel/user clear cutoffs in the native
adapter so a late-arriving older chat message cannot reappear. Compare message
IDs for deletion and envelope timestamps for clear cutoffs, retaining ingress
sequence as the local tie-breaker. Equal-time older/ambiguous messages may be
suppressed conservatively. State the same bounded horizon as the dedupe cache.
Generation changes clear stale preparation/render state, without removing a
newer generation's messages.

Community summaries and individual gifts are different facts: retain both with
their common community ID and never add the summary count to individual counts.
Do not heuristically deduplicate by username, text, amount, or timestamp.
`Cheer` is an activity and a cheer-bearing `ChatMessage` is chat content; only the
dedicated activity feeds alert consumers. Subscription notices yield their typed
payload once, not an additional synthetic `Subscription` from another feed.

## Implementation order for the next prompt

Each work package should remain buildable and retain the existing chat tests.

| Order | Files / work package | Completion evidence |
| --- | --- | --- |
| 1 | Extend `src/chat/chat-types.hpp`; update catalog header/implementation, layout field access, synthetic messages/tests; add `src/core/event-types.hpp`. | One canonical model; IDs/users/providers/badges/ranges survive parsing; existing layout behavior passes. |
| 2 | Add `src/twitch/event-normalizer.hpp/.cpp`, `tests/event-normalizer-tests.cpp`, synthetic EventSub fixtures. | Every required payload and malformed/unknown/anonymous/shared-origin case tested without OBS or credentials. |
| 3 | Add `src/core/event-dispatcher.hpp/.cpp`, `tests/event-dispatcher-tests.cpp`. | Independent fan-out, RAII close/move, shutdown, concurrent mailbox operations, filtering and overflow verified. |
| 4 | Add `src/core/ordered-event-pipeline.hpp/.cpp`; adapt `src/chat/emote-service.*` ticket completion; test with fake clock/transport. | Mixed event order, inline completion, timeout/pressure fallback, dedupe and late completion are deterministic. |
| 5 | Add `src/core/plugin-runtime.*`, `src/twitch/twitch-service.*`; refactor `src/twitch/twitch-client.*` ownership, QObject callback contexts, injected transport/logging and explicit stop. | Lifecycle harness verifies Qt affinity and OBS integration thread assumptions before expanding production subscriptions. |
| 6 | Generalize subscription/session management in Twitch service/client, using the normalizer and pipeline; add `tests/twitch-service-tests.cpp`. | One auth/session setup for two attachments; capability matrix, cancellation generations, reconnect handoff, retries and revocation verified. |
| 7 | Add `src/renderer/source-event-adapter.*`; modify `src/renderer/chat-source.*`, `src/renderer/render-types.hpp`, `src/plugin-main.cpp`. | Sources subscribe/detach safely; settings/tokens retained; existing Floating rendering and test buttons work; minimal moderation integration. |
| 8 | Update `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/README.md`, `docs/ARCHITECTURE.md`, applicable setup/status documentation as each target lands. | Existing and new CTest suites pass on Linux/Windows; document manual OBS checks and actual guarantees. |

For build organization, add an OBS-independent `event-core` static target for
types/dispatcher/pipeline and a testable `twitch-support` target for service/client/
normalizer. Reuse `chat-support` for existing CPU enrichment. Remove direct OBS
logging dependency from Twitch through an injected sink; keep OBS bridging in the
plugin module. No new external library is required. Avoid a large reshuffle of
the existing layout target merely to perfect target naming.

## Required V1 tests and acceptance criteria

| Area | Required cases |
| --- | --- |
| Normalization | All ten payloads; both clear scopes; invalid/missing IDs and variant bodies; unknown types/versions; nullable anonymous identities; Prime/tier/months; community linkage; IDs/timestamps not confused; shared origin; malformed fragments retain text. |
| Structured assets | Twitch, FFZ, BTTV, 7TV identity; unchanged precedence/aliases/overlays; source ranges across Unicode; badges survive without URLs; emote owner/set/formats; mentions/cheermotes not replaced by provider tokens; no HTML or raw JSON in core types. |
| Dispatcher | Two and three consumers receive identical ordered pointers independently; close one without affecting others; move/double-close/default empty handle; dispatcher dies first; register/close during publication; takeBatch/close race; filter isolation; no callback execution under locks. |
| Pressure | One slow consumer overflows without affecting a fast consumer; bounded bytes/counts including large assets; explicit presentation reset; pipeline pressure releases fallback in order; batch retention bounded. |
| Ordering | Slow chat followed by delete/clear/follow; cached synchronous image completion; reversed image completion; late reply after timeout; channel change with pending jobs; delete before delayed chat arrival; no post-clear resurrection. |
| Duplicates | Same transport ID before and after handoff; cache expiry/capacity; identical text with different IDs remains distinct; summary and recipient gift events stay distinct; raid notice does not duplicate dedicated Raid. |
| Auth/backend | Two sources cause one validate/lookup/subscription set; missing optional scopes preserve chat; stale refresh results ignored; no parallel Device Flow polling/refresh attempts; conflicting settings never open a second backend or overwrite saved settings. |
| Session | Fresh welcome versus reconnect welcome; old socket retained until handoff succeeds; no resubscription on transferred session; keepalive expiry; revocation; bounded retries; session loss status; shutdown during HTTP/socket callbacks. |
| Ownership | Destroy source during queued delivery, layout, catalog/image/auth load, and token persistence; taken batches outlive mailbox safely; active asset references survive cache eviction; all weak source promotions released. |
| Shutdown | No graphics calls in producer/CPU adapter; stop idempotent; last source detaches; module stops with consumers outstanding; owner/off-thread stop; queued plugin tasks finished/cancelled before unload; no callback after dependent member teardown. |
| Compatibility | Source ID and property keys unchanged; existing saved scene and tokens load; refreshed tokens persist to matching sources without recursive auth; different per-source font/lane settings remain independent; source-local test buttons stay local. |

Extract fake HTTP helpers from existing tests if useful. Use a fake socket/session
transport and injectable clock rather than real Twitch access or wall-clock
sleep-heavy tests. Run AddressSanitizer for lifecycle tests and ThreadSanitizer
where supported; these are targeted new concurrency checks, not substitutes for
documented ownership. Production GPU behavior still needs manual OBS preview.

Manual acceptance: load two sources with the same channel and different Floating
settings, verify one backend connection, remove either source while traffic/assets
are active, recreate it, and close OBS during reconnect/loading. Verify moderation,
reopen the scene, and confirm credentials and settings remain intact. Repeat on
Windows; this Linux audit cannot validate Windows lifecycle/font behavior.

## Baseline validation and handoff

Executed against unchanged production code on the new branch:

```text
cmake --preset linux-x86_64 -DENABLE_TESTS=ON
cmake --build --preset linux-x86_64
ctest --test-dir build/linux-x86_64 --output-on-failure
```

Configuration and incremental build passed. CTest passed **5/5 suites**:
`chat-tests`, `updater-tests`, `post-exit-tests`, `manifest-tests`, and
`linux-installer-tests` (5.28 seconds total). Configuration reported
`Could NOT find WrapVulkanHeaders (missing: Vulkan_INCLUDE_DIR)`; it was nonfatal.
No compiler warnings appeared in this incremental build. Windows and live OBS /
Twitch testing were not run. These results establish the existing baseline;
they do not validate the proposed event architecture.

Final intended Git state for this audit: branch `feature/v1-event-foundation`,
with only untracked `docs/V1_EVENT_FOUNDATION_PLAN.md` added and no staged changes.
The next prompt should implement the ordered work packages above, beginning with
model/normalizer/dispatcher tests, and keep all excluded widget/browser work out
of scope. Resolve the OBS shutdown-executor contract in the lifecycle harness
before calling shared runtime integration complete.
