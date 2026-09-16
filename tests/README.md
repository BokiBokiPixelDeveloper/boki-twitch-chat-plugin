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
path requires a manual source preview. Use **Test emojis and emotes**, then check
live messages containing native Twitch emotes and enabled channel emotes from all
three providers. Also verify that existing font, lane and separate GIF settings
continue to work after an OBS restart.

## Unicode layout portability

`coloredUnicode()` checks one extended grapheme per fixture (including selectors,
skin tones, ZWJ sequences, flags and keycaps), no cursor boundary inside it, and
one non-missing shaped glyph. The shaped font's `head`, `cmap` and `GSUB` tables
must match the bundled `NotoColorEmoji.ttf`; registering an application font alone
does not establish which fallback face Qt used. The raster must remain valid and
visible, preserve emoji colors, and be identical when the same code points arrive
as adjacent text fragments.

There is deliberately no fixed total-image width limit. `layoutMessage()` includes
`": "` even with an empty username, plus document margins. If Liberation Sans is
absent, the prefix also uses different fallback metrics. With Qt 6.11.2, the seven
fixtures measured 100 px with local system fonts and 173 px when Fontconfig could
see only the bundled font; the new semantic checks passed in both environments.
This reproduces the reported CI failure without a shaping error or Qt version
change. The exact runner font inventory was not inspected.

To exercise a minimal font environment without modifying system font settings:

```bash
FONT_TEST_DIR="$(mktemp -d)"
cat > "$FONT_TEST_DIR/fonts.conf" <<EOF
<fontconfig>
  <dir>$PWD/resources/fonts</dir>
  <cachedir>$FONT_TEST_DIR/cache</cachedir>
</fontconfig>
EOF
QT_QPA_PLATFORM=offscreen FONTCONFIG_FILE="$FONT_TEST_DIR/fonts.conf" \
  build/linux-x86_64/tests/chat-tests coloredUnicode
```

## Updater regression tests

`updater-tests` uses fake network replies and an OBS UI task queue stub; no live
network, installed plugin, or real user cache is touched. It covers:

- Idle → Checking → Available → Downloading → Ready and status text
- No property notification from check/install calls; completion notifications
  occur only when the queued OBS UI task is executed
- Duplicate check/install suppression while busy and after staging
- SHA-256 and size rejection, network and staging failures, download retry
- Current, malformed and incomplete manifests
- Atomic pending-file contents and unchanged installed version
- Destruction with a pending UI notification

A manual Linux/Qt6 OBS check is still needed for actual button widget dispatch:
click **Check for updates**, then **Install update**, including repeated
clicks and closing the properties/source while requests are pending. Confirm that
the loaded `.so` stays unchanged and the verified file appears only in `pending`.
