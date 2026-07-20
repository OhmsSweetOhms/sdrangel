#!/usr/bin/env python3
"""FFT_BENCH -- explicit, supervised FFTW wisdom provisioning (plan-05 Step 3).

FFT_BENCH is the project-facing name for the offline operation that runs
FFTW's FFTW_PATIENT search and produces reusable wisdom. It is deliberately
NOT something sdrangelsrv ever runs on its own: normal server startup and
device-set creation only ever attempt a bounded FFTW_PATIENT|FFTW_WISDOM_ONLY
lookup, falling back to FFTW_ESTIMATE when no matching plan exists (see
sdrbase/dsp/fftwengine.cpp). FFT_BENCH is the tool an operator runs once, by
hand, on the actual deployed target, to produce the wisdom that lookup uses.

Root-cause context: findings-2026-07-20-fft-bench-root-cause.md (thread
cross-cutting/20260711-sdrangel-host-perchannel-sigmf) -- an unrestricted
FFTW_PATIENT benchmark running synchronously on the REST thread during
DeviceSet construction pinned a CPU and made the server unresponsive on a
slow ARM64 target. This tool exists so that expensive search happens once,
explicitly, outside the server control plane.

What this script does, in order:
  1. Print a START banner: signatures requested, deadline, exact command.
  2. Run the target's own `fftwf-wisdom` binary against a TEMPORARY output
     file, emitting an elapsed-time heartbeat at least every
     --heartbeat-seconds (default 10s) while it runs.
  3. Enforce a hard external deadline (--deadline-seconds). On timeout or on
     SIGINT/SIGTERM, the child is killed, the temporary file is discarded,
     and the last known-good release wisdom (if any) is left untouched.
  4. On successful completion, VALIDATE the generated wisdom against the
     exact bundled libfftw3f before it is ever treated as good: import it
     and confirm FFTW_PATIENT|FFTW_WISDOM_ONLY resolves a plan for every
     requested 1-D complex signature, via ctypes against that exact .so
     (not merely "the tool exited 0").
  5. Only after validation passes does it atomically replace the release
     wisdom file (os.replace(), same filesystem as the temp file -> atomic)
     and write a JSON receipt describing the run.

A killed, failed, or timed-out run therefore can never leave a zero-byte or
partial release wisdom file: the release path is only ever touched by the
single atomic replace at the very end of a PASS.

Packaging note (bundle scope -- Step 6, not this hop): on the ARM64 board
bundle, point --wisdom-tool and --libfftw3f at the bundled binary/library
closure described in plan-04-step-4.1-build-brief.md (bundle-relative
`bin/fftwf-wisdom`, `lib/libfftw3f.so*`), NOT the host's own copies. The
supervision logic in this script is target-agnostic: it has been exercised
here against the native x86 `fftwf-wisdom` and libfftw3f (this worktree
build host), and works unchanged against a cross-built AArch64 pair as long
as it runs ON that target (ctypes.CDLL cannot load a foreign-arch .so).

Usage:
    python3 fft_bench.py --output /path/to/release/cof1024.wisdom \\
        [--signature cof1024 [--signature ...]] \\
        [--wisdom-tool fftwf-wisdom] [--libfftw3f libfftw3f.so.3] \\
        [--deadline-seconds 3600] [--heartbeat-seconds 10] \\
        [--receipt /path/to/release/cof1024.receipt.json]

Re-running FFT_BENCH is always an explicit, separate operator action --
nothing in sdrangelsrv, MainServer, DSPEngine, or FFTFactory ever invokes
this script; starting or switching devices never triggers it.
"""

import argparse
import ctypes
import ctypes.util
import hashlib
import json
import os
import platform
import re
import signal
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone

# FFTW planner-flag bit values (fftw3.h) -- duplicated here rather than
# parsed from the header so this script has no build-time dependency on the
# SDRangel source tree; verified against /usr/include/fftw3.h on the plan-05
# build host (FFTW 3.3.8).
FFTW_PATIENT = 1 << 5
FFTW_ESTIMATE = 1 << 6
FFTW_WISDOM_ONLY = 1 << 21
FFTW_FORWARD = -1
FFTW_BACKWARD = 1

# <type><inplace><direction><geometry> per `fftwf-wisdom --help`. This tool
# and the runtime engine (FFTWEngine::configure(), sdrbase/dsp/fftwengine.cpp)
# only ever plan 1-D complex transforms (fftwf_plan_dft_1d) -- FFTFactory
# never requests real or multi-dimensional transforms -- so validation is
# scoped to that subset. Other signatures can still be *requested* (passed
# through to fftwf-wisdom for generation) but are not ctypes-validated here.
SIGNATURE_RE = re.compile(r"^c(?P<inplace>[io])(?P<direction>[fb])(?P<n>\d+)$")


