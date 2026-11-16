#!/usr/bin/env python3
"""Generate OFDM-friendly unicast Wi-Fi packets for CYD CSI capture.

Run on a LAN computer (wired to the AP is ideal). AP forwards each UDP packet
to the CYD receiver. The CYD listens on UDP/5051. No internet traffic.
"""
import argparse
import ipaddress
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nodes", nargs="+", help="CYD IPv4 addresses")
    parser.add_argument("--hz", type=int, default=30, help="Packets/s per board (1-50)")
    parser.add_argument("--seconds", type=int, default=60,
                        help="Duration in seconds; 0 = run continuously until Ctrl+C")
    parser.add_argument("--port", type=int, default=5051)
    args = parser.parse_args()
    if not 1 <= args.hz <= 50 or args.seconds < 0:
        parser.error("--hz must be 1..50 and --seconds must be >= 0")
    for addr in args.nodes:
        try:
            ip = ipaddress.IPv4Address(addr)
        except ipaddress.AddressValueError:
            parser.error(f"Invalid IPv4: {addr}")
        if not ip.is_private:
            parser.error(f"Refusing to send probes outside a private LAN: {addr}")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        deadline = time.monotonic() + args.seconds if args.seconds else None
        tick = time.monotonic()
        sent = 0
        try:
            while deadline is None or time.monotonic() < deadline:
                for ip in args.nodes:
                    sock.sendto(b"CYD-CSI-PROBE:" + (sent & 0xffffffff).to_bytes(4, "little"),
                                (ip, args.port))
                sent += 1
                tick += 1.0 / args.hz
                time.sleep(max(0, tick - time.monotonic()))
        except KeyboardInterrupt:
            pass
        print(f"Sent {sent} probes to each of {len(args.nodes)} node(s)")


if __name__ == "__main__":
    main()
