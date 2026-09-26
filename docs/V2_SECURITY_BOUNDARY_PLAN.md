# V2 security boundary and local EventSub testing plan

Status: V2 security-boundary and local EventSub development mode implemented on
2026-09-26. Target release: `0.1.0-alpha.13`. The documented policy is
implemented by the central pipeline and asset cache; no release version is
changed by this implementation step.

## 1. Repository baseline and scope

- Starting branch: `main`; starting workspace: clean, including untracked files.
- Base: `3e1c5bf` (`chore: bump version to 0.1.0-alpha.12`). Local `main` and
  the existing `origin/main` tracking ref agree; `origin/HEAD` points to it.
  This identifies the local release base, not a claim that a remote fetch ran.
  The V1 event foundation is already merged into this base.
- Created and switched to `feature/v2-security-boundary`; it did not exist among
  the inspected local or remote-tracking branches. Branch creation required
  sandbox escalation because `.git` is mounted read-only; it succeeded.
- Existing work was preserved. No commit, push, tag, release, history rewrite,
  reset, forced checkout, or destructive cleanup occurred.
- This audit adds only this document. Implementation work is a subsequent step.

Preserve source ID `bokis_twitch_chat_plugin`, all saved OBS property keys and
settings, the shared Twitch producer, and the native Floating renderer. Network
callbacks and validators must not access OBS graphics. Lane scheduling remains
renderer-owned. All proposed identifiers, diagnostics, tests and documentation
are English and implementation remains C++20.

V2 does not implement a browser runtime, CEF rendering, StreamElements
compatibility, `onWidgetLoad`, `onEventReceived`, `SE_API`, widget ZIP import,
packages, instances, `fields.txt`, or `data.txt`. Existing web-theme resources do
not authorize adding any of those features.

## 2. Actual current data flow

```text
PluginRuntime::Service (Qt application thread; one selected account/channel)
  -> TwitchClient: authentication, broadcaster lookup, subscriptions
  -> WebSocketTransport / EventSubSocket::messageReceived(QString)
  -> TwitchClient::handleEventSubMessage
       control messages -> welcome / keepalive / reconnect / revocation handling
       notification -> OrderedEventPipeline::ingest(QByteArray)
         -> normalizeTwitchEvent
              -> Reader + parseTwitchMessage + parseTwitchUser
         -> channel check -> bounded transport-ID deduplication -> sequence
         -> Pending ticket inserted BEFORE possibly synchronous enrichment
         -> EmoteService::resolve / start
              -> EmoteCatalog::apply (external catalog metadata)
              -> ImageCache::request / pump (external image bytes)
         -> OrderedEventPipeline::flush (ready, deadline, or pressure)
         -> make_shared<const PluginEvent>
         -> EventDispatcher::publish -> filtered, bounded mailboxes
              +-> PluginRuntime::Service::pump -> NativeEventAdapter::accept
              |    -> CPU preparation -> OBS tick/render and GPU resources
              +-> other EventSubscription consumers
```

Current producers normalize ten payload alternatives: chat, message deletion,
channel/user clear, follow, subscription, resubscription, individual gift,
community gift, cheer and incoming raid. Subscription variants come from
`channel.chat.notification`, including shared-chat variants. They do not come
from `channel.subscribe`, `channel.subscription.message`, or
`channel.subscription.gift`. Unknown event/version and unsupported notice types
are ignored. Announcement notices and standalone reward-redemption events are
not normalized today. Chat reward IDs, message type and system text are retained.

### Exact integration sites

Line numbers describe the inspected base and will move during implementation.

| Responsibility | Current site | Required V2 change |
| --- | --- | --- |
| Wire limits | `src/twitch/eventsub-socket.cpp`, `WebSocketTransport` constructor | Keep 1 MiB frame/message limits; verify invalid UTF-8 handling with real frames. |
| Control routing | `src/twitch/twitch-client.cpp:294`, `handleEventSubMessage` | Validate control fields and reconnect destinations separately; notifications still enter the pipeline. |
| Provider mapping | `src/twitch/event-normalizer.cpp:85`, `normalizeTwitchEvent`; `src/twitch/chat-message-parser.cpp:13`, `parseTwitchMessage` | Preserve strict raw-field evidence before lossy QColor/QUrl/numeric conversions; use the same field policy helpers. |
| Central entry validation | `src/core/ordered-event-pipeline.cpp`, `OrderedEventPipeline::ingest` | Immediately after successful normalization, before channel/dedup mutation, sequence increment, `Pending` allocation and `EmoteService::resolve`. |
| Catalog entry validation | `src/chat/emote-catalog.cpp:28`, `parseEmoteCatalog`; `EmoteCatalog::replace` | Validate external catalog fields before caching; injected catalog entries cannot bypass policy. |
| Enrichment occurrence validation | `src/chat/emote-service.cpp:130`, `EmoteService::start` | After `catalog_.apply`, before any image request; validate additions and ranges. |
| Network side-effect guard | `src/chat/image-cache.cpp:157`, `ImageCache::request`; `pump` at line 187 | Enforce URL policy before cache lookup or fetch; validate redirects before following them. |
| Enrichment completion | `src/core/ordered-event-pipeline.cpp`, lambda passed to `emotes_.resolve` | Keep weak ticket/QPointer guards and ready check; mutable result must pass final validation. |
| Final validation and immutable publication | `src/core/ordered-event-pipeline.cpp:119`, `flush`, immediately before current line 126 | Validate the completed or fallback event and freeze it through a restricted factory. Every deadline/pressure path uses this check. |
| Dispatch begins | `src/core/event-dispatcher.cpp:100`, `EventDispatcher::publish` | Require a validated publication handle rather than arbitrary `EventPtr`. |
| Consumer delivery | `src/core/plugin-runtime.cpp`, `Service::pump`; `src/renderer/native-event-adapter.cpp:38`, `accept` | Continue consuming immutable semantic events, without revalidation or HTML conversion. |

**Conclusion:** `OrderedEventPipeline::ingest` remains the correct central
insertion point. A check only there is insufficient: catalog enrichment can
introduce data later, and publication currently has an unrestricted public API.

### Existing protections and gaps

- Normalization already bounds envelopes at 1 MiB, checks supported schemas,
  required identities and timezone-bearing timestamps. `Reader::count` checks
  JSON number type, finiteness, integrality, minimum and `int` overflow.
- Optional counts currently invalidate an entire event through `Reader::valid`.
  Chat cheer metadata uses weaker conversion; cheermote bits/tier use `toInt()`.
  These paths need one consistent required/optional policy.
- `QColor(raw)` accepts more than Twitch's `#RRGGBB` format. A central check of
  the converted color alone cannot discover whether input was `red` or hex.
- `QUrl` construction can repair malformed input before later checks. Raw URL
  spelling must be checked before tolerant parsing loses evidence.
- Fragment text mismatch already falls back to the whole original message.
  Ranges are half-open UTF-16 code-unit offsets, not UTF-8 bytes or code points.
- Catalog URL checks and image requests require HTTPS and a host, but do not
  constrain hosts. Image redirects use `NoLessSafeRedirectPolicy`, which is not
  a provider-host allowlist. Legacy GIF metadata can retain arbitrary valid
  schemes in a published event even if ImageCache refuses to download them.
- Badge lookup/drawing is not implemented; badge provider/type/version/info are
  retained, with image URL empty. V2 defines the future URL rule without adding
  badge rendering.
- The pipeline has 128-ticket/64 MiB budgets, a 2.5-second deadline, 32 MiB
  per-event decoded-asset fallback, and ten-minute/16,384-entry deduplication.
  Decoder/download limits already exist. These do not replace semantic limits.
