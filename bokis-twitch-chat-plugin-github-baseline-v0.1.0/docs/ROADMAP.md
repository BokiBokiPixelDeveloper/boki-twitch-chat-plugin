# Roadmap

## Phase 1 — Repository baseline
- OBS-plugin-oriented project structure
- Generic product naming: Bokis Twitch Chat Plugin
- CMake presets
- GitHub CI
- Tag-driven release pipeline
- SHA-256 release artifacts

## Phase 2 — Stabilize shared architecture
- Split Twitch client, normalized message core, asset cache and renderers
- Move current lane/sub-lane implementation behind a Floating renderer boundary
- Add unit tests for non-OBS logic
- Preserve source ID/settings

## Phase 3 — Additional presentation modes
- Native Classic vertical renderer
- Shared badge/emote/GIF model across renderers
- Renderer profile/settings model
- Define localhost message-bridge contract
- Add first HTML/CSS/JavaScript Classic web theme

## Phase 4 — Release hardening
- Pin GitHub Actions to immutable commit SHAs
- Pin canonical build environment instead of floating Arch packages
- Enable protected `main` ruleset
- Enable immutable GitHub Releases
- Add GitHub artifact attestations

## Phase 5 — Update checker
- Check stable/beta channel metadata
- Compare semantic versions
- Check OBS compatibility
- Present release notes in OBS

## Phase 6 — Safe updater
- Download to staging
- Verify SHA-256 and provenance/signature
- Install only after OBS exits
- Atomic replacement
- Backup + rollback
