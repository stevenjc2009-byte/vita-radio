# Changelog

## [3.0.0] - 2026-09-16

A four-tab interface, filter chips over the station directory, colour themes,
and a System tab that owns the updater.

### Added

- **Four tabs** — Stations, Favourites, Search and System — cycled with L/R,
  with a tab strip across the top of the screen.
- **Filter chips** on the Stations tab: Built-in, Popular, Rock, Jazz, News,
  Classical, Dance, UK and US. Moving onto a chip fetches it. This wires up
  `rb_top_click`, `rb_by_tag` and `rb_by_country`, three radio-browser.info
  endpoints that had shipped in 2.0.0 with no caller.
- **Four colour themes** — Midnight, Deep blue, True black and Graphite —
  chosen in System → Theme. Every colour the UI draws now comes from the theme
  table, so no drawing code holds a literal colour.
- **Settings are saved** to `ux0:data/VitaRadio/settings.tsv`, written through a
  `.tmp` and a rename so an interrupted save cannot destroy the file already
  there. A missing file is not an error; a corrupt or out-of-range value falls
  back to the default rather than leaving the UI with no valid theme.
- **A download progress bar** on the System tab. When the server sends no
  `Content-Length` it shows an indeterminate bar instead of inventing a
  percentage.
- **System tab rows** for the updater, the theme and an about line, each with
  contextual help beside it.
- **Empty states** that say what to press — Favourites with nothing in it now
  points at TRIANGLE instead of showing a blank panel.
- **A now-playing card** with the state, ICY title, codec, sample rate and
  buffer fill, and a detail panel for the selected station.

### Changed

- **Controls remapped.** L/R switches tab rather than list. Left/Right drives
  the filter chips, or the value on the System tab. **TRIANGLE** now toggles a
  favourite (it was SELECT); **SELECT** jumps to the System tab; the updater
  moved off TRIANGLE onto the System tab's own row.
- A directory station's country is appended to its format line
  (`MP3 / 128 kbps · GB`) rather than stored in a new field, so `favourites.tsv`
  keeps the same columns and a favourites file written by 1.0.0 or 2.0.0 still
  loads.

### Fixed

- The updater's download percentage is clamped to 100, so a server that sends
  more bytes than it promised can no longer display a figure above 100%, and it
  is reset at the start and end of every attempt so a stale percentage from a
  previous attempt cannot appear under a fresh one.
- A failed `rb_search_name()` no longer leaks its result.
- A station list that fills up no longer drops stations silently — the discarded
  return from `sl_add()` is now checked, and the count actually shown is
  reported.
- `test_ringbuf` reported only "ALL PASS" and so contributed nothing to the
  aggregate check count; its 54 assertions are now counted with the rest.
- The host test suite runs with `-fno-sanitize-recover=all`, so a
  UndefinedBehaviorSanitizer diagnostic now fails the build instead of printing
  and exiting 0.


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
