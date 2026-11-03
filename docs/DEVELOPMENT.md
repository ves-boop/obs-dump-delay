# Developing Dump Delay

## Layout

| Path | What |
|---|---|
| `src/dump-delay-output.c` | The RTMP output: delay buffer of encoded packets, dump, delay rebuild, muxing with libavformat |
| `src/dump-delay-service.c` | Streaming service whose preferred output is the one above; sets keyframe interval |
| `src/dump-delay-control.c` | Plugin config, switching streaming to/from the service, slate scene creation |
| `src/dump-delay-ui.cpp` | Qt dock (status + PANICONE button) and settings window |
| `src/plugin-main.c` | Module entry, hotkey, Tools menu |
| `data/` | Images and translations (`locale/*.ini`) installed next to the DLL |
| `installer/` | Inno Setup script, artwork, `build-release.ps1` |
| `choco/` | Chocolatey package (filled in by `build-release.ps1`) |
| `test/` | End-to-end test harness |

The project is based on [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).

## Build

Requirements: Visual Studio 2022 Build Tools (C++ workload, Windows SDK),
CMake 3.28+, Git.

```powershell
cmake --preset windows-x64          # downloads OBS sources, obs-deps and Qt into .deps/, builds libobs
cmake --build --preset windows-x64
```

`buildspec.json` pins the OBS version (33.0.1) and the matching obs-deps/Qt
packages. They must match the OBS release you run: the plugin links against
OBS's FFmpeg (`avformat-62`) and Qt 6 DLLs. For a new OBS major version, update
the versions and hashes there (OBS lists its own dependency versions in its
`CMakePresets.json`).

If your Windows SDK differs from the preset's (10.0.22621), add a
`CMakeUserPresets.json` inheriting `windows-x64` with
`"architecture": "x64,version=<your SDK>"`.

Install for development (elevated, OBS closed):

```powershell
cmake --install build_x64 --config RelWithDebInfo --prefix C:\ProgramData\obs-studio\plugins
```

This uses the current OBS plugin layout
(`plugins\obs-dump-delay\obs-dump-delay.dll` + `data\`); see
[OBS's plugin location notes](https://obsproject.com/kb/legacy-plugin-developer-info).

## Release

```powershell
installer\build-release.ps1
```

builds the plugin and writes to `release\`: the ZIP, the Inno Setup installer
(needs Inno Setup 6), the Chocolatey `.nupkg` (embeds the installer, with
`VERIFICATION.txt`) and `SHA256SUMS.txt`. The version comes from `buildspec.json`.
`installer\make-assets.ps1` regenerates the icon and wizard images from
`data\panicone.png`.

## How it works

The plugin registers its own RTMP output that buffers **encoded** packets
(a few MB, no extra encoding) and muxes them to the server with libavformat.
It connects only shortly before the first packet is due: servers drop
publishers that send nothing (MediaMTX after 10 s).

On a dump:

1. all buffered packets are discarded;
2. OBS switches to the slate scene;
3. packets are dropped until the first keyframe rendered after the cut
   (press time + cut margin), so nothing from before the cut leaks;
4. the slate is sent **at half speed** (timestamps × 2). A static slate in slow
   motion looks the same, and because output time runs twice as fast as capture
   time, the delay buffer refills by itself. Audio is replaced by AAC silence;
5. the first slate keyframe that will have been held for the full delay is the
   switch point: when it arrives the previous scene is restored, and when it is
   sent timestamps return to 1× and real audio resumes.

Speed changes happen only at keyframes, anchored on their presentation
timestamp, so B-frames never straddle the boundary. Viewers see a freeze of up
to one keyframe interval, then the slate for about twice the delay.

Why slow motion instead of a pre-encoded filler clip: OBS has no public API to
push arbitrary frames into an encoder, and a filler from another encoder risks
mismatched codec headers. Slow motion reuses the stream's own encoder.

## Testing

`test/` drives OBS through obs-websocket against a local
[MediaMTX](https://github.com/bluenviron/mediamtx) server:

1. Enable OBS's WebSocket server (Tools → WebSocket Server Settings).
2. Run MediaMTX with RTMP on `127.0.0.1:1935`.
3. `pwsh test\run-dumptest.ps1` creates a `DumpDelayTest` scene collection (green
   live source, blue `Censored` slate), streams for 80 s, turns the live source
   red for 5 s, presses the dump hotkey and records what the server receives.
4. `pwsh test\analyze-dumptest.ps1` classifies every received frame by color
   and checks that **no red frame** arrived, plus timestamp order and audio
   continuity.

`test\set-test-service.ps1` / `test\restore-obs.ps1` switch streaming to the
local server and back for manual checks.

For Twitch, append `?bandwidthtest=true` to the stream key: Twitch accepts the
stream but doesn't broadcast it.

## Translations

Copy `data/locale/en-US.ini` to `<lang>-<REGION>.ini` and translate the
values. Use OBS's own wording for its menus (see OBS's
`data/obs-studio/locale/<lang>.ini`).
