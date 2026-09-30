"""Read-only MSP v1 inventory and IMU timing capture; no motor/configuration API.

Only explicit ports are opened. Raw values are retained without assuming sensor
units. Host request/reply times are NOT hardware sample timestamps.
"""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import platform
import statistics
import struct
import time


READ_COMMANDS = {1: "api_version", 2: "fc_variant", 3: "fc_version",
                 4: "board_info", 5: "build_info", 101: "status", 102: "raw_imu"}


def request_packet(command: int) -> bytes:
    if command not in READ_COMMANDS:
        raise ValueError("Only whitelisted, empty-payload sensor/inventory queries are allowed")
    return b"$M<" + bytes((0, command, command))


class Parser:
    def __init__(self):
        self.buffer = bytearray()
        self.bad_checksums = 0

    def feed(self, data: bytes):
        self.buffer.extend(data)
        frames = []
        while len(self.buffer) >= 3:
            if self.buffer[:3] not in (b"$M>", b"$M!"):
                del self.buffer[0]
                continue
            if len(self.buffer) < 6:
                break
            size = self.buffer[3]
            length = size + 6
            if len(self.buffer) < length:
                break
            checksum = 0
            for byte in self.buffer[3:length - 1]:
                checksum ^= byte
            if checksum != self.buffer[length - 1]:
                self.bad_checksums += 1
                del self.buffer[0]
                continue
            frames.append((self.buffer[4], bytes(self.buffer[5:length - 1]),
                           self.buffer[2] == ord("!")))
            del self.buffer[:length]
        return frames


def query(port, command: int, timeout: float = .3):
    packet = request_packet(command)
    parser = Parser()
    started = time.monotonic_ns()
    port.write(packet)
    deadline = started + int(timeout * 1e9)
    while time.monotonic_ns() < deadline:
        for reply_command, payload, unsupported in parser.feed(port.read(1)):
            if reply_command != command:
                continue
            if unsupported:
                raise ValueError(f"Controller does not support {READ_COMMANDS[command]}")
            return payload, {"request_monotonic_ns": started,
                             "reply_monotonic_ns": time.monotonic_ns(),
                             "reply_wall_ns": time.time_ns(),
                             "bad_checksums": parser.bad_checksums}
    # Stop on a sampling timeout rather than assign a delayed response to the
    # next same-command request: MSP v1 has no transaction or sample identifier.
    raise TimeoutError(f"No checksum-valid response to {READ_COMMANDS[command]}")


def decode_imu(payload: bytes):
    if len(payload) != 18:
        raise ValueError(f"Expected 18 IMU payload bytes, received {len(payload)}")
    values = struct.unpack("<9h", payload)
    return {"acc_raw": list(values[:3]), "gyro_raw": list(values[3:6]),
            "mag_raw": list(values[6:])}


def decode_inventory(command: int, payload: bytes):
    result = {"payload_hex": payload.hex()}
    if command in (1, 3) and len(payload) >= 3:
        result["version_bytes"] = list(payload[:3])
    if command == 2:
        result["identifier"] = payload.decode("ascii", errors="replace")
    if command == 4 and len(payload) >= 6:
        result.update(board_identifier=payload[:4].decode("ascii", errors="replace"),
                      hardware_revision=struct.unpack("<H", payload[4:6])[0])
    if command == 101 and len(payload) >= 6:
        cycle, errors, bits = struct.unpack("<HHH", payload[:6])
        result.update(cycle_time_us=cycle, i2c_errors=errors, sensor_bits=bits,
                      accelerometer_reported=bool(bits & 1))
    return result


