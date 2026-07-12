# plan-03 Step 3 evidence — 5 device sets (1 fast + 4 slow), no-IQ, measured wire rate

Thread: `cross-cutting/20260711-sdrangel-host-perchannel-sigmf` (gps_design side,
read-only from this fork). This evidence lives on the fork branch
`udmabufinput-plugin`, worktree-relative paths only.

## What this proves

5 concurrent `UdmaBufInput` device sets on one headless `sdrangelsrv`, each with
its own `SpectrumVis` -> `WSSpectrum` websocket server on a distinct port
(8887-8891). No `RemoteSink`/`RemoteInput` channel exists on any device set --
the 5 spectrum websockets (+ the single REST control port 8091) are the *only*
network egress, same posture as Step 1, generalized to 5 bands. All 5 decode to
their exact injected CF/tone, and packet captures show the wire carries only
REST JSON + the documented spectrum-frame format on 6 known ports.

## The 5 bands

| device set | label | ring | declared SR | CF (Hz) | tone offset | fftSize | ws port | rate class |
|---|---|---|---|---|---|---|---|---|
| 0 | RF Wideband | `rf_wb.udma` | 20,480,000 | 1,575,420,000 | +100 kHz | 32768 | 8887 | fast |
| 1 | GPS L1 | `l1.udma` | 2,560,000 | 1,575,420,000 | +50 kHz | 4096 | 8888 | slow |
| 2 | GPS L2C | `l2c.udma` | 2,560,000 | 1,227,600,000 | -100 kHz | 4096 | 8889 | slow |
| 3 | GPS L5 | `l5.udma` | 2,560,000 | 1,176,450,000 | +200 kHz | 4096 | 8890 | slow |
| 4 | Iridium | `iridium.udma` | 2,560,000 | 1,621,250,000 | -200 kHz | 4096 | 8891 | slow |

All 5 use `--amplitude 16384` (16384/32768 = 0.5 FS = -6.02 dBFS, the same
Step-1 convention) so every band's peak lands at the same, independently
known, -6.02 dB level.

**fftSize choice for the wideband band (32768, the max FFT size the codebase
allows -- `SpectrumSettings::m_log2FFTSizeMax = 15`):** chosen so the
wideband band's *frame rate* (not byte rate -- see below) lands in the same
ballpark as the 4 slow bands' already-Step1/Step2-proven ~500 fps at
fftSize=4096, rather than an 8x-higher message rate that would stress the
Python relay/frontend for no benefit (see "Measured wire byte-rate" below --
choosing a smaller or larger fftSize for the wideband band does not change
its byte rate, only its message rate).

## Setup

- `sdrangelsrv` (headless, `--scratch --api-address 127.0.0.1 --api-port 8091`),
  the same `codex-handoff/plan-02/temp/build-host/sdrangelsrv` binary as Step 1
  (git-ignored scratch build; not rebuilt for Step 3).
- 5 fake producers (`codex-handoff/plan-02/scripts/fake_udmabuf_producer.py`,
  identical to the canonical `diagnostics/fake_udmabuf_producer.py` copy on the
  gps_design thread), one ring file each, launched by
  `step3_launch_5band.sh`.

## REST configuration (`step3_configure_5band.py`)

Generalizes Step 1's `step3_configure_spectrum.py` to N device sets: adds
device sets in `--band` argument order (device set index == creation order),
then per device set runs the same 8-call sequence Step 1 proved (add device
set, select `UdmaBufInput`, point it at the ring, run, read back the source
report, configure spectrum settings incl. a **distinct** `wsSpectrumPort`,
start the spectrum server, read back the server status). See
`step3-5band-rest-transcript.log` for the full transcript -- all 5 device
sets came back `run=1` with the requested port, e.g.:

```
[OK] device set 0 "RF Wideband": ... sampleRate=20480000 centerFrequency=1575420000 fftSize=32768 ws=127.0.0.1:8887 (requested 8887) run=1
[OK] device set 1 "GPS L1": ... sampleRate=2560000 centerFrequency=1575420000 fftSize=4096 ws=127.0.0.1:8888 (requested 8888) run=1
[OK] device set 2 "GPS L2C": ... sampleRate=2560000 centerFrequency=1227600000 fftSize=4096 ws=127.0.0.1:8889 (requested 8889) run=1
[OK] device set 3 "GPS L5": ... sampleRate=2560000 centerFrequency=1176450000 fftSize=4096 ws=127.0.0.1:8890 (requested 8890) run=1
[OK] device set 4 "Iridium": ... sampleRate=2560000 centerFrequency=1621250000 fftSize=4096 ws=127.0.0.1:8891 (requested 8891) run=1
No RemoteSink / RemoteInput channel was created on any device set.
```

**Port gotcha confirmed and handled:** `wsSpectrumPort` defaults to 8887 for
every new device set; `step3_configure_5band.py` requires a distinct `port=`
per `--band` and asserts on collision before touching the API
(`--band ports must be distinct`).

