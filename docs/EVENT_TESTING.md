# Event Testing

Version 2 provides two separate event-testing mechanisms.

## Built-in synthetic events

Open the source properties and expand **Event Testing**. **Enable Event Test
Mode** is off by default and is saved with that OBS source. The action buttons
are disabled until the setting is enabled.

The section provides Test Chat Message, Test Delete Message, Test Clear Chat,
Test Follow, Test Subscription, Test Resubscription, Test Gift Subscription,
Test Community Gift Subscription, Test Cheer, and Test Raid actions.

The display name, chat text, cheer amount, raid viewer count, community gift
count, and resubscription month count are editable. Defaults are deterministic.
The delete action targets the most recently generated synthetic chat message,
or a stable placeholder ID if no synthetic message has been generated yet.

A button creates one normalized event in the shared runtime. It enters the
ordered pipeline before ingress validation, passes through enrichment and final
validation, becomes immutable, and is published once to every matching
consumer. Test events do not call the dispatcher directly and renderers do not
need test-specific behavior. Viewer-like test text remains semantic text, so a
value such as `<script>alert(1)</script>` exercises the same policy as Twitch
text without being converted to HTML.

Each event header records an origin: `Production`, `LocalTransportTest`, or
`SyntheticTest`. Origin is diagnostic metadata and does not grant different
validation or rendering behavior.

## Twitch CLI transport testing

Built-in testing exercises normalized event handling, validation, enrichment,
dispatch, and consumers. Twitch CLI testing additionally exercises the local
WebSocket transport and Twitch provider parser. See
[LOCAL_EVENTSUB_TESTING.md](LOCAL_EVENTSUB_TESTING.md) for setup.

The local transport remains a module-wide development override configured by
`BOKIS_EVENTSUB_TEST_URL` and `BOKIS_EVENTSUB_TEST_CHANNEL_ID`. It is not stored
in an individual source because all sources share one Twitch backend. The
precedence is:

1. A complete, valid environment-variable override selects local Twitch CLI
   transport.
2. With both variables absent, the production Twitch endpoint is used.
3. A partial or invalid override fails visibly instead of falling back.

Synthetic event mode is independent of transport selection. Removing both
environment variables and restarting OBS restores production transport. Both
synthetic mode and local transport are off after a normal installation unless
the user explicitly enables or configures them.
