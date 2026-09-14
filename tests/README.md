# Tests

```bash
cmake --preset linux-x86_64 -DENABLE_TESTS=ON
cmake --build --preset linux-x86_64
ctest --test-dir build/linux-x86_64 --output-on-failure
```

Qt Test runs offscreen without OBS, Twitch credentials or network access. Coverage:

- Twitch fragment order, static/animated URLs and malformed-data text fallback
- 7TV aliases/overlays, BetterTTV channel/shared emotes and FFZ default set filtering
- Whole-token matching, provider precedence and channel changes
- PNG, GIF and animated WebP decoding, transparency, frame delays and input limits
- Complete 492-frame animation playback within the decoded-memory budget
- Per-frame animation timing, large time steps and sub-millisecond accumulation
- Color Unicode glyphs, variation selectors, skin tones, flags and ZWJ shaping
- Inline sizing, aspect ratios, overlay positioning and missing-image text fallback
- Download coalescing, caching, negative caching and credential-free requests
- Ordered asynchronous delivery, static fallback, timeout and destruction during loading

`fixtures/animation.gif` and `fixtures/animation.webp` are original generated test
images: three 16×12 frames with transparent borders and a red, green, then blue
rectangle, lasting 40, 120 and 80 ms. GIF uses restore-to-background disposal;
WebP was encoded losslessly with libwebp. They contain no third-party artwork.

The test suite checks CPU layout and animation state. The final OBS graphics draw
path requires a manual source preview. Use **Emojis und Emotes testen**, then check
live messages containing native Twitch emotes and enabled channel emotes from all
three providers. Also verify that existing font, lane and separate GIF settings
continue to work after an OBS restart.
