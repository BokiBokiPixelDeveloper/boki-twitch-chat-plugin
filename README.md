# Bokis Twitch Chat Plugin

GitHub-oriented OBS Studio plugin baseline for a **general Twitch chat rendering platform**.

The project is intentionally not named after one visual style. The current implementation provides the native floating/right-to-left renderer, while the architecture is intended to support a classic vertical chat renderer and optional creator-authored HTML/CSS/JavaScript themes later.

## Current baseline

- Native OBS source (no Browser Source / CEF for the current floating renderer)
- Twitch Device Flow + EventSub WebSocket
- Native message movement tied to OBS tick/render
- Floating renderer with lane/sub-lane logic
- GIF objects with lifetime/bounce behavior
- Configurable font settings
- Inline static/animated Twitch, 7TV, BetterTTV and FrankerFaceZ emotes
- Bundled color Unicode emoji font (including skin tones, flags and ZWJ sequences)
- GitHub CI baseline
- Tag-driven GitHub Release baseline
- SHA-256 release package + update manifest generation
- Built-in update checker in OBS properties
- Verified Linux update download staged for installation after OBS exits
- Architecture reserved for additional render modes and web themes

See `docs/ARCHITECTURE.md` for the multi-renderer direction.

## Planned presentation modes

- **Floating** — current native right-to-left mode with lanes, dynamic speed/size and free-moving GIFs.
- **Classic vertical** — conventional stacked chat that moves upward.
- **Web themes** — optional HTML/CSS/JavaScript themes fed by a future local message bridge.

Twitch connectivity and normalized message data should be shared across all modes.

## Emoji and emote support

Twitch emotes are read from the ordered EventSub fragments, using the animated CDN
variant when available. Global and channel emote catalogs for **7TV, BetterTTV and
FrankerFaceZ** load automatically when connecting and refresh every five minutes.
Third-party codes match whole, case-sensitive whitespace-separated tokens; native
Twitch fragments take precedence. Channel catalogs override global catalogs, with
7TV, then BetterTTV, then FrankerFaceZ resolving collisions within each scope.
7TV zero-width emotes and BetterTTV modifier emotes overlay the preceding emote.

PNG, GIF and animated WebP images render inline at the message's font size with
their original aspect ratio. Animations use individual frame delays and loop with
OBS timing. Existing free-moving GIF settings continue to control separate GIF
objects; they do not disable inline emotes.

Image requests share a 64 MiB CPU cache and at most six concurrent downloads. A
message waits up to 2.5 seconds for its assets while preserving arrival order;
failed/late images retain their original text. Downloads are limited to 8 MiB,
source dimensions to 2048 px, and decoded images to 512 frames / 16 MiB with frames
scaled to at most 256 px. Longer animations use smaller frames to fit the memory
budget while retaining their complete loop; animations beyond 512 frames are
shortened. Active messages retain shared image ownership beyond the cache's lifetime.

The native text renderer uses the bundled **Noto Color Emoji** font, so Unicode
emoji work offline without a separately installed font. Its license and pinned
upstream revision are in `resources/fonts/`. This adds about 10 MiB to the plugin.
Building now requires **Qt 6.9+** for modern emoji shaping and **libwebp** for WebP
decoding; the Arch bootstrap and CI install the dependencies.

In OBS source properties, use **Nachrichten → Emojis und Emotes testen** to display
Unicode emoji plus a static and animated sample without Twitch access. Live Twitch
emotes and provider catalogs require an internet connection. Cheermotes, personal
7TV emote sets and provider-specific CSS/mask effects are not implemented.

## Local Arch development

Install dependencies:

```bash
./scripts/dev-bootstrap-arch.sh
```

Build:

```bash
./scripts/build-local.sh
```

Run the offline regression tests:

```bash
cmake --preset linux-x86_64 -DENABLE_TESTS=ON
cmake --build --preset linux-x86_64
ctest --test-dir build/linux-x86_64 --output-on-failure
```

Install into the current user's OBS profile:

```bash
./scripts/install-user.sh
```

Then restart OBS completely.

## Repository workflow

Use feature/fix branches. Do not develop directly on `main`.

Example:

```bash
git switch -c feature/classic-vertical-renderer
```

Run the build before merging:

```bash
./scripts/build-local.sh
```

## Release flow

1. Update `VERSION` and `buildspec.json` to the same SemVer.
2. Commit and merge to `main`.
3. Create an annotated version tag, e.g. `v0.2.0`.
4. Push the tag.
5. GitHub Actions builds the Linux package, generates SHA-256 and `update-manifest.json`, then creates a GitHub Release.

The release pipeline is intentionally a baseline. Before public production use, follow `docs/ROADMAP.md` and harden the build environment, action pinning, attestations, and release immutability.

## Separate GitHub account

See `docs/GITHUB_SETUP.md`.

## Agent usage

OpenCode/Codex/other coding agents should read `AGENTS.md` before modifying this repository.

## Built-in updater

The OBS source properties include an **Updates** section from the first installation:

- current version
- automatic update check on startup
- **Nach Updates suchen**
- **Update installieren** when a newer compatible release is available

The updater asynchronously downloads the release binary, checks its mandatory size and SHA-256 from `update-manifest.json`, and atomically stages it in `$XDG_CACHE_HOME/bokis-twitch-chat-plugin/pending` (default `~/.cache/...`). It persists `pending.json` before starting the installed `bokis-twitch-chat-updater` helper. The UI displays **Update bereit – OBS vollständig schließen. Die Installation erfolgt automatisch nach dem Beenden.**

The detached Linux helper waits for the originating OBS process to exit, verifies the pending binary again, creates a durable backup, and atomically replaces the plugin using a temporary file in the plugin directory. Fully close OBS and wait for `last-update-result.json` before opening OBS again. The next source initialization consumes this result and displays it alongside subsequent update status. Source IDs, settings, and renderer behavior remain compatible.

Install the complete ZIP once to obtain the helper: older installations containing only a `.so` cannot bootstrap it through the existing binary-only updater. The public update manifest remains schema 1 and may include `platforms.linux-x86_64.helper` with URL, SHA-256, and size. When present, both binaries are verified and installed together with backups and rollback; otherwise updates remain plugin-only. Paired updates require a helper supporting local pending schema 2. Before swapping, the helper also checks `/proc/*/maps` for other processes using the installed plugin. See [Linux post-exit updater](docs/POST_EXIT_UPDATER.md) for the lifecycle, local metadata schema, failure semantics, limitations, and manual tests.

Property buttons request their rebuild by returning `true`; changing updater status never triggers a property rebuild. Network completion notifications use `obs_queue_task(OBS_TASK_UI, ..., false)` and ignore callbacks whose updater has already been destroyed.

For unauthenticated update checks the GitHub release feed must be publicly readable. If the source repository remains private, use a separate public release/feed repository rather than embedding a GitHub personal access token in the plugin.