- Only one production dispatcher publication call exists today, but
  `publish(EventPtr)` can accept any constructed event. `shared_ptr<const T>`
  expresses const access, not proof of validation or absence of mutable aliases.
- Invalid notifications currently generate one warning each: attacker-controlled
  traffic can flood logs despite the warning not containing the payload.

## 3. Threat model and terminology

Treat Twitch notifications, user-controlled fields carried by Twitch, third-party
catalogs, image responses/redirects, and all local mock messages as untrusted.
TLS authenticates a connection; it does not make text appropriate for DOM use.
Provider enums record source identity, not a claim of sanitization. A local
process can impersonate a mock endpoint; development mode is not authenticated.

Protect downstream semantic integrity, moderation targeting, account credentials,
host/network access, bounded CPU/memory, renderer stability, and useful logs.
Attacks include script-like strings becoming markup in a future consumer, URL
scheme abuse and SSRF, forged IDs/provider data, malformed ranges, spoofed labels,
numeric overflow, replay, decompression bombs, and log/resource exhaustion.

| Surface | Threat and mitigation |
| --- | --- |
| Chat, resub/cheer/system/announcement/reward text | Markup injection, huge strings, controls and malformed Unicode: bounded semantic text policy; future safe sinks. Apply the same policy when currently unsupported fields become events. |
| Display/login names and nested mention/gifter/recipient users | Spoofed attribution, terminal controls, accidental Unicode damage: separate label and identity policies; never authorize by display name. |
| User/channel/message/event/origin/reply/community/reward IDs | Collisions after normalization, wrong moderation targets, unbounded dedup keys: opaque bounded IDs, no trimming/case folding, required-field rejection. |
| Colors, numeric values, tiers and timestamps | CSS fragments, overflow, false counts, destructive clear ordering: typed bounded values and strict conversions. |
| Emote/badge/provider metadata and ranges | Forged provider provenance, invalid indexing and hidden text loss: registered providers, bounded records, range consistency and text fallback. |
| Asset URLs, catalog URLs, redirects and image bytes | Local file access, script/data schemes, remote/local fetch abuse, decoder pressure: restrictive URL purposes, redirect gate and existing byte/pixel budgets. |
| Welcome/reconnect/revocation data | Session confusion and reconnect to arbitrary hosts: bounded controls, sender/generation checks, mode-specific endpoint policy. |
| Logging and development configuration | Credential disclosure, forged log lines, log flooding, silent production changes: fixed diagnostics, bounded counters, explicit persistent test status and fail-closed configuration. |

The boundary does not protect against malicious native code in the same process,
a compromised OS/Qt/TLS stack, or all possible image-decoder defects. Keep codec
updates and rendering safeguards. It does not provide durable exactly-once
delivery, authenticate mock senders, or make strings suitable for every output
context.

Definitions:

- **Input validation:** decide whether a value satisfies a field's type, encoding,
  size, domain and relationship constraints; reject or report invalid input.
- **Normalization:** choose a documented equivalent representation, such as
  CRLF to LF or timestamps to UTC. Never silently rewrite opaque identities.
- **Sanitization:** remove or replace prohibited parts under a specific policy,
  such as replacing NUL in display text or dropping an invalid optional URL.
  V2 has field repair; it has no HTML sanitizer.
- **Output encoding:** escape for the actual destination syntax at the point of
  output, e.g. JSON serialization or HTML text escaping in a future template.
  It is not performed globally on internal strings.
- **Safe rendering:** use APIs which treat values as data. Future web consumers
  must use `textContent`/text nodes for text, numeric CSS/color properties from
  typed values, and policy-approved URLs for images. No `innerHTML`, HTML string
  interpolation, event-handler attributes, or URL-to-code conversion. Their
  bridge and DOM tests are future work, not a V2 implementation.

`Hello <b>world</b>` remains exactly that semantic text. `&lt;b&gt;` remains the
literal entity spelling; do not entity-decode or double-encode it. A badge or
emote name resembling markup is either a valid literal label or invalid under
its field grammar; it is never an HTML object. Validated does not mean executable
or trusted markup.

## 4. Concrete field policy

These are proposed resource limits for this plugin, not assertions about Twitch
service maxima. Keep them in one `EventValidationPolicy`, with no user-configurable
production bypass. Tests may inject smaller limits. Lengths below are UTF-16 code
units unless explicitly described as bytes. Enforce bounds before expensive work.

### 4.1 Semantic text and Unicode

| Field | Limit and behavior |
| --- | --- |
| `ChatMessage::text`, notice text, `Cheer::text` | 8,192 code units; truncate overlong valid text at the last complete extended grapheme cluster within the limit. Empty text is permitted, including empty subscription notices. |
| `ChatMetadata::systemText`; future announcement/reward input | Same 8,192 limit and newline/control policy. No new event type in this step. |
| Aggregate textual content | 64 KiB UTF-8 after repair across retained fields, including repeated fragment text; 256 KiB retained non-image metadata budget. Drop optional collections first; reject if required semantic fields alone exceed budget. |
| Fragment and badge counts | At most 512 fragments and 64 badges. Excess fragments become a single whole-text fragment; excess badges retain the first valid 64. |
| Media records | At most 8 validated entries; drop invalid/excess entries. |
| Unknown object fields | Discard at provider mapping; never preserve arbitrary provider JSON or markup blobs for forwarding. |

Wire JSON is UTF-8 with valid Unicode scalar values. Invalid UTF-8/JSON rejects the
envelope. Test QWebSocket protocol rejection using malformed bytes; the QString
signal cannot recover original bytes after decoding. Direct QByteArray ingestion
must independently verify UTF-8. After parsing, check every retained QString for
unpaired UTF-16 surrogates, including escaped `\uD800`/`\uDC00` and directly
constructed test values. Reject an event with malformed primary text; drop an
optional malformed field or label and use its fallback. Do not replace malformed
identity bytes into potentially colliding IDs. A literal valid U+FFFD is allowed.

For semantic text: normalize CRLF and lone CR to LF; preserve LF and tabs. Replace
embedded NUL, other C0 controls, DEL and C1 controls with U+FFFD, recording a repair.
Preserve ordinary spaces, quotes, apostrophes, angle brackets and visible content.
Preserve Unicode normalization form: no NFC/NFKC conversion, case folding or
blanket trimming of chat. Preserve emoji, variation selectors, combining marks,
ZWJ/ZWNJ, zero-width spaces and legitimate RTL text. Preserve Unicode format/bidi
controls as literal semantic data and classify their presence in bounded
validation issue flags; never use these labels as identity or put them in logs.
Future presentation should isolate user text directionally. Presence alone is
not a warning-level security incident.

Any change in primary text, including truncation, invalidates its original range
mapping: rebuild a single text fragment and discard occurrence-specific emote,
mention and cheermote metadata. This deliberately trades optional decoration for
correct text rather than guessing new offsets. Do not split graphemes or surrogate
pairs. A single over-limit grapheme can yield empty truncated text; retain the
event identity/counts and report the repair. Enforce total input bounds before
grapheme scanning to avoid pathological combining-sequence costs.
Release excess retained string/vector capacity after a repair or collection
fallback before budget accounting; a short view or resized container retaining
the original large allocation is not a successful memory-bound repair.

### 4.2 IDs and names

