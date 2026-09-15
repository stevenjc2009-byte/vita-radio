# Changelog

## [1.0.0] - 2026-09-15

First release.

### Added

- Internet radio playback on the PS Vita: HTTP and HTTPS streams, MP3 and AAC,
  decoded with FFmpeg and played through `sceAudioOut`.
- Built-in station list with a scrolling, auto-repeating selector.
- Live stream panel: state, ICY track title, codec, sample rate, channels,
  HTTP status, buffer fill, bytes received and the stream URL.
- Certificate verification for HTTPS using the bundled Mozilla CA bundle.
- Station switching that never blocks on the previous connection: each play
  owns its ring buffer and stream, and a reaper thread retires the old one.
- Built-in updater: TRIANGLE checks the latest GitHub release, and a second
  press downloads it, unpacks it, hands over to the Vita Radio Updater title
  and relaunches the app once it is installed.
