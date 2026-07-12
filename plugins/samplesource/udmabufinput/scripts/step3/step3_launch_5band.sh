#!/usr/bin/env bash
# plan-03 Step 3 launcher: 5 fake UdmaBufInput producers (1 wideband "fast" +
# 4 narrowband "slow") + headless sdrangelsrv + the 5-device-set REST config
# (step3_configure_5band.py). NO RemoteSink/RemoteInput anywhere -- each
# device set's spectrum websocket is the only network egress this stands up.
#
# Run from the sdrangel-udmabufinput repo root:
#   plugins/samplesource/udmabufinput/scripts/step3/step3_launch_5band.sh
#
# Leaves sdrangelsrv and the 5 producers running in the background; PIDs are
# written to codex-handoff/plan-03/temp/5band/pids.txt for the matching
# teardown script (step3_teardown_5band.sh) -- run that when done, per the
# project convention of not leaving background processes running.

set -euo pipefail

if [ ! -f "codex-handoff/plan-02/temp/build-host/sdrangelsrv" ]; then
  echo "ERROR: run this from the sdrangel-udmabufinput repo root" >&2
  exit 1
fi

SCRATCH="codex-handoff/plan-03/temp/5band"
mkdir -p "$SCRATCH"
PIDFILE="$SCRATCH/pids.txt"
: > "$PIDFILE"

PRODUCER="codex-handoff/plan-02/scripts/fake_udmabuf_producer.py"

echo "[1/3] starting 5 fake producers ..."

# fast: RF Wideband -- 20.48 MSPS, wide span, +100 kHz tone, port 8887
python3 "$PRODUCER" \
  --path "$SCRATCH/rf_wb.udma" --pattern tone \
  --sample-rate 20480000 --center-frequency 1575420000 \
  --tone-frequency 100000 --amplitude 16384 \
  --capacity 4194304 --block-samples 4096 --duration 1800 \
  > "$SCRATCH/producer-rf_wb.log" 2>&1 &
echo $! >> "$PIDFILE"

# slow: GPS L1 -- 2.56 MSPS, +50 kHz tone, port 8888
python3 "$PRODUCER" \
  --path "$SCRATCH/l1.udma" --pattern tone \
  --sample-rate 2560000 --center-frequency 1575420000 \
  --tone-frequency 50000 --amplitude 16384 \
  --capacity 262144 --block-samples 1024 --duration 1800 \
  > "$SCRATCH/producer-l1.log" 2>&1 &
echo $! >> "$PIDFILE"

# slow: GPS L2C -- 2.56 MSPS, -100 kHz tone, port 8889
python3 "$PRODUCER" \
  --path "$SCRATCH/l2c.udma" --pattern tone \
  --sample-rate 2560000 --center-frequency 1227600000 \
  --tone-frequency -100000 --amplitude 16384 \
  --capacity 262144 --block-samples 1024 --duration 1800 \
  > "$SCRATCH/producer-l2c.log" 2>&1 &
echo $! >> "$PIDFILE"

# slow: GPS L5 -- 2.56 MSPS, +200 kHz tone, port 8890
python3 "$PRODUCER" \
  --path "$SCRATCH/l5.udma" --pattern tone \
  --sample-rate 2560000 --center-frequency 1176450000 \
  --tone-frequency 200000 --amplitude 16384 \
  --capacity 262144 --block-samples 1024 --duration 1800 \
  > "$SCRATCH/producer-l5.log" 2>&1 &
echo $! >> "$PIDFILE"

# slow: Iridium -- 2.56 MSPS, -200 kHz tone, port 8891
python3 "$PRODUCER" \
  --path "$SCRATCH/iridium.udma" --pattern tone \
  --sample-rate 2560000 --center-frequency 1621250000 \
  --tone-frequency -200000 --amplitude 16384 \
  --capacity 262144 --block-samples 1024 --duration 1800 \
  > "$SCRATCH/producer-iridium.log" 2>&1 &
echo $! >> "$PIDFILE"

sleep 1

echo "[2/3] starting sdrangelsrv (API 8091) ..."
./codex-handoff/plan-02/temp/build-host/sdrangelsrv \
  --scratch --api-address 127.0.0.1 --api-port 8091 \
  > "$SCRATCH/sdrangelsrv.log" 2>&1 &
echo $! >> "$PIDFILE"

# wait for the REST API to come up
for i in $(seq 1 40); do
  if curl -sf "http://127.0.0.1:8091/sdrangel/deviceset" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done

echo "[3/3] configuring 5 device sets over REST (fftSize=32768 for the wideband"
echo "      band to keep its frame rate in the same ballpark as the 4 slow"
echo "      bands at fftSize=4096 -- see step3-5band-evidence.md for why) ..."
python3 plugins/samplesource/udmabufinput/scripts/step3/step3_configure_5band.py \
  --band "ring=$(pwd)/$SCRATCH/rf_wb.udma,port=8887,label=RF Wideband,fft_size=32768" \
  --band "ring=$(pwd)/$SCRATCH/l1.udma,port=8888,label=GPS L1,fft_size=4096" \
  --band "ring=$(pwd)/$SCRATCH/l2c.udma,port=8889,label=GPS L2C,fft_size=4096" \
  --band "ring=$(pwd)/$SCRATCH/l5.udma,port=8890,label=GPS L5,fft_size=4096" \
  --band "ring=$(pwd)/$SCRATCH/iridium.udma,port=8891,label=Iridium,fft_size=4096" \
  | tee "$SCRATCH/step3-5band-rest-transcript.log"

echo
echo "Launched. PIDs recorded in $PIDFILE."
echo "Teardown: plugins/samplesource/udmabufinput/scripts/step3/step3_teardown_5band.sh"