| Field | Accepted representation | Failure action |
| --- | --- | --- |
| Event ID, channel ID, required user ID, chat/deleted-message ID, required community-gift ID | Nonempty, at most 256 UTF-8 bytes; valid scalar Unicode; no whitespace, control or format characters. Otherwise opaque, case-sensitive strings. No numeric conversion, UUID requirement, trimming or Unicode normalization. | Reject event. Do not synthesize moderation or dedup identities. |
| Optional origin IDs, reward ID, individual gift community ID, reply IDs | Same grammar; absence/empty allowed. Reply metadata requires both valid IDs when present. | Drop invalid field; drop entire reply record if incomplete. Required shared-chat origin ID remains required and rejects on failure. |
| Provider-specific IDs | At most 256 bytes and opaque ID policy, plus provider grammar only where needed to safely construct a path. Twitch emote path IDs retain existing `[A-Za-z0-9_-]+`; percent-encode other path segments instead of concatenating raw IDs. | Drop metadata record or downgrade emote to text. Invalid optional Twitch owner/set IDs are dropped. |
| Login name | 1–64 ASCII `[A-Za-z0-9_]`; normalize ASCII uppercase to lowercase after validation. No surrounding-space repair of externally supplied login. | Drop invalid login; user ID can still identify the user. |
| Display name | At most 128 code units, valid Unicode; preserve case, script, combining marks and emoji. Semantic control repair; CR/LF/tab become spaces for this single-line field. Grapheme-safe truncation. | Invalid/empty label falls back to valid login, else literal `Unknown user`. Anonymous actors remain explicitly anonymous. |
| Emote name/token | 1–128 code units, valid Unicode, no whitespace/C0/C1 controls; preserve case and normalization form. ZWJ/variation selectors are allowed and classified. | Drop catalog entry or retain occurrence as text. Never substitute a new token in message text. |
| Badge type/version | 1–128 ASCII `[A-Za-z0-9_-]` each. | Drop badge record. |
| Badge info | Optional plain semantic text, at most 256 code units, single line. | Drop malformed value; truncate valid overlong value. |
| Message type/provider tags | Message type: optional ASCII `[A-Za-z0-9_-]`, at most 64; preserve bounded unknown types for forward compatibility. Providers: only declared enum members. | Drop invalid message type; reject invalid provider record, never reinterpret it as Twitch. |

User/channel IDs are not limited to decimal digits. ASCII restrictions on logins,
badge keys and constructed emote paths are separate provider-format rules.
Provider ID normalization never changes an identity. For anonymous gifts/cheers,
strip supplied actor identity rather than exposing it; require recipient identity
where the model needs it. Invalid optional mention/gifter records are dropped;
invalid required actors reject. Do not accidentally turn a malformed targeted
`ChatCleared::user` into a whole-channel clear.

Deduplicate only validated transport event IDs, preserving the existing ten-minute
TTL and 16,384-entry bound. Repeats produce `IngestResult::Duplicate`, no routine
warning, and no ticket. Never deduplicate gifts by community ID, different
transport events by message ID, or shared-chat data across destination channels.
Validate before inserting the ID, so an invalid message cannot poison `seen_`.
An event rejected at final validation remains seen for this session: bounded
replay suppression is preferable to repeatedly enriching the same poison event.

### 4.3 Colors, counts, enums and time

Color input is absent/empty or exactly `#[0-9A-Fa-f]{6}`. Validate the raw string
before constructing `QColor`; store opaque RGB with alpha 255 and serialize as
canonical uppercase hex only when needed. Invalid CSS names, `#RGB`, alpha hex,
functions, whitespace or malformed values become invalid `QColor{}` so native
renderers use their existing fallback. A programmatically constructed color must
be valid opaque RGB or the unavailable sentinel. Never carry raw CSS downstream.

| Numeric field | Inclusive range | Invalid required / optional action |
| --- | --- | --- |
| `Cheer::bits` | 1–2,147,483,647 | Reject event. |
| Chat optional cheer bits | 0–2,147,483,647 | Drop optional value. |
| Cheermote bits and tier threshold | 1–2,147,483,647; threshold must not exceed bits | Drop cheermote metadata, preserve its text. Tier threshold is not `SubscriptionTier`. |
| Raid viewers | 0–2,147,483,647 | Reject event. |
| Community gift count | 1–2,147,483,647 | Reject event. |
| Optional cumulative gift total | 0–2,147,483,647 | Drop optional value. |
| Duration/cumulative subscription months | 1–12,000 | Reject required invalid value. |
| Optional streak months | 0–12,000 and no greater than cumulative months | Drop invalid optional value. |

Required numeric input must be a finite integral JSON number in range before any
cast; strings, booleans, fractions, overflow, negative values, null and missing
required fields reject. Optional null/missing means absence without warning;
malformed supplied optional numbers drop with a compact issue. Central validation
rechecks typed ranges for non-JSON producers. Do not clamp money/activity facts
into fabricated counts. Use checked wider arithmetic for aggregation, byte
accounting and time conversion. INT_MAX is a compatibility/representation ceiling,
not a believable purchase limit; 12,000 months is a deliberate generous safety
cap. Test maximum plus one as well as JSON precision boundaries around 2^53.

Subscription tier strings `1000`, `2000`, `3000` map to the existing enum; a
nonempty unknown tier up to 32 ASCII digits maps to `Unknown` with a diagnostic.
Missing, wrong-type or malformed required tier rejects; an invalid in-memory enum
becomes `Unknown`. Preserve valid optional Prime flags; malformed optional flags
drop, while required malformed booleans reject. `isGift` and anonymous flags must
remain consistent with their optional actors; never infer a real user for an
anonymous event.

Timestamps require strict RFC3339 with explicit offset/Z, valid calendar values,
years 2000–9999, and 0–9 fractional digits; normalize to UTC milliseconds by
discarding sub-millisecond precision. Reject invalid required event/follow times;
`receivedAt` is assigned locally and must be valid. Do not clamp server timestamps
or reject old fixture data merely because it is old. Preserve monotonic local
timers for TTL/deadlines and sequence-based FIFO; do not sort by untrusted time.
Non-finite/overflowed dates reject. Sequence/generation are local counters, never
read from external JSON; exhaustion stops/restarts the feed with an explicit
error rather than wrapping. Test delayed moderation against newer messages.

### 4.4 Asset URL policy

Use one `AssetUrlPolicy` with explicit provider/purpose. Validate the raw string
and parse in strict mode; final events contain `QUrl` values produced by that
validator. Revalidate typed values at publication, recognizing that the original
spelling is only available at parse time.

- Maximum 2,048 UTF-8 bytes. Require an absolute HTTPS URL, nonempty exact
  registered ASCII host, no credentials, no fragment, and default port or 443.
- Reject leading/trailing whitespace, literal controls/whitespace, backslashes,
  invalid percent escapes, percent-encoded controls, encoded path separators or
  dot-segment traversal, empty host, IP literals, trailing-dot/IDN host aliases,
  and relative URLs. Do not repeatedly percent-decode. Encoded spaces in a valid
  path may remain; they do not repair a whitespace-prefixed scheme.
- Scheme/host comparisons are case-insensitive; normalize their case only.
  Preserve path/query spelling, case and ordering. Provider URLs may carry
  bounded queries, but queries never become log content. No URL userinfo.
- `https:` alone is insufficient: `https://attacker.example/a.png` fails the
  allowlist. `http:`, `data:`, `file:`, `javascript:`, `blob:`, `ftp:`, `ws:`,
  `wss:` and unknown schemes are invalid for externally supplied assets.
- Known catalog adapters alone may resolve protocol-relative `//host/path` to
  HTTPS before the same checks, matching existing FFZ/7TV data. The central
  event layer accepts absolute URLs only. Do not upgrade arbitrary HTTP input.

