# Changelog

All notable changes to Dump Delay are listed here.

## 0.1.2 (2026-11-26)

### Added
- **The Dump Delay panel shows the Twitch VOD Track** while you stream, under
  the status line. *On (audio track 2)* means Twitch is getting the VOD track
  from that mixer track. An amber warning means the VOD track is off: your VOD
  gets the live audio, music included. To turn it on, go to OBS Settings →
  Output → Twitch VOD Track. Hover over the line for help.

## 0.1.1 (2026-11-26)

### Fixed
- **Twitch VOD Track works with dump delay on.** OBS can send Twitch a second
  audio track that Twitch saves in the VOD instead of the live audio (Settings →
  Output → Streaming, Advanced mode, "Twitch VOD Track"). Streamers use it to
  keep music out of VODs. Before this fix, turning dump delay on dropped that
  track, so the VOD got the live audio, music included, which can cause
  copyright strikes. The VOD track is now sent like OBS sends it, and goes
  through the same delay as the live audio. When you press the button, it goes
  silent along with the live audio.

  If you already had dump delay on, nothing needs changing: the fix applies the
  next time you start streaming.

## 0.1.0 (2026-11-03)

First release: a stream delay with a panic button for OBS Studio 33 on Windows.

- Dump Delay panel with a status display and the panic button (or a hotkey).
  Pressing it throws away the seconds viewers haven't seen yet, puts a slate
  scene on air while the delay rebuilds, and keeps the connection to Twitch
  open.
- Adjustable delay (3–60 s), slate scene (can be created in one click) and cut
  margin.
- Warns when OBS's own Stream Delay is also on.
- English and Italian.
