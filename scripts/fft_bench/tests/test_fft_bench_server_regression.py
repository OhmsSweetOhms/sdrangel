#!/usr/bin/env python3
"""FFT_BENCH server-level regression (plan-05 Step 1).

Drives a real `sdrangelsrv` process through the exact asynchronous REST
sequence that exposed the root cause (findings-2026-07-20-fft-bench-root-
cause.md): `POST /sdrangel/deviceset` returns 202 once creation is merely
QUEUED, and DeviceSet -> SpectrumVis -> FFTFactory::getEngine() ->
FFTWEngine::configure() construction happens afterward, on the server's
single event-loop thread, which also serves REST. Pre-fix, that
construction ran an unrestricted FFTW_PATIENT benchmark and made the server
unresponsive; post-fix (sdrbase/dsp/fftwengine.cpp, plan-05 Step 2) it is
always bounded.

Three scenarios, each launching a fresh server:
  1. no wisdom configured            -> expect ESTIMATE fallback, bounded
  2. a real cof1024 PATIENT wisdom   -> expect OptimizedWisdom, bounded
  3. a corrupt/garbage wisdom file   -> expect ESTIMATE fallback, bounded
     (never a crash or wedge -- "the service must remain useful when wisdom
     is absent/invalid", plan-05 "Runtime policy -- settled")

For each scenario this script:
  - starts sdrangelsrv with --fftwf-wisdom pointed at the scenario's file
    (or omitted for scenario 1),
  - waits for the REST API to come up,
  - issues POST /sdrangel/deviceset and times how long the very next GET
    /sdrangel/devicesets takes to report devicesetcount == 1 (the exact
    request order the pre-fix bug misattributed to the following PUT),
  - asserts that round-trip is bounded (well under the wedge timescale the
    root-cause findings measured),
  - greps the server's own stdout/stderr for the diagnostic FFTWEngine
    logs (sdrbase/dsp/fftwengine.cpp) and asserts the expected planner mode
    string appears for the 1024-point forward transform (the SpectrumVis
    default), distinguishing ready/optimized from ready/estimate-fallback
    exactly as Step 1's acceptance criteria require.

Usage:
    python3 test_fft_bench_server_regression.py --binary /path/to/sdrangelsrv \\
        [--api-port 18093] [--bounded-seconds 5.0]

Exit code 0 on all three scenarios passing, non-zero otherwise. Not wired
into CTest (it needs a real sdrangelsrv binary + free port, and spawns a
real process for tens of seconds) -- run by hand or from CI as an explicit
integration step.
"""

import argparse
import ctypes
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

FFTW_PATIENT = 1 << 5


def rest(method, url, body=None, timeout=5):
    data = json.dumps(body).encode("utf-8") if body is not None else None
    headers = {"Content-Type": "application/json"} if body is not None else {}
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
            return resp.status, (json.loads(raw) if raw else {})
    except urllib.error.HTTPError as exc:
        raw = exc.read()
        try:
            return exc.code, json.loads(raw)
        except json.JSONDecodeError:
            return exc.code, {}
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        return None, {"error": repr(exc)}


def free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def make_cof1024_wisdom(path):
    """Generates a real PATIENT-quality cof1024 wisdom file via the FFTW C
    API directly (no dependency on the fftwf-wisdom CLI being on PATH)."""
    lib = ctypes.CDLL("libfftw3f.so.3")
    lib.fftwf_malloc.restype = ctypes.c_void_p
    lib.fftwf_malloc.argtypes = [ctypes.c_size_t]
    lib.fftwf_plan_dft_1d.restype = ctypes.c_void_p
    lib.fftwf_plan_dft_1d.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_uint]
    lib.fftwf_export_wisdom_to_filename.restype = ctypes.c_int
    lib.fftwf_export_wisdom_to_filename.argtypes = [ctypes.c_char_p]
    lib.fftwf_forget_wisdom()

    n = 1024
    buf_in = lib.fftwf_malloc(8 * n)
    buf_out = lib.fftwf_malloc(8 * n)
    plan = lib.fftwf_plan_dft_1d(n, buf_in, buf_out, -1, FFTW_PATIENT)
    assert plan, "fftwf_plan_dft_1d(FFTW_PATIENT) unexpectedly returned NULL"
    ok = lib.fftwf_export_wisdom_to_filename(str(path).encode("utf-8"))
    assert ok, "fftwf_export_wisdom_to_filename failed"
    lib.fftwf_forget_wisdom()