Initial registry, based on current code and repository fixtures (verify provider
fixtures during implementation; do not broaden to wildcard suffixes):

| Purpose | Exact accepted hosts |
| --- | --- |
| Twitch emote | `static-cdn.jtvnw.net` |
| Twitch badge, when available | `static-cdn.jtvnw.net`, with badge-specific path fixtures |
| 7TV emote | `cdn.7tv.app` |
| BTTV emote | `cdn.betterttv.net` |
| FFZ emote | `cdn.frankerfacez.com` |
| Catalog HTTP API (separate purpose) | `7tv.io`, `api.betterttv.net`, `api.frankerfacez.com` |
| Legacy media / future asset providers | No arbitrary host by default; require an explicit reviewed provider/purpose registration. |

The current provider code needs no plain HTTP asset support. Its protocol-relative
catalog URLs already become HTTPS. Badge fetching has no current runtime
requirement beyond the future policy. Dropping previously accepted arbitrary
legacy GIF URLs is an intentional security tightening; retain the message text
and event, document this compatibility impact, and do not silently create an
unrestricted exception to preserve a test fixture. A future reviewed media
provider can regain supported URLs without changing saved OBS keys.

Validate primary and fallback URLs independently. Invalid optional URL -> empty
URL and no associated image; a valid fallback may supply the image. If neither
URL is usable, keep valid emote identity/provider/name as text fallback. Invalid
media URL -> drop media record. Invalid badge URL -> clear URL while retaining
valid badge keys. Never allow provider mismatch (e.g. a SevenTV enum with BTTV
host) just because both hosts occur in the registry.

Disable automatic redirects. Follow at most three redirects after resolving and
validating each destination under the same purpose/provider policy; reject HTTPS
downgrade, credentials, local/private IP destinations and off-registry hosts.
Guard catalog redirects too. Never attach Twitch headers/cookies to asset
requests. Use isolated asset networking so auth/session state cannot leak through
a shared cookie jar. Host allowlisting limits SSRF but does not prove DNS answers
are public under a compromised resolver; do not claim complete DNS-rebinding
protection. No blocking DNS checks on the graphics or application event path.

Keep existing 8 MiB downloads, 16 MiB decoded image budget, 2,048 source-dimension,
256 target-dimension, 512-frame, six-concurrent-request, cache and queue limits.
Check final unread response bytes as well as `readyRead` chunks before appending
or parsing. Restrict decoding to existing raster formats (PNG/GIF/JPEG/WebP), no
SVG/HTML. Decoded images are immutable CPU assets, with no mutable owner retained
after publication. URL validation never substitutes for decoder limits.

### 4.5 Emotes, badges and optional metadata relationships

An emote occurrence has a known provider, valid nonempty ID/name, allowed URLs
(or explicit unavailable-image fallback), and a range into the final message.
Twitch metadata belongs only to Twitch. Unknown enum values or inconsistent
metadata drop the emote interpretation, leaving semantic text. Catalog records
have no occurrence range; `EmoteCatalog::apply` assigns it later. Reject malformed
FFZ numeric IDs before converting them into strings; avoid current default-zero
conversion. Bound catalog entries to 10,000 and total retained catalog data to
8 MiB per bucket, checking before insertion; cap scans/arrays as well as accepted
entries so duplicate names cannot evade iteration limits.

Ranges are `[offset, offset + length)` in QString UTF-16 units. Require nonnegative
offset, positive length for an occurrence, `offset <= text.size()` and
`length <= text.size() - offset` (subtraction avoids overflow). Endpoints must not
bisect a surrogate pair or extended grapheme cluster; fragment text must equal
the referenced substring. Require ordered, nonoverlapping coverage whose
concatenation equals the whole message. Empty chat gets one empty text fragment.
Malformed, missing, overlapping, out-of-bounds or inconsistent coverage causes
whole-message text fallback; do not silently delete or duplicate visible text.
Structurally valid ranges with invalid optional emote data downgrade only that
fragment. Adjacent text fragments may be coalesced with checked arithmetic.

Provider `zeroWidth` is an overlay presentation hint, distinct from a zero-length
range or Unicode zero-width character. Preserve it on valid provider emotes;
clear it on text fallbacks. An overlay still consumes its actual token range.

Badges require a known `BadgeProvider` (currently Twitch only), type and version.
Drop invalid records and duplicate `(provider,type,version)` entries, keep stable
first-valid order, and validate optional info/URL independently. There is no badge
HTML/markup field. Validate nested mention users, reply IDs, reward IDs, optional
cheer bits and Twitch owner/set IDs with the same helpers rather than per-event
exceptions. The ten-event visitor must be exhaustive so a future payload cannot
silently skip policy.

### 4.6 Disposition and diagnostics

| Invalid class | Outcome | Warning policy |
| --- | --- | --- |
| Malformed envelope/Unicode primary text, required identity/count/time, targeted-clear identity | Reject event before tickets, or before final publication if introduced later. | Rate-limited reject counter. |
| Optional identity/number/metadata record | Drop field or containing optional record. | Rate-limited repair counter. |
| Color/display label/fragment decoration | Unavailable color, safe label or exact text fallback. | Rate-limited repair counter; ordinary absence is quiet. |
| Overlong valid text/label | Grapheme-safe truncation; rebuild affected ranges. | Rate-limited truncation counter. |
| Invalid URL/provider/range | Drop URL/record or use text fallback according to rules above. | Rate-limited fixed reason. |
| Unknown supported-format tier | Replace with `Unknown`. | Bounded diagnostic. |
| Duplicate transport ID/unsupported event type | Ignore; no downstream publication. | Counters/debug only. |
| Out-of-range transport keepalive | Clamp to existing 10–600 seconds before bounded arithmetic. | Diagnostic only; this is a timer, not an activity count. |

Warnings supplement a disposition; “warn and continue” never means retain a
policy-invalid field. Use enum reason/field/action codes, event kind and generation,
not raw values, display names, URLs, IDs or complete payloads. Example:
`Event validation repaired kind=ChatMessage field=AssetUrl reason=DisallowedScheme count=12`.
No tokens, OAuth codes, secrets, query strings or remote response bodies.

Use a fixed-cardinality counter table and monotonic global token bucket: burst 5,
refill one warning per 10 seconds, plus one compact aggregate report per minute.
Never allocate a log bucket for each attacker-supplied value. Keep totals for
accept/reject/drop/repair/duplicate; reset bounded diagnostics at feed lifecycle
boundaries. Avoid warning twice in TwitchClient and the validator for the same
rejection. Classification of valid RTL/format characters is diagnostic data only.

## 5. Minimal C++ design and publication invariant

Implementation note: the existing renderer-independent dispatcher retains its
generic `publish(EventPtr)` core API so its mailbox/threading contract and focused
tests remain independent of Twitch policy. Production has exactly one publication
call, in `OrderedEventPipeline::flush`, immediately after the final whole-event
validation and immutable allocation. No transport, parser, runtime or renderer
publishes directly. Future external consumers must receive `EventSubscription`
mailboxes from `PluginRuntime`; the low-level dispatcher publication API is not a
provider integration interface. This is the only deliberate difference from the
private `ValidatedEvent` wrapper sketched below.

Proposed names (sketch, not code added by this audit):

