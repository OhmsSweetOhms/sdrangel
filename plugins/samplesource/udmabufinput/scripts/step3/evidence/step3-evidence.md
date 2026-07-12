# plan-03 Step 1 evidence — spectrum-only wire, no-IQ proof

Thread: `cross-cutting/20260711-sdrangel-host-perchannel-sigmf` (gps_design side,
read-only from this fork). This evidence lives on the fork branch
`udmabufinput-plugin`, worktree-relative paths only (no absolute paths /
usernames per project convention).

## What this proves

One `UdmaBufInput` device set (fake producer -> mmap ring -> plugin) feeding
core `SpectrumVis`, with its websocket spectrum server as the *only* network
egress -- no `RemoteSink`/`RemoteInput` channel was ever created. The spectrum
frames decode to the exact injected tone bin/level, and packet captures show
the wire carries only the documented spectrum-frame format, never raw ci16 IQ.

## Setup

- `sdrangelsrv` (headless, `--scratch --api-address 127.0.0.1 --api-port 8091`),
  built at `codex-handoff/plan-02/temp/build-host/sdrangelsrv` (git-ignored
  scratch build, plan-02).
- Fake producer (`codex-handoff/plan-02/scripts/fake_udmabuf_producer.py`)
  writing a ci16 tone into a git-ignored scratch ring file:
  `--pattern tone --sample-rate 2560000 --center-frequency 1575420000
  --tone-frequency 100000 --amplitude 16384 --capacity 262144
  --block-samples 1024`.
  - Declared rate 2,560,000 S/s, CF 1,575,420,000 Hz (L1), tone at +100 kHz.
  - Amplitude 16384/32768 = 0.5 full scale = -6.02 dBFS (matches the plan-02
    handback note's independent amplitude-conversion measurement).

## REST configuration (`step3_configure_spectrum.py`)

See `step3-rest-transcript.log` for the full transcript. Summary:

1. Reset device sets to a clean slate.
2. `POST /sdrangel/deviceset?direction=0` -- add device set 0.
3. `PUT /sdrangel/deviceset/0/device` `{"hwType":"UdmaBufInput","direction":0}`.
4. `PATCH /sdrangel/deviceset/0/device/settings` -- point `udmaBufInputSettings.fileName`
   at the ring file.
5. `POST /sdrangel/deviceset/0/device/run` -- start the engine.
6. `GET /sdrangel/deviceset/0/device/report` -- confirms
   `sampleRate=2560000 centerFrequency=1575420000` (read straight from the
   ring's 64-byte header by `UdmaBufInput::openMmapFile`; the plugin has no
   independent rate/CF override).
7. `PATCH /sdrangel/deviceset/0/spectrum/settings` --
   `{"fftSize":4096,"fftWindow":4,"averagingMode":0,"linear":0,
   "wsSpectrumAddress":"127.0.0.1","wsSpectrumPort":8887}`.
   (`fftWindow=4` is `FFTWindow::Hanning`, `averagingMode=0` is `AvgModeNone`
   -- both are also `SpectrumSettings::resetToDefaults()`'s own defaults, so
   this pins the stock/sensible settings explicitly rather than changing
   them.) Confirmed via `GET` readback.
8. `POST /sdrangel/deviceset/0/spectrum/server` -- starts the `WSSpectrum`
   websocket listener. `GET` readback confirms `run=1
   listeningAddress=127.0.0.1 listeningPort=8887`.

No `RemoteSink`/`RemoteInput` channel or device set was created at any point.

## Frame format — confirmed against source, with one correction to the findings note

Read `sdrbase/dsp/spectrumvis.h`/`.cpp` and `sdrbase/websockets/wsspectrum.{h,cpp}`
directly (not just the swagger docs). `WSSpectrum::buildPayload` (little-endian,
native x86_64 struct packing):

| offset | field | type | note |
|---|---|---|---|
| 0 | centerFrequency | **uint64** | findings note said "int64"; same 8-byte wire size, no decode impact |
| 8 | fftTimeMs | int64 | matches findings note |
| 16 | timestampMs (unix ms) | **uint64** | findings note said "int64"; same wire size |
| 24 | fftSize | int32 | matches |
| 28 | bandwidth (Hz) | int32 | matches; equals the device's sample rate, not a separate config value |
| 32 | indicators | int32 | bit0 linear, bit1 ssb, bit2 usb — matches |
| 36 | power spectrum | N x float32 | `Real` is `typedef float Real` (`dsp/dsptypes.h`); matches |

Bin ordering (not explicitly stated in the findings note, confirmed by reading
`SpectrumVis::processFFT`'s `AvgModeNone`/`reorder=true`/`positiveOnly=false`
branch — `UdmaBufInput` is complex-I/Q so `positiveOnly = m_realElseComplex =
false` per `DSPSignalNotification`'s default): the spectrum is fftshifted,
index 0 = -bandwidth/2, index N/2 = DC (the tuned CF), index N-1 =
+bandwidth/2 - bin width. Units: `linear` bit 0 (our config) means the
float32 values are `10*log10(|FFT_bin|^2 / fftSize^2)` dB, the same units
the stock GUI spectrum widget shows.

## Frame decode (`spectrum_frame_decode.py` against `reference-frames/spectrum-frames.bin`)

20 captured frames, every one identical because the tone is stationary:

- `centerFrequency = 1575420000` Hz -- MATCH, 20/20.
- `fftSize = 4096`, `bandwidth = 2560000` Hz -> bin width 625 Hz.
- Peak bin = **2208** (of 4096), i.e. `+100000.0 Hz` from CF -- exactly the
  injected tone offset (`2048 + 100000/625 = 2208`) -- MATCH, 20/20.
- Peak value = **-6.02 dB**, exactly the expected `-6.02 dBFS` for a
  16384/32768 amplitude ci16 tone.
- `indicators = 0x4` (usb bit set, `linear=0` -> dB units) on every frame.

See `reference-frames/spectrum-frames-decoded.txt` for the full per-frame dump.

## No-IQ proof

1. **Listening-socket enumeration** (`step3-ss-listening-ports.txt`,
   `ss -tlnp`): only two TCP listeners belong to `sdrangelsrv` --
   `127.0.0.1:8091` (REST) and `127.0.0.1:8887` (spectrum websocket). No
   `RemoteSink` data port (default 9999) or any other port is bound.
2. **Dedicated capture on the spectrum port** (`spectrum-port-capture.pcap`,
   `tcp port 8887`, 15 s, 50 packets, 0 dropped): every push from
   `127.0.0.1:8887` is exactly 16424 bytes = a 4-byte WS binary-frame header
   (`82 7e`, extended 16-bit length `40 24` = 16420) + the 16420-byte
   spectrum frame (36-byte header + 4096 float32 = matches `fftSize=4096`
   exactly). See `step3-tcpdump-summary.txt`.
3. **Byte-level header verification independent of our own decoder**
   (`step3-hex-header-verification.txt`): hand-parsed the raw hex from one
   captured packet -- `centerFrequency` bytes decode to exactly `1575420000`,
   `fftSize` bytes decode to exactly `4096`, `bandwidth` bytes decode to
   exactly `2560000`. This confirms the wire format directly from the pcap,
   not just through `spectrum_frame_decode.py`.
4. **Unfiltered broad capture** (`lo-broad-no-other-egress.pcap`, no BPF
   filter beyond excluding DNS-port-53 noise, 8 s, 20 packets while 5 more
   spectrum frames were pulled): every single packet on loopback is between
   `127.0.0.1` and port `8887` (server) and the ephemeral client port. No
   packet touches any other port -- no RemoteSink UDP, nothing else.

Conclusion: for the whole test, the only bytes ever on the wire, on any port,
were REST control-plane JSON (port 8091) and the documented spectrum-frame
format (port 8887). No ci16 IQ left the process at any point.

## Tooling note: `tcpdump` capture privilege

Plain `tcpdump -i lo` requires `CAP_NET_RAW`/root on this host
(`tcpdump: lo: You don't have permission to capture on that device`), and
running it via `sudo` is against the task's no-sudo rule. `dumpcap` (part of
the same Wireshark toolchain) is already installed with
`cap_net_admin,cap_net_raw=eip` and the working user is already in the
`wireshark` group, so **`dumpcap` did the live capture** and plain `tcpdump -r`
/ read-only analysis (which needs no special privilege) did the offline
decode shown above. This is a like-for-like substitution within the same
toolchain, not a workaround of a safety control -- no privilege escalation
was used or requested.

## Reproduce

```bash
# 1. headless server + fake producer (see codex-handoff/plan-02 for the producer)
./codex-handoff/plan-02/temp/build-host/sdrangelsrv --scratch --api-address 127.0.0.1 --api-port 8091 &
python3 codex-handoff/plan-02/scripts/fake_udmabuf_producer.py \
  --path codex-handoff/plan-03/temp/tone.udma --pattern tone \
  --sample-rate 2560000 --center-frequency 1575420000 \
  --tone-frequency 100000 --amplitude 16384 --duration 300 &

# 2. configure device set + spectrum server over REST
python3 plugins/samplesource/udmabufinput/scripts/step3/step3_configure_spectrum.py \
  --ring-path "$(pwd)/codex-handoff/plan-03/temp/tone.udma"

# 3. capture + decode
python3 plugins/samplesource/udmabufinput/scripts/step3/spectrum_ws_capture.py \
  --count 20 --out frames.bin
python3 plugins/samplesource/udmabufinput/scripts/step3/spectrum_frame_decode.py \
  --input frames.bin --expected-cf 1575420000 --expected-tone-offset-hz 100000

# 4. no-IQ proof
dumpcap -i lo -f "tcp port 8887" -a duration:15 -w capture.pcap   # or sudo tcpdump
tcpdump -r capture.pcap -nn
ss -tlnp | grep sdrangelsrv
```
