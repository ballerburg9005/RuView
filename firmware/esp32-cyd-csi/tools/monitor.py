#!/usr/bin/env python3
"""Diagnostic receiver for RuView ADR-018 CSI packets from CYD nodes.

Use this BEFORE running the full RuView aggregator (both default to UDP 5005).
No third-party packages. No medical inference.
"""
import argparse
import csv
import socket
import struct
import time
from collections import defaultdict
from dataclasses import dataclass

MAGIC = 0xC5110001
HEADER = struct.Struct("<IBBHIIbbBB")


@dataclass(frozen=True)
class Frame:
    node: int
    antennas: int
    carriers: int
    mhz: int
    sequence: int
    rssi: int
    noise: int
    ppdu: int
    flags: int
    iq: bytes


def parse_packet(data: bytes) -> Frame:
    if len(data) < HEADER.size:
        raise ValueError("short header")
    magic, node, antennas, carriers, mhz, seq, rssi, noise, ppdu, flags = (
        HEADER.unpack_from(data)
    )
    if magic != MAGIC:
        raise ValueError("unrecognized magic")
    if antennas < 1 or carriers < 1:
        raise ValueError("invalid CSI dimensions")
    if len(data) != HEADER.size + antennas * carriers * 2:
        raise ValueError("CSI payload does not match dimensions")
    if not (2412 <= mhz <= 2484):
        raise ValueError("unexpected frequency for ESP32")
    return Frame(node, antennas, carriers, mhz, seq, rssi, noise,
                 ppdu, flags, data[HEADER.size:])


def mean_magnitude(frame: Frame) -> float:
    # ESP-IDF CSI bytes are signed 8-bit real/imaginary components.
    # Simple magnitude proxy for testing received CSI, NOT a vital sign.
    iq = frame.iq
    return sum((int.from_bytes(iq[i:i + 1], "little", signed=True) ** 2 +
                int.from_bytes(iq[i + 1:i + 2], "little", signed=True) ** 2) ** .5
               for i in range(0, len(iq), 2)) / (len(iq) // 2)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=5005)
    ap.add_argument("--csv", help="Optional path to CSV measurements")
    args = ap.parse_args()
    counts = defaultdict(int)
    last = {}
    errors = 0
    writer = None
    csv_file = None
    if args.csv:
        csv_file = open(args.csv, "w", encoding="utf-8", newline="")
        writer = csv.writer(csv_file)
        writer.writerow(["unix_time", "node", "sequence", "mhz", "rssi_dbm",
                         "noise_dbm", "subcarriers", "mean_iq_magnitude"])
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    sock.settimeout(.5)
    print(f"Listening on UDP {args.bind}:{args.port}; Ctrl+C to stop", flush=True)
    next_report = time.monotonic() + 5
    try:
        while True:
            try:
                raw, address = sock.recvfrom(2048)
                frame = parse_packet(raw)
                counts[frame.node] += 1
                last[frame.node] = (frame, address[0], mean_magnitude(frame))
                if writer:
                    writer.writerow([f"{time.time():.3f}", frame.node,
                                     frame.sequence, frame.mhz, frame.rssi,
                                     frame.noise, frame.carriers,
                                     f"{last[frame.node][2]:.3f}"])
            except socket.timeout:
                pass
            except ValueError:
                errors += 1
            now = time.monotonic()
            if now >= next_report:
                if not last:
                    print("No valid CSI received; check Wi-Fi, node IP, UDP and probe traffic")
                for node, (frame, src, mag) in sorted(last.items()):
                    print(f"node {node:3d} {src:>15} "
                          f"{counts[node] / 5:5.1f} CSI/s "
                          f"seq={frame.sequence:8d} ch={frame.mhz}MHz "
                          f"RSSI={frame.rssi:4d}dBm CSI={frame.carriers:3d} "
                          f"meanMagnitude={mag:6.1f}")
                if errors:
                    print(f"  rejected packets: {errors}")
                counts.clear()
                next_report = now + 5
                if csv_file:
                    csv_file.flush()
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()
        if csv_file:
            csv_file.close()


if __name__ == "__main__":
    main()
