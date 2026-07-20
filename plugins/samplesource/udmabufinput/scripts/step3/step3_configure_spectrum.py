#!/usr/bin/env python3
"""Configure the plan-03 Step-1 chain on a headless sdrangelsrv over REST.

Chain: UdmaBufInput (device set 0) -> SpectrumVis -> spectrum websocket
server (WSSpectrum). NO RemoteSink, NO RemoteInput -- the spectrum
websocket is the *only* network egress this script wires up. That is
the point of plan-03 Step 1: prove the host can get spectrum-only
telemetry with zero IQ on the wire.

Frame format sent by the spectrum websocket server (confirmed against
sdrbase/websockets/wsspectrum.cpp -- see WSSpectrum::buildPayload):

    offset  0  uint64  center frequency (Hz)
    offset  8  int64   effective FFT time (ms, QElapsedTimer::restart())
    offset 16  uint64  unix timestamp (ms)
    offset 24  int32   FFT size (N)
    offset 28  int32   FFT bandwidth (Hz) -- this is the device sample rate
    offset 32  int32   indicators bitfield (bit0 linear, bit1 ssb, bit2 usb)
    offset 36  N x float32  power spectrum, fftshifted (index 0 = -Fs/2,
                             index N/2 = DC, index N-1 = +Fs/2 - binwidth)

(This corrects two cosmetic details versus the plan-03 findings note,
which called the two 8-byte fields "int64": center frequency and the
unix timestamp are written as uint64_t in the C++ source. Same wire
size, no decode impact for realistic values.)

Usage:
    python3 step3_configure_spectrum.py --ring-path /path/to/ring.udma \
        [--api http://127.0.0.1:8091] [--fft-size 4096] \
        [--ws-address 127.0.0.1] [--ws-port 8887]
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

# FFTWindow::Function enum (sdrbase/dsp/fftwindow.h) -- Hanning is index 4
# and is also SpectrumSettings::resetToDefaults()'s default, so this is a
# "sensible" (== stock default) window, pinned explicitly for the record.
FFT_WINDOW_HANNING = 4

# SpectrumSettings::AveragingMode enum (sdrbase/dsp/spectrumsettings.h) --
# AvgModeNone = 0, also the resetToDefaults() default. No averaging is
# what we want for a single-tone bin-level proof: we want to see the
# raw per-FFT peak, not a smoothed one.
AVERAGING_MODE_NONE = 0


def rest(method, url, body=None):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            raw = resp.read()
            return resp.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            return exc.code, json.loads(raw)
        except json.JSONDecodeError:
            return exc.code, {"raw": raw.decode("utf-8", "replace")}
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        # Connection-level failure (refused, reset, DNS, timeout -- not an
        # HTTP error status). Surfaced as status=None so a bounded poll loop
        # (wait_for_deviceset_ready) can treat it as "not ready yet" and
        # keep retrying within its own deadline, instead of an uncaught
        # traceback interrupting the bounded wait.
        return None, {"error": repr(exc)}


def step(label, method, url, body=None, expect=(200, 202)):
    status, payload = rest(method, url, body)
    ok = status in expect
    marker = "OK" if ok else "FAIL"
    print(f"[{marker}] {label}: {method} {url} -> {status}")
    if not ok:
        print(json.dumps(payload, indent=2))
        raise SystemExit(f"step failed: {label}")
    return payload


def wait_for_deviceset_ready(base, expected_count, timeout_s=30.0, poll_interval_s=0.2):
    """Polls GET /sdrangel/devicesets until devicesetcount >= expected_count.

    plan-05 Step 5: POST /sdrangel/deviceset returns 202 once creation is
    merely QUEUED -- DeviceSet -> SpectrumVis -> FFTFactory construction
    still runs asynchronously on the server's event-loop thread afterward
    (see findings-2026-07-20-fft-bench-root-cause.md). Sending the
    device-selection PUT before that finishes attributes the queued
    creation's latency to the PUT instead. This polls devicesetcount
    (GET /sdrangel/devicesets, SWGDeviceSetList) so the PUT is only ever
    sent once the server confirms the device set actually exists.
    """
    deadline = time.monotonic() + timeout_s
    last_count = None
    while time.monotonic() < deadline:
        status, payload = rest("GET", f"{base}/devicesets")
        if status == 200:
            last_count = payload.get("devicesetcount")
            if last_count is not None and last_count >= expected_count:
                print(f"[OK] device-set ready: devicesetcount={last_count} (>= expected {expected_count})")
                return last_count
        time.sleep(poll_interval_s)

    # A creation timeout is reported as exactly that -- phase=deviceset-creation
    # -- and NEVER as a device-selection/switch failure, even though the next
    # step in this script would otherwise have been the device-selection PUT.
    raise SystemExit(
        f"step failed: DeviceSet CREATION timed out after {timeout_s}s waiting for "
        f"devicesetcount >= {expected_count} (last observed devicesetcount={last_count}); "
        f"phase=deviceset-creation -- NOT sending the device-selection PUT"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--api", default="http://127.0.0.1:8091")
    parser.add_argument("--ring-path", required=True)
    parser.add_argument("--fft-size", type=int, default=4096)
    parser.add_argument("--ws-address", default="127.0.0.1")
    parser.add_argument("--ws-port", type=int, default=8887)
    parser.add_argument("--deviceset-ready-timeout", type=float, default=30.0,
        help="bounded wait (seconds) for devicesetcount to confirm DeviceSet creation before PUT (plan-05 Step 5)")
    args = parser.parse_args()

    base = args.api.rstrip("/") + "/sdrangel"

    # --- Reset to a clean, known state -------------------------------------
    for _ in range(8):
        status, _ = rest("DELETE", f"{base}/deviceset")
        if status == 404:
            break
        print(f"[INFO] cleared a prior device set (status {status})")
        time.sleep(0.2)

    # --- Device set 0: UdmaBufInput -----------------------------------------
    status, payload = rest("GET", f"{base}/devicesets")
    baseline_count = payload.get("devicesetcount", 0) if status == 200 else 0

    step("add device set 0 (Rx)", "POST", f"{base}/deviceset?direction=0")
    wait_for_deviceset_ready(base, baseline_count + 1, timeout_s=args.deviceset_ready_timeout)
    step(
        "select UdmaBufInput on device set 0",
        "PUT",
        f"{base}/deviceset/0/device",
        {"hwType": "UdmaBufInput", "direction": 0},
    )
    step(
        "point UdmaBufInput at the ring file",
        "PATCH",
        f"{base}/deviceset/0/device/settings",
        {
            "deviceHwType": "UdmaBufInput",
            "direction": 0,
            "udmaBufInputSettings": {"fileName": args.ring_path},
        },
    )
    step("start device set 0 engine", "POST", f"{base}/deviceset/0/device/run", {})
    time.sleep(1.0)
    report = step(
        "read back UdmaBufInput report (source rate + CF)",
        "GET",
        f"{base}/deviceset/0/device/report",
    )
    udma_report = report.get("udmaBufInputReport", {})
    source_rate = udma_report.get("sampleRate")
    source_cf = udma_report.get("centerFrequency")
    print(f"    source sampleRate={source_rate} centerFrequency={source_cf}")

    # --- Spectrum settings: FFT size + window + averaging + ws address:port
    step(
        "configure spectrum FFT size/window/averaging + ws bind",
        "PATCH",
        f"{base}/deviceset/0/spectrum/settings",
        {
            "fftSize": args.fft_size,
            "fftWindow": FFT_WINDOW_HANNING,
            "averagingMode": AVERAGING_MODE_NONE,
            "linear": 0,
            "wsSpectrumAddress": args.ws_address,
            "wsSpectrumPort": args.ws_port,
        },
    )
    settings_readback = step(
        "read back spectrum settings",
        "GET",
        f"{base}/deviceset/0/spectrum/settings",
    )
    print(
        f"    fftSize={settings_readback.get('fftSize')} "
        f"fftWindow={settings_readback.get('fftWindow')} "
        f"averagingMode={settings_readback.get('averagingMode')} "
        f"linear={settings_readback.get('linear')} "
        f"wsSpectrumAddress={settings_readback.get('wsSpectrumAddress')} "
        f"wsSpectrumPort={settings_readback.get('wsSpectrumPort')}"
    )

    # --- Start the spectrum websocket server (NO RemoteSink/RemoteInput) ---
    step(
        "start spectrum websocket server",
        "POST",
        f"{base}/deviceset/0/spectrum/server",
    )
    time.sleep(0.3)
    server_status = step(
        "read back spectrum server status",
        "GET",
        f"{base}/deviceset/0/spectrum/server",
    )
    print(
        f"    run={server_status.get('run')} "
        f"listeningAddress={server_status.get('listeningAddress')} "
        f"listeningPort={server_status.get('listeningPort')} "
        f"clients={server_status.get('clients')}"
    )

    print("\n=== Step-1 (plan-03) configuration summary ===")
    print(f"UdmaBufInput ring         : {args.ring_path}")
    print(f"source sampleRate         : {source_rate}")
    print(f"source centerFrequency    : {source_cf}")
    print(f"spectrum fftSize          : {settings_readback.get('fftSize')}")
    print(f"spectrum bandwidth (Hz)   : {source_rate} (== device sample rate, "
          f"carried per-frame in the wire header, not this settings call)")
    print(f"spectrum ws address:port  : {server_status.get('listeningAddress')}:"
          f"{server_status.get('listeningPort')}")
    print("No RemoteSink / RemoteInput channel was created on this device set.")

    if not (source_rate and source_cf and server_status.get("run")):
        print("WARN: one or more expected fields missing -- inspect manually.")
        sys.exit(1)


if __name__ == "__main__":
    main()
