# Tests

Windows uses the same chat/update-checker suites, plus a native post-exit suite:

    ./scripts/bootstrap-windows.ps1
    cmake --preset windows-x86_64
    cmake --build --preset windows-x86_64
    ctest --test-dir build/windows-x86_64 -C RelWithDebInfo --output-on-failure
    ./scripts/package-windows.ps1
    ./tests/windows-installer-tests.ps1

Bootstrap builds the matching upstream QtTest module because OBS's Qt package
omits it. GUI tests use the Windows platform plugin; Linux uses offscreen.
The Windows suite covers Unicode/space paths, PE validation, pending metadata,
hash/size rejection, exact process handles and creation times, inherited locks,
loaded DLLs through hardlinks, conservative relevant-process classification,
unrelated denied processes, both rename failures, prepared/committed journal
recovery, backups, cleanup/results, and plugin-only/paired temporary-runner updates.
The runner lifecycle strips Qt/OBS/developer directories from PATH.

Installer smoke tests use temporary Unicode destinations and state directories,
disable environment registration, and refuse to run over an existing Installed
Apps entry. They verify checksums, ZIP layout, metadata, silent install/reinstall,
loaded-file/lock/pending refusal, silent uninstall, preservation of state/unknown
files, and ZIP/developer script installation. They never install into a real OBS
directory. A tiny fixture DLL supplies a real Windows image mapping without OBS.

The Python manifest suite runs on both platforms and verifies platform entries,
size/hash correspondence, legacy Linux shape and corrupt/missing payload refusal.

On Linux, the installer/package tests also require Python 3 and `zip` (test-time
dependencies only). CI installs them before configuring.

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

The later Windows pixel failures are separate from that width assertion. Windows
now embeds Qt's recommended `NotoColorEmoji_WindowsCompatible.ttf` from the same
upstream revision (see `resources/fonts/README.md`); Linux retains its original
resource. This addresses a font-format compatibility suspect, not a confirmed
DirectWrite defect. A native Windows run is required to confirm the two failures
are resolved. Neither pixel threshold is relaxed.

Each emoji row reports the actual isolated font/glyph, color-font tables, image
size, visible/color pixel counts, and ink bounds for isolated rendering and the
production document with/without outline. Windows saves those three PNGs for
every row in `tests/emoji-diagnostics/` under the build directory; Linux saves
them on pixel failure. CI includes them in `windows-test-diagnostics`.
`QT_LOGGING_RULES=bokis.render.emoji.debug=true` additionally traces the real
document's font runs and glyph positions. This is enabled for Windows tests only;
production logging is off by default. General Qt plugin discovery logging is no
longer enabled.

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

## Linux installer tests

`linux-installer-tests` runs the release installer with disposable HOME/XDG
directories, runtime/process command stubs and real filesystem locks. It covers
install/reinstall/uninstall, preservation of settings and unknown files, persistent
lock identity, corrupt/missing payloads, path traversal and symlinks, incompatible
runtime/CPU, active OBS/updaters, pending journals, relative XDG fallback, rollback
after partial replacement/removal and staged corruption. It also packages the real
built binaries, verifies downloadable checksum files, extracts the ZIP and exercises
installation/removal from that archive. CI's root container drops to an unprivileged
UID for installation tests after checking the root refusal.

Runtime stubs let these cases run offline without launching OBS. A real
`bash install.sh --check` and an OBS launch on each supported distribution remain
necessary to validate release compatibility. Tests never install into a real user
profile or stop a running OBS process.
