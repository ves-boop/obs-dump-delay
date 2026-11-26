"""Analyze a run-vodtest.ps1 recording.

For every audio stream: per 100 ms window, the level of the 1000 Hz "music"
tone and the 440 Hz "voice" beeps (Goertzel, no numpy). Then checks:
  * two audio streams received;
  * stream 0 (live) carries music + voice;
  * stream 1 (VOD) carries voice and never music;
  * voice beep onsets line up across the two streams;
  * with a dump: both streams have a silent stretch of similar length.
Usage: python analyze-vodtest.py <recording.mkv>
"""
import json
import math
import struct
import subprocess
import sys

RATE = 8000
WIN = RATE // 10  # 100 ms
ON_DB = -30.0     # tone present above this (dBFS-ish)


def probe(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_streams", "-of", "json", path],
        capture_output=True, text=True, check=True).stdout
    return json.loads(out)["streams"]


def decode(path, audio_idx):
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-map", f"0:a:{audio_idx}",
         "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-"],
        capture_output=True, check=True).stdout
    n = len(raw) // 2
    return struct.unpack(f"<{n}h", raw[: n * 2])


def goertzel_db(block, freq):
    k = 2 * math.cos(2 * math.pi * freq / RATE)
    s1 = s2 = 0.0
    for x in block:
        s0 = x + k * s1 - s2
        s2, s1 = s1, s0
    power = s1 * s1 + s2 * s2 - k * s1 * s2
    amp = 2 * math.sqrt(max(power, 0)) / len(block) / 32768
    return 20 * math.log10(amp) if amp > 0 else -200.0


def levels(samples):
    rows = []
    for i in range(0, len(samples) - WIN + 1, WIN):
        b = samples[i:i + WIN]
        rows.append((goertzel_db(b, 1000), goertzel_db(b, 440)))
    return rows


def onsets(samples):
    """Times (s) where the 440 Hz beep starts, 10 ms resolution."""
    step = RATE // 100
    out, prev = [], False
    for i in range(0, len(samples) - step + 1, step):
        on = goertzel_db(samples[i:i + step], 440) > ON_DB
        if on and not prev:
            out.append(i / RATE)
        prev = on
    return out


def silent_runs(rows):
    runs, start = [], None
    for i, (m, v) in enumerate(rows + [(0, 0)]):
        quiet = m < ON_DB and v < ON_DB
        if quiet and start is None:
            start = i
        elif not quiet and start is not None:
            if i - start >= 20:  # >= 2 s: longer than the beep gaps
                runs.append((start / 10, i / 10))
            start = None
    return runs


def main(path):
    streams = probe(path)
    audio = [s for s in streams if s["codec_type"] == "audio"]
    print(f"streams: {[(s['index'], s['codec_type'], s.get('codec_name')) for s in streams]}")
    ok = True
    if len(audio) != 2:
        print(f"FAIL: expected 2 audio streams, got {len(audio)}")
        return False

    data = [decode(path, i) for i in range(2)]
    lv = [levels(d) for d in data]
    for i, rows in enumerate(lv):
        n = len(rows)
        music = sum(1 for m, _ in rows if m > ON_DB)
        voice = sum(1 for _, v in rows if v > ON_DB)
        print(f"audio {i}: {n / 10:.1f} s, music in {music} windows, voice in {voice} windows,"
              f" silent runs {silent_runs(rows)}")

    if not any(m > ON_DB for m, _ in lv[0]):
        print("FAIL: live track has no music")
        ok = False
    if any(m > ON_DB for m, _ in lv[1]):
        bad = [i / 10 for i, (m, _) in enumerate(lv[1]) if m > ON_DB]
        print(f"FAIL: music on the VOD track at {bad[:10]} s")
        ok = False
    if not any(v > ON_DB for _, v in lv[1]):
        print("FAIL: VOD track has no voice")
        ok = False

    o0, o1 = onsets(data[0]), onsets(data[1])
    diffs = []
    for t in o1:
        near = min(o0, key=lambda x: abs(x - t), default=None)
        if near is not None and abs(near - t) < 0.5:
            diffs.append(t - near)
    if diffs:
        worst = max(abs(d) for d in diffs)
        print(f"beep onsets: {len(o0)} live, {len(o1)} VOD, matched {len(diffs)},"
              f" worst offset {worst * 1000:.0f} ms")
        if worst > 0.05:
            print("FAIL: tracks out of sync")
            ok = False
    else:
        print("FAIL: no matching beeps")
        ok = False

    r0, r1 = silent_runs(lv[0]), silent_runs(lv[1])
    if r0 or r1:
        print(f"silence live {r0}, VOD {r1}")
        # the VOD track only has the 1 Hz beeps, so its silence is seen to
        # start after the last beep and end at the next one: allow one beep
        # period on the outer side of each edge
        if len(r0) != len(r1) or any(not (-1.1 <= b[0] - a[0] <= 0.3) or not (-0.3 <= b[1] - a[1] <= 1.1)
                                     for a, b in zip(r0, r1)):
            print("FAIL: dump silence differs between tracks")
            ok = False

    print("PASS" if ok else "FAILED")
    return ok


if __name__ == "__main__":
    sys.exit(0 if main(sys.argv[1]) else 1)