def log(msg):
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    print(f"[{ts}] {msg}", flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_cpu_model():
    try:
        with open("/proc/cpuinfo") as f:
            text = f.read()
    except OSError:
        return platform.processor() or platform.machine()

    for key in ("model name", "Model", "Hardware", "cpu model"):
        m = re.search(rf"^{re.escape(key)}\s*:\s*(.+)$", text, re.MULTILINE)
        if m:
            return m.group(1).strip()

    # ARM /proc/cpuinfo often only carries "CPU part" + "CPU implementer".
    part = re.search(r"^CPU part\s*:\s*(.+)$", text, re.MULTILINE)
    impl = re.search(r"^CPU implementer\s*:\s*(.+)$", text, re.MULTILINE)
    if part or impl:
        return f"implementer={impl.group(1).strip() if impl else '?'} part={part.group(1).strip() if part else '?'}"

    return platform.processor() or platform.machine()


def resolve_libfftw3f(explicit_path):
    if explicit_path:
        return explicit_path

    # ctypes.util.find_library() returns a bare soname on Linux (e.g.
    # "libfftw3f.so.3"), not a filesystem path -- fine for dlopen but not
    # for sha256'ing the exact library identity into the receipt, so resolve
    # a real path via ldconfig's cache first and fall back to the bare name
    # (still loadable, just not hashable) if ldconfig isn't available.
    try:
        out = subprocess.run(["ldconfig", "-p"], capture_output=True, text=True, timeout=10).stdout
        for line in out.splitlines():
            if "libfftw3f.so" in line and "=>" in line:
                candidate = line.split("=>", 1)[1].strip()
                if os.path.exists(candidate):
                    return candidate
    except (OSError, subprocess.SubprocessError):
        pass

    found = ctypes.util.find_library("fftw3f")
    if not found:
        raise SystemExit("FFT_BENCH FAIL: could not locate libfftw3f (pass --libfftw3f explicitly)")
    return found


def get_fftw_version(wisdom_tool):
    try:
        out = subprocess.run(
            [wisdom_tool, "--version"], capture_output=True, text=True, timeout=10
        ).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        return f"unknown (--version failed: {exc!r})"
    m = re.search(r"FFTW version\s+([^\s.]+(?:\.[^\s.]+)*)", out)
    return m.group(1) if m else out.strip().splitlines()[0] if out.strip() else "unknown"


def parse_signature(sig):
    m = SIGNATURE_RE.match(sig)
    if not m:
        return None
    return {
        "n": int(m.group("n")),
        "inplace": m.group("inplace") == "i",
        "direction": FFTW_FORWARD if m.group("direction") == "f" else FFTW_BACKWARD,
    }


def validate_wisdom(libfftw3f_path, wisdom_path, signatures):
    """Loads libfftw3f_path via ctypes, imports wisdom_path, and confirms
    FFTW_PATIENT|FFTW_WISDOM_ONLY resolves a plan for every 1-D complex
    signature in `signatures` -- the exact runtime lookup FFTWEngine performs.
    Returns (ok: bool, detail: str)."""
    lib = ctypes.CDLL(libfftw3f_path)

    lib.fftwf_malloc.restype = ctypes.c_void_p
    lib.fftwf_malloc.argtypes = [ctypes.c_size_t]
    lib.fftwf_free.argtypes = [ctypes.c_void_p]
    lib.fftwf_plan_dft_1d.restype = ctypes.c_void_p
    lib.fftwf_plan_dft_1d.argtypes = [
        ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_uint
    ]
    lib.fftwf_destroy_plan.argtypes = [ctypes.c_void_p]
    lib.fftwf_import_wisdom_from_filename.restype = ctypes.c_int
    lib.fftwf_import_wisdom_from_filename.argtypes = [ctypes.c_char_p]
    lib.fftwf_forget_wisdom.argtypes = []

    lib.fftwf_forget_wisdom()
    rc = lib.fftwf_import_wisdom_from_filename(wisdom_path.encode("utf-8"))
    if rc == 0:
        return False, f"fftwf_import_wisdom_from_filename('{wisdom_path}') failed (rc=0)"

    checked = []
    for sig in signatures:
        parsed = parse_signature(sig)
        if parsed is None:
            checked.append(f"{sig}: SKIPPED (not a 1-D complex signature; not runtime-validated)")
            continue

        n = parsed["n"]
        complex_bytes = 8 * n  # fftwf_complex = 2 x float
        buf_in = lib.fftwf_malloc(complex_bytes)
        buf_out = buf_in if parsed["inplace"] else lib.fftwf_malloc(complex_bytes)
        if not buf_in or not buf_out:
            return False, f"{sig}: fftwf_malloc failed"

        plan = lib.fftwf_plan_dft_1d(
            n, buf_in, buf_out, parsed["direction"], FFTW_PATIENT | FFTW_WISDOM_ONLY
        )
        ok = bool(plan)
        if plan:
            lib.fftwf_destroy_plan(plan)
        lib.fftwf_free(buf_in)
        if not parsed["inplace"]:
            lib.fftwf_free(buf_out)

        if not ok:
            return False, f"{sig}: FFTW_WISDOM_ONLY found no matching plan after import"
        checked.append(f"{sig}: OK (FFTW_WISDOM_ONLY resolved a plan)")

    lib.fftwf_forget_wisdom()
    return True, "; ".join(checked)


def run_fft_bench(args):
    start_dt = datetime.now(timezone.utc)
    start_monotonic = time.monotonic()

    output_dir = os.path.dirname(os.path.abspath(args.output)) or "."
    os.makedirs(output_dir, exist_ok=True)
    tmp_fd, tmp_path = tempfile.mkstemp(
        prefix=".fft_bench-", suffix=".wisdom.tmp", dir=output_dir
    )
    os.close(tmp_fd)
    os.remove(tmp_path)  # fftwf-wisdom -o must create it fresh

    command = [args.wisdom_tool, "-v", "-o", tmp_path] + list(args.signature)

    log(f"FFT_BENCH START signatures={list(args.signature)} "
        f"deadline_s={args.deadline_seconds} heartbeat_s={args.heartbeat_seconds}")
    log(f"FFT_BENCH command: {' '.join(command)}")

    receipt = {
        "tool": "FFT_BENCH",
        "cpu_model": read_cpu_model(),
        "host_platform": platform.platform(),
        "fftw_tool_version": get_fftw_version(args.wisdom_tool),
        "wisdom_tool_path": os.path.abspath(args.wisdom_tool) if os.path.sep in args.wisdom_tool
            or os.path.exists(args.wisdom_tool) else args.wisdom_tool,
        "requested_signatures": list(args.signature),
        "exact_command": command,
        "deadline_seconds": args.deadline_seconds,
        "start_time": start_dt.isoformat(),
    }

    def cleanup_tmp():
        if os.path.exists(tmp_path):
            os.remove(tmp_path)

    proc = None
    # The signal handler only sets a flag. It deliberately does NOT touch
    # `proc` itself: fftwf-wisdom is a shell script on some targets, and a
    # shell blocked in a foreground child (e.g. a benchmark subprocess of
    # its own) can defer a plain SIGTERM sent to just the shell's own PID
    # until that foreground child exits naturally -- observed directly while
    # building this tool (a SIGTERM delivered to the direct child during a
    # simulated slow run did not take effect until the underlying `sleep`
    # completed on its own, ~20s later, even though the signal handler ran
    # immediately). All actual process-group termination happens from the
    # main loop below via terminate_process_group(), never from inside the
    # handler -- calling back into subprocess/os internals from a raw signal
    # handler is also a known source of reentrancy trouble.
    interrupted = {"flag": False, "signum": None}

    def handle_signal(signum, _frame):
        interrupted["flag"] = True
        interrupted["signum"] = signum

    old_sigint = signal.signal(signal.SIGINT, handle_signal)
    old_sigterm = signal.signal(signal.SIGTERM, handle_signal)

    def terminate_process_group(grace_seconds=2.0):
        """Kills the benchmark's entire process group, not just the direct
        child -- required so a wisdom-tool wrapper (shell script, etc.) can't
        outlive a plain SIGTERM to itself while a grandchild keeps running
        past the deadline/interrupt. start_new_session=True at Popen() makes
        proc.pid the process-group id."""
        if proc is None or proc.poll() is not None:
            return
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        deadline = time.monotonic() + grace_seconds
        while proc.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)
        if proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        proc.wait()

    try:
        proc = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            start_new_session=True,
        )

        last_heartbeat = start_monotonic
        result = None

        while True:
            try:
                proc.wait(timeout=0.5)
                break
            except subprocess.TimeoutExpired:
                pass

            now = time.monotonic()
            elapsed = now - start_monotonic

            if interrupted["flag"]:
                log(f"FFT_BENCH received signal {interrupted['signum']}; "
                    "terminating benchmark and preserving last known-good wisdom")
                terminate_process_group()
                result = "FAIL"
                receipt["fail_reason"] = "interrupted (SIGINT/SIGTERM)"
                break

            if elapsed >= args.deadline_seconds:
                log(f"FFT_BENCH TIMEOUT elapsed={elapsed:.1f}s >= deadline={args.deadline_seconds}s -- killing benchmark")
                terminate_process_group()
                result = "TIMEOUT"
                break

            if now - last_heartbeat >= args.heartbeat_seconds:
                log(f"FFT_BENCH HEARTBEAT elapsed={elapsed:.1f}s (running)")
                last_heartbeat = now

        if result is None:
            result = "PASS" if proc.returncode == 0 else "FAIL"
            if result == "FAIL":
                receipt["fail_reason"] = f"fftwf-wisdom exited {proc.returncode}"
                if proc.stdout:
                    receipt["tool_output_tail"] = proc.stdout.read()[-2000:]

        if result == "PASS":
            if not os.path.exists(tmp_path) or os.path.getsize(tmp_path) == 0:
                result = "FAIL"
                receipt["fail_reason"] = "fftwf-wisdom exited 0 but produced no/empty output"

        if result == "PASS":
            libfftw3f_path = resolve_libfftw3f(args.libfftw3f)
            ok, detail = validate_wisdom(libfftw3f_path, tmp_path, args.signature)
            receipt["libfftw3f_path"] = libfftw3f_path
            try:
                receipt["libfftw3f_sha256"] = sha256_file(libfftw3f_path)
            except OSError:
                receipt["libfftw3f_sha256"] = None  # bare soname resolved by the loader, not a filesystem path
            receipt["validation_detail"] = detail
            if not ok:
                result = "FAIL"
                receipt["fail_reason"] = f"post-generation validation failed: {detail}"

        end_dt = datetime.now(timezone.utc)
        elapsed_total = time.monotonic() - start_monotonic
        receipt["end_time"] = end_dt.isoformat()
        receipt["elapsed_seconds"] = round(elapsed_total, 3)
        receipt["result"] = result

        if result == "PASS":
            receipt["wisdom_path"] = os.path.abspath(args.output)
            receipt["wisdom_sha256"] = sha256_file(tmp_path)
            os.chmod(tmp_path, 0o644)
            os.replace(tmp_path, args.output)  # atomic on the same filesystem
            log(f"FFT_BENCH PASS elapsed={elapsed_total:.1f}s wisdom={args.output} sha256={receipt['wisdom_sha256']}")
        else:
            cleanup_tmp()
            log(f"FFT_BENCH {result} elapsed={elapsed_total:.1f}s -- release wisdom untouched "
                f"({args.output if os.path.exists(args.output) else 'no prior release file'})")

        write_receipt(args.receipt or (args.output + ".receipt.json"), receipt)
        return 0 if result == "PASS" else 1

    finally:
        signal.signal(signal.SIGINT, old_sigint)
        signal.signal(signal.SIGTERM, old_sigterm)
        cleanup_tmp()


