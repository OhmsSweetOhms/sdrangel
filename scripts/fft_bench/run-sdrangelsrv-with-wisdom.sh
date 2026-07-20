#!/bin/bash
# Reference bundle launcher (plan-05 Step 4): resolves a bundle-relative
# FFT_BENCH wisdom file next to this script and passes it to sdrangelsrv via
# the existing --fftwf-wisdom option, so a valid-wisdom bundle boots
# optimized with no code change to sdrangelsrv itself.
#
# Expected bundle-relative layout (this script is meant to live at
# <bundle>/bin/ or <bundle>/, next to the sdrangelsrv binary -- see
# plan-04-step-4.1-build-brief.md for the ARM64 bundle's actual {bin,lib}
# layout, which this convention slots into without changing it):
#
#   <bundle>/bin/sdrangelsrv
#   <bundle>/bin/run-sdrangelsrv-with-wisdom.sh   (this script)
#   <bundle>/etc/fft_bench/cof1024.wisdom          (FFT_BENCH release wisdom)
#   <bundle>/etc/fft_bench/cof1024.wisdom.receipt.json
#
# Wisdom is OPTIONAL by design (plan-05 "settled" runtime policy: the
# service must remain useful when wisdom is absent). If the file is
# missing, this script logs a warning and starts sdrangelsrv anyway --
# FFTWEngine::configure() falls back to FFTW_ESTIMATE on its own
# (sdrbase/dsp/fftwengine.cpp) and stays responsive; it does not need this
# script's help to do that. This script's only job is to make the launch
# state visible and pass the flag through when there is something to pass.
#
# Usage:
#   run-sdrangelsrv-with-wisdom.sh [--wisdom <file>] [sdrangelsrv args...]
#
# --wisdom overrides the bundle-relative default. Anything else is passed
# straight through to sdrangelsrv.

set -euo pipefail

# Resolve the bundle root relative to THIS script's real location, never a
# host-absolute path baked in at build time -- the bundle must stay
# relocatable (plan-05 Step 4 acceptance).
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
BUNDLE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

WISDOM_PATH="$BUNDLE_ROOT/etc/fft_bench/cof1024.wisdom"
ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --wisdom)
            WISDOM_PATH="$2"
            shift 2
            ;;
        *)
            ARGS+=("$1")
            shift
            ;;
    esac
done

SDRANGELSRV="$BUNDLE_ROOT/bin/sdrangelsrv"
LOADER="$BUNDLE_ROOT/lib/ld-linux-aarch64.so.1"

if [ ! -x "$SDRANGELSRV" ]; then
    echo "run-sdrangelsrv-with-wisdom: FATAL: sdrangelsrv not found at $SDRANGELSRV" >&2
    exit 1
fi

FFTW_FLAG=()
if [ -s "$WISDOM_PATH" ]; then
    echo "run-sdrangelsrv-with-wisdom: FFTW wisdom FOUND: $WISDOM_PATH -- passing --fftwf-wisdom" >&2
    RECEIPT="$WISDOM_PATH.receipt.json"
    if [ -f "$RECEIPT" ]; then
        echo "run-sdrangelsrv-with-wisdom: receipt: $RECEIPT" >&2
    else
        echo "run-sdrangelsrv-with-wisdom: WARNING: no receipt sidecar next to $WISDOM_PATH (unprovenanced wisdom)" >&2
    fi
    FFTW_FLAG=(--fftwf-wisdom "$WISDOM_PATH")
else
    echo "run-sdrangelsrv-with-wisdom: FFTW wisdom MISSING or empty at $WISDOM_PATH -- starting WITHOUT it." >&2
    echo "run-sdrangelsrv-with-wisdom: sdrangelsrv will use ESTIMATE fallback (bounded, responsive, unoptimized)." >&2
    echo "run-sdrangelsrv-with-wisdom: run FFT_BENCH (scripts/fft_bench/fft_bench.py) on this target to provision it." >&2
fi

# ARM64 bundle direct-exec gotcha (plan-04-step-4.1-build-brief.md): the
# kernel reads PT_INTERP literally and can't expand $ORIGIN, so a bare exec
# of the bundled binary fails. Launch through the bundled loader explicitly
# when present; fall back to a plain exec otherwise (e.g. this same bundle
# convention exercised on x86 during development/test, where no bundled
# loader is shipped and the host's own loader is already correct).
if [ -x "$LOADER" ]; then
    exec "$LOADER" --library-path "$BUNDLE_ROOT/lib" "$SDRANGELSRV" "${FFTW_FLAG[@]}" "${ARGS[@]}"
else
    exec "$SDRANGELSRV" "${FFTW_FLAG[@]}" "${ARGS[@]}"
fi
