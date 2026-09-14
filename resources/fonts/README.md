# Noto Color Emoji

Unmodified `NotoColorEmoji.ttf` and its SIL Open Font License are vendored from
https://github.com/googlefonts/noto-emoji/tree/8998f5dd683424a73e2314a8c1f1e359c19e8742/fonts.

The font and license are embedded in the plugin so Unicode emoji also work on
systems without an installed color emoji font and with the existing single-binary
updater. The font adds approximately 10 MiB to the binary. Qt's own text shaping
handles variation selectors, skin tones, flags and ZWJ sequences; no network
request is needed for Unicode emoji.