def write_receipt(path, receipt):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(receipt, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)
    log(f"FFT_BENCH receipt: {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", required=True, help="release wisdom file path (only replaced atomically on PASS)")
    parser.add_argument("--signature", action="append", default=None,
        help="fftwf-wisdom size signature, e.g. cof1024 (repeatable; default: cof1024)")
    parser.add_argument("--wisdom-tool", default="fftwf-wisdom", help="path to the target's fftwf-wisdom binary")
    parser.add_argument("--libfftw3f", default=None,
        help="path to the exact libfftw3f shared library to validate against (default: resolved via ldconfig/PATH)")
    parser.add_argument("--deadline-seconds", type=int, default=3600, help="hard external deadline (default: 3600)")
    parser.add_argument("--heartbeat-seconds", type=int, default=10, help="heartbeat interval (default: 10, plan minimum)")
    parser.add_argument("--receipt", default=None, help="JSON receipt path (default: <output>.receipt.json)")
    args = parser.parse_args()

    if not args.signature:
        args.signature = ["cof1024"]

    if args.heartbeat_seconds > 10:
        parser.error("--heartbeat-seconds must be <= 10 to satisfy the plan-05 "
                      "\"at least every 10s\" operator-visibility acceptance")

    return run_fft_bench(args)


if __name__ == "__main__":
    sys.exit(main())