```cpp
struct EventValidationPolicy;
struct AssetUrlPolicy;
enum class ValidationDisposition { Accepted, Repaired, Rejected };
enum class ValidationAction { RejectEvent, DropField, ReplaceValue, TruncateValue, ClampValue };
enum class ValidationIssueCode;
enum class ValidationField;
struct ValidationIssue;             // fixed codes, no raw untrusted values
struct EventValidationResult;       // disposition + bounded issues/counters
class ValidatedEvent;               // private construction; owns immutable EventPtr
struct EventPublicationResult;      // optional ValidatedEvent + validation result

EventValidationResult validateEvent(PluginEvent &, const EventValidationPolicy &);
EventPublicationResult validateAndFreezeEvent(PluginEvent, const EventValidationPolicy &);
// Field helpers: validateSemanticText, validateOpaqueId, validateDisplayName,
// validateLoginName, parseUserColor, parseCount, validateAssetUrl,
// validateEmoteMetadata, validateBadgeMetadata, validateFragmentRanges.
```

Keep the validator as free functions and a small policy/value module, no plugin
registry framework, inheritance hierarchy or separate event hierarchy. Place
policy functions in `src/core/event-validation.hpp/.cpp`; do not pull Twitch
networking or renderer dependencies into `event-core`. URL syntax uses QtCore's
QUrl; networking enforcement stays in the chat transport. Have `chat-support`
link `event-core` for shared policy; the dependency remains acyclic.

`validateEvent` repairs a uniquely owned mutable event at ingress. The pipeline
then owns a pending ticket; do not call its mutable contents “immutable safe
data.” Enrichment can add untrusted fields. In `flush`,
`validateAndFreezeEvent(std::move(job->event), policy)` rechecks the whole bounded
event, creates `make_shared<const PluginEvent>` only on success, and returns a
`ValidatedEvent` whose constructor is inaccessible to callers. Dispatcher
publication accepts only that handle and extracts `EventPtr` internally. Consumers
keep the existing `EventPtr` API. Remove the arbitrary-`EventPtr` publish overload;
tests construct complete valid events through the same factory, without a
production validation bypass.

The factory takes ownership by value and creates a fresh const allocation; it
must not certify a caller-owned shared mutable PluginEvent. Image producers must
also relinquish mutable aliases. Existing const decoded-image ownership can be
kept; any future producer supplying decoded assets needs the same ownership
contract and image budget validation.

Parser helpers must validate raw color/URL/number types before conversion and
propagate bounded field issues through `NormalizationResult`. They implement
the same policy, not independent conflicting rules. Required schema rejection
can still occur before the central event exists. Ingress validates the resulting
representation and its relationships. Enrichment checks prevent network side
effects; final validation prevents publication of newly introduced bad metadata.
These checks have different purposes. Do not add a third full scan in dispatcher
or repeat validation for each consumer.

Keep all mutation on the producer's Qt thread. Preserve insertion-before-inline-
callback, weak tickets, QObject guards, generation checks and FIFO. Remove a
rejected final ticket and continue flushing later events; no stalled head item.
Never invoke log callbacks while holding dispatcher/attachment locks. The
security boundary introduces no OBS work, blocking network access or cross-thread
mutable shared events.

## 6. Local EventSub development mode

### Current connection architecture

`TwitchClient::startOrResume` requires client ID and access token, validates against
`id.twitch.tv`, resolves the broadcaster through Helix, then connects.
`connectEventSub` has a production URL default in `twitch-client.hpp:48`.
`subscribeOne` hardcodes the production Helix subscription URL and calls
`apiRequest`, which attaches saved access-token and client-ID headers. Timer
reconnects call the default connect method; handoffs accept any valid `wss:` URL
today. Runtime selection also refuses configurations without a client ID.

Changing only the socket URL therefore leaves production authentication and
subscription behavior active, rejects local `ws:` handoffs, and makes an ordinary
retry return to production. All three are design errors to avoid.

Control messages stay outside the event dispatcher, but still need bounded
validation: session IDs use the 256-byte opaque-ID rule; welcome timeout is an
integral number before its documented timer clamp; revocation type must match an
existing subscription key, with status treated as a bounded ASCII token (128
bytes). Reject malformed controls without updating session or subscription state.
Keep the sender/generation checks and 1 MiB envelope bound. Validate reconnect
URLs before opening sockets; never route controls into a permissive event bypass.

### Proposed configuration

Choose process-scoped environment configuration, available in the real plugin
build and read once when constructing the plugin runtime. It requires no OBS
property migration, per-test source edit or special debug binary. Add:

| Variable | Rule |
| --- | --- |
| `BOKIS_EVENTSUB_TEST_URL` | Explicit opt-in; require `ws://127.0.0.1:<port>/ws` or `ws://[::1]:<port>/ws`, with port 1–65535. No userinfo, query, fragment, whitespace, hostname aliases or non-loopback IP. No fixed plugin port. |
| `BOKIS_EVENTSUB_TEST_CHANNEL_ID` | Required when test URL is set; validated opaque ID used for channel/user/broadcaster test identity and subscription conditions. |

If neither is present, behavior is production. Any partial, empty or invalid
test configuration yields visible configuration error and no connection; never
silently fall back to production. Loopback literals avoid DNS/localhost aliases.
Plain local WS is the initial supported mode; CLI TLS testing can be added with
normal certificate verification later, without disabling TLS checks.

Use `EventSubConnectionMode { Production, LocalTest }`,
`EventSubConnectionSettings`, and `readEventSubConnectionSettings`. Production
defaults are `wss://eventsub.wss.twitch.tv/ws` and
`https://api.twitch.tv/helix/eventsub/subscriptions`. Local test subscription URL
is derived, not independently configurable:
`http://<same-loopback-host>:<same-port>/eventsub/subscriptions` (no `/helix`).
Reject redirects for local subscription requests and bypass system proxies for
both local HTTP and WebSocket connections. Never allow configuration to redirect
OAuth endpoints.

Read environment in `src/plugin-main.cpp` and inject immutable settings through
`TwitchClient::Dependencies`; unit tests pass explicit settings and do not depend
on a developer's shell. `PluginRuntime::Service` must select a synthetic effective
test configuration even with blank saved client credentials. All test-mode
attachments join that one environment-selected test channel, with the existing
reset/mailbox lifecycle. Do not modify, compare for authorization, or persist
synthetic values into source settings; preserve saved real credentials exactly.

### Test-mode behavior and credential isolation

1. Log a startup warning with only validated loopback host/port and a persistent
   status prefix: `LOCAL EVENTSUB TEST MODE - simulated events`. Apply the prefix
   to connecting, connected, failed, retrying and revoked states; a later normal
   status update must not erase it. Do not log session/reconnect query secrets.
2. Skip device flow, token validation/refresh and Helix broadcaster lookup. Set
   synthetic client ID `bokis-local-eventsub-test` and configured test user/channel
   ID in effective runtime state. Do not start the 45-minute token timer. Connect
   actions in the OBS UI restart the local feed, never open a browser.
3. Keep persisted Twitch configuration separate from effective local identity.
   Never call the token persistence callback in local mode, including on errors,
   shutdown or settings changes. No production authorization header is constructed
   for a local request. Supply a synthetic `Client-Id` and JSON content type only.
4. On welcome, use the real session ID and existing subscription bodies/versions,
   with test identities. Attempt the existing subscriptions with synthetic local
   capabilities; report unsupported mock types as failed/unavailable, not as a
   real Twitch permission problem. A supported subset can still receive events.
5. For deterministic offline local testing, disable remote catalog refresh and
   asset downloads in this mode. Retain validated URLs/metadata and native text
   fallback. Production uses normal enrichment. Fake-network tests still exercise
   the asset path; test mode is not a URL-policy bypass for loopback assets.
6. Disconnection/watchdog retries use the stored initial endpoint, never a default
   production argument. Fresh welcome creates subscriptions again; successful
   handoff preserves subscriptions and deduplication. Keep exponential backoff
   (1 second up to 30 seconds), generation guards, and 30-second handoff deadline.
