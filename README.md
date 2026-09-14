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
- GitHub CI baseline
- Tag-driven GitHub Release baseline
- SHA-256 release package + update manifest generation
- Built-in update checker in OBS properties
- Verified in-place Linux plugin update with automatic backup and restart prompt
- Architecture reserved for additional render modes and web themes

See `docs/ARCHITECTURE.md` for the multi-renderer direction.

## Planned presentation modes

- **Floating** — current native right-to-left mode with lanes, dynamic speed/size and free-moving GIFs.
- **Classic vertical** — conventional stacked chat that moves upward.
- **Web themes** — optional HTML/CSS/JavaScript themes fed by a future local message bridge.

Twitch connectivity and normalized message data should be shared across all modes.

## Local Arch development

Install dependencies:

```bash
./scripts/dev-bootstrap-arch.sh
```

Build:

```bash
./scripts/build-local.sh
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

The updater downloads the release binary, verifies its SHA-256 from `update-manifest.json`, backs up the currently loaded plugin, and replaces the on-disk `.so` atomically. OBS keeps the already loaded binary mapped until exit; the new version becomes active after a full OBS restart.

For unauthenticated update checks the GitHub release feed must be publicly readable. If the source repository remains private, use a separate public release/feed repository rather than embedding a GitHub personal access token in the plugin.
