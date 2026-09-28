# V4 StreamElements compatibility and ZIP import audit

Audit date: 2026-09-27. Target implementation release: `0.1.0-alpha.17`.
This document records the pre-implementation fixture audit and the design that
guided the V4 runtime/importer work. Sections labeled proposed or planned retain
that historical wording; they are not a claim of completed visual acceptance.

## Findings that affect acceptance

The real Scrapbook export is a StreamElements Custom Widget, not a Streamer.bot
widget. Its chat, follow, subscription, gift and cheer handlers can consume an
adapter above `window.BokiChat`. The current generic event DTO already carries
most necessary data. The following limitations must remain visible in planning:

1. No production badge URL resolver exists. This explains the V3 text fallback.
2. The widget requires jQuery, field values, and platform template substitution.
   Simply loading its original JS/CSS plus dispatching events is insufficient.
3. It has **no raid handler** and **does not read subscription message text**.
   A correct adapter alone cannot make those visible in this unchanged widget.
4. It handles deletion by message or user, but no channel-wide clear event.
5. Its HTML interpolation is less safe than the bundled V3 DOM renderer.
   V2 validation is not HTML escaping; the compatibility boundary needs its own
   sink-specific protection without changing generic DTOs or original scripts.
6. Exact visual parity needs platform template expansion. Literal prohibitions
   on rewriting JS/CSS also constrain generated runtime derivatives. This audit
   does not assume permission to transform executable widget source. Resolve
   this design constraint before implementing a template compiler.

## A. Actual fixture and ZIP structure

Found outside the repository at `/home/boki/Downloads/ScrapbookChatWidget8.zip`.
`/home/boki/Downloads/ScrapbookChatWidget8(1).zip` is byte-identical.
Neither archive was modified, copied into the repository, or executed.
No Scrapbook fixture was found under the repository/local fixture directories.

Archive SHA-256:
`a1b8c3c174837ec5659198518199648c3052c47bfc88faeacb0aecdf978a1cca`.
Archive size: 137,936 bytes; expanded total: 137,360 bytes. All six entries
are at the root and use ZIP stored compression (compressed size equals size).

| Actual entry | Bytes | Role |
|---|---:|---|
| `widget.ini` | 127 | Maps HTML/CSS/JS/FIELDS/DATA sections to the five files below |
| `html.txt` | 34 | One `div.main-container` |
| `css.txt` | 6,760 | Layout, colors, animations; unresolved field placeholders |
| `js.txt` | 113,023 | All widget logic, inline SVG artwork, five test-message objects |
| `fields.txt` | 14,536 | JSON field schema and defaults |
| `data.txt` | 2,880 | JSON saved field values |

There are no directories, standalone images, fonts, or bundled libraries.
SVG artwork is embedded in JS template strings. `widget.ini` explicitly names
`html.txt`, `css.txt`, `js.txt`, `fields.txt`, and `data.txt`; do not infer an
`index.html` entry. Fixture line references below refer to these original files.

| Entry | SHA-256 |
|---|---|
| `widget.ini` | `fd113ef94590b8d5f603d157973126c34c01731d70a9539db719b153f8286a6f` |
| `html.txt` | `d60a4200c6f5124f1ebd383a0494991a144a583a7e1594f869c9c64117f4b9ee` |
| `css.txt` | `15af4c4ee5cc1e55e4eb7efe36c8e134495abeb145816c324e29ac459604689f` |
| `js.txt` | `def8d6f0946e7ab565fe9f5fee73ffa73738452140d0d0e0104a0411687a38fb` |
| `fields.txt` | `45da13bf16e42efd6bbb1a37ed9fa34b7213b1284dbb30a1a225b1bbfd3632a8` |
| `data.txt` | `8d612de6e6717d1d5e2d6a098b1e3e161cc62b25fc09ff825eae546c8bcbd03c` |

## B. Complete platform and browser inventory

Inspection covered the entire JS file, including its SVG strings and embedded
test objects, plus HTML, CSS, fields and data.

| Dependency/access | Actual use |
|---|---|
| `window.addEventListener('onWidgetLoad', ...)` | One async handler, JS 1155–1194; no await inside |
| `window.addEventListener('onEventReceived', ...)` | One synchronous handler, JS 824–1151 |
| `SE_API` | **No accesses or calls** |
| `fieldData` | Load settings, chat filters, alert switches; see I |
| `channel` | Reads `detail.channel.username`; assigned `channelName` is not subsequently used |
| `currency` | Reads `detail.currency.symbol` unconditionally; used for tip display |
| `session`, `recents`, `onSessionUpdate` | No reads/listeners |
| Listeners handled | `message`, `delete-message`, `delete-messages`, `follower-latest`, `subscriber-latest`, `tip-latest`, `cheer-latest` |
| Editor buttons | Checks `detail.event.listener === 'widget-button'`, then `event.field`; nested listener differs from normal outer routing |
| Button field names | `testtestMessage`, `testMessageLong`, `testMessageSub`, `testMessageVip`, `testMessageMod` |
| Gift/resub behavior | Branches on `bulkGifted`, `gifted`, `isCommunityGift`, `amount` under `subscriber-latest` |
| Raid/host/redemption | No JS handler; similarly named saved data does not establish support |
| jQuery | Required global `$`; selectors, `$.parseHTML`, DOM insertion/removal, `.html`, `.css`, `.find`, `.children`, `.first`, `.last`, `.not`, `.hide`, `.slideToggle`, `.fadeOut` |
| Remote fonts | Constructs Google Fonts CSS2 imports for `msgFont` and `namesFont`; fonts may then fetch from Google's font host |
| Remote images | Event-provided badge URLs and emote URLs; embedded test objects contain Twitch badge URLs |
| Other networking | No `fetch`, AJAX, XMLHttpRequest, WebSocket, or provider catalog requests in fixture |
| Remote libraries | None declared or bundled; relies on platform-provided jQuery |
| Browser storage | No local/session storage, cookies, IndexedDB, or SE store access |
| Timers | No explicit `setTimeout`, `setInterval`, or `requestAnimationFrame`; jQuery animations schedule internally |
| Animations | jQuery slide/fade plus CSS keyframes and dynamically assigned inline styles |
| DOM | `.main-container`, `.message-row`, `.alert-row`, inline SVG, message-ID classes and unquoted attribute selectors |

