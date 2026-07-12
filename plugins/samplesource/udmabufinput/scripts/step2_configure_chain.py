#!/usr/bin/env python3
"""Configure the plan-02 Step-2 chain on a headless sdrangelsrv over REST.

Chain: UdmaBufInput (device set 0) -> RemoteSink channel (decimator/
channelizer, log2Decim) -> loopback UDP -> RemoteInput (device set 1).

This is a desk/loopback proof that the whole chain is expressible purely
over REST and that the Remote Sink meta block carries the *post-decimation*
sample rate + center frequency through to a receiving Remote Input device.
It does not require the real ZCU102/udmabuf ring -- the source is the
Step-1 fake_udmabuf_producer.py file-backed mmap (see plan-02 Step 1).

Usage:
    python3 step2_configure_chain.py --ring-path /path/to/ring.mmap \
        [--api http://127.0.0.1:8091] [--log2-decim 3] \
        [--data-port 9999] [--nb-fec-blocks 8]
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request


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


def step(label, method, url, body=None, expect=(200, 202)):
    status, payload = rest(method, url, body)
    ok = status in expect
    marker = "OK" if ok else "FAIL"
    print(f"[{marker}] {label}: {method} {url} -> {status}")
    if not ok:
        print(json.dumps(payload, indent=2))
        raise SystemExit(f"step failed: {label}")
    return payload


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--api", default="http://127.0.0.1:8091")
    parser.add_argument("--ring-path", required=True)
    parser.add_argument("--log2-decim", type=int, default=3,
                         help="power-of-2 decimation applied by the RemoteSink "
                              "channelizer (2^N); RemoteSink has no non-power-"
                              "of-2 mode")
    parser.add_argument("--filter-chain-hash", type=int, default=None,
                         help="base-3 half-band filter chain selector. Each "
                              "digit is 0=lower/1=centered/2=higher for that "
                              "decimation stage (see HBFilterChainConverter). "
                              "Default: all-centered digits, i.e. the "
                              "decimated slice stays on the source center "
                              "frequency (a true L1 slice, no CF shift).")
    parser.add_argument("--data-address", default="127.0.0.1")
    parser.add_argument("--data-port", type=int, default=9999)
    parser.add_argument("--nb-fec-blocks", type=int, default=8)
    args = parser.parse_args()

    if args.filter_chain_hash is None:
        # All-centered chain: digit 1 at every stage in base 3.
        args.filter_chain_hash = sum(3 ** k for k in range(args.log2_decim)) if args.log2_decim else 0

    base = args.api.rstrip("/") + "/sdrangel"

    # --- Reset to a clean, known state -------------------------------------
    while True:
        status, _ = rest("GET", f"{base}")
        break
    # Drain any existing device sets from a prior run of this script.
    for _ in range(8):
        status, payload = rest("DELETE", f"{base}/deviceset")
        if status == 404:
            break
        print(f"[INFO] cleared a prior device set (status {status})")
        time.sleep(0.2)

    # --- Device set 0: UdmaBufInput -----------------------------------------
    step("add device set 0 (Rx)", "POST", f"{base}/deviceset?direction=0")
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

    # --- Add the RemoteSink channel (this IS the decimator/channelizer) ----
    step(
        "add RemoteSink channel on device set 0",
        "POST",
        f"{base}/deviceset/0/channel",
        {"channelType": "RemoteSink", "direction": 0},
    )
    step(
        "configure RemoteSink decimation + FEC + destination",
        "PATCH",
        f"{base}/deviceset/0/channel/0/settings",
        {
            "channelType": "RemoteSink",
            "direction": 0,
            "originatorChannelIndex": 0,
            "originatorDeviceSetIndex": 0,
            "RemoteSinkSettings": {
                "log2Decim": args.log2_decim,
                "filterChainHash": args.filter_chain_hash,
                "dataAddress": args.data_address,
                "dataPort": args.data_port,
                "nbFECBlocks": args.nb_fec_blocks,
                "nbTxBytes": 2,
                "title": "Step2 L1-slice decimator",
            },
        },
    )
    time.sleep(0.5)
    chan_settings = step(
        "read back RemoteSink settings",
        "GET",
        f"{base}/deviceset/0/channel/0/settings",
    )
    rs = chan_settings.get("RemoteSinkSettings", {})
    print(f"    RemoteSink log2Decim={rs.get('log2Decim')} "
          f"filterChainHash={rs.get('filterChainHash')} "
          f"dataAddress={rs.get('dataAddress')} dataPort={rs.get('dataPort')} "
          f"nbFECBlocks={rs.get('nbFECBlocks')}")

    # --- Device set 1: RemoteInput, loopback to the RemoteSink above -------
    step("add device set 1 (Rx)", "POST", f"{base}/deviceset?direction=0")
    step(
        "select RemoteInput on device set 1",
        "PUT",
        f"{base}/deviceset/1/device",
        {"hwType": "RemoteInput", "direction": 0},
    )
    step(
        "point RemoteInput at the RemoteSink UDP + control API",
        "PATCH",
        f"{base}/deviceset/1/device/settings",
        {
            "deviceHwType": "RemoteInput",
            "direction": 0,
            "remoteInputSettings": {
                "apiAddress": "127.0.0.1",
                "apiPort": int(args.api.rsplit(":", 1)[-1]),
                "dataAddress": args.data_address,
                "dataPort": args.data_port,
            },
        },
    )
    step("start device set 1 engine", "POST", f"{base}/deviceset/1/device/run", {})

    # Let a few frames cross the loopback UDP socket before reading back.
    time.sleep(3.0)

    final_report = step(
        "read back RemoteInput report (post-decimation rate + CF)",
        "GET",
        f"{base}/deviceset/1/device/report",
    )
    ri_report = final_report.get("remoteInputReport", {})
    recv_rate = ri_report.get("sampleRate")
    recv_cf = ri_report.get("centerFrequency")
    print(f"    RemoteInput reports sampleRate={recv_rate} centerFrequency={recv_cf}")

    expected_rate = source_rate >> args.log2_decim if source_rate else None
    print("\n=== Step-2 verification summary ===")
    print(f"source (UdmaBufInput) sampleRate : {source_rate}")
    print(f"log2Decim applied by RemoteSink   : {args.log2_decim} (factor {2**args.log2_decim})")
    print(f"expected decimated rate           : {expected_rate}")
    print(f"RemoteInput received sampleRate   : {recv_rate}")
    print(f"RemoteInput received centerFreq   : {recv_cf} (source CF was {source_cf})")
    if expected_rate is not None and recv_rate == expected_rate:
        print("PASS: Remote Sink meta carries the post-decimation sample rate.")
    else:
        print("WARN: decimated rate mismatch -- inspect manually before trusting the chain.")
        sys.exit(1)


if __name__ == "__main__":
    main()
