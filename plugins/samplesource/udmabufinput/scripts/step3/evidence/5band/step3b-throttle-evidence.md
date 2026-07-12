# plan-03 Step 3b evidence — fpsPeriodMs throttle on the headless WSSpectrum path

Thread: `cross-cutting/20260711-sdrangel-host-perchannel-sigmf` (gps_design side,
read-only from this fork). This evidence lives on the fork branch
`udmabufinput-plugin`, worktree-relative paths only. Extends
`step3-5band-evidence.md` (Step 3), which found the stock headless
`WSSpectrum` push unthrottled at ~312.7 Mbps aggregate across the same 5
bands — the same order of magnitude as raw IQ, not the plan's "few Mbps"
target.

## What changed

A small, upstreamable C++ patch to `sdrbase/dsp/spectrumvis.{cpp,h}` (only
those two files touched) gates the websocket forward
(`m_wsSpectrum.newSpectrum(...)` in `SpectrumVis::processFFT`) on
`m_settings.m_fpsPeriodMs`, which was already fully wired end-to-end via REST
(`GET`/`PATCH .../spectrum/settings`, key `fpsPeriodMs`) but had **no
consumer** on the headless data path — only the GUI's repaint `QTimer`
(`sdrgui/gui/glspectrumview.cpp`) read it, and headless `sdrangelsrv` never
instantiates that GUI. The gate sits *before* `newSpectrum()` (i.e. before
`buildPayload()`/`sendBinaryMessage()`), so a skipped frame costs nothing on
the wire — this is what actually reduces byte-rate, not just message count.
The GUI path (`m_glSpectrum->newSpectrum(...)`) is untouched.

Full diff (also reproducible via `git diff -- sdrbase/dsp/spectrumvis.cpp
sdrbase/dsp/spectrumvis.h` on this worktree — **not committed**):

- `sdrbase/dsp/spectrumvis.h`: adds `#include <QElapsedTimer>` and a
  `QElapsedTimer m_wsSpectrumTimer;` member (same pattern
  `sdrbase/websockets/wsspectrum.cpp` already uses for its own
  `fftTimeMs`/`m_timer`).
