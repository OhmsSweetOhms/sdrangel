#!/usr/bin/env python3
"""Capture raw binary frames from the SpectrumVis websocket server.

This deliberately does two independent things at once:

1. Uses a plain TCP socket + a hand-rolled RFC 6455 client handshake and
   frame parser (no third-party websocket library) so the bytes we write
   to disk are traceable, in this script, all the way from the kernel
   socket recv() call to the output file -- nothing is hiding behind a
   library's internal buffering.
2. Writes the concatenated *payload* bytes of each binary WS message
   back-to-back to one output file. Frames are self-delimiting (each
   36-byte header carries its own FFT size at offset 24), so a reader
   can walk the file without an extra length prefix -- see
   spectrum_frame_decode.py.

No IQ ever appears in this path: the only channel opened is the
spectrum server TCP port (default 8887). Compare against the tcpdump/
dumpcap evidence in evidence/ for the same conclusion at the packet
level.

Usage:
    python3 spectrum_ws_capture.py --host 127.0.0.1 --port 8887 \
        --count 20 --out frames.bin
"""

import argparse
import base64
import hashlib
import os
import socket
import struct
import sys
import time

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_handshake(sock: socket.socket, host: str, port: int, path: str = "/") -> None:
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock.sendall(request.encode("ascii"))

    # Read the HTTP response headers (terminated by CRLFCRLF).
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("socket closed during WS handshake")
        buf += chunk

    header_end = buf.index(b"\r\n\r\n") + 4
    headers = buf[:header_end].decode("iso-8859-1", "replace")
    if "101" not in headers.splitlines()[0]:
        raise ConnectionError(f"WS handshake rejected: {headers.splitlines()[0]!r}")

    expected_accept = base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode("ascii")).digest()
    ).decode("ascii")
    if expected_accept not in headers:
        raise ConnectionError("Sec-WebSocket-Accept did not match -- not a real WS peer")

    # Anything already read past the header terminator belongs to the
    # first WS frame; hand it back to the caller via a small buffer object
    # the caller manages (see recv_exact below through the shared reader).
    return buf[header_end:]


class FrameReader:
    """Buffers raw socket bytes so ws frame parsing can request exact counts."""

    def __init__(self, sock: socket.socket, pre_buffered: bytes):
        self._sock = sock
        self._buf = bytearray(pre_buffered)

    def recv_exact(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ConnectionError("socket closed while reading a WS frame")
            self._buf.extend(chunk)
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    def read_binary_message(self) -> bytes:
        """Reads one complete WS message, concatenating fragments, and
        returns only binary-opcode message payloads (server->client frames
        are unmasked per RFC 6455)."""
        payload = bytearray()
        while True:
            hdr2 = self.recv_exact(2)
            b0, b1 = hdr2[0], hdr2[1]
            fin = (b0 & 0x80) != 0
            opcode = b0 & 0x0F
            masked = (b1 & 0x80) != 0
            length = b1 & 0x7F

            if length == 126:
                length = struct.unpack(">H", self.recv_exact(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self.recv_exact(8))[0]

            mask_key = self.recv_exact(4) if masked else None
            data = self.recv_exact(length) if length else b""
            if mask_key:
                data = bytes(b ^ mask_key[i % 4] for i, b in enumerate(data))

            if opcode == 0x8:  # close
                raise ConnectionError(f"server sent WS close frame: {data!r}")
            if opcode in (0x1, 0x2):  # text or binary (start of message)
                payload = bytearray(data)
            elif opcode == 0x0:  # continuation
                payload.extend(data)
            # opcode 0x9/0xA (ping/pong) are ignored (SpectrumVis's WSSpectrum
            # never pings, but be tolerant).

            if fin:
                return bytes(payload)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8887)
    parser.add_argument("--count", type=int, default=20, help="number of frames to capture")
    parser.add_argument("--timeout", type=float, default=30.0, help="overall capture timeout (s)")
    parser.add_argument("--out", required=True, help="output file for concatenated raw frame payloads")
    args = parser.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=args.timeout)
    sock.settimeout(args.timeout)
    try:
        leftover = ws_handshake(sock, args.host, args.port)
        print(f"[OK] WS handshake complete with {args.host}:{args.port}")
        reader = FrameReader(sock, leftover)

        captured = 0
        sizes = []
        start = time.monotonic()
        with open(args.out, "wb") as fh:
            while captured < args.count:
                if time.monotonic() - start > args.timeout:
                    print(f"[WARN] timeout after {captured} frames (wanted {args.count})")
                    break
                payload = reader.read_binary_message()
                fh.write(payload)
                sizes.append(len(payload))
                captured += 1
                if captured == 1 or captured % 5 == 0:
                    print(f"[OK] frame {captured}/{args.count}: {len(payload)} bytes")

        print(f"\n=== capture summary ===")
        print(f"frames captured : {captured}")
        print(f"output file     : {args.out}")
        print(f"total bytes     : {sum(sizes)}")
        if sizes:
            print(f"per-frame sizes : min={min(sizes)} max={max(sizes)} "
                  f"(constant means fixed fftSize across the run: "
                  f"{'yes' if min(sizes) == max(sizes) else 'no'})")

        if captured < args.count:
            sys.exit(1)
    finally:
        sock.close()


if __name__ == "__main__":
    main()