def summarize(rows):
    replies = [r["reply_monotonic_ns"] for r in rows]
    intervals = [(b - a) / 1e6 for a, b in zip(replies, replies[1:])]
    rtts = [(r["reply_monotonic_ns"] - r["request_monotonic_ns"]) / 1e6 for r in rows]
    vectors = Counter(tuple(r["acc_raw"] + r["gyro_raw"]) for r in rows)
    return {"samples": len(rows), "distinct_acc_gyro_vectors": len(vectors),
            "reply_rate_hz": ((len(replies) - 1) * 1e9 / (replies[-1] - replies[0]))
            if len(replies) > 1 and replies[-1] > replies[0] else None,
            "median_reply_interval_ms": statistics.median(intervals) if intervals else None,
            "max_reply_gap_ms": max(intervals) if intervals else None,
            "median_round_trip_ms": statistics.median(rtts) if rtts else None,
            "bad_checksums": sum(r["bad_checksums"] for r in rows),
            "note": "Reply rate is not sensor update rate. No hardware sample timestamps or calibrated units."}


def capture(port, output: Path, seconds: float, rate: float):
    output.mkdir(parents=True, exist_ok=False)
    metadata = {"created_at": datetime.now(timezone.utc).isoformat(),
                "host": platform.node(), "platform": platform.platform(),
                "port": port.port, "baud": port.baudrate, "target_reply_hz": rate,
                "duration_requested_seconds": seconds, "read_only": True,
                "sensor_timestamps": "unavailable in MSP_RAW_IMU",
                "units": "raw protocol values; firmware-specific conversion not applied",
                "timing": "host monotonic request/reply; not synchronized with camera SensorTimestamp",
                "state": "inventory", "inventory": {}}
    rows = []
    failure = None
    try:
        # Do not attempt later commands with an unknown API major version.
        payload, _ = query(port, 1)
        metadata["inventory"]["api_version"] = decode_inventory(1, payload)
        if len(payload) != 3 or payload[1] != 1:
            raise ValueError("Unsupported MSP API major version")
        for command in (2, 3, 4, 5, 101):
            payload, _ = query(port, command)
            metadata["inventory"][READ_COMMANDS[command]] = decode_inventory(command, payload)
        metadata["state"] = "recording"
        (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        start = time.monotonic()
        due = start
        with (output / "imu.jsonl").open("x") as stream:
            while time.monotonic() - start < seconds:
                delay = due - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
                if time.monotonic() - start >= seconds:
                    break
                payload, timing = query(port, 102)
                row = {"index": len(rows), **timing, **decode_imu(payload)}
                stream.write(json.dumps(row, separators=(",", ":")) + "\n")
                rows.append(row)
                # No catch-up request burst after a slow response.
                due = max(due + 1 / rate, time.monotonic())
        metadata["state"] = "complete"
    except (Exception, KeyboardInterrupt) as exc:
        metadata.update(state="interrupted" if isinstance(exc, KeyboardInterrupt) else "failed",
                        error=str(exc) or type(exc).__name__)
        failure = exc
    finally:
        (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        report = {**summarize(rows), "state": metadata["state"], "error": metadata.get("error")}
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
    if failure:
        raise failure
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="List serial ports without opening any")
    parser.add_argument("--port", help="Explicit FC USB serial device; never autodetected or scanned")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=10)
    parser.add_argument("--rate", type=float, default=100)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    import serial
    from serial.tools.list_ports import comports
    if args.list:
        print(json.dumps([dict(device=p.device, description=p.description, hwid=p.hwid)
                          for p in comports()], indent=2))
        return
    if not args.port or args.output is None:
        parser.error("--port and a new --output directory are required")
    if not (math.isfinite(args.seconds) and 0 < args.seconds <= 300 and
            math.isfinite(args.rate) and 1 <= args.rate <= 200 and 0 < args.baud <= 921600):
        parser.error("Use 0–300 seconds, 1–200 Hz and a positive supported baud rate")
    if args.output.exists():
        parser.error("Output directory exists; choose a new path to preserve previous captures")
    # Construct closed first to avoid deliberately asserting reset/control lines.
    kwargs = {"exclusive": True} if os.name == "posix" else {}
    port = serial.Serial(port=None, baudrate=args.baud, timeout=.01, write_timeout=.3, **kwargs)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        port.open()
        port.reset_input_buffer()
        capture(port, args.output, args.seconds, args.rate)
    finally:
        port.close()


if __name__ == "__main__":
    main()
