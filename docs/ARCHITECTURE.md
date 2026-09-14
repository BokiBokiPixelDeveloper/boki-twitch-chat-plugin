# Architecture — Bokis Twitch Chat Plugin

The project is deliberately **not tied to one chat visual style**. Twitch ingestion and chat data are shared; renderers are interchangeable consumers of the same normalized message model.

## Product goal

Bokis Twitch Chat Plugin should support multiple presentation modes from one Twitch connection:

1. **Floating / Danmaku renderer** — the current native OBS renderer. Messages travel horizontally with lane/sub-lane scheduling, dynamic speed/size and independent GIF objects.
2. **Classic vertical renderer** — a conventional chat overlay where messages stack vertically and move upward as new messages arrive.
3. **Web themes** — optional HTML/CSS/JavaScript themes for creators who want to author conventional browser-style chat overlays themselves.

Individual visual styles must never become product-level identifiers. They belong only to specific renderer modes.

## Intended shared pipeline

```text
Twitch Device Flow / EventSub
            |
            v
   Normalized ChatMessage
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

The shared core should eventually own:

- Twitch authentication and EventSub lifecycle
- normalized users, text fragments, emotes, badges and GIFs
- asset/cache management
- message timestamps and IDs
- moderation/deletion events where supported
- renderer-independent configuration

Renderer-specific behavior must stay outside the Twitch client. Lane scheduling belongs to the floating renderer; vertical stacking belongs to the classic renderer.

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