- `sdrbase/dsp/spectrumvis.cpp`:
  - In `processFFT`'s websocket-forward block: if
    `m_settings.m_fpsPeriodMs > 0`, forward only when
    `!m_wsSpectrumTimer.isValid() || m_wsSpectrumTimer.elapsed() >=
    m_settings.m_fpsPeriodMs`; `m_fpsPeriodMs <= 0` means "no limit"
    (matches the field's documented semantics in `spectrumsettings.h`). The
    first frame ever forwarded (timer not yet valid) always goes out
    immediately.
  - In `handleWSOpenClose`, `m_wsSpectrumTimer.invalidate()` on socket open,
    so a freshly (re)connected client gets its first frame immediately
    rather than waiting out whatever was left of the previous window.
  - Both additions are marked `// FORK PATCH (plan-03 Step 3b): ...` for an
    obvious, self-contained upstream-diff boundary.

**REST wiring double-checked, not assumed**: `fpsPeriodMs` PATCHed to
`/sdrangel/deviceset/{i}/spectrum/settings` flows through
`WebAPIRequestMapper::extractKeys` (generic key capture) ->
`SpectrumVis::webapiUpdateSpectrumSettings` (prefixes to
`spectrumConfig.fpsPeriodMs`) -> `SpectrumSettings::updateFrom` (`if
(keys.contains("spectrumConfig.fpsPeriodMs")) m_fpsPeriodMs =
swgSpectrum->getFpsPeriodMs();`). The large block of individually
commented-out `if (jsonObject.contains("fpsPeriodMs")) ...` lines in
`webapirequestmapper.cpp::validateSpectrumSettings` (~line 3990) is dead code
superseded by the generic `extractKeys` + `fromJsonObject` path — confirmed
by the fact that `wsSpectrumPort` is *also* in that same commented-out block,
yet Step 1/3 already proved `wsSpectrumPort` sets correctly over REST. Not
touched; scope stayed inside `spectrumvis.{cpp,h}` as instructed.

## Build

Incremental build, reusing the existing configured tree (no full rebuild):

```
cmake --build codex-handoff/plan-02/temp/build-host --target sdrangelsrv --parallel 8
```

Result: `sdrbase` (contains the changed `spectrumvis.cpp`) recompiled and
relinked (`../lib/libsdrbase.so`), then `sdrsrv` and `sdrangelsrv` relinked
against it. No errors, no new warnings from the changed lines. Wall time
~39s. Both `codex-handoff/plan-02/temp/build-host/lib/libsdrbase.so` and
`codex-handoff/plan-02/temp/build-host/sdrangelsrv` show a fresh mtime
matching the build run.

## Config-script change

`step3_configure_5band.py` gained `--fps-period-ms` (default `50` = 20 fps,
`0` = unthrottled/pre-Step-3b behavior for comparison), PATCHed as
`fpsPeriodMs` into every device set's `spectrum/settings` body alongside the
existing `fftSize`/`fftWindow`/`wsSpectrumPort` fields. The per-device-set
readback and the final summary line now also print/verify
`fpsPeriodMs=<value> (requested <value>)`. Re-running
`step3_launch_5band.sh` (unchanged, still calls the script with no extra
flags -> default 50) confirmed all 5 device sets came back
`fpsPeriodMs=50 (requested 50)`:

```
[OK] device set 0 "RF Wideband": ... fftSize=32768 fpsPeriodMs=50 (requested 50) ws=127.0.0.1:8887 (requested 8887) run=1
[OK] device set 1 "GPS L1": ... fftSize=4096 fpsPeriodMs=50 (requested 50) ws=127.0.0.1:8888 (requested 8888) run=1
[OK] device set 2 "GPS L2C": ... fftSize=4096 fpsPeriodMs=50 (requested 50) ws=127.0.0.1:8889 (requested 8889) run=1
[OK] device set 3 "GPS L5": ... fftSize=4096 fpsPeriodMs=50 (requested 50) ws=127.0.0.1:8890 (requested 8890) run=1
[OK] device set 4 "Iridium": ... fftSize=4096 fpsPeriodMs=50 (requested 50) ws=127.0.0.1:8891 (requested 8891) run=1
```

Full transcript: `step3b-5band-rest-transcript.log`.

## Wire-rate measurement — before vs after, same host/producers

Method: same as Step 3's `byte-rate-calc.txt` — decode each band's captured
frames with `spectrum_frame_decode.py`, read the `fftTimeMs` header field
(now the *forwarded*-frame period, since the FORK PATCH gates which FFTs
reach `WSSpectrum::newSpectrum`, whose `QElapsedTimer::restart()` produces
that field), average over 19 intervals per 20-frame capture, multiply by the
captured wire frame size. Full computation and quantization note in
`byte-rate-calc-throttled.txt`.

| band | port | fftSize | Step-3 (unthrottled) fps / Mbps | Step-3b (fpsPeriodMs=50) fps / Mbps | reduction |
|---|---|---|---|---|---|
| RF Wideband | 8887 | 32768 | 47.6 fps / 49.95 Mbps | 15.09 fps / 15.83 Mbps | 3.16x |
| GPS L1 | 8888 | 4096 | 500.0 fps / 65.70 Mbps | 19.23 fps / 2.527 Mbps | 26.0x |
| GPS L2C | 8889 | 4096 | 500.0 fps / 65.70 Mbps | 19.27 fps / 2.532 Mbps | 26.0x |
| GPS L5 | 8890 | 4096 | 500.0 fps / 65.70 Mbps | 19.96 fps / 2.622 Mbps | 25.1x |
| Iridium | 8891 | 4096 | 500.0 fps / 65.70 Mbps | 19.23 fps / 2.527 Mbps | 26.0x |
| **aggregate** | | | **312.7 Mbps** | **26.04 Mbps (3.25 MB/s)** | **12.0x** |

**26.04 Mbps aggregate is below the plan's "few tens of Mbps" acceptance
target (~31 Mbps)** and comfortably inside 1 GbE headroom. It beats the
task's own ~31 Mbps estimate because this host's fake producers are
CPU-limited (Step 3 finding: ~1.2-2 MSPS actual vs. declared) — the same
caveat applies here in the throttled direction: fewer physical FFTs
completed per second all around, plus the wideband quantization effect below.

**Same-process A/B control** (isolates the effect to the throttle, not
host/run-to-run variance): with the 5-band server still running, PATCHed
`fpsPeriodMs` 50 -> 0 -> 50 on all 5 device sets and recaptured:

| port | fpsPeriodMs=50 (before flip) | fpsPeriodMs=0 (unthrottled control) | fpsPeriodMs=50 (restored) |
|---|---|---|---|
| 8887 (RF Wideband) | mean fftTimeMs 66.26 ms (15.09 fps) | mean fftTimeMs 22.00 ms (45.45 fps) | mean fftTimeMs 67.63 ms (14.79 fps) |
| 8888 (GPS L1) | mean fftTimeMs 52.00 ms (19.23 fps) | mean fftTimeMs 2.32 ms (431.8 fps) | (not re-measured; port 8887 sufficient to demonstrate the mechanism) |

The `fpsPeriodMs=0` control cadence (22.0 ms / 2.32 ms) matches Step 3's
original unthrottled findings (21 ms / 2 ms) almost exactly, confirming the
FORK PATCH's `<=0` branch reproduces the pre-patch (upstream) behavior
byte-for-byte in cadence terms, and that the 50ms throttle is what's driving
the ~66ms / ~52ms cadence, not host jitter. Raw captures:
`frames-8887-unthrottled-control.bin`, `frames-8888-unthrottled-control.bin`,
`frames-8887-restored-control.bin`.

**Wideband quantization note (not a bug):** the throttle gate can only
release a frame on an FFT-completion boundary. The wideband FFT completes
every ~22 ms on this CPU-limited host (Step 3 finding), so the earliest
release point >= 50 ms after the last forwarded frame lands ~3 FFT periods
later (~66 ms = 15.09 fps), not exactly at 50 ms/20 fps. The 4 slow bands
(~2 ms FFT period, much finer than the 50 ms target) land within 0-2 ms of
nominal (50.1-52.0 ms observed) because the quantization step is small
relative to `fpsPeriodMs`. On real hardware (Step 4) with genuine RF at the
declared sample rates, FFT completion would be far faster than 50 ms for
every band, so quantization error would be negligible everywhere, and the
achieved fps would sit very close to the requested 20 fps.

## Frame decode — throttled frames still correct

`spectrum_frame_decode.py` against 20 throttled frames per band, same
expected CF/tone-offset arguments as Step 3:

| port | label | CF (Hz) | expected-bin matches | CF matches | peak dB |
|---|---|---|---|---|---|
| 8887 | RF Wideband | 1575420000 | 20/20 | 20/20 | -6.02 |
| 8888 | GPS L1 | 1575420000 | 20/20 | 20/20 | -6.02 |
| 8889 | GPS L2C | 1227600000 | 20/20 | 20/20 | -6.02 |
| 8890 | GPS L5 | 1176450000 | 20/20 | 20/20 | -6.02 |
| 8891 | Iridium | 1621250000 | 20/20 | 20/20 | -6.02 |

Identical to Step 3's unthrottled decode results — throttling drops frames
in time (fewer of them arrive), it does not corrupt or misalign the ones
that do. Full dumps: `frames-<port>-throttled-decoded.txt`.

## No-IQ posture unchanged

`ss -tlnp` with the throttle active still shows exactly the 6 known
listeners belonging to `sdrangelsrv` — `127.0.0.1:8091` (REST) and
`127.0.0.1:{8887,8888,8889,8890,8891}` (the 5 spectrum servers). No new port
opened by this change. See `ss-listening-ports-throttled.txt`.

## Evidence files (this directory)

- `frames-<port>-throttled.bin` / `frames-<port>-throttled-decoded.txt` —
  20-frame capture + decode per band at `fpsPeriodMs=50`.
- `frames-8887-unthrottled-control.bin`, `frames-8888-unthrottled-control.bin`,
  `frames-8887-restored-control.bin` — same-process A/B control captures
  (`fpsPeriodMs` 50 -> 0 -> 50).
- `byte-rate-calc-throttled.txt` — the throttled byte-rate computation +
  quantization note, same format as Step 3's `byte-rate-calc.txt`.
- `step3b-5band-rest-transcript.log` — full REST transcript from
  `step3_launch_5band.sh` showing all 5 `fpsPeriodMs=50 (requested 50)`
  readbacks.
- `ss-listening-ports-throttled.txt` — no-IQ posture confirmation.

## Reproduce

```bash
# from the sdrangel-udmabufinput repo root -- rebuild after the patch:
cmake --build codex-handoff/plan-02/temp/build-host --target sdrangelsrv --parallel 8

# launch (default --fps-period-ms 50 baked into step3_configure_5band.py):
plugins/samplesource/udmabufinput/scripts/step3/step3_launch_5band.sh

# capture + decode one band, e.g. the wideband:
python3 plugins/samplesource/udmabufinput/scripts/step3/spectrum_ws_capture.py \
    --host 127.0.0.1 --port 8887 --count 20 --out /tmp/frames-8887.bin
python3 plugins/samplesource/udmabufinput/scripts/step3/spectrum_frame_decode.py \
    --input /tmp/frames-8887.bin --expected-cf 1575420000 --expected-tone-offset-hz 100000

# teardown when done
plugins/samplesource/udmabufinput/scripts/step3/step3_teardown_5band.sh
```

## Deviations from the task brief

None. Line numbers, the `SpectrumSettings::m_fpsPeriodMs` field, and the
REST wiring were exactly as briefed once the vestigial commented-out block in
`webapirequestmapper.cpp` was traced past (see "REST wiring double-checked"
above) — that file was not touched. The only judgment call beyond the brief:
added `m_wsSpectrumTimer.invalidate()` on socket (re)open in
`handleWSOpenClose` (still confined to `spectrumvis.cpp`) so a fresh client
connection gets an immediate first frame, matching the brief's "handle the
socket just opening gracefully" instruction literally.