7. Local reconnect URL must keep exact configured scheme, loopback host, port
   and `/ws` path. Allow a bounded opaque query (2,048-byte complete URL), preserve
   it exactly, and reject credentials/fragments/controls. Never allow mode changes
   through reconnect messages. Production reconnect permits only HTTPS-equivalent
   secure WS (`wss`) to exact `eventsub.wss.twitch.tv`, default/443 port, with the
   supplied path/query preserved; reject other hosts and safely retry the initial
   endpoint if handoff cannot be accepted. Cover host-policy compatibility with
   real recorded Twitch reconnect fixtures before release.
8. Restarting OBS with the same launch environment reconnects locally. Removing
   both variables and restarting OBS restores the normal saved production setup.
   Do not reread environment mid-session or store test mode in scene collections.

The local exception is for EventSub transport only; it does not make HTTP assets
or arbitrary local URLs valid. An inherited opt-in environment can remain active,
so the persistent status and startup warning are required, not just debug logs.

### Twitch CLI compatibility verified during this audit

The CLI offers a local WS server plus `/eventsub/subscriptions`, configurable
port, strict subscription mode, event forwarding and reconnect simulation.
Strict mode requires a subscription soon after welcome; without it, forwarding
does not require subscriptions. [Twitch CLI WebSocket documentation](https://dev.twitch.tv/docs/cli/websocket-event-command/).

The inspected mock HTTP handler requires nonempty `Client-Id`, validates
type/version and session, and returns 202 on creation; it does not validate an
OAuth Authorization header. A synthetic client ID suffices for this implementation.
The handler rejects event types missing from its registry. Recheck against the
CLI version used for acceptance and record `twitch version`.
[CLI mock manager source](https://github.com/twitchdev/twitch-cli/blob/main/internal/events/websocket/mock_server/manager.go).

The inspected registry has follow, cheer and raid generators, but no chat-message,
chat-moderation or chat-notification generators. It cannot demonstrate the
plugin's subscription/resub/gift event path simply by triggering `subscribe` or
`gift`: those generate different EventSub types, deliberately ignored by this
plugin. Do not add a second production subscription producer or weaken schema
validation to accommodate the tool. [CLI event registry](https://github.com/twitchdev/twitch-cli/blob/main/internal/events/types/types.go),
[CLI event commands](https://github.com/twitchdev/twitch-cli/blob/main/docs/event.md).

Source revision recorded during inspection:
`fd7dac646eea56c9ac9c1d36893e611de855c21e`. The registry and the subscription,
subscription-message and gift generators were inspected directly; this is source
compatibility evidence, not a claim that a particular installed CLI release ran.

The inspected server's strict forwarding check matches subscription type/version,
not all production condition/authorization semantics. Reconnect uses a query
parameter to transfer subscriptions; preserve that query. Successful mock tests
do not prove real scope authorization or correct broadcaster conditions.
[CLI WebSocket server source](https://github.com/twitchdev/twitch-cli/blob/main/internal/events/websocket/mock_server/server.go).

Production ordinary disconnect requires resubscription; handoff retains the old
socket until replacement welcome and migrates subscriptions. Keep those behaviors
in local mode. [Twitch WebSocket lifecycle](https://dev.twitch.tv/docs/eventsub/handling-websocket-events/).

Proposed Linux manual procedure, **usable after implementation, not today**:

```bash
twitch version
twitch event websocket start-server --help
twitch event websocket start-server --ip 127.0.0.1 --port 8099 --require-subscription
```

Launch the actual built plugin in OBS from another terminal (fully exit any old
OBS process first so it inherits the environment):

```bash
BOKIS_EVENTSUB_TEST_URL=ws://127.0.0.1:8099/ws \
BOKIS_EVENTSUB_TEST_CHANNEL_ID=100 obs
```

Then generate supported events and a reconnect:

```bash
twitch event trigger channel.follow --version 2 --transport websocket --from-user 200 --to-user 100
twitch event trigger channel.cheer --transport websocket --from-user 200 --to-user 100 --cost 100
twitch event trigger channel.raid --transport websocket --from-user 200 --to-user 100
twitch event websocket reconnect
```

8099 is an example, not a product default. Verify installed CLI command help;
its forwarding commands use its own local control mechanism rather than accepting
the plugin's WS URL as a webhook forwarding address. Windows uses the equivalent
process environment before launching OBS. Do not insert real tokens into commands.

To meet the full real-build testing objective despite CLI limitations, add a
small **test-only** Qt WebSocket/HTTP fixture server in
`tests/eventsub-fixture-server.cpp` under `ENABLE_TESTS`. It binds only loopback,
accepts an explicit port, serves welcome/keepalive and mock subscription responses,
and sends fixture envelopes through a real socket. No OBS or plugin-only injection
API is added. It consumes the existing ten-event fixtures plus malicious cases,
supports replay/delay/reconnect controls and clean shutdown, and never saves
tokens. Use the same environment mode against it to verify subscriptions,
resubscriptions, Bits, gifts and hostile chat in the actual plugin. Implement
enough bounded HTTP parsing for the test subscription endpoint; do not expose a
general HTTP service or introduce a new production dependency.

Follow/cheer/raid have no Floating alert presentation today; their absence on
screen is not failed ingestion. Use bounded developer counters (kind/disposition,
no text) and the integration suite's second mailbox to verify dispatch. Real chat
fixtures verify native visible text; two OBS sources verify shared ingestion and
independent presentation. No purchases are needed in either local test path.

## 7. Test matrix and acceptance criteria

Run the shared field vectors through direct validators, provider raw parsing,
pipeline and final publication. Test all ten variants, nested users and notice
text, not just ordinary chat. Every accepted event must satisfy the invariant
in two independent consumers; every rejection must create no pending work or
network side effects. Tests are new implementation work, not completed here.

### Semantic strings

| Input/case | Expected result |
| --- | --- |
| `<script>alert(1)</script>` | Exact semantic text, no HTML encoding or executable representation. |
| `<img src=x onerror=alert(1)>` | Exact text. |
| `<svg onload=alert(1)>`, `<iframe src=javascript:alert(1)>` | Exact text; no URL extraction into asset fields. |
| `"double" 'single' < > &` | Exact punctuation preserved. |
| `&lt;script&gt;`, `&#x3c;`, already escaped text | Literal entity spelling preserved. |
| `line1\r\nline2\rline3\n\tend` | CRLF/CR -> LF; LF/tab preserved. |
| CJK, Arabic/Hebrew, accented names | Unicode preserved; display-name and login policies differ. |
| `😀`, skin tones, family ZWJ emoji, flags, `e` + U+0301 | No surrogate/grapheme split; unchanged normalization form. |
| ZWJ/ZWNJ/ZWSP, variation selectors, bidi isolates/overrides | Preserved and classified; no raw log output. |
| Embedded `\u0000`, U+0001, U+007F, U+0085 | U+FFFD repair in text; reject required IDs. |
| Truncated UTF-8, overlong UTF-8, escaped lone surrogate | Reject primary text/envelope; optional label fallback. Test bytes on a real WS connection too. |
| 8,191 / 8,192 / 8,193 code units; giant combining cluster | Exact boundary behavior, grapheme-safe truncation and range rebuild. |
| Envelope at 1 MiB / one byte larger; too many collections | Bounded acceptance/fallback/rejection; no excessive allocations. |

Test literals beginning with markup in display names and labels too: no deletion
just because they resemble HTML. V2 tests assert semantic representation and
native plain-text behavior. They do **not** claim browser execution was tested;
future bridge/DOM tests must prove safe sinks before any HTML renderer ships.

### URLs and assets

| Input/case | Expected result |
| --- | --- |
| `https://static-cdn.jtvnw.net/emoticons/v2/25/static/dark/3.0` with Twitch provider | Accept. |
| `HTTPS://CDN.BETTERTTV.NET/emote/abc/3x` with BTTV provider | Accept; canonical scheme/host. |
| Valid provider URLs with static/animated fallback | Preserve provider; independent fallback validation. |
| HTTP URL, `javascript:alert(1)`, `file:///etc/passwd`, `data:image/svg+xml,...`, `blob:` | Drop URL/record per field policy; zero fetches. |
| ` javascript:...`, `\tHTTPS://...`, trailing whitespace | Reject raw input; no trim-and-accept. |
| Missing host, `https://`, bad `%`, backslash, credentials, fragment, non-443 port | Drop. |
| Exactly 2,048 bytes / 2,049 bytes | Boundary acceptance if otherwise valid / drop. |
| `//cdn.7tv.app/emote/a/3x.webp` | Catalog adapter may resolve to HTTPS; raw event asset URL rejects. |
| Unlisted HTTPS host; `cdn.7tv.app.attacker.example`; IP/private/loopback host | Drop; test that no request is issued. |
| Correct host with wrong provider; `https://cdn.7tv.app@attacker.example/a` | Drop. |
| Redirect to allowed same-provider CDN; redirect loop or fourth hop | Bounded success / fail without extra fetch. |
| Redirect to HTTP/file/local IP/off-list host, including after valid first fetch | Reject before following; no token/cookie forwarded. |
| HTML/SVG bytes returned as image; oversized final chunk; decompression/animation pressure | Decode failure or existing bounded fallback, never active content. |

### Structured fields, numeric values and publication

- IDs: empty, 256/257 UTF-8 bytes, Unicode opaque IDs, whitespace/control/format
  characters, case-distinct values, origin/reply/community IDs. Confirm invalid
  targeted clears reject rather than clearing all chat. Anonymous actor identities
  are stripped while recipients remain validated.
- Colors: empty, `#a1B2c3`, `red`, `#123`, `#12345678`, CSS functions and NUL.
  Only six-digit hex becomes typed RGB; invalid input takes renderer fallback.
- Emotes: invalid provider enum, wrong provider URL, missing ID/name, unexpected
  but legitimate Unicode name, optional malformed owner/set ID, missing primary
  or fallback URL, zero-width overlay token. Preserve valid metadata/text.
- Ranges: negative offset/length, zero occurrence length, overflow near
  `qsizetype` max, endpoint at text length, out-of-bounds, overlap, unsorted ranges,
  substring mismatch, gap, surrogate/grapheme split and absent range. Full-text
  fallback is deterministic; invalid optional decoration cannot erase text.
- Badges: empty/invalid type/version, unknown provider, duplicate record, bad URL,
  oversized info, 64/65 entries. Plain metadata only, consistent drop behavior.
- Counts: `-1`, `0`, `1`, specified maximum and maximum+1 for every field;
  INT_MIN/INT_MAX, 2^31, 2^53±1, fraction, numeric string, boolean, null/missing,
  non-finite programmatic values. Required rejects; optional drops. Zero follows
  the table, not one global rule. Verify unknown tier and inconsistent streak.
- Time: explicit offsets, zero/3/9 fractional digits, invalid date/leap values,
  missing zone, out-of-range year, old fixture date, delayed clear. Verify UTC
  milliseconds and no unintended chronology changes.
- Ingress/final validation idempotence: validating repaired data again produces
  no further semantic changes. Same field policy across all ten variants.
- Dedup: valid repeated event ID; invalid-then-valid same ID; two gifts sharing
  community ID; TTL/cap eviction; reconnect vs generation reset. No poison ID
  entry on initial rejection; no sequence wrap.
- Malicious catalog after valid ingress, cached synchronous completion, expired
  ticket and pressure flush, stopped/replaced channel, callback after destruction.
  Final validation is unavoidable; dropped jobs do not stall FIFO.
- Publication: direct `publish(EventPtr)` no longer compiles; only the factory can
  create the accepted wrapper. Null/invalid factory result cannot publish; both
  consumers receive the same immutable event and independent queues still work.
- Resource/log stress: 10,000 bad events, adversarial unique fields, huge catalogs
  and duplicate names, overflowing retained capacities. Fixed memory/log bounds;
  no secrets or raw text in diagnostics; no graphics/network work under locks.

### Local mode and reconnect

| Scenario | Required outcome |
| --- | --- |
| Environment absent | Existing production URLs, auth/scopes, subscriptions, assets and settings behavior. |
| Valid test environment and no saved credentials | Connect locally; fake Client-Id only; no OAuth/Helix/catalog/image requests. |
| Real saved credentials plus test environment | Credentials remain byte-for-byte unchanged and absent from every local request/log. |
| Empty/partial/invalid config, hostname/remote URL, wrong scheme, port 0/65536 | Clear persistent error, no silent production or external connection. |
| CLI non-strict and strict modes | Supported events delivered; required subscription timing met; unsupported chat types reported. |
| Startup server absent, server restart, watchdog timeout, duplicate welcome | Bounded retry to the same local endpoint; fresh sessions resubscribe once. |
| Handoff with opaque `reconnect_id` query | Preserve query, old socket until welcome, no duplicate subscriptions. |
| Handoff to another port/host/scheme; redirecting HTTP endpoint | Reject destination; no remote or production fallback. |
| OBS restart with/without launch variables | Local resumes with warning / production resumes using untouched settings. |
| Connect button, source settings changes, two sources, last source removed | Correct local lifecycle; no auth UI or token writes; no second producer. |
| All ten event kinds through real-socket fixture server | Same validation/publication as production; hostile text remains semantic data. |
| Mock follow/cheer/raid absent from Floating display | Assert dispatcher counters/mailboxes, not invented alert rendering. |

Acceptance requires Linux and Windows configure/build/tests, fake-network assertions
of every request destination/header, a real-loopback integration test, and manual
OBS runs with Twitch CLI plus the fixture server. Record CLI version, commands,
plugin build and coverage limitations. No live paid actions or production token
access is required for local acceptance.

## 8. Implementation sequence and expected files

1. Add shared field policy, bounded diagnostics and focused table-driven tests.
   Fix raw parse conversions and optional-field dispositions first so the central
   validator receives meaningful typed values and issue evidence.
2. Integrate ingress and final validation, restricted publication handle and
   dispatcher API migration. Update existing tests to use realistic valid events.
3. Apply the same policy to catalog entries/occurrences, network requests,
   redirects and final response-byte limits. Retest Unicode/range/text fallback.
4. Implement immutable production/local connection settings, isolated request
   construction, runtime attachment selection, persistent status and reconnect.
5. Add real-socket loopback tests and test-only fixture server; exercise supported
   CLI events and the remaining event kinds without changing the production event
   model. Update developer docs and test instructions.
6. Run quality gates, review changed behavior and only then prepare the
   `0.1.0-alpha.13` version metadata. Version change is not a release/tag/push.

Complete expected file inventory for that implementation (grouped pairs mean
both named `.hpp` and `.cpp` files):

| Files | Expected work |
| --- | --- |
| **New** `src/core/event-validation.hpp`, `src/core/event-validation.cpp` | Policies, result/issue types, helpers and restricted validated-publication factory. |
| `src/core/event-dispatcher.hpp`, `src/core/event-dispatcher.cpp` | Publish validated handle; preserve consumer API and threading. |
| `src/core/ordered-event-pipeline.hpp`, `src/core/ordered-event-pipeline.cpp` | Policy/diagnostic ownership, entry/final checks, local enrichment option. |
| `src/twitch/event-normalizer.hpp`, `src/twitch/event-normalizer.cpp` | Bounded parse issues, strict required/optional values and timestamp handling. |
| `src/twitch/chat-message-parser.hpp`, `src/twitch/chat-message-parser.cpp` | Raw color/count/URL evidence, shared policy and bounded collection parsing. |
| `src/chat/chat-types.hpp` | Document semantic strings, range/URL/ownership contracts; preserve current structures unless diagnostics require a minimal signature adjustment. |
| `src/chat/emote-catalog.hpp`, `src/chat/emote-catalog.cpp` | Shared field policy, catalog resource limits and safe range assignment. |
| `src/chat/emote-service.hpp`, `src/chat/emote-service.cpp` | Validate before fetch, catalog redirect handling, isolated asset requests, local no-network enrichment. |
| `src/chat/image-cache.hpp`, `src/chat/image-cache.cpp` | Purpose/provider-aware URL requests, manual redirect guard, final byte-limit checks. |
| **New** `src/twitch/eventsub-connection-settings.hpp`, `src/twitch/eventsub-connection-settings.cpp` | Environment parsing, modes and endpoint/reconnect rules; no OAuth secrets. |
| `src/twitch/twitch-client.hpp`, `src/twitch/twitch-client.cpp` | Inject settings, auth isolation, mock subscriptions, persistent status, reconnect and log aggregation. |
| `src/twitch/eventsub-socket.hpp`, `src/twitch/eventsub-socket.cpp` | Pass local proxy/connection options; retain frame limits and real WS seam. |
| `src/core/plugin-runtime.hpp`, `src/core/plugin-runtime.cpp` | Effective local configuration and attachment matching without changing persisted credentials. |
| `src/plugin-main.cpp` | Read process environment once and inject settings. |
| `CMakeLists.txt`, `tests/CMakeLists.txt` | New sources, acyclic policy linkage, test targets and fixture-server target under ENABLE_TESTS. |
| **New** `tests/event-validation-tests.cpp`, `tests/eventsub-connection-tests.cpp` | Policy matrix and real-loopback endpoint/credential/reconnect tests. |
| **New** `tests/eventsub-fixture-server.cpp` | Manual real-OBS fixture transport, loopback only. |
| **New** `tests/fixtures/security-events.json` | Hostile and boundary inputs; only synthetic data. |
| `tests/event-normalizer-tests.cpp`, `tests/event-dispatcher-tests.cpp` | Raw conversion regressions and restricted publication fixtures. |
| `tests/chat-tests.hpp`, `tests/chat-tests.cpp` | Provider/URL/range/redirect/decoder regression coverage. |
| `tests/twitch-producer-tests.hpp`, `tests/twitch-producer-tests.cpp` | Pipeline final validation, logs, local credentials/runtime and lifecycle tests. |
| `tests/fixtures/eventsub-events.json` | Complete valid test identities plus reusable local cases where required; preserve ten-event coverage. |
| `docs/ARCHITECTURE.md`, `docs/EVENT_FOUNDATION.md`, `docs/TWITCH_PRODUCER_INTEGRATION.md` | Update actual contracts after implementation, preserving historical audit context. |
| **New** `docs/LOCAL_EVENTSUB_TESTING.md`; `tests/README.md`; `README.md` | Setup, persistent mode warning, CLI coverage limits, fixtures and test commands. |
| `VERSION`, `buildspec.json` | Set `0.1.0-alpha.13` in both project-version fields only when implementation is ready; do not change dependency versions or run the publishing release script. |

This plan file remains the historical audit; subsequent implementation results
belong in current architecture/testing docs. No change is expected to renderer
layout/lane/GPU code, updater/release logic, web-theme resources, OBS property keys
or source registration. If implementation uncovers another necessary file, report
the reason rather than silently expanding scope.

## 9. Risk review

| Risk | Decision and verification |
| --- | --- |
| Double validation / drift | Share helpers. Raw parsing checks information that conversion destroys; ingress checks typed semantics; pre-fetch checks side effects; final bounded scan certifies publication. No scan per consumer. Test idempotence. |
| Performance | Bound envelope/collections/catalogs before scans; linear string/range traversal, checked sizes and fixed diagnostic tables. Benchmark worst allowed messages and catalog application on the Qt thread; no blocking I/O added. |
| Broken Unicode / legitimate text corruption | No HTML escaping, Unicode normalization or blanket format-character stripping. Preserve graphemes and literal entities; rebuild ranges only after text repair. Record deliberate truncation/control changes. |
| URL false positives | Explicit host registry from fixtures; protocol-relative conversion only in known adapters. Log reason/provider, not URL. New CDN hosts require reviewed fixtures; arbitrary GIF tightening is documented. |
| SSRF/redirect/auth leaks | Validate before network side effects, manual redirects, isolated assets, synthetic-only local headers, proxy bypass and negative destination assertions. Host policy does not claim to solve compromised DNS. |
| Races/reentrancy | Producer-thread mutation, weak/QPointer/generation guards, immutable freeze, no mutable aliases and no callbacks under locks. Test inline completion, teardown, pressure and reentrant log sinks. |
| Reconnect and duplication | Remove default-argument production fallback, validate mode-specific handoff, retain query and dedup, fresh-session resubscription only. Test lost replacement welcome and stale subscription replies. |
| Test mode leaking into production | Explicit environment, fail-closed partial config, persistent visible status, no settings persistence, no live endpoint fallback. Test restart both ways with real credentials represented by sentinels. |
| Mock coverage mistaken for Twitch coverage | Pin/record CLI version, distinguish supported subset, use fixture server for missing chat types, retain separate production auth/scope tests. Native non-chat rendering is out of scope. |
| Future renderer misuses validated strings | Document that validation does not grant HTML trust; future safe DOM sinks/output encoding remain mandatory and require their own tests. |

## 10. Audit validation and final Git report

Baseline verification was run after creating the branch, without changing runtime
code:

```bash
cmake --preset linux-x86_64 -DENABLE_TESTS=ON
cmake --build --preset linux-x86_64 -j 4
ctest --test-dir build/linux-x86_64 --output-on-failure
```

- CMake configure passed. Existing optional dependency notice remains:
  `Could NOT find WrapVulkanHeaders (missing: Vulkan_INCLUDE_DIR)`.
- Plugin and test build passed; no compiler warnings appeared in build output.
- Whitespace checks passed for both tracked changes and the new plan file; the
  existing CTest log contained no warning/failure diagnostics.
- **8/8 existing suites passed**: chat, event dispatcher, event normalizer,
  updater, post-exit, manifest, Linux installer and Twitch producer (13.33 seconds).
- These are baseline tests, not validation of the proposed V2 boundary. No V2
  implementation, live OBS GUI/GPU test, Windows build, live Twitch test, actual
  CLI server run or new sanitizer run was performed. Twitch CLI is not installed
  in this environment; compatibility analysis used its official docs/source.
- Original branch: `main`. Working branch: `feature/v2-security-boundary`.
  Workspace started clean. The only project file added by this task is
  `docs/V2_SECURITY_BOUNDARY_PLAN.md`; existing tracked files are unchanged.
  Build outputs are under the ignored `build/` directory. Nothing is staged,
  committed, pushed, tagged or released.

Expected final porcelain status:

```text
?? docs/V2_SECURITY_BOUNDARY_PLAN.md
```