SVG namespace URIs are not outbound API requests. Load JS as a classic script:
the original assigns some undeclared globals; forcing strict mode/modules would
change behavior. The font code inserts a nested `<style>` string into a style
element; real CEF validation must check whether fonts load. CSS also contains a
`//` comment at line 280, which is not valid CSS comment syntax. Record upstream
quirks; do not silently repair the source.

## C. Event compatibility matrix

Here `data` means the current `WebEventSerializer` event's `data`, and `header`
means its header. These are proposed adapter mappings, not implemented APIs.
Field names read by Scrapbook are established by the fixture, not inferred from
another widget. Official event documentation corroborates the general envelope
and listener conventions: [StreamElements widget events](https://docs.streamelements.com/overlays/events).

### Chat and moderation

| StreamElements field | Required by Scrapbook? | Plugin source | Conversion needed | Missing? |
|---|---|---|---|---|
| `event.data.msgId` | Yes, DOM identity | `data.messageId` | Stable safe identifier projection; same mapping on deletion | No source gap |
| `event.data.userId` | Yes, user deletion | `data.user.id` | Safe identifier projection | No source gap |
| `event.data.displayName` | Yes | `data.user.displayName` | HTML-safe presentation for this fixture's interpolation | No source gap |
| `event.data.text` | Yes | `data.text` | Preserve plain text; fixture escapes it itself | No |
| `event.data.badges` | Yes, array even if empty | `data.badges` | Map each item; preserve order/count | URLs unresolved |
| `event.data.badges[].type` | Yes, role styling | `badges[].type` | Copy validated token | No |
| `event.data.badges[].url` | Yes, native badge images | `badges[].imageUrl` | Copy only real validated URL; omit image-bearing entry if unavailable, diagnose loss of role presentation | Resolver absent |
| `event.data.badges[].version` | Present in samples, not read | `badges[].version` | Copy | No |
| `event.data.tags.badges` | Yes, empty-string check | badge type/version | Comma-join `type/version`, empty string for none | Derived |
| `event.data.emotes[].name` | Yes if emotes | fragment `text` | Copy occurrence name | No |
| `event.data.emotes[].urls[1/2/4]` | Yes if emotes | fragment `imageUrl` | Use same valid URL for each scale initially | No true scale variants |
| `event.msgId` (`delete-message`) | Yes | `MessageDeleted.messageId` | Reuse message key projection | No |
| `event.userId` (`delete-messages`) | Yes | `ChatCleared.user.id` | Reuse user key projection | No |
| Channel-wide clear | No such handler | `ChatCleared.user == null` | Fan out user deletions from bounded instance ledger; reload if ledger completeness lost | Adapter work |

Do not send `userId: null` to mean clear-all: Scrapbook would select only a
literal null sender. Ledger entries must cover every dispatched chat user until
clear/reset; if a configured cap is exceeded, reset/reload before forgetting
entries. A reload also clears alert rows; document this stronger reset behavior.

### Channel events

| StreamElements field | Required by Scrapbook? | Plugin source | Conversion needed | Missing? |
|---|---|---|---|---|
| `follower-latest: event.name` | Yes when enabled | `Follow.user.displayName` | Safe presentation | No |
| `subscriber-latest: event.name` | Yes for normal/resub | `notice.user.displayName`; gift recipient for individual gifts | Select actor by variant | No |
| `subscriber-latest: event.amount` | Yes; `> 1` means resub | Subscription: 1; Resubscription: `cumulativeMonths`; community: `count` | Variant-specific; never use duration as cumulative months | No |
| `subscriber-latest: event.gifted` | Yes | Event variant / `Resubscription.isGift` | Boolean | No; gift-resub presentation may differ |
| `subscriber-latest: event.bulkGifted` | Yes | CommunityGiftSubscription variant | True for aggregate, false otherwise | No |
| `subscriber-latest: event.isCommunityGift` | Yes | GiftSubscription `communityGiftId` | True for linked child gifts | No when correlation exists |
| `subscriber-latest: event.sender` | Yes for gifts | `gifter.user.displayName` | Anonymous display fallback when `anonymous`; never invent identity | No |
| `subscriber-latest: event.message` | **Not read** | `notice.text` | Preserve viewer text, not `metadata.systemText` | Widget display gap |
| Subscription tier/streak fields | **Not read; exact fixture field names absent** | `terms.tier`, `isPrime`, `streakMonths`, `durationMonths` | Keep in generic DTO; establish wider SE contract before adding aliases | No core gap; no verified SE alias here |
| `cheer-latest: event.name` | Yes when enabled | `Cheer.user.displayName` | Anonymous display fallback | No |
| `cheer-latest: event.amount` | Yes | `Cheer.bits` | Integer | No |
| `cheer-latest: event.message` | Not read | `Cheer.text` | Preserve text in adapter where contract supports it | Widget display gap |
| `raid-latest: event.name` | **No raid handler** | `Raid.from.displayName` | Safe presentation | Widget display gap |
| `raid-latest: event.amount` | **No raid handler** | `Raid.viewers` | Integer | Widget display gap |
| `tip-latest: event.name/amount` | Yes, if delivered | No plugin tip event | Do not fabricate tips from Twitch events | Unsupported producer |

Community aggregates show sender/count; linked individual gifts are suppressed
by this fixture. Single gifts display the sender, not recipient. Gifted resubs
can take the gift branch instead of resub branch; preserve actual gift state
rather than forcing a cosmetic branch. `amount` is not a universal months field.

### Other chat metadata: actual gaps versus unused data

Login is available as `user.login` (possible `nick` mapping); display color as
`user.color` (possible `displayColor`); event time as `header.timestamp`
(ISO UTC to epoch milliseconds if needed). Scrapbook does not read these, even
though its samples contain color and role tags. It gets role styling from the
**first badge**, and role count from badge array length. There is no explicit
roles object read. Moderator/subscriber/broadcaster flags can be derived from
validated badge types when a future consumer requires them; do not claim
unavailable IRC metadata is original Twitch data.

`metadata.messageType` survives in the DTO but is not used by Scrapbook; do not
equate it with an `isAction` flag without a verified mapping. Raw IRC tags,
badge descriptions, channel login in each event, and all emote scale variants
are absent. Channel login can be supplied through safe instance bootstrap.
Badge description is unnecessary here. Provider names, zero-width flags and
Twitch animation capability are present in fragments but ignored by the widget.

## D. Badge root cause and repair location

The complete production path is:

1. `src/twitch/chat-message-parser.cpp:35` stores badge set ID as `type`, badge
   ID as `version`, and `info`; it deliberately initializes `imageUrl` empty.
2. `src/core/ordered-event-pipeline.cpp` validates ingress, enriches via
   `EmoteService`, validates again, then publishes immutable `PluginEvent`.
3. `src/chat/emote-service.cpp` resolves emote catalogs, fragment images and
   media. It never resolves badges. Repository search found no Helix badge
   catalog request or production assignment to `ChatBadge::imageUrl`.
4. `src/core/event-validation.cpp:195` retains valid badge metadata and accepts
   valid URL syntax only on `static-cdn.jtvnw.net` for badge images.
5. `src/web/web-event-serializer.cpp:68` serializes a valid image URL as
   `imageUrl`; an empty/disallowed URL becomes JSON null. Serialization is
   already capable of preserving real badge URLs.
6. `resources/web-themes/development/widget.js:17` displays an image when URL
   exists; otherwise it displays the badge type. Image load failure also
   replaces the image with type text.

Therefore this is **not only a synthetic-badge issue**, and adding a
StreamElements adapter does not fix it. The current synthetic producer creates
no badges at all; test fixtures can carry metadata without URLs. Exact
capitalization of the reported labels was not reproduced in OBS, but the missing
production resolver is confirmed in source. Network/image failures remain a
second possible fallback cause once resolution is implemented.

Add a credential-owning backend badge catalog service using Twitch's global and
channel badge endpoints, keyed by channel/set/version with channel overrides.
Helix provides image URLs; do not derive badge UUID URLs from badge type or copy
Scrapbook's sample URLs. See [Twitch badge reference](https://dev.twitch.tv/docs/api/reference/#get-channel-chat-badges).
Fetch asynchronously, bound responses/timeouts, cancel on channel generation
changes, cache immutable snapshots, and fill URLs in the existing enrichment
stage before final validation. Never expose Helix credentials to the browser.
Handle origin-channel custom badges deliberately for shared chat; do not resolve
them against an unrelated channel. Failed lookups keep metadata without a fake
image. Native graphics ownership and dispatch order stay unchanged.

Scrapbook reads `type` and `url`, not a badge name alone. It can render real URLs,
but its saved defaults replace broadcaster/moderator/VIP/partner and subscriber
images with inline SVG. Test real badge images with `badgesCustom=false`, and
subscriber images with appropriate custom-badge settings disabled. Custom SVG
appearance is not evidence that real URL resolution works.

## E. Emote conversion and animation

`msgDiv`, JS 1198 onward, HTML-escapes message text, compares whitespace-delimited
tokens with emote names, and emits images from `urls[1]`; emote-only messages
prefer `[4]` for one entry or `[2]` for multiple entries. It special-cases malformed
FFZ duplicate protocol strings. It does not consume IDs, ranges, provider type,
`gif`, animation metadata or zero-width flags.

| Provider | Existing data | Proposed conversion and limitation |
|---|---|---|
| Twitch | ID, text, UTF-16 range, animated/default/static URL, static fallback, Twitch format flags | Preserve chosen primary URL; browser animates it; do not substitute static fallback preemptively |
| 7TV | ID, token, range, provider URL, zeroWidth | Map name/URL; Scrapbook has no overlay/zero-width positioning |
| BTTV | ID, token, range, provider URL | Map name/URL; preserve GIF/animated resource |
| FFZ | ID, token, range, provider URL | Map already normalized URL; no need to introduce fixture's malformed protocol |

Use validated primary URL in all three URL slots when only one resolution is
available. Retain fallback internally for a controlled load-error policy. The
DTO does not carry a universal animation boolean for third-party providers;
do not guess from a URL suffix. Supplying the original animated image resource
preserves animation without decoding it into a static bitmap.

For broader SE emote consumers, map half-open UTF-16 `offset,length` to inclusive
`start=offset,end=offset+length-1`; reject absent/invalid ranges rather than invent
positions. Provider enum spelling conversion must be explicit. Scrapbook's
name-based matching cannot distinguish identical names from different providers
or partial-token occurrences. Its sizing also uses array length, so preserve
occurrences rather than silently deduplicating. These are fixture limitations,
not reasons to replace core structured fragments with HTML.

## F. onWidgetLoad payload and startup order

Exact minimal structure this fixture dereferences:

```js
new CustomEvent('onWidgetLoad', {detail: {
  fieldData: resolvedFieldValues,
  channel: {username: configuredChannelLogin},
  currency: {symbol: ''}
}})
```

`symbol: ''` means currency unavailable, not fabricated USD/EUR. Tips have no
producer. If tips are added later, require an actual currency configuration.
No API token is included. Never emulate `channel.apiToken` with a Twitch token.

| Requirement | Classification |
|---|---|
| `fieldData` | Safely generated from field schema + saved data + instance overrides |
| `channel.username` | Available from configured login; new safe bootstrap plumbing needed |
| `currency.symbol` | Required object access; partial emulation with explicit empty value |
| `session` / `recents` | Unused and unnecessary for basic chat; omit, do not fabricate historical totals |
| Platform channel IDs/API token | Not required; remote SE account identity is unsupported |

Wrapper sequence: create DOM/body and package base path; load pinned, reviewed
jQuery; install BokiChat bridge and compatibility adapter; load original classic
JS after dependencies; await bridge welcome and script-load completion; dispatch
one load event; then release buffered events in FIFO order. Bound pre-load events
and reset on overflow. Current bridge sends ready immediately after welcome, so
adapter buffering/readiness needs explicit handling. Async widget load handlers
are not awaited by `dispatchEvent`; this fixture performs its setup synchronously
before returning its Promise. Future async widgets need a documented readiness
contract. Reload destroys the instance environment, listeners and timers.

For this fragment export, the separate generated wrapper owns the document and
includes the original HTML fragment unchanged, links the original CSS through a
role-aware resource URL, and loads the original JS as an external classic script.
It supplies the bootstrap and adapter separately. Do not splice bootstrap into
`html.txt` or `js.txt`. A generic package containing a complete document needs a
separate embedding/bridge contract; do not assume every ZIP is an HTML fragment.

## G. onEventReceived and presentation safety

The implemented compatibility boundary HTML-encodes viewer-controlled identity
and alert presentation strings: login/display name, badge description,
subscription message, cheer message, and channel-event names/senders. Numeric
entities preserve ordinary Unicode and display the original value as text when
Scrapbook passes it through `$.parseHTML`. Chat message text and emote names stay
as semantic text because Scrapbook's `msgDiv` applies `html_encode` to both; an
adapter encoding there would double-encode ampersands and display entities.
IDs, booleans, numeric values, badge/emote structure, and validated URLs remain
typed data and are not converted into HTML.

Dispatch on `window`, matching the actual listener target:

```js
new CustomEvent('onEventReceived', {detail: {
  listener: 'message', event: {data: mappedChat}
}})
new CustomEvent('onEventReceived', {detail: {
  listener: 'delete-message', event: {msgId: mappedMessageId}
}})
new CustomEvent('onEventReceived', {detail: {
  listener: 'delete-messages', event: {userId: mappedUserId}
}})
new CustomEvent('onEventReceived', {detail: {
  listener: 'subscriber-latest', event: mappedSubscription
}})
```

Channel listeners carry direct event fields, not `event.data`. Normal
subscriptions use `amount:1`, false gift flags; resubs use cumulative months;
individual gifts use recipient name/sender/gift flags; community aggregates use
sender/count/`bulkGifted:true`. Follow, cheer and raid names follow C.
Preserve subscription text in `event.message` even though this widget ignores
it; preserve tier/streak/Prime data on BokiChat without inventing undocumented
SE field names. Visible subscription text or raids require a separate,
explicitly identified wrapper presentation extension, or an upstream widget
that supports them. Do not mislabel a raid as a follow/sub to trigger rendering.

For optional fixture editor buttons only, supply both the normal outer listener
and its expected `event.listener`, plus `event.field`. Five buttons use the
fixture's own static preview data; these are not proof of real Twitch mapping.

Security finding: chat text is escaped by `html_encode`, but display names,
alert names/senders, IDs and URLs are interpolated into HTML/selector strings.
V2 accepts plain text containing markup and IDs containing punctuation; trusted
transport does not make those strings safe HTML. The Scrapbook presentation
profile must HTML-escape names/senders and attribute-bound URLs exactly once,
keep message text raw for the widget's own escaping, and consistently project
message/user IDs to collision-free prefixed safe tokens per instance. Badge type
is already an ASCII token; keep that restriction. Do not mutate BokiChat data.
Because other widgets may use textContent, this presentation policy is
fixture-specific, not a universal SE escaping rule. Test the actual sinks.
Unknown imported widgets remain executable third-party code; V2 alone cannot
guarantee safe rendering in arbitrary widget DOM code.

## H. SE_API plan and diagnostics

The complete fixture inventory is empty: **zero SE_API methods/properties**.
Thus no API call is required to start chat. Do not build an account service to
support methods this fixture never calls. Official APIs have semantic behavior,
including persistent account storage, so successful fabricated values would be
misleading: [SE_API reference](https://docs.streamelements.com/overlays/custom-widget).

| Access | Used by Scrapbook? | Signature / result | V4 plan / safe fallback |
|---|---|---|---|
| `store.get` | No | `(key) -> Promise<object>` | Unsupported remote storage; reject with named error and diagnostic |
| `store.set` | No | `(key,value)`, no documented return | Unsupported; throw named error; never pretend persistence |
| `counters.get` | No | `(counterName) -> Promise<{id,count}>` | Unsupported; reject, not fabricated zero |
| Unknown path | No | Sync/async contract unknown | Log access; callable sentinel throws on invocation; do not invent return type |

Implement a namespace Proxy with explicit supported-method registry. Known async
unsupported methods return rejected Promises; known sync methods throw.
Unknown property paths retain their full path in a diagnostic sentinel. Return
undefined for `then` and handle symbol inspection explicitly to avoid accidental
thenable assimilation. Block prototype-mutating paths; do not accumulate
unbounded diagnostic keys. Warn once per API path per instance, then summarize
suppressed repetitions. Mark supported/unsupported capabilities in plugin-owned
diagnostics rather than claiming unknown feature-detection checks are reliable.
Any later local store must be labeled instance-local emulation, not SE account
storage, and needs its own bounded persistence design.

Use package and instance IDs in every compatibility log, e.g.
`[StreamElements][Package:<id>][Instance:<id>] onWidgetLoad dispatched`.
Log unsupported APIs/listeners, unavailable badge counts, dependency load errors,
template issues and resets. Omit viewer payloads, capability URLs and tokens.
Unsupported producer types such as tips should be reported as capability status,
not as a fabricated incoming event. Bound browser-to-host diagnostic messages.

## I. Fields, saved data, and unresolved templates

`fields.txt` contains 102 entries: 42 colorpicker, 25 hidden, 10 checkbox,
10 text, 6 dropdown, 5 button, 2 googleFont, 2 number. There are 71 explicit
`value` defaults. Button/hidden entries are UI metadata; `msgLimit` is a checkbox
without a default. `data.txt` contains 97 saved values, including 25 keys not in
the schema (legacy settings). It is neither an event preview array nor an SE
store/session snapshot.

Directly read field values are:
`hideCommands`, `ignoredUsers`, `alignMessages`, `msgHideOpt`, `badgesCustom`,
`subBadgeCustom`, `largeEmotes`, `badgesDisplay`, `msgLimit`, `msganim`,
`msgLimitAmount`, `msgFont`, `namesFont`, `alertson`, `alertsfollower`,
`alertssub`, `alertsdonation`, `alertsbits`.

Defaults relevant to startup: fonts `Quicksand`, alignment `bottom`, animations
and large emotes `on`, hide commands `yes`, ignored users `StreamElements,OtherBot`,
badge options true, alerts enabled, subs/tips/cheers true, follower alerts false,
hide false/7 seconds, message limit amount 7. Saved data changes follower alerts
to true and limit amount to 3, explicitly sets limit false, and changes some
hex-color letter casing. Schema defaults are enough to initialize JS, but saved
data must be merged to preserve the supplied widget's behavior.

Proposed precedence: explicit schema `value` -> validated matching saved value
-> validated instance override. Preserve booleans/numbers/strings; validate
dropdown membership and field types; preserve unknown keys in immutable source
but do not blindly activate legacy settings. Missing checkbox defaults can be
false with a documented import diagnostic; do not overwrite explicit false/0/"".
Reject prototype-related object keys, deeply nested/oversized JSON and invalid
root types. No visual editor is needed for V4; a small set of fixture test
overrides can exist in the test harness.

Template fields in CSS include fonts, weights, colors, hide time and SVG fills.
JS contains literal `{followerAlertMessage}`, `{subAlertMessage}`, gift/resub/
cheer/tip labels, role colors, SVG colors, `{msgHide}` and `{msgLimitAmount}`.
Those are distinct from real JavaScript `${...}` interpolation. `fieldData`
alone does not substitute them. A naive brace regex would also damage the JS
Unicode regex `\p{C}`. `stikersFillcolor` appears in SVG templates but has no
field or saved value; diagnose it rather than inventing a value.

The [platform field documentation](https://docs.streamelements.com/overlays/widget-structure)
describes field substitution; the exact single-brace syntax here is directly
observed in the fixture. Under the strict no-rewrite requirement, keep original
JS/CSS bytes untouched and expose unresolved-template diagnostics. A separate
wrapper can supply styles or presentation extensions, but cannot transparently
fix arbitrary selectors and JS strings without some form of template evaluation
or interception. A later design decision must choose between explicit runtime
template compilation into disposable derivatives (never overwriting originals)
and constrained wrapper emulation with documented loss of parity. This audit
does not silently authorize either as a complete solution.

## J. Secure ZIP importer and resource serving

Use a dedicated ZIP importer, separate from the update/release installer. A
maintained ZIP library must be selected and pinned with license/build review;
do not shell out to an unrestricted extraction command. Start with these limits:

| Resource | Proposed hard limit |
|---|---:|
| Input archive | 32 MiB |
| Total expanded bytes | 128 MiB |
| Single expanded file | 16 MiB |
| Entries, including directories | 2,048 |
| Path length / component | 512 / 128 UTF-8 bytes |
| Directory depth | 16 |
| Compression expansion | 200:1 per entry and aggregate |
| INI / fields / saved data | 64 KiB / 1 MiB / 1 MiB |
| JSON depth / field count | 16 / 512 |
| Import elapsed time | 15 seconds, cancellable between bounded chunks |
| Concurrent imports | One worker job; no render/tick work |
| Nested archive extraction | Zero; never recurse |

Preflight metadata, then enforce actual streamed counts and byte budgets; size
headers are not proof. Verify CRCs, stream termination, local/central directory
consistency and supported compression methods. Reject encrypted/multivolume
archives, unsupported entry types and malformed/truncated records. Overflow-safe
arithmetic and bounded library allocations are mandatory; a wall-clock timeout
cannot rescue an uninterruptible parser, so use bounded/incremental operations.

Reject absolute/drive/UNC/device paths, NUL/control characters, `.`/`..`, empty
components, backslash ambiguity, Windows ADS colons/reserved names and trailing
dot/space aliases. Normalize only for collision checking; do not silently rename
source files. Reject duplicate paths, Unicode-normalization/casefold collisions,
file/directory prefix conflicts, symlinks, hard links, reparse points and special
nodes. Nested archives may be stored as inert bounded files but are never opened
recursively. Reject if a nested archive is presented as a widget entry point.

Create a private random staging directory on the same filesystem as package
storage. Use exclusive no-follow creation relative to a trusted directory handle
(equivalent reparse checks on Windows), including parent directories. Verify
containment before every write and serve; string-prefix checks alone are unsafe.
Never trust `widget.ini` paths more than ZIP entry paths. Hash original archive
and each file, write manifest outside `original/`, then atomically rename the
complete directory into the package store. Failure/cancellation removes only
that job's staging directory. Duplicate hash imports reuse verified packages;
source changes create a new package. Hashes prove integrity, not publisher trust.

Current loopback routing in `widget-resource-request.cpp` permits only single
filenames and runtime serves four compiled resources plus bootstrap. Extend it
with distinct wrapper and package namespaces, manifest-indexed nested asset
paths, strict single URL decoding and rejection of encoded separators/traversal.
Permit normal asset query strings for cache-busting without making them part of
filesystem resolution. Map imported `.txt` roles to correct response MIME types
without renaming stored files. No directory listing or arbitrary disk path API.
Retain strict Host validation, loopback binding, unpredictable instance capability,
WebSocket Origin/auth checks, bounded queues/requests and lifecycle cleanup.

Each instance can serve only its selected package's manifest entries. Never serve
the archive, private manifest, OBS configuration, home, token files or another
package. Set no-referrer/nosniff headers; no permissive CORS. Cap active sockets,
response sizes and outgoing bytes, not only the pending connection backlog.
Prevent symlink swaps on reads. Immutable package data must not be writable by
widget JavaScript through any host endpoint.

Imported widgets need a separate CSP policy: allow ordinary HTTPS scripts,
styles/fonts/images and API/WebSocket networking as needed, while retaining
object/file/navigation restrictions. Inline styles/SVG are required by this
fixture; inline script/event handlers and eval are not required by this fixture
and should remain disallowed. Keep plugin-owned bridge code and authentication bootstrap separate
from external script parameters. Do not expose native OBS control or generic
host filesystem APIs. Pin jQuery locally for reproducible startup; final version
and license belong to the implementation dependency review.

One isolated loopback origin per live instance (as current random ports provide),
scoped capabilities and no directory exposure prevent accidental cross-package
reads. Capability secrecy is not a defense against scripts deliberately loaded
inside the same widget: they can read that widget's events/capability. Never put
plugin secrets in this origin. Verify CEF local-file restrictions, popups and
cross-origin behavior in integration tests before asserting a sandbox guarantee.
Ordinary networking does not mean privileged native access.

## K. Package/instance C++ ownership and compatibility detection

Proposed types, not declarations added to production:

```cpp
enum class WidgetCompatibility { Auto, GenericWebWidget, StreamElements };
struct WidgetFile {
    QString relativePath;
    QByteArray sha256;
    quint64 size = 0;
};
struct WidgetEntrypoints {
    QString html, css, javascript, fields, data;
};
struct WidgetPackage {
    QString id; // Archive SHA-256 identity, independent of filename.
    QString displayName;
    QByteArray archiveSha256;
    QDateTime importedAt;
    QString originalRoot; // Host-only; never serialized into widget bootstrap.
    std::vector<WidgetFile> files;
    WidgetEntrypoints entrypoints;
    WidgetCompatibility detectedCompatibility;
    QStringList detectionEvidence;
    QJsonObject fieldDefaults;
    QJsonObject savedFieldValues;
};
struct WidgetInstanceSettings {
    QString packageId;
    uint32_t width = 1920, height = 1080;
    WidgetCompatibility compatibility = WidgetCompatibility::Auto;
    QJsonObject fieldOverrides;
};
class WidgetInstance; // Owns ID, settings, runtime, diagnostics and adapter state.
class WidgetPackageStore; // Owns shared_ptr<const WidgetPackage> cache/index.
class WidgetPackageImporter; // Owns cancellable worker jobs and staging RAII.
```

Store layout: `widgets/packages/<package-id>/original/<archive-relative-path>`
and sibling versioned `manifest.json`. Any authorized generated wrapper/cache is
outside `original/`, keyed by package hash + adapter version + instance settings.
The manifest includes schema version, paths, hashes, detected mode/evidence,
import time and importer version. Avoid retaining unnecessary full import paths.
Keep persistent instance settings in OBS source data; runtime state is not shared
between sources and not written into the package.

`ChatSource` owns `unique_ptr<WidgetInstance>`; each instance pins a
`shared_ptr<const WidgetPackage>` and owns its own `WebWidgetRuntime`, browser
child, subscription, ID ledger and field copy. Store/import workers do no OBS
graphics work. Detach/cancel callbacks safely if properties/source close during
import; only attach an installed package on the source's existing serialized
control path. Duplicating an OBS source creates a new instance ID while reusing
package bytes. No deletion/GC of packages pinned by live instances.

Detection: parse and validate INI role mappings, then inspect bounded text for
event-listener registration, `fieldData` use and SE_API access. Prefer parser/token
evidence over comments/string matches. This fixture has INI roles plus both
listeners plus fieldData; SE_API is not required. Two independent strong signals
produce StreamElements with recorded evidence. Weak/conflicting evidence yields
Generic/unknown diagnostic, not forced compatibility. `Auto` is an instance
selection mode; package detected default is a concrete mode. An explicit override
wins without modifying the immutable package. Do not treat arbitrary JS text as
trusted configuration or execute it during detection.

The stable event path remains Twitch/EventSub/Synthetic -> OrderedEventPipeline
-> V2 validation -> immutable PluginEvent -> PluginRuntime::subscribe -> generic
Web DTO -> BokiChat -> StreamElements adapter. Badge enrichment is an additive
backend service, not a dispatch redesign. Floating stays native; lane state,
source ID and existing property keys remain untouched.

## L. OBS properties import flow

For Web Widget mode, retain `renderer_mode=web_widget` and existing dimensions.
Add a ZIP path picker with `obs_properties_add_path(..., OBS_PATH_FILE, ...)`,
a button labeled **Import Widget ZIP** via `obs_properties_add_button2`, an
installed-package list via `obs_properties_add_list`, informational status via
`OBS_TEXT_INFO`, and a compatibility combo: Auto / Generic Web Widget /
StreamElements. Auto's status displays the detected mode. Retain `web_refresh`
as the reload property key; its label may become **Reload Widget**.

New proposed keys: `web_widget_zip_path`, `web_widget_package_id`,
`web_widget_compatibility`, `web_widget_field_overrides`. These are additions;
old sources without them continue loading the bundled development widget.
Store the import path only transiently if possible; package selection is the
persistent reference. On import success select the installed package and reload.
On failure retain the active package/settings and show a concise reason.

Show before import: “This widget contains executable JavaScript and may access
network resources. Only install widgets from sources you trust.” Use a clear
confirmation in the import dialog or a transient trust checkbox gating the import
button. Do not execute during preflight. This is the future product flow, not a
permission request for this read-only audit. Refresh properties on the UI path,
never from graphics/network callbacks under source locks. Import progress must
not block OBS rendering; support cancellation and source destruction.

## M. File-by-file implementation plan

Sequence the next tasks so each can be reviewed independently:

| Files | Work |
|---|---|
| This audit + future fixture inventory | Resolve template/presentation constraints before claiming acceptance parity |
| `src/web/widget-package.hpp/.cpp` (new) | Immutable package manifest, entrypoint roles, hashes, compatibility enum |
| `src/web/widget-package-importer.hpp/.cpp` (new) | Bounded ZIP reader, path rules, staging/rollback/cancellation |
| `src/web/widget-package-store.hpp/.cpp` (new) | Index, atomic install, integrity verification, shared ownership |
| `src/web/widget-fields.hpp/.cpp` (new) | Schema/default/saved value parsing and override validation |
| `src/web/widget-compatibility-detector.hpp/.cpp` (new) | Static evidence collection, explicit overrides |
| `src/web/widget-instance.hpp/.cpp` (new) | Per-source configuration/runtime lifetime and selected package pin |
| `src/web/widget-resource-request.hpp/.cpp` | Safe namespaced nested asset routes, strict URL handling |
| `src/web/web-widget-runtime.hpp/.cpp` | Package serving, safe bootstrap channel info, CSP profiles, instance diagnostics |
| `resources/web-runtime/wrapper.*` (new) | Separate wrapper and dependency ordering; never writes original files |
| `resources/web-runtime/streamelements-adapter.js` (new) | BokiChat mapping, startup buffering, clear ledger, explicit unsupported API behavior |
| `resources/web-runtime/scrapbook-profile.js` (new, only if needed) | Audited sink protection and clearly labeled optional presentation extensions; no source rewriting |
| `resources/web-runtime/vendor/` (new) | Pinned licensed jQuery after dependency review |
| `src/twitch/badge-service.hpp/.cpp` (new) | Authenticated global/channel catalog fetching and bounded cache |
| `src/twitch/twitch-client.hpp/.cpp` | Own/invoke badge service with existing credential lifetime |
| `src/core/ordered-event-pipeline.hpp/.cpp` | Accept immutable badge catalog/enrichment hook before final validation; retain FIFO/deadlines |
| `src/web/web-event-serializer.cpp` | Verify existing URL preservation; add only demonstrated generic DTO gaps |
| `src/renderer/chat-source.hpp/.cpp` | Import/select/override/reload properties; preserve source ID and all old keys |
| `resources/web-widget.qrc`, `CMakeLists.txt`, `tests/CMakeLists.txt` | Register new resources/library/tests and ZIP dependency |
| `.gitignore`, `tests/README.md` | Ignore optional local-only fixture tree; document externally supplied path/hash |
| `tests/widget-package-tests.cpp` (new) | Package, INI, field, identity and security tests |
| `tests/streamelements-adapter-tests.js` (new) | Event/API/escaping/order tests using synthetic data |
| `tests/scrapbook-fixture-tests.*` (new) | Opt-in real fixture/browser tests, never commit archive or source copies |
| `tests/twitch-producer-tests.cpp`, `tests/web-event-tests.cpp` | End-to-end badge resolution and serialization with fake transports |
| `tests/web-runtime-tests.cpp` | Multi-instance package serving, import cancellation, reload/shutdown regressions |
| `VERSION`, `buildspec.json` | Bump to alpha.17 only in the later release implementation task |

Do not add a broad SE SDK, Twitch authentication in widgets, session-history
emulation, visual field editor or browser-based Floating renderer as part of V4.

## N. Validation and acceptance tests

Planned unit tests cover every mapping row, empty optional values, gift aggregates
versus child gifts, anonymous events, resub text versus system text, Unicode,
UTF-16 ranges, repeated/provider-colliding names, static and animated URLs,
zero-width limitations, unsafe IDs and markup-bearing viewer names. Assert the
generic DTO is unchanged. Test HTML escaping exactly once and deletion identity
consistency. Unsupported APIs must fail clearly, with bounded logs and no fake
storage/counters. Verify no events precede onWidgetLoad and no duplicate listener
installation on reload.

Importer/security tests include traversal using both separator forms, absolute/
UNC/drive/ADS paths, Unicode and case collisions, duplicate entries, prefix
conflicts, symlinks/reparse points, special nodes, CRC/size/header mismatch,
encrypted files, malformed streams, ZIP bombs, count/depth/size/ratio limits,
nested archives, unsafe INI entrypoints, failed atomic installs and cancellation.
Use generated synthetic malicious archives, not modified third-party fixtures.
Test percent/double encoding and query handling at HTTP routing separately from
extraction. Exercise socket/backpressure limits and cross-package denial.

Fixture tests accept an explicit external ZIP path and verify the audited hash.
Read-only archive inspection can run without execution; browser integration must
use an isolated test profile with fake events and controlled remote dependencies.
Hash every original before/after import, load, reload and shutdown. Check missing
jQuery/font failures, placeholders, follower toggle, real badge mode and image
errors. Two OBS sources must share package identity with independent fields,
message history, clear state and runtime teardown. Also test source destruction
during import and backend generation/reset changes.

Manual V4 acceptance checklist (none claimed executed in this audit):

1. Start OBS; select/add the existing plugin source and choose Web Widget.
2. Select ZIP in properties; acknowledge warning; import without manual extraction.
3. Confirm StreamElements detection, installed package and unchanged original hashes.
4. Load widget; show a synthetic chat message, then real/structured animated emotes.
5. Show Follow using merged saved data (`alertsfollower=true`).
6. Show Subscription and resub; assert viewer text survives DTO and SE adapter.
   The unchanged fixture does not render the supplied text in every path.
7. Show single/community gifts without double-rendered child gifts; show Cheer.
8. Deliver Raid and verify the adapter payload; the unchanged fixture has no raid
   display handler.
9. Delete one message, clear one user, clear the whole chat including ledger overflow.
10. Disable custom badges and verify actual resolved Twitch images.
11. Reload repeatedly, switch renderer, duplicate/remove sources, then shut down OBS.
12. Confirm Floating/native rendering and existing settings still work.

Passing adapter unit tests does not prove CEF rendering or animated asset
playback. Raid visibility and omitted subscription presentation are documented
limitations of the unchanged fixture when the adapter payload is correct.

Baseline validation executed for this audit:

| Check | Result |
|---|---|
| `cmake -S . -B build/v4 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON` | Passed |
| `cmake --build build/v4 -j2` | Passed; plugin module linked |
| `ctest --test-dir build/v4 --output-on-failure` | 14/15 passed in the restricted sandbox; only the loopback runtime suite could not bind |
| `ctest --test-dir build/v4 -R '^web-runtime-tests$' --output-on-failure` with socket access | Passed, 1/1; all 15 suites passed across the two runs |
| Selected ASan/UBSan suites | Four suites passed; LeakSanitizer itself cannot run under the environment's ptrace runner |
| Isolated OBS 32.2.2 smoke | Final alpha.17 module loaded and shut down; allocation-counter result investigated in P |
| `git diff --check` | Passed |

Configure reported `Could NOT find WrapVulkanHeaders (missing: Vulkan_INCLUDE_DIR)`;
this was non-fatal. No compiler warnings appeared in the build output. An OBS
module lifecycle smoke was performed; full visual fixture acceptance and a
Windows build remain manual release-build checks.

## O. Git and audit execution state

Work continues on `feature/v4-streamelements-core` with the V4 implementation,
tests, version files, and this audit uncommitted. The local Scrapbook archive and
its unpacked reference remain under the ignored `local-fixtures/` tree. No
commit, merge, push, tag, release, reset, or history rewrite was performed.

## P. OBS allocation-counter investigation

An isolated OBS 32.2.2 shutdown reported one remaining libobs allocation when
the plugin module was present and zero when the module file was absent. A
temporary diagnostic build sampled `bnum_allocs()` around every operation in
`obs_module_load` and `obs_module_unload`. The updater process lock,
`PluginRuntime`, and Qt font registration changed the libobs allocation count by
zero. `obs_register_source` added three module/source-registration allocations.
Those registration allocations were reclaimed during shutdown.

A second diagnostic build omitted `obs_register_source` completely and still
finished with exactly one reported allocation. Its count at the beginning of
our `obs_module_load` body already included the retained allocation. This proves
the counter difference is caused by OBS module-loader/profiler bookkeeping for
the additional loaded module, not a browser source, active child, bridge thread,
timer, event subscription, package reference, or plugin-owned OBS source
reference. All diagnostic instrumentation and the source-registration omission
were removed after the comparison.
