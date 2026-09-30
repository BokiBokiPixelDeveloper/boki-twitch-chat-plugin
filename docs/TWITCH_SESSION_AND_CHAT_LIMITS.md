# Twitch sessions and chat limits

The plugin renews Twitch authorization automatically when validation or an API
request reports an expired access token. Validation runs at least every 45 minutes
and shortly after the expiry reported by Twitch. Temporary network errors,
HTTP 408/429 and server errors retry automatically with backoff, up to 60 seconds
between attempts. Successful renewal saves the rotated token pair to the source
settings and establishes a fresh EventSub session. Stop cancels pending retries.
A rejected refresh token still requires **Connect to Twitch (Device Flow)**.

Twitch does not offer permanent Device Flow tokens. Its documentation specifies
four-hour access tokens, single-use refresh tokens, and refresh-token expiry after
30 days of inactivity. Revocation can also require authorization again. See
[Twitch Device Flow](https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/#device-code-grant-flow).

In **Web Widget** source properties:

- **StreamElements chat: Maximum messages** defaults to 20 (range 1–200).
- **StreamElements chat: Maximum height (px)** defaults to 600 (range 64–4320),
  also constrained by the existing canvas height.

The managed layout supports Scrapbook-style `.main-container` widgets with
`.message-row` and `.alert-row` children. New messages appear at the bottom;
scrolling follows changes in row size, animation, image loading and fonts.
Old rows are removed, including alert rows, and adapter moderation history is
bounded as well. The imported widget files remain unchanged. Applying new limits
recreates the widget, clearing its current history. Arbitrary generic web widgets
still control their own DOM and layout; these settings are not a universal memory
limit for third-party JavaScript or externally loaded assets.

Regression coverage includes temporary validation/refresh outages, rejected
refresh tokens, stopping during retry, renewal at expiry, unauthorized EventSub
subscription recovery, bounded moderation history, and a real Chromium Scrapbook
probe that overfills the box with message and alert rows and verifies the row cap,
pixel height and bottom scroll position.

## Changed files

- `src/twitch/twitch-client.cpp` and `.hpp`: expiry-aware validation, retry and renewal recovery.
- `src/plugin-main.cpp`: defaults for the new settings.
- `src/renderer/chat-source.cpp` and `.hpp`: property controls and runtime reconfiguration.
- `src/web/web-widget-runtime.cpp` and `.hpp`: pass limits into the adapter configuration.
- `resources/web-runtime/streamelements-adapter.js`: bounded history, row pruning and bottom-following layout.
- `tests/twitch-producer-tests.cpp` and `.hpp`: authentication regression tests.
- `tests/streamelements-adapter-tests.js`: bounded moderation history.
- `tests/web-runtime-tests.cpp` and `tests/widget-browser-probe.js`: custom-limit browser coverage.
- `docs/TWITCH_SESSION_AND_CHAT_LIMITS.md`: behavior, settings and limitations.
