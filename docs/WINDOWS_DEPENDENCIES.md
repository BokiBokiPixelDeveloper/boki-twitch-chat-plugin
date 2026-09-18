# Windows bundled components

All exact download URLs and SHA-256 values are recorded in
scripts/windows-dependencies.json. Bootstrap verifies each before extraction.

| Component | Source/build | Distribution |
| --- | --- | --- |
| Plugin and helper worker/bootstrap | This repository, version in VERSION | Repository LICENSE |
| OBS SDK 32.2.2 | https://github.com/obsproject/obs-studio/tree/32.2.2 | Build dependency; OBS is installed separately |
| Qt Core 6.11.1 | https://github.com/obsproject/obs-deps/releases/tag/2026-07-15 | Embedded DLL in helper; dynamically loaded by extracted worker |
| Qt WebSockets 6.11.1 | https://github.com/qt/qtwebsockets/tree/v6.11.1 | Separate DLL in plugin/bin/64bit |
| libwebp 1.6.0 | https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz | Statically linked decoder/demux; COPYING included |
| Noto Color Emoji | resources/fonts/README.md in this repository | Embedded font; OFL included |
| Microsoft Visual C++ runtime | Visual Studio redistributable files selected by CMake | Embedded helper runtime; Microsoft redistributable terms apply |
| Inno Setup 6.5.4 | https://github.com/jrsoftware/issrc/releases/tag/is-6_5_4 | Installer compiler/stub |

Qt's upstream license texts are included under Qt and QtWebSockets in the
package's licenses directory, including LGPL-3.0 and GPL alternatives.
Qt Core source is available from https://github.com/qt/qtbase/tree/v6.11.1;
OBS's dependency build scripts and patches are at
https://github.com/obsproject/obs-deps/tree/2026-07-15.
The worker links Qt dynamically. Its source and resource-embedding recipe are
in this repository; rebuilding the helper permits use of a modified compatible
Qt library. Extraction does not require OBS or Qt to be installed globally.
The plugin's Qt Core/GUI/Network runtime comes from the installed OBS distribution.

Windows supplies the ICU and UCRT system APIs used by the pinned Qt build.
They are not copied out of Windows or redistributed by this package.
See [Microsoft's ICU documentation](https://learn.microsoft.com/en-us/windows/win32/intl/international-components-for-unicode--icu-).

The repository's existing project LICENSE has not been changed. Dependency
license texts do not replace that license.
