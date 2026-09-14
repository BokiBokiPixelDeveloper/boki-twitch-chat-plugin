# Agent Instructions — Bokis Twitch Chat Plugin

## Product scope
- This is a general Twitch chat plugin, not a product dedicated to one visual style.
- The current right-to-left lane renderer is the **Floating renderer**.
- Future modes include a native **Classic vertical renderer** and optional HTML/CSS/JavaScript **Web themes**.

## Architecture
- Keep Twitch ingestion and normalized chat data renderer-independent.
- The current Floating renderer is a native OBS renderer. Do not move it back to CEF/browser rendering.
- Twitch chat uses EventSub over WebSocket.
- Keep network callbacks away from OBS graphics operations; GPU resources belong on the OBS graphics/render path.
- Renderer-specific state belongs to the renderer. Lane scheduling must not leak into the shared Twitch/core model.
- Future web themes should consume a local normalized-message bridge rather than implement Twitch authentication independently.

## Compatibility
- Do not change the OBS source ID `bokis_twitch_chat_plugin` without explicit approval.
- Existing OBS source property keys are API-like compatibility surface. Renames require migration.
- Preserve existing user settings across plugin updates.

## Security
- Never commit Twitch tokens, OAuth codes, client secrets, private keys, or credentials.
- Prefer Twitch Device Flow; do not embed a client secret in the plugin binary.
- Update packages must be verified before installation.

## Git
- Do not push directly to `main`.
- Work on a feature/fix branch.
- Do not commit, push, tag, publish a release, or rewrite history unless explicitly requested.
- Keep changes scoped to the requested task.

## Quality Gate
Before calling a coding task complete:
1. Configure with CMake.
2. Build the plugin.
3. Run available tests.
4. Report changed files.
5. Report build/test status and remaining warnings.

## Style
- C++20.
- Prefer RAII and explicit ownership.
- Avoid blocking network operations on OBS render/tick paths.
- Keep update/release code separated from Twitch and renderer code.
- Keep product/core naming generic; visual-style terminology belongs only to renderer modules/settings.
