# Architecture — Bokis Twitch Chat Plugin

The project is deliberately **not tied to one chat visual style**. Twitch ingestion and chat data are shared; renderers are interchangeable consumers of the same normalized message model.

## Product goal

Bokis Twitch Chat Plugin should support multiple presentation modes from one Twitch connection:

1. **Floating / Danmaku renderer** — the current native OBS renderer. Messages travel horizontally with lane/sub-lane scheduling, dynamic speed/size and independent GIF objects.
2. **Classic vertical renderer** — a conventional chat overlay where messages stack vertically and move upward as new messages arrive.
3. **Web themes** — optional HTML/CSS/JavaScript themes for creators who want to author conventional browser-style chat overlays themselves.

Individual visual styles must never become product-level identifiers. They belong only to specific renderer modes.

## Shared pipeline

The [internal event foundation](EVENT_FOUNDATION.md) supplies the normalized event
variant, pure Twitch normalizer, ordered enrichment, and RAII mailbox dispatcher.
`PluginRuntime` owns one application-thread Twitch client. Compatible sources
attach independent consumer mailboxes; conflicting settings produce a visible
status instead of a second connection. Future consumers subscribe to the same
runtime. See [the integration report](TWITCH_PRODUCER_INTEGRATION.md).

Built-in synthetic events are already normalized, but join the same
`OrderedEventPipeline` before ingress validation. `EventHeader::origin`
distinguishes production, local transport, and synthetic events for diagnostics
without changing consumer behavior. See [event testing](EVENT_TESTING.md).

```text
Twitch Device Flow / EventSub
            |
            v
   Normalized PluginEvent
            |
   Central validation policy
            |
   Provider-safe enrichment
            |
    Final validation/freeze
            |
      Message Bus/Core
      /             \
     v               v
Native renderers   Web bridge (future)
     |               |
     v               v
OBS render loop    HTML/CSS/JS themes
```

## Core responsibilities

The shared backend owns:

- Twitch authentication and EventSub lifecycle
- normalized users, text fragments, emotes, badges and GIFs
- asset/cache management
- message timestamps and IDs
- moderation/deletion events where supported
- renderer-independent configuration

Renderer-specific behavior must stay outside the Twitch client. Lane scheduling belongs to the floating renderer; vertical stacking belongs to the classic renderer.

### Current emoji/emote pipeline

`ChatMessage` contains text and ordered `ChatFragment` records, including emote
URLs, fallback text and shared immutable decoded images. It contains no positions,
speeds, lane state or textures. `EmoteCatalog` normalizes Twitch metadata and
third-party catalogs; `EmoteService` loads the channel/global catalogs and resolves
images in arrival order. `ImageCache` downloads and decodes CPU images on the Qt
thread, with size, concurrency, timeout and cache limits.

The Floating renderer builds `MessageLayout` using Qt text shaping and inline
image slots before enqueueing `PreparedMessage`. Color glyphs are rasterized as
text rather than converted to vector outlines. `RenderMessage` and `RenderEmote`
own movement, animation cursors and texture handles exclusively inside the renderer.
OBS tick advances each animation using its frame delays; OBS render creates,
updates and destroys GPU textures. Expired messages defer texture destruction to
the graphics path, including all inline emote textures.

## Native renderers

Native modes should remain tied to OBS `video_tick` / `video_render` so continuous motion is not dependent on CEF/browser-source frame pacing.

### Floating

Current baseline implementation:

- right-to-left messages
- primary lanes + jitter + sub-lanes
- queue-pressure-dependent size/speed
- independent bouncing GIF objects

### Classic vertical

Planned renderer:

- bottom-up or top-down stacking
- configurable history length
- enter/leave animation
- user/badge/emote layout
- configurable background/card treatment

## Web themes

Web themes are intentionally a separate optional presentation path. A future local bridge can expose normalized messages to a browser theme over localhost/WebSocket. This keeps creator-authored HTML/CSS/JavaScript possible without forcing the native floating renderer back through CEF.

The initial theme resource location is:

```text
resources/web-themes/
```

A web theme should eventually be a self-contained directory containing at least:

```text
index.html
style.css
chat.js
theme.json
```

The plugin/core should provide data; themes should own appearance.

## Compatibility rule

Product-level identifiers use `bokis-twitch-chat-plugin` / `bokis_twitch_chat_plugin`. Renderer names are configuration-level concepts and may evolve independently.
