# Internal event foundation

Implemented on `feature/v1-event-foundation`, following
[the V1 audit](V1_EVENT_FOUNDATION_PLAN.md). The live EventSub client now publishes
all ten normalized payloads through the central dispatcher. A plugin-owned runtime
shares one Twitch backend across compatible OBS sources and future consumers.
No browser/widget runtime or StreamElements code is included.

## Implemented flow

```text
Twitch Device Flow / Helix subscription setup / EventSub WebSocket
          |
TwitchClient -> normalizeTwitchEvent -> PluginEvent + ingress sequence
          |
OrderedEventPipeline: deduplication, emote/media enrichment, ordered completion
          |
immutable EventPtr -> EventDispatcher
          +--> source A mailbox -> native adapter -> CPU layout -> OBS tick/render
          +--> source B mailbox -> native adapter -> CPU layout -> OBS tick/render
          +--> future consumer mailbox (no additional Twitch connection)
```

See [the producer integration report](TWITCH_PRODUCER_INTEGRATION.md) for the
subscription matrix, ownership/threading, connection conflicts, validation, and
complete changed-file inventory. Source ID and saved property keys are unchanged.

## Model and normalization

`PluginEvent` combines one `EventHeader` with a `std::variant` containing
`ChatMessage`, `MessageDeleted`, `ChatCleared`, `Follow`, `Subscription`,
`Resubscription`, `GiftSubscription`, `CommunityGiftSubscription`, `Cheer`, or
`Raid`. `ChatCleared::user` distinguishes a whole-channel clear from a user clear.
Transport event ID and chat message ID are separate. UTC timestamps have
millisecond precision; the producer supplies ingress sequence, generation, and
receive time. The envelope preserves shared-chat origin/channel/message identity.

`ChatMessage::user` is the canonical user identity, including ID, login, display
name, and optional/invalid color. There are no parallel `userName`/`userColor`
fields. Structured badges retain provider, type, version, info, and an optional
asset URL. Unresolved badges are valid. Ordered fragments are the emote collection;
there is no duplicate emote list. They retain provider, ID, name/text, URLs,
fallback URL, overlay flag, immutable CPU images, and occurrence ranges. Twitch
set/owner/formats, mentions, cheermotes, replies, reward IDs, and notice system text
are typed data. No shared type contains Twitch JSON or HTML.

Ranges use half-open UTF-16 code units into `ChatMessage::text`, matching QString.
Third-party catalog entries have no occurrence range; `EmoteCatalog::apply`
assigns ranges to each occurrence. Existing precedence and aliases/overlays remain
intact. Native Twitch has an explicit provider value but cannot index a third-party
catalog bucket. Mentions and cheermotes cannot be replaced by third-party tokens.
Malformed fragment arrays still fall back to the complete message text.

The pure normalizer accepts a bounded JSON envelope (1 MiB maximum). It returns
`Event`, `Ignored`, or `Invalid`, without performing I/O, requesting images, or
logging input data. Known events require consistent subscription type/version,
valid IDs/timestamps, required variant bodies, and valid counts. Unsupported
versions, controls, and unrelated notices are ignored. Diagnostic strings are
fixed English text. Optional/null data is not fabricated as an identity or count.

