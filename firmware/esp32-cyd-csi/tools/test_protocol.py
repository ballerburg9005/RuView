#!/usr/bin/env python3
"""No-hardware protocol validation: python -m unittest discover -s tools -p 'test_*.py'"""
import struct
import unittest
from monitor import HEADER, MAGIC, parse_packet, mean_magnitude


class ADR018Tests(unittest.TestCase):
    def valid_packet(self):
        iq = bytes([3, 4, 0xfd, 0xfc])
        return HEADER.pack(MAGIC, 3, 1, 2, 2437, 19, -51, -90, 0, 0x20) + iq

    def test_frame(self):
        x = parse_packet(self.valid_packet())
        self.assertEqual((x.node, x.carriers, x.sequence), (3, 2, 19))
        self.assertEqual(x.rssi, -51)
        self.assertAlmostEqual(mean_magnitude(x), 5.0)

    def test_reject_invalid_magic(self):
        buf = bytearray(self.valid_packet())
        buf[0] = 0
        with self.assertRaises(ValueError):
            parse_packet(bytes(buf))

    def test_reject_length_mismatch(self):
        with self.assertRaises(ValueError):
            parse_packet(self.valid_packet()[:-1])

    def test_reject_invalid_dimensions(self):
        buf = bytearray(self.valid_packet())
        struct.pack_into("<H", buf, 6, 0)
        with self.assertRaises(ValueError):
            parse_packet(bytes(buf))


if __name__ == "__main__":
    unittest.main()