**Deviation from the single-band script:** the REST call timeout was raised
from 5s to 20s (`REST_TIMEOUT_S`), with one retry on a bare socket
timeout/`OSError`. The very first bring-up run hit a ~20s stall on the
`GET .../spectrum/settings` readback immediately after configuring device set
0's `fftSize=32768` -- a one-time cost from `sdrangelsrv` allocating/planning
a new (larger, previously-unused) FFT engine size on its single Qt event-loop
thread, which also serves REST. Not a hang: the process recovered and stayed
responsive for the rest of the run (confirmed by re-querying
`/spectrum/settings` manually while it was "stuck" -- it answered once the
allocation finished). This is a legitimate config-script robustness fix
(patience, not a workaround), not a plugin-code change.

## Frame decode (`spectrum_frame_decode.py` against 20 captured frames per band)

All 5 bands: 20/20 CF match, 20/20 expected-bin match, exact -6.02 dB peak
(matches the Step-1 amplitude convention). See `frames-<port>-decoded.txt`
for the full per-frame dumps; summary:

| port | label | CF (Hz) | fftSize | bandwidth (Hz) | peak bin | peak offset | peak dB |
|---|---|---|---|---|---|---|---|
| 8887 | RF Wideband | 1575420000 | 32768 | 20480000 | 16544 | +100000 Hz | -6.02 |
| 8888 | GPS L1 | 1575420000 | 4096 | 2560000 | 2128 | +50000 Hz | -6.02 |
| 8889 | GPS L2C | 1227600000 | 4096 | 2560000 | 1888 | -100000 Hz | -6.02 |
| 8890 | GPS L5 | 1176450000 | 4096 | 2560000 | 2368 | +200000 Hz | -6.02 |
| 8891 | Iridium | 1621250000 | 4096 | 2560000 | 1728 | -200000 Hz | -6.02 |

All 5 distinct CFs, distinct tone offsets, identical -6.02 dB level, exactly
as configured -- the 5 device sets do not cross-talk or collide.

## No-IQ proof

1. **Listening-socket enumeration** (`ss-listening-ports.txt`, `ss -tlnp`):
   exactly 6 TCP listeners belong to `sdrangelsrv` -- `127.0.0.1:8091` (REST)
   and `127.0.0.1:{8887,8888,8889,8890,8891}` (the 5 spectrum servers). No
   `RemoteSink` data port (default 9999) or any other port is bound.
2. **Dedicated 5-port capture** (`spectrum-5port-capture.pcap`, filtered to
   the 5 spectrum ports + REST, ~2 s wall time while 5 parallel clients
   pulled frames): 6225 packets total, every one of them between
   `127.0.0.1` and one of the 5 spectrum ports (no REST traffic happened to
   land in this particular 2 s window -- see the broader capture below for
   REST). `tcpdump-5port-summary.txt` has the full port-pair breakdown and a
   hex dump of the first data-bearing packet on each port; wire lengths
   match the expected frame sizes exactly (16424 bytes = 4-byte WS header +
   16420-byte spectrum frame for the 4 slow bands at fftSize=4096; the
   wideband's 131118-byte message spans multiple ~32-36 KB TCP segments, as
   expected for a payload > 65535 bytes).
