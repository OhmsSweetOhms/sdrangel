#!/usr/bin/env python3
"""Compute the plan-02 Step-2 on-wire link budget for the Remote Sink UDP
transport (Cauchy MDS block-erasure FEC framing).

This mirrors the numbers reported live by step2_configure_chain.py's REST
run against the real UdmaBufInput -> RemoteSink -> RemoteInput chain, and
tabulates every power-of-2 decimation option so the choice of log2Decim is
traceable to the 1 GbE link budget rather than picked by feel.

A Remote Sink channel decimates by powers of two only (log2Decim selects
a half-band-filter-chain depth of 2**log2Decim); it cannot express the
plan's illustrative "/10 -> 2.048 MSPS" ratio exactly. log2Decim=3 (/8,
2.56 MSPS) is the nearest power-of-2 match to that L1-slice class.
"""

import argparse


def on_wire_bps(rate_sps: int, bytes_per_component: int, nb_fec_blocks: int,
                 nominal_blocks: int = 128, data_blocks: int = 127) -> float:
    bytes_per_complex_sample = 2 * bytes_per_component  # I + Q
    raw_payload_bps = rate_sps * bytes_per_complex_sample * 8
    total_blocks = nominal_blocks + nb_fec_blocks
    framing_overhead_ratio = total_blocks / data_blocks
    return raw_payload_bps * framing_overhead_ratio


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-rate", type=int, default=20_480_000,
                         help="UdmaBufInput native device rate, S/s "
                              "(20.48 MSPS = the real ZCU102 RX target)")
    parser.add_argument("--bytes-per-component", type=int, default=2,
                         help="RemoteSinkSettings.nbTxBytes (1, 2, or 4)")
    parser.add_argument("--nb-fec-blocks", type=int, default=8)
    parser.add_argument("--gbe-ceiling-mbps", type=float, default=940.0,
                         help="practical 1 GbE ceiling, Mbps")
    args = parser.parse_args()

    gbe_bps = args.gbe_ceiling_mbps * 1e6

    print(f"source rate: {args.source_rate:,} S/s   "
          f"nbTxBytes={args.bytes_per_component}   "
          f"nbFECBlocks={args.nb_fec_blocks}   "
          f"1GbE ceiling={args.gbe_ceiling_mbps:.0f} Mbps\n")
    print(f"{'log2Decim':>9} {'factor':>7} {'rate (S/s)':>12} {'on-wire (Mbps)':>15} {'margin':>8}  fits?")
    for l2 in range(0, 6):
        rate = args.source_rate // (2 ** l2)
        wire = on_wire_bps(rate, args.bytes_per_component, args.nb_fec_blocks)
        margin = gbe_bps / wire
        fits = "yes" if wire < gbe_bps else "NO"
        print(f"{l2:>9} {'1/' + str(2**l2):>7} {rate:>12,} {wire/1e6:>15.2f} {margin:>7.2f}x  {fits}")


if __name__ == "__main__":
    main()
