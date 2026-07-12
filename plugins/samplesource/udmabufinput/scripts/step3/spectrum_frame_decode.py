#!/usr/bin/env python3
"""Decode raw SpectrumVis websocket frames captured by spectrum_ws_capture.py.

Wire format per frame (confirmed against sdrbase/websockets/wsspectrum.cpp,
WSSpectrum::buildPayload -- little-endian, native x86_64 struct layout):

    offset  0  uint64  centerFrequency (Hz)
    offset  8  int64   fftTimeMs        (effective FFT period, ms)
    offset 16  uint64  timestampMs      (unix time, ms)
    offset 24  int32   fftSize          (N)
    offset 28  int32   bandwidth        (Hz -- equals the device sample rate)
    offset 32  int32   indicators       (bit0 linear, bit1 ssb, bit2 usb)
    offset 36  N x float32  power spectrum, fftshifted:
                 index 0       = -bandwidth/2
                 index N/2     = 0 Hz (DC, i.e. the tuned center frequency)
                 index N-1     = +bandwidth/2 - bandwidth/N
               (SpectrumVis::processFFT's "reorder" path -- confirmed by
               reading the AvgModeNone/positiveOnly=false branch in
               sdrbase/dsp/spectrumvis.cpp; UdmaBufInput is a complex-sample
               device so positiveOnly=m_realElseComplex=false.)

If indicators bit0 (linear) is 0 -- the default -- the float32 values are
already in dB: 10*log10(|FFT_bin|^2 / fftSize^2), the same units the stock
SDRangel GUI spectrum widget displays (see SpectrumVis::m_mult /
m_powFFTMul in spectrumvis.cpp). If bit0 is 1, values are linear power.

Usage:
    python3 spectrum_frame_decode.py --input frames.bin \
        [--expected-cf 1575420000] [--expected-tone-offset-hz 100000] \
        [--dump-out frames-decoded.txt] [--max-print 3]
"""

import argparse
import struct
import sys

HEADER = struct.Struct("<QqQiii")
HEADER_BYTES = HEADER.size
assert HEADER_BYTES == 36, HEADER_BYTES


def iter_frames(data: bytes):
    offset = 0
    n = len(data)
    while offset < n:
        if offset + HEADER_BYTES > n:
            remaining = n - offset
            raise ValueError(f"{remaining} trailing bytes after last frame "
                              f"(short header) at offset {offset}")
        (center_freq, fft_time_ms, timestamp_ms, fft_size, bandwidth,
         indicators) = HEADER.unpack_from(data, offset)
        body_offset = offset + HEADER_BYTES
        body_bytes = fft_size * 4
        if body_offset + body_bytes > n:
            raise ValueError(
                f"frame at offset {offset} declares fftSize={fft_size} "
                f"({body_bytes} bytes) but only {n - body_offset} bytes remain"
            )
        powers = struct.unpack_from(f"<{fft_size}f", data, body_offset)
        yield {
            "offset": offset,
            "center_frequency_hz": center_freq,
            "fft_time_ms": fft_time_ms,
            "timestamp_ms": timestamp_ms,
            "fft_size": fft_size,
            "bandwidth_hz": bandwidth,
            "indicators": indicators,
            "linear": bool(indicators & 1),
            "ssb": bool(indicators & 2),
            "usb": bool(indicators & 4),
            "powers": powers,
        }
        offset = body_offset + body_bytes


def analyze_frame(frame, expected_cf=None, expected_tone_offset_hz=None):
    powers = frame["powers"]
    fft_size = frame["fft_size"]
    bandwidth = frame["bandwidth_hz"]
    bin_hz = bandwidth / fft_size if fft_size else float("nan")
    half = fft_size // 2

    peak_bin = max(range(fft_size), key=lambda i: powers[i])
    peak_val = powers[peak_bin]
    peak_offset_hz = (peak_bin - half) * bin_hz

    result = {
        "peak_bin": peak_bin,
        "peak_value": peak_val,
        "peak_offset_hz": peak_offset_hz,
        "bin_width_hz": bin_hz,
        "units": "linear power" if frame["linear"] else "dB (10*log10(|FFT|^2/N^2))",
    }

    if expected_cf is not None:
        result["cf_match"] = (frame["center_frequency_hz"] == expected_cf)
    if expected_tone_offset_hz is not None:
        expected_bin = half + round(expected_tone_offset_hz / bin_hz)
        result["expected_bin"] = expected_bin
        result["expected_offset_hz"] = expected_tone_offset_hz
        result["bin_match"] = (peak_bin == expected_bin)

    return result


