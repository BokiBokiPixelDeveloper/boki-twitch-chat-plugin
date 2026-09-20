# Noto Color Emoji

Unmodified `NotoColorEmoji.ttf` and its SIL Open Font License are vendored from
https://github.com/googlefonts/noto-emoji/tree/8998f5dd683424a73e2314a8c1f1e359c19e8742/fonts.

The font and license are embedded in the plugin so Unicode emoji also work on
systems without an installed color emoji font and with the existing single-binary
updater. The font adds approximately 10 MiB to the binary. Qt's own text shaping
handles variation selectors, skin tones, flags and ZWJ sequences; no network
request is needed for Unicode emoji.

Windows embeds the unmodified `NotoColorEmoji_WindowsCompatible.ttf` from the
same upstream commit, aliased to the same resource path. Linux continues to
embed the original font. Qt explicitly recommends this variant for its font
loader: https://doc.qt.io/qt-6/android-emojis.html#obtaining-a-font.

The Windows variant adds `glyf`/`loca`, a compatible `cmap`, and adjusted font
headers/metrics. Its `CBDT`, `CBLC`, `GSUB`, `hmtx`, and `vmtx` tables are byte
identical to the original: emoji artwork, skin tones and sequence substitutions
are unchanged. Only one font is embedded in each platform's plugin and tests.
SHA-256 of the Windows font:
`19473341d23f8fdf90e91ffca381d727c43f7bc05b2758dec9687a58fbb81150`.