3. **Byte-level header verification independent of our own decoder**: hand-
   parsed the raw hex bytes of the port-8888 packet's WS payload (right
   after the `82 7e 40 24` WS binary-frame header) -- decodes to
   `centerFrequency=1575420000`, `fftTimeMs=2`, exactly matching the
   independently-decoded capture. (Reproduced inline in this doc's git
   history / the session transcript; not re-saved as a separate file since
   `tcpdump-5port-summary.txt` already carries the same packet's full hex.)
4. **Unfiltered broad capture** (`lo-broad-no-other-egress.pcap`, no BPF
   filter beyond excluding DNS port 53, ~2 s, while pulling frames from all 5
   ports *and* issuing one REST GET mid-capture): `tcpdump-broad-summary.txt`
   -- every packet is between `127.0.0.1` and one of the 6 known ports
   (8091 REST: 10 packets; 8887-8891 spectrum: 163-210 packets each). No
   packet touches any other port.

Conclusion: across both capture windows (5-port-filtered and broad), the
only bytes ever on the wire, on any port, were REST control-plane JSON (port
8091) and the documented spectrum-frame format (ports 8887-8891). No ci16 IQ
left the process at any point, with 5 concurrent bands running.

## Measured wire byte-rate — NOT "few Mbps"; a source-grounded finding

**Headline number: 312.7 Mbps aggregate across the 5 ports** (39.1 MB/s),
computed in `byte-rate-calc.txt` from each band's own `fftTimeMs` header
field (the server's measured inter-frame period) and its captured wire
frame size -- not from a live rate race, so it's not sensitive to how fast
my own capture client happened to read.

| band | port | fftSize | fftTimeMs | fps | wire bytes/frame | measured Mbps |
|---|---|---|---|---|---|---|
| RF Wideband | 8887 | 32768 | 21 | 47.6 | 131118 | 49.95 |
| GPS L1 | 8888 | 4096 | 2 | 500.0 | 16424 | 65.70 |
| GPS L2C | 8889 | 4096 | 2 | 500.0 | 16424 | 65.70 |
| GPS L5 | 8890 | 4096 | 2 | 500.0 | 16424 | 65.70 |
| Iridium | 8891 | 4096 | 2 | 500.0 | 16424 | 65.70 |
| **total** | | | | | | **312.7** |

**This contradicts the plan-03 Step-3 acceptance line's "total wire ≈ few
Mbps" -- by roughly 60-300x depending on how literally "few" is read.** This
is not a bug in the Step-3 config; it is how the stock headless `WSSpectrum`
path behaves, confirmed by reading the source (not inferred):

- `sdrbase/websockets/wsspectrum.cpp::sendPayload` sends the built payload to
  every connected client on *every* call to `newSpectrum` -- no rate check,
  no skip logic.
- `sdrbase/dsp/spectrumvis.cpp:625` calls `m_wsSpectrum.newSpectrum(...)`
  unconditionally whenever `m_wsSpectrum.socketOpened()`, once per completed
  FFT -- i.e. once per `fftSize` (no-overlap; `m_fftOverlap` defaults to 0)
  new samples consumed.
- The GUI-side FPS cap (`SpectrumSettings::m_fpsPeriodMs`, default 50 ms /
  20 fps) is a **display-only** throttle wired into `sdrgui/gui/glspectrumview.cpp`
  (`m_timer.start(m_fpsPeriodMs)`, a Qt paint timer) -- it has no effect on
  `sdrbase`'s headless FFT-to-websocket path, and the REST hook for it is
  explicitly **commented out**:
  `sdrbase/webapi/webapirequestmapper.cpp:3990-3993`
  (`// if (jsonObject.contains("fpsPeriodMs")) ... setFpsPeriodMs(...)`).

**Consequence:** for a no-overlap FFT, wire byte-rate is bytes/sec =
`(sampleRate/fftSize) * (36 + 4*fftSize)` ≈ `4 * sampleRate` for any
fftSize large enough that the 36-byte header is negligible -- i.e. **the
unthrottled spectrum-frame byte-rate is mathematically the same order as
raw ci16 IQ at that sample rate** (both are 4 bytes/sample). Choosing a
different `fftSize` changes the *message rate*, not the *byte rate* (this
is why 32768 was chosen for the wideband band above -- for message-rate
comfort, not bandwidth).

**Why the measured number (312.7 Mbps) is lower than the ~983 Mbps a naive
`4 * declared_sample_rate` sum would predict:** the pure-Python fake
producer cannot sustain its declared sample rate in real time. A microbenchmark
of the producer's tone-generation inner loop (including the `mmap` /
`struct.pack_into` write) measured **~1.24M samples/sec** achievable on this
host -- 6% of the wideband band's declared 20.48 MSPS, 48% of the slow
bands' declared 2.56 MSPS. The "implied actual sample rate" column in
`byte-rate-calc.txt` (back-computed from each band's measured `fftTimeMs`)
confirms this: ~1.56 MSPS actual for the wideband band, ~2.05 MSPS actual for
each slow band -- both in the microbenchmark's ballpark, not the declared
rate. **This means real hardware (Step 4, where the sample source is
genuine RF, not a CPU-bound Python loop) would very likely see a *higher*
aggregate wire rate than measured here, not lower** -- the declared rates
would actually be sustained, pushing the aggregate toward the ~983 Mbps
`4 * sample_rate` ceiling.

**Not fixed here** (would require a plugin/core code change, out of scope
for this REST-orchestration task): there is no REST-exposed way to cap the
headless spectrum push rate independently of `fftSize`. Reducing wire
bandwidth to "few Mbps" would need either (a) a genuine FFT-rate throttle in
`WSSpectrum`/`SpectrumVis` (C++ change), or (b) decimating the sample rate
fed into `SpectrumVis` before the FFT (e.g. a channel/decimator ahead of the
spectrum tap), or (c) accepting the current per-completed-FFT push behavior
and treating "few Mbps" as a stale assumption to correct in the plan doc.
Flagged for the orchestrator's decision, not decided here.

## Reproduce

```bash
# from the sdrangel-udmabufinput repo root
plugins/samplesource/udmabufinput/scripts/step3/step3_launch_5band.sh

# ... capture / decode / verify (see step3_launch_5band.sh output for the
# assigned ports; then reuse spectrum_ws_capture.py / spectrum_frame_decode.py
# per port exactly as in Step 1, one invocation per port)

# teardown when done
plugins/samplesource/udmabufinput/scripts/step3/step3_teardown_5band.sh
```
