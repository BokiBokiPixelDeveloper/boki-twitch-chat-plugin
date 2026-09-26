# Local EventSub testing

For routine UI and consumer testing, use the built-in source-property controls
described in [EVENT_TESTING.md](EVENT_TESTING.md). This local Twitch CLI mode is
for tests that must also cover the EventSub WebSocket transport and provider
parser.

Local EventSub mode lets a development OBS process use Twitch CLI's mock
WebSocket transport while retaining the plugin's production socket, parser,
validation pipeline, dispatcher and consumers. It is disabled by default.

Start the mock server on a port you choose:

```bash
twitch event websocket start-server --ip 127.0.0.1 --port 8099 --require-subscription
```

Start OBS from the same terminal with both required environment variables:

```bash
BOKIS_EVENTSUB_TEST_URL=ws://127.0.0.1:8099/ws \
BOKIS_EVENTSUB_TEST_CHANNEL_ID=100 \
obs
```

The plugin accepts only `ws://127.0.0.1:<port>/ws` and
`ws://[::1]:<port>/ws`. It rejects `localhost`, remote hosts, secure WS URLs,
paths other than `/ws`, missing variables, and malformed values. The persistent
status and log output include `LOCAL EVENTSUB TEST MODE`; startup also logs the
validated endpoint. No Twitch OAuth validation, broadcaster lookup, refresh, or
saved-token update occurs in this mode. Mock subscription requests use a synthetic
client ID and never carry saved OAuth credentials.

Twitch CLI documents these supported examples:

```bash
twitch event trigger channel.follow --version 2 --transport websocket --from-user 200 --to-user 100
twitch event trigger channel.cheer --transport websocket --from-user 200 --to-user 100 --cost 100
twitch event trigger channel.raid --transport websocket --from-user 200 --to-user 100
twitch event websocket reconnect
```

This repository's automated tests use a socket/HTTP mock with the same protocol
shape. Twitch CLI was not installed in the development environment used for this
change, so the commands above were verified against the current official Twitch
CLI documentation and source, not executed locally.

The CLI mock supports follow, cheer and raid generators. It does not provide the
plugin's `channel.chat.message`, chat moderation, or `channel.chat.notification`
subscription/resubscription/gift/community-gift producers. Its `subscribe`,
`subscribe-message`, and `channel-gift` generators use different EventSub types,
which this plugin correctly ignores. Use the existing fixture-based producer suite
for those event kinds.

To disable local mode, remove both environment variables and restart OBS. Normal
production behavior then connects to `wss://eventsub.wss.twitch.tv/ws` and uses
the saved Twitch configuration. Local mode is intended only for development on a
trusted loopback machine; it is not a substitute for production authorization or
scope testing.
