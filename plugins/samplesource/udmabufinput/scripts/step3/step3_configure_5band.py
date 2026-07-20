#!/usr/bin/env python3
"""Configure the plan-03 Step-3 chain on a headless sdrangelsrv over REST:
N UdmaBufInput device sets (one per band), each with its own SpectrumVis
-> spectrum websocket server (WSSpectrum). Generalizes
step3_configure_spectrum.py (Step 1, one device set) to N device sets for
the Step-3 multi-band stand-up (1 fast + 4 slow in the canonical use).
Still NO RemoteSink / RemoteInput anywhere -- the per-device-set spectrum
websocket remains the only network egress this script wires up.

Device set index i (0-based) is assigned by creation order: this script
adds device sets in the order --band arguments are given, so the first
--band becomes device set 0, the second device set 1, etc.

Per device set i:
    POST   /sdrangel/deviceset?direction=0
    PUT    /sdrangel/deviceset/{i}/device            {"hwType":"UdmaBufInput","direction":0}
    PATCH  /sdrangel/deviceset/{i}/device/settings    udmaBufInputSettings.fileName = <ring>
    POST   /sdrangel/deviceset/{i}/device/run
    GET    /sdrangel/deviceset/{i}/device/report      (readback source sampleRate/centerFrequency)
    PATCH  /sdrangel/deviceset/{i}/spectrum/settings   fftSize/fftWindow/averagingMode/linear/
                                                        wsSpectrumAddress/wsSpectrumPort=<distinct port>
    POST   /sdrangel/deviceset/{i}/spectrum/server
    GET    /sdrangel/deviceset/{i}/spectrum/server     (readback run/listeningPort/clients)

*** Port gotcha (plan-03 Step 3 recon): wsSpectrumPort DEFAULTS to 8887 for
EVERY device set. Each --band MUST carry a distinct port= or the second
spectrum/server start collides with the first. ***

Usage:
    python3 step3_configure_5band.py \\
        --band "ring=/abs/path/rf_wb.udma,port=8887,label=RF Wideband,fft_size=32768" \\
        --band "ring=/abs/path/l1.udma,port=8888,label=GPS L1" \\
        --band "ring=/abs/path/l2c.udma,port=8889,label=GPS L2C" \\
        --band "ring=/abs/path/l5.udma,port=8890,label=GPS L5" \\
        --band "ring=/abs/path/iridium.udma,port=8891,label=Iridium" \\
        [--api http://127.0.0.1:8091] [--ws-address 127.0.0.1] \\
        [--fps-period-ms 50]

Each --band is a comma-separated key=value list:
    ring=<path>       required -- absolute path to the mmap ring file
    port=<int>        required -- distinct wsSpectrumPort for this device set
    label=<text>       optional -- human label (default: the ring path)
    fft_size=<int>     optional -- per-band FFT size (default: 4096)

plan-03 Step 3b: --fps-period-ms (default 50, i.e. 20 fps) is PATCHed into
every device set's spectrum/settings as fpsPeriodMs. This throttles the
headless WSSpectrum websocket push (sdrbase/dsp/spectrumvis.cpp FORK PATCH,
plan-03 Step 3b) -- Step 3's finding was an unthrottled push at ~4
bytes/sample, the same order as raw IQ. wsSpectrum still only opens the
per-band spectrum port; no IQ path is touched. Pass 0 to restore the old
unthrottled (one frame per completed FFT) behavior for comparison.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

# FFTWindow::Function enum (sdrbase/dsp/fftwindow.h) -- Hanning is index 4
# and is also SpectrumSettings::resetToDefaults()'s default.
FFT_WINDOW_HANNING = 4

# SpectrumSettings::AveragingMode enum -- AvgModeNone = 0, the default.
AVERAGING_MODE_NONE = 0

DEFAULT_FFT_SIZE = 4096


# 20s (vs the single-band script's 5s): a first-time large FFT engine
# allocation (e.g. the 32768-point wideband FFT) briefly stalls sdrangelsrv's
# single Qt event loop thread -- and the REST handler runs on that same
# thread -- observed once during Step-3 bring-up (a fresh fftSize the
# process hadn't planned yet). Not a hang; just slower than the single-band
# case's small, already-warm FFT sizes.
REST_TIMEOUT_S = 20


def rest(method, url, body=None):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=REST_TIMEOUT_S) as resp:
            raw = resp.read()
            return resp.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            return exc.code, json.loads(raw)
        except json.JSONDecodeError:
            return exc.code, {"raw": raw.decode("utf-8", "replace")}


def step(label, method, url, body=None, expect=(200, 202)):
    # One retry on a bare socket/timeout error (not an HTTP error status --
    # those come back as a normal (code, payload) pair above and are not
    # retried here) -- covers the one-time FFT-engine-allocation stall.
    for attempt in (1, 2):
        try:
            status, payload = rest(method, url, body)
            break
        except (TimeoutError, OSError) as exc:
            if attempt == 2:
                raise SystemExit(f"step failed: {label}: {method} {url} -> {exc!r}")
            print(f"[WARN] {label}: {method} {url} -> {exc!r} -- retrying once")
            time.sleep(2.0)
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

    Unlike step()/rest() above, this catches (TimeoutError, OSError)
    directly rather than relying on step()'s one-shot retry: a transient
    connection hiccup during the poll window should just count as "not
    ready yet" and be retried on the next iteration, not consume the
    single retry step() reserves for its own large-FFT-allocation stall.
    """
    deadline = time.monotonic() + timeout_s
    last_count = None
    while time.monotonic() < deadline:
        try:
            status, payload = rest("GET", f"{base}/devicesets")
        except (TimeoutError, OSError):
            status, payload = None, {}
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