class Server:
    def __init__(self, binary, api_port, wisdom_path, log_path):
        self.binary = binary
        self.api_port = api_port
        self.wisdom_path = wisdom_path
        self.log_path = log_path
        self.proc = None
        self.log_file = None

    def __enter__(self):
        cmd = [self.binary, "-p", str(self.api_port), "--scratch"]
        if self.wisdom_path:
            cmd += ["--fftwf-wisdom", str(self.wisdom_path)]
        self.log_file = open(self.log_path, "w")
        env = dict(os.environ)
        self.proc = subprocess.Popen(
            cmd, stdout=self.log_file, stderr=subprocess.STDOUT, env=env, start_new_session=True
        )
        self._wait_for_rest_up(timeout_s=20.0)
        return self

    def _wait_for_rest_up(self, timeout_s):
        deadline = time.monotonic() + timeout_s
        base = f"http://127.0.0.1:{self.api_port}/sdrangel"
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"sdrangelsrv exited early (rc={self.proc.returncode}); see {self.log_path}")
            status, _ = rest("GET", f"{base}/devicesets", timeout=1)
            if status == 200:
                return
            time.sleep(0.2)
        raise RuntimeError(f"sdrangelsrv REST API did not come up within {timeout_s}s; see {self.log_path}")

    def __exit__(self, exc_type, exc, tb):
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
                self.proc.wait(timeout=5)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        if self.log_file:
            self.log_file.close()


def log_contains(log_path, needle):
    with open(log_path, "r", errors="replace") as f:
        return needle in f.read()


def run_scenario(name, binary, api_port, wisdom_path, log_path, bounded_seconds, expect_needle):
    print(f"\n=== scenario: {name} ===")
    with Server(binary, api_port, wisdom_path, log_path) as server:
        base = f"http://127.0.0.1:{api_port}/sdrangel"

        # The exact pre-fix-misattributed sequence: POST /deviceset, then
        # measure how long DeviceSet construction (SpectrumVis -> FFTFactory
        # -> FFTWEngine::configure()) actually takes by polling
        # /devicesets for devicesetcount to reach 1 -- this is Step 5's
        # readiness poll, used here as the regression's timing probe.
        t0 = time.monotonic()
        status, _ = rest("POST", f"{base}/deviceset?direction=0", body={})
        assert status in (200, 202), f"POST /deviceset -> {status}, expected 200/202"

        deadline = t0 + bounded_seconds
        ready = False
        while time.monotonic() < deadline:
            status, payload = rest("GET", f"{base}/devicesets", timeout=1)
            if status == 200 and payload.get("devicesetcount", 0) >= 1:
                ready = True
                break
            time.sleep(0.05)
        elapsed = time.monotonic() - t0

        assert ready, (
            f"DeviceSet creation did not become visible via GET /devicesets within "
            f"{bounded_seconds}s (elapsed={elapsed:.2f}s) -- REST wedged, this is exactly "
            f"the pre-fix POST-then-unresponsive signature"
        )
        print(f"[PASS] DeviceSet ready in {elapsed:.3f}s (bounded < {bounded_seconds}s)")

        # REST must still be responsive for an unrelated request too, not
        # just the one we were polling.
        status, payload = rest("GET", f"{base}/devicesets", timeout=2)
        assert status == 200, f"REST unresponsive after DeviceSet creation: GET /devicesets -> {status}"
        print(f"[PASS] REST remains responsive post-creation (devicesetcount={payload.get('devicesetcount')})")

    assert log_contains(log_path, expect_needle), (
        f"expected diagnostic '{expect_needle}' not found in server log {log_path}"
    )
    print(f"[PASS] server log contains expected diagnostic: {expect_needle!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True, help="path to the built sdrangelsrv")
    parser.add_argument("--api-port", type=int, default=None, help="default: an OS-assigned free port")
    parser.add_argument("--bounded-seconds", type=float, default=5.0,
        help="max time DeviceSet creation may take before this is treated as the wedge (default 5.0)")
    args = parser.parse_args()

    if not os.path.isfile(args.binary) or not os.access(args.binary, os.X_OK):
        raise SystemExit(f"--binary not found or not executable: {args.binary}")

    with tempfile.TemporaryDirectory(prefix="fft_bench_server_regression-") as tmp:
        tmp = os.path.abspath(tmp)
        cof1024_path = os.path.join(tmp, "cof1024.wisdom")
        corrupt_path = os.path.join(tmp, "corrupt.wisdom")
        make_cof1024_wisdom(cof1024_path)
        with open(corrupt_path, "wb") as f:
            f.write(b"this is not a wisdom file\x00\x01\x02garbage")

        scenarios = [
            ("no-wisdom", None, "estimate-fallback"),
            ("valid-cof1024-wisdom", cof1024_path, "optimized-wisdom"),
            ("corrupt-wisdom", corrupt_path, "estimate-fallback"),
        ]

        for i, (name, wisdom_path, expect_needle) in enumerate(scenarios):
            port = args.api_port or free_tcp_port()
            log_path = os.path.join(tmp, f"{name}.log")
            run_scenario(name, args.binary, port, wisdom_path, log_path,
                         args.bounded_seconds, expect_needle)

        print(f"\n=== all {len(scenarios)} scenarios PASS ===")


if __name__ == "__main__":
    main()
