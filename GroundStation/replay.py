"""Replay recorded sensor packets through fusion without hardware or network I/O.

Input JSONL records have {"received_at": <monotonic seconds>, "packet": {...}}.
Output preserves packet fields and adds spatial_people and an explicit replay flag.
"""
import argparse
import json
from pathlib import Path
import sys

from .fusion import FusionConfig, FusionEngine, number


def reject_constant(value):
    raise ValueError(f"Nonfinite JSON constant: {value}")


def replay(records, config=None):
    engine = FusionEngine(config)
    previous = None
    for line_number, line in enumerate(records, start=1):
        if not line.strip():
            continue
        try:
            record = json.loads(line, parse_constant=reject_constant)
            if not isinstance(record, dict):
                raise ValueError("Record must be an object")
            received_at = record.get("received_at")
            if not number(received_at) or (previous is not None and received_at < previous):
                raise ValueError("Receipt timestamps must be finite and nondecreasing")
            packet = record.get("packet")
            spatial = engine.ingest(packet, now=received_at)
            previous = received_at
            yield dict(packet, spatial_people=spatial, is_replay=True)
        except (ValueError, TypeError) as error:
            raise ValueError(f"Record {line_number}: {error}") from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--config", type=Path, help="JSON object containing FusionConfig settings")
    args = parser.parse_args(argv)
    try:
        config = (FusionConfig(**json.loads(args.config.read_text(), parse_constant=reject_constant))
                  if args.config else FusionConfig())
        with args.recording.open() as records:
            for packet in replay(records, config):
                print(json.dumps(packet, allow_nan=False, separators=(",", ":")))
    except (OSError, ValueError, TypeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