Subscription payloads use the four selected chat-notification variants, including
their shared-chat forms. They describe chat-visible subscription activity rather
than subscription accounting. Community summaries and individual recipients remain
distinct and retain their correlation ID. Raid chat notices are ignored in favor
of dedicated Raid events. Anonymous flags suppress sender identities. These
mappings follow the [Twitch EventSub reference](https://dev.twitch.tv/docs/eventsub/eventsub-reference/).

## Dispatcher API and contract

```cpp
EventDispatcher dispatcher; // In production, owned by PluginRuntime.
auto consumer = dispatcher.subscribe({channelId, {EventKind::ChatMessage}});

// On the single producer thread, after normalization/enrichment:
auto event = std::make_shared<const PluginEvent>(std::move(normalizedEvent));
const auto result = dispatcher.publish(std::move(event));

// On the consumer's own thread:
auto batch = consumer.takeBatch(); // Default maximum: 32 events.
for (const auto &item : batch.events) {
    // Process locally. No dispatcher or mailbox lock is held here.
}
consumer.close(); // Also done by its destructor.
dispatcher.shutdown();
```

- Each move-only `EventSubscription` owns an independent mailbox. The registry
  stores only weak references; it stores no consumer callback, QObject, or OBS
  source pointer. Empty/moved-from subscriptions are safely closed.
- Empty channel/kind filters mean all channels/kinds. Filters are immutable;
  change them by closing and creating a subscription. New subscriptions get
  future publication only; no replay/history is retained.
- The first valid publication binds the producer thread. A different thread gets
  `WrongProducerThread`, with no fan-out. `publish` also reports `InvalidEvent`
  for null/valueless input and `RuntimeStopped` after shutdown. `Published` means
  the publication was processed; it does not imply every consumer accepted it
  (a consumer may be filtered, closed, or overflowing).
- Every active matching consumer sees publication order. Timestamps/sequence
  fields are not sorted or rewritten. The dispatcher itself does not deduplicate;
  two calls with the same ID are two publications. Future ingestion owns dedupe
  and ordering across delayed enrichment.
- Registry and mailbox mutexes protect short bookkeeping operations. Fan-out
  snapshots the registry and releases that lock before enqueueing. Consumers
  drain batches outside the producer thread, with no consumer code under locks.
  Discarded event/asset owners are released after locks, including on overflow
  and shutdown, so deleters cannot re-enter a locked dispatcher.
- `close()` and `takeBatch()` may run concurrently on a still-live handle.
  Moving/destroying that handle requires exclusive access to the C++ object.
  Prefer one drainer per mailbox to preserve application processing order.
- Closing clears pending events. A batch already taken remains caller-owned and
  valid; close cannot retract it. Source teardown must invalidate its own
  processing state before closing, as described in the audit. Keeping an event
  alive does not keep its consumer or dispatcher alive.
- Default per-mailbox limits are 256 events and 32 MiB of accounted data. The
  estimator counts retained string/vector capacity and decoded image bytes,
  conservatively charging shared assets per reference. It is not an exact total
  heap bound: allocator overhead, Qt internals, externally retained batches, and
  producer-owned data are outside the queue's accounting. Consumers must bound
  their own retained batches and finish one batch before taking another.
- Exceeding either limit, including one oversized event, clears only that mailbox
  and leaves it terminally `Overflowed`. Other consumers continue. A consumer
  must clear its local presentation and deliberately resubscribe; silently
  retaining a message while dropping its deletion would be unsafe. Zero limits
  reject the first matching publication.
- `shutdown()` is thread-safe and idempotent. It closes active mailboxes as
  `RuntimeStopped`, clears pending events, and rejects later registration/work.
  Existing Closed/Overflowed terminal reasons are preserved. An in-flight publish
  may return Published while shutdown closes its mailboxes, but cannot enqueue
  after shutdown returns. Already drained values remain valid.
- Destroying the dispatcher calls shutdown, and subscriptions can outlive it.
  As with any C++ object, join/quiesce callers before destroying the dispatcher
  itself; concurrent use of a destroyed dispatcher is not supported. This library
  starts no threads, schedules no tasks, and touches no OBS graphics resources.

## Production integration and validation

The [producer integration report](TWITCH_PRODUCER_INTEGRATION.md) documents the
live wiring, file responsibilities, current build/test results and remaining
manual OBS/Twitch checks. `docs/V1_EVENT_FOUNDATION_PLAN.md` remains the historical
pre-implementation audit, rather than being rewritten as an implementation report.

## Version 2 security boundary (not implemented)

Insert the entry boundary in `OrderedEventPipeline::ingest`, immediately after
`normalizeTwitchEvent` returns a successful `NormalizationResult::event`, before
creating a pending ticket or calling `EmoteService::resolve`:

```text
Provider normalization -> Security / Validation Boundary -> Safe Internal Event
                                                               |
                                                       enrichment / dispatch
```

The affected interfaces are `NormalizationResult::event`, the pipeline's pending
`PluginEvent`, `EmoteService::resolve`, and the immutable `EventPtr` construction
in `OrderedEventPipeline::flush` before `EventDispatcher::publish`. Third-party
catalog enrichment introduces additional external text/URLs, so V2 must apply its
policy to those additions before image requests and recheck the completed event
before publication. Today's immutable `EventPtr` is not a security certification.
No security layer, HTML sanitization, browser runtime or widget interface is
implemented by this V1 boundary note.