def format_frame(index, frame, analysis):
    lines = [
        f"--- frame {index} (byte offset {frame['offset']}) ---",
        f"  centerFrequency : {frame['center_frequency_hz']} Hz",
        f"  fftTimeMs       : {frame['fft_time_ms']} ms",
        f"  timestampMs     : {frame['timestamp_ms']} ms (unix)",
        f"  fftSize         : {frame['fft_size']}",
        f"  bandwidth       : {frame['bandwidth_hz']} Hz",
        f"  indicators      : 0x{frame['indicators']:x} "
        f"(linear={frame['linear']} ssb={frame['ssb']} usb={frame['usb']})",
        f"  bin width       : {analysis['bin_width_hz']:.3f} Hz",
        f"  peak bin        : {analysis['peak_bin']} "
        f"(offset {analysis['peak_offset_hz']:+.1f} Hz from CF)",
        f"  peak value      : {analysis['peak_value']:.2f} {analysis['units']}",
    ]
    if "expected_bin" in analysis:
        lines.append(
            f"  expected bin    : {analysis['expected_bin']} "
            f"(offset {analysis['expected_offset_hz']:+.1f} Hz) "
            f"-> {'MATCH' if analysis['bin_match'] else 'MISMATCH'}"
        )
    if "cf_match" in analysis:
        lines.append(
            f"  CF check        : {'MATCH' if analysis['cf_match'] else 'MISMATCH'}"
        )
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--expected-cf", type=int, default=None)
    parser.add_argument("--expected-tone-offset-hz", type=float, default=None)
    parser.add_argument("--dump-out", default=None,
                         help="write a full human-readable dump of every frame here")
    parser.add_argument("--max-print", type=int, default=3,
                         help="frames to print to stdout in full (all are still scanned for stats)")
    args = parser.parse_args()

    with open(args.input, "rb") as fh:
        data = fh.read()

    frames = list(iter_frames(data))
    print(f"[OK] parsed {len(frames)} frame(s) from {args.input} ({len(data)} bytes)")

    dump_lines = []
    peak_bins = []
    peak_values = []
    bin_matches = []
    cf_matches = []

    for i, frame in enumerate(frames):
        analysis = analyze_frame(frame, args.expected_cf, args.expected_tone_offset_hz)
        text = format_frame(i, frame, analysis)
        dump_lines.append(text)
        peak_bins.append(analysis["peak_bin"])
        peak_values.append(analysis["peak_value"])
        if "bin_match" in analysis:
            bin_matches.append(analysis["bin_match"])
        if "cf_match" in analysis:
            cf_matches.append(analysis["cf_match"])

        if i < args.max_print:
            print(text)

    if args.dump_out:
        with open(args.dump_out, "w") as fh:
            fh.write("\n\n".join(dump_lines) + "\n")
        print(f"[OK] full decoded dump written to {args.dump_out}")

    print("\n=== decode summary ===")
    print(f"frames                : {len(frames)}")
    print(f"peak bin (all frames) : {sorted(set(peak_bins))}")
    print(f"peak value range      : {min(peak_values):.2f} .. {max(peak_values):.2f}")
    if bin_matches:
        print(f"expected-bin matches  : {sum(bin_matches)}/{len(bin_matches)}")
    if cf_matches:
        print(f"CF matches            : {sum(cf_matches)}/{len(cf_matches)}")

    if bin_matches and not all(bin_matches):
        sys.exit(1)
    if cf_matches and not all(cf_matches):
        sys.exit(1)


if __name__ == "__main__":
    main()