def parse_band_spec(spec: str) -> dict:
    fields = {}
    for part in spec.split(","):
        if "=" not in part:
            raise SystemExit(f"bad --band field (expected key=value): {part!r} in {spec!r}")
        key, _, value = part.partition("=")
        fields[key.strip()] = value.strip()
    if "ring" not in fields or "port" not in fields:
        raise SystemExit(f"--band spec missing required ring=/port=: {spec!r}")
    fields["port"] = int(fields["port"])
    fields["fft_size"] = int(fields.get("fft_size", DEFAULT_FFT_SIZE))
    fields.setdefault("label", fields["ring"])
    return fields


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--api", default="http://127.0.0.1:8091")
    parser.add_argument(
        "--band",
        action="append",
        dest="bands",
        required=True,
        help="ring=<path>,port=<int>[,label=<text>][,fft_size=<int>] -- repeat once "
        "per device set, in the order device sets should be created",
    )
    parser.add_argument("--ws-address", default="127.0.0.1")
    parser.add_argument(
        "--fps-period-ms",
        type=int,
        default=50,
        help="spectrum/settings fpsPeriodMs PATCHed to every device set "
        "(plan-03 Step 3b websocket throttle; default 50 = 20 fps; 0 = "
        "unthrottled, one frame per completed FFT, the pre-Step-3b behavior)",
    )
    parser.add_argument("--deviceset-ready-timeout", type=float, default=30.0,
        help="bounded wait (seconds) per device set for devicesetcount to confirm "
        "DeviceSet creation before PUT (plan-05 Step 5)")
    args = parser.parse_args()

    bands = [parse_band_spec(s) for s in args.bands]
    ports = [b["port"] for b in bands]
    if len(set(ports)) != len(ports):
        raise SystemExit(f"--band ports must be distinct (Port gotcha): {ports}")

    base = args.api.rstrip("/") + "/sdrangel"

    # --- Reset to a clean, known state (once, before any device set exists) ---
    for _ in range(8):
        status, _ = rest("DELETE", f"{base}/deviceset")
        if status == 404:
            break
        print(f"[INFO] cleared a prior device set (status {status})")
        time.sleep(0.2)

    results = []
    for i, band in enumerate(bands):
        print(f"\n=== device set {i}: {band['label']} (ws port {band['port']}) ===")
        status, payload = rest("GET", f"{base}/devicesets")
        baseline_count = payload.get("devicesetcount", 0) if status == 200 else 0

        step(f"add device set {i} (Rx)", "POST", f"{base}/deviceset?direction=0")
        wait_for_deviceset_ready(base, baseline_count + 1, timeout_s=args.deviceset_ready_timeout)
        step(
            f"select UdmaBufInput on device set {i}",
            "PUT",
            f"{base}/deviceset/{i}/device",
            {"hwType": "UdmaBufInput", "direction": 0},
        )
        step(
            f"point device set {i} UdmaBufInput at its ring file",
            "PATCH",
            f"{base}/deviceset/{i}/device/settings",
            {
                "deviceHwType": "UdmaBufInput",
                "direction": 0,
                "udmaBufInputSettings": {"fileName": band["ring"]},
            },
        )
        step(f"start device set {i} engine", "POST", f"{base}/deviceset/{i}/device/run", {})
        time.sleep(1.0)
        report = step(
            f"read back device set {i} report", "GET", f"{base}/deviceset/{i}/device/report"
        )
        udma_report = report.get("udmaBufInputReport", {})
        source_rate = udma_report.get("sampleRate")
        source_cf = udma_report.get("centerFrequency")
        print(f"    source sampleRate={source_rate} centerFrequency={source_cf}")

        # *** Port gotcha: wsSpectrumPort defaults to 8887 for every device
        # set -- always PATCH a distinct port explicitly. ***
        step(
            f"configure device set {i} spectrum FFT/window/averaging + ws bind",
            "PATCH",
            f"{base}/deviceset/{i}/spectrum/settings",
            {
                "fftSize": band["fft_size"],
                "fftWindow": FFT_WINDOW_HANNING,
                "averagingMode": AVERAGING_MODE_NONE,
                "linear": 0,
                "wsSpectrumAddress": args.ws_address,
                "wsSpectrumPort": band["port"],
                "fpsPeriodMs": args.fps_period_ms,
            },
        )
        settings_readback = step(
            f"read back device set {i} spectrum settings",
            "GET",
            f"{base}/deviceset/{i}/spectrum/settings",
        )

        step(
            f"start device set {i} spectrum websocket server",
            "POST",
            f"{base}/deviceset/{i}/spectrum/server",
        )
        time.sleep(0.3)
        server_status = step(
            f"read back device set {i} spectrum server status",
            "GET",
            f"{base}/deviceset/{i}/spectrum/server",
        )
        print(
            f"    run={server_status.get('run')} "
            f"listeningAddress={server_status.get('listeningAddress')} "
            f"listeningPort={server_status.get('listeningPort')} "
            f"clients={server_status.get('clients')}"
        )

        results.append(
            {
                "index": i,
                "label": band["label"],
                "ring": band["ring"],
                "requestedPort": band["port"],
                "sourceSampleRate": source_rate,
                "sourceCenterFrequency": source_cf,
                "fftSize": settings_readback.get("fftSize"),
                "fpsPeriodMs": settings_readback.get("fpsPeriodMs"),
                "wsAddress": server_status.get("listeningAddress"),
                "wsPort": server_status.get("listeningPort"),
                "run": server_status.get("run"),
            }
        )

    print(f"\n=== Step-3 (plan-03) {len(bands)}-band configuration summary ===")
    ok = True
    for r in results:
        row_ok = bool(
            r["run"]
            and r["sourceSampleRate"]
            and r["sourceCenterFrequency"]
            and r["wsPort"] == r["requestedPort"]
            and r["fpsPeriodMs"] == args.fps_period_ms
        )
        ok = ok and row_ok
        print(
            f"[{'OK' if row_ok else 'FAIL'}] device set {r['index']} \"{r['label']}\": "
            f"ring={r['ring']} sampleRate={r['sourceSampleRate']} "
            f"centerFrequency={r['sourceCenterFrequency']} fftSize={r['fftSize']} "
            f"fpsPeriodMs={r['fpsPeriodMs']} (requested {args.fps_period_ms}) "
            f"ws={r['wsAddress']}:{r['wsPort']} (requested {r['requestedPort']}) run={r['run']}"
        )
    print("No RemoteSink / RemoteInput channel was created on any device set.")

    if not ok:
        print("WARN: one or more device sets failed verification -- inspect manually.")
        sys.exit(1)


if __name__ == "__main__":
    main()
