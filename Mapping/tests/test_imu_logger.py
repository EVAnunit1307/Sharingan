import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from Mapping.imu_logger import Parser, capture, decode_imu, query, request_packet, summarize


def response(command, payload=b"", direction=b">"):
    body = bytes((len(payload), command)) + payload
    checksum = 0
    for byte in body:
        checksum ^= byte
    return b"$M" + direction + body + bytes((checksum,))


class MSPTests(unittest.TestCase):
    def test_motor_configuration_and_reboot_commands_are_rejected(self):
        for command in (11, 68, 104, 200, 214, 240, 245, 250, 255):
            with self.assertRaises(ValueError):
                request_packet(command)
        self.assertEqual(request_packet(102), b"$M<\x00ff")

    def test_fragmented_noise_corruption_and_error_frames(self):
        parser = Parser()
        corrupt = bytearray(response(102, b"abc")); corrupt[-1] ^= 1
        wire = b"noise$M<\x00ff" + corrupt + response(102, b"abc") + response(5, direction=b"!")
        frames = []
        for byte in wire:
            frames.extend(parser.feed(bytes((byte,))))
        self.assertEqual(frames, [(102, b"abc", False), (5, b"", True)])
        self.assertEqual(parser.bad_checksums, 1)

    def test_signed_imu_and_truncated_payload_rejected(self):
        values = [-32768, 0, 32767, -1, 2, -3, 4, 5, 6]
        sample = decode_imu(struct.pack("<9h", *values))
        self.assertEqual(sample["acc_raw"], values[:3])
        self.assertEqual(sample["gyro_raw"], values[3:6])
        with self.assertRaises(ValueError):
            decode_imu(b"\0" * 12)

    def test_query_discards_other_command_and_rejects_unsupported(self):
        class Port:
            def __init__(self, wire): self.input = io.BytesIO(wire); self.writes = []
            def read(self, count): return self.input.read(count)
            def write(self, data): self.writes.append(data)
        port = Port(response(101) + response(102, b"abc"))
        payload, timing = query(port, 102)
        self.assertEqual(payload, b"abc")
        self.assertEqual(port.writes, [request_packet(102)])
        self.assertGreaterEqual(timing["reply_monotonic_ns"], timing["request_monotonic_ns"])
        with self.assertRaises(ValueError): query(Port(response(102, direction=b"!")), 102)
        with self.assertRaises(TimeoutError): query(Port(b""), 102, timeout=.001)

    def test_timing_statistics_do_not_treat_repeated_vectors_as_new_updates(self):
        rows = [dict(request_monotonic_ns=i*10000000, reply_monotonic_ns=i*10000000+2000000,
                     bad_checksums=0, acc_raw=[0,0,512], gyro_raw=[0,0,0]) for i in range(3)]
        report = summarize(rows)
        self.assertEqual(report["reply_rate_hz"], 100)
        self.assertEqual(report["median_round_trip_ms"], 2)
        self.assertEqual(report["distinct_acc_gyro_vectors"], 1)

    def test_unknown_api_stops_before_sensor_requests_and_retains_failure(self):
        class Port: port = "test"; baudrate = 115200
        with tempfile.TemporaryDirectory() as root, patch("Mapping.imu_logger.query", return_value=(b"\0\2\0", {})) as call:
            output = Path(root) / "capture"
            with self.assertRaises(ValueError): capture(Port(), output, 1, 100)
            self.assertEqual(call.call_count, 1)
            self.assertEqual(json.loads((output / "summary.json").read_text())["state"], "failed")


if __name__ == "__main__": unittest.main()
