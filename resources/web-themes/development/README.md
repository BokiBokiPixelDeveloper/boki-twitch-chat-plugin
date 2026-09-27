# Development Web Widget

This bundled widget is a reference and runtime-testing surface for the generic
V3 Web Event Bridge. It is not StreamElements compatibility.

In the OBS source properties:

1. Set **Renderer Mode** to **Web Widget**.
2. Set **Width** and **Height** for a vertical, horizontal, or square viewport.
3. Enable **Event Test Mode**.
4. Use **Test Chat Message**, then **Test Delete Message** and **Test Clear Chat**.
5. Use the Follow, Subscription, Resubscription, Gift Subscription, Community
   Gift Subscription, Cheer, and Raid buttons. Each event should become visible.

**Refresh Web Widget** creates a new private browser runtime and clears its DOM,
JavaScript globals, timers, listeners, and live event subscription. Multiple Web
Widget sources each own independent browser and DOM state; the shared validated
event pipeline fans an injected event out to every accepted source once.

The widget renders viewer text through DOM text nodes and `textContent`. Emotes
and resolved badge images use controlled `img` element creation. Badge data
without a resolved image URL uses a text label. Widget resources are embedded in
the plugin and served only through its per-instance loopback endpoint.
