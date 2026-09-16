# Changelog

## [2.0.0] - 2026-09-16

HLS support, station search, favourites, and a much larger built-in list.

### Added

- **HLS streaming.** Master and media playlists (RFC 8216), live-edge start
  three target durations back, reload cadence per §6.3.4, discontinuity
  handling, and `EXT-X-KEY` AES-128-CBC decryption. Segments are demuxed from
  MPEG-TS to ADTS, or scrubbed of interleaved ID3 tags when they are raw `.aac`.
- **Playlist links are followed.** A `.pls` or `.m3u` URL now resolves to the
  stream it points at instead of being refused.
- **Station search** over the radio-browser.info directory, with an on-screen
  keyboard (SQUARE). Searches run against a live mirror and show codec and
  bitrate per result.
- **Favourites.** SELECT adds or removes the selected station; the list is
  saved and reloaded between sessions, and a star marks favourites wherever
  they appear.
- **Three station lists** — Built-in, Favourites and Search — cycled with L/R.
- **90 built-in stations**, up from 7, every one verified live at build time.
  Includes 30 BBC networks; 38 of the 90 are HLS. Grouped by region and genre.

### Changed

- The player can now swap its source mid-playback without dropping the
  connection machinery, so an HTTP body that turns out to be a playlist or an
  HLS manifest continues into the real stream rather than erroring.
- `HLS stations are not supported yet` and
  `Playlist link (.pls/.m3u) not supported yet` are gone.

### Fixed

- A source swap no longer carries the previous body's content type forward,
  which could re-classify the new stream as a playlist and abort it.
- A swapped-away source can no longer overwrite the new station's title while
  it is being retired.

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
