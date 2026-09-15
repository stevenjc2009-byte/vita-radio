# Vita Radio

An internet radio player for the PlayStation Vita. Browse a built-in list of
stations, press X, and it streams — MP3 and AAC over HTTP and HTTPS, with the
ICY track title shown while it plays. Written in C with VitaSDK and vita2d.

## Features

- **Built-in station list** with the codec and stream type shown per station.
- **Live stream info:** state, ICY track title, codec, sample rate, channels,
  HTTP status, buffer fill and bytes received.
- **HTTPS streams** with certificate verification against a bundled CA store.
- **Fast station switching** — starting a new station never waits on the old
  one's socket to close.
- **Built-in updater:** *Check for Updates* (TRIANGLE) downloads and installs
  the latest GitHub release, then restarts itself.

## Install

<img src="docs/install-qr.png" alt="QR code for the VitaRadio.vpk 1.0.0 download" width="200" align="right">

Scan the code with the Vita's own browser (**Browser → ☰ → QR code reader**) and
it downloads `VitaRadio.vpk` for 1.0.0 straight to the console — no PC, no USB.
Then install the downloaded file with VitaShell.

Or do it by hand:

1. Download `VitaRadio.vpk` from the
   [latest release](https://github.com/stevenjc2009-byte/vita-radio/releases/latest).
2. Copy it to your Vita and install it with VitaShell.
3. Enable **Unsafe Homebrew** in HENkaku Settings — the built-in updater needs
   it to install the package it downloads.

From 1.0.0 onwards you only have to do this once: later versions arrive through
*Check for Updates* (TRIANGLE) inside the app.

## Controls

| Button | Action |
| --- | --- |
| Up / Down | Move through the station list (hold to repeat) |
| X | Play the selected station |
| O | Stop playback |
| TRIANGLE | Check for updates; press again to install one |
| START | Quit |

## Building (WSL + VitaSDK)

```bash
export VITASDK=$HOME/vitasdk   # wherever your VitaSDK lives
make                      # -> build/VitaRadio.vpk
make BUILD=build-verify   # build into a separate directory
```

Host unit tests for the pure-C modules (ring buffer, ICY parser, format
sniffer, player lifecycle, version/JSON parsing, SHA-1 + head.bin, zip
extractor) run outside the SDK:

```bash
make -C tests
```

## Credits

- The fake-PKG header template `assets/head.bin` is the one used by VitaShell,
  VHBB and VitaDB Downloader, from
  [VitaShell](https://github.com/TheOfficialFloW/VitaShell) (GPL-3.0).
- `assets/cacert.pem` is the Mozilla CA certificate bundle as distributed by
  [curl](https://curl.se/docs/caextract.html).
- Built on [VitaSDK](https://vitasdk.org), [vita2d](https://github.com/xerpi/libvita2d),
  [FFmpeg](https://ffmpeg.org), [curl](https://curl.se), [mbedTLS](https://www.trustedfirmware.org/projects/mbed-tls/)
  and [zlib](https://zlib.net).

## License

GPL-3.0. See [LICENSE](LICENSE).
