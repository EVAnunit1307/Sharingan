#!/usr/bin/env python3
"""Replay presence-labelled clips through the actual production detector.

Reports observed people separately from held tracks. These are frame-level
presence metrics, not box AP, person-count accuracy, or independent test data.
"""
import argparse
import hashlib
import json
from pathlib import Path
import time

import cv2
import numpy as np
from detector.person_detector import PersonDetector, CONFIG_PATH


def load_frame(root, row, rotation, legacy_channels):
    image = cv2.imread(str(root / row['path']))
    if image is None:
        raise ValueError(f"Unreadable frame: {row['path']}")
    if row.get('legacy_channel_swap', legacy_channels):
        image = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
    if rotation == 180:
        image = cv2.rotate(image, cv2.ROTATE_180)
    return image


def evaluate(root, config, overrides=None, threads=2, rotation_override=None, legacy_channels=False):
    manifest = json.loads((root / 'manifest.json').read_text())
    # The original capture_evalset.py incorrectly swapped RGB888, which is
    # already BGR. New captures explicitly tag their channel order per row.
    legacy = legacy_channels
    rotation = rotation_override if rotation_override is not None else manifest.get('camera_rotation_deg')
    if rotation not in (0, 180):
        raise ValueError('Manifest must specify camera_rotation_deg: 0 or 180')
    labels = json.loads(overrides.read_text())['overrides'] if overrides else {}
    detector = PersonDetector(config, threads)
    groups = {}
    for row in manifest['frames']:
        groups.setdefault(row['scenario'], []).append(row)
    report = {'metric': 'frame-level person presence, observed boxes only',
              'limitation': 'Development footage used in tuning; no ground-truth bounding boxes or held-out people/rooms.',
              'config': detector.cfg, 'label_overrides': labels,
              'legacy_channel_correction': legacy,
              'camera_rotation_deg': rotation,
              'manifest_sha256': hashlib.sha256((root/'manifest.json').read_bytes()).hexdigest(),
              'scenarios': {}, 'frames': []}
    timing = []
    totals = dict(tp=0, fn=0, fp=0, tn=0, held_only_frames=0)
    for name, frames in sorted(groups.items()):
        detector.reset()
        metrics = dict(tp=0, fn=0, fp=0, tn=0, held_only_frames=0)
        for row in sorted(frames, key=lambda r: r['t']):
            image = load_frame(root, row, rotation, legacy)
            start = time.perf_counter()
            people = detector.detect(image, timestamp=row['t'])
            timing.append((time.perf_counter()-start)*1000)
            hit = any(p['observed'] for p in people)
            truth = labels.get(row['path'], row['person_present'])
            key = ('tp' if hit else 'fn') if truth else ('fp' if hit else 'tn')
            metrics[key] += 1
            metrics['held_only_frames'] += bool(people) and not hit
            report['frames'].append(dict(path=row['path'], truth=truth, observed=hit,
                                         people=people, raw=detector.last_raw))
        report['scenarios'][name] = metrics
        for key in totals:
            totals[key] += metrics[key]
        print(name, metrics, flush=True)
    totals['recall'] = totals['tp']/max(1, totals['tp']+totals['fn'])
    totals['precision'] = totals['tp']/max(1, totals['tp']+totals['fp'])
    report['totals'] = totals
    report['latency_ms'] = {'median': float(np.median(timing)), 'p95': float(np.percentile(timing, 95))}
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('evalset', type=Path)
    p.add_argument('--config', default=CONFIG_PATH)
    p.add_argument('--overrides', type=Path)
    p.add_argument('--out', type=Path, default=Path('evaluation.json'))
    p.add_argument('--threads', type=int, default=2)
    p.add_argument('--rotation', type=int, choices=(0, 180), help='Override missing/incorrect manifest orientation')
    p.add_argument('--legacy-channel-swap', action='store_true', help='Correct clips from the original RGB888 capture bug')
    args = p.parse_args()
    cv2.setNumThreads(1)
    report = evaluate(args.evalset, args.config, args.overrides, args.threads,
                      args.rotation, args.legacy_channel_swap)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'totals': report['totals'], 'latency_ms': report['latency_ms']}, indent=2))


if __name__ == '__main__':
    main()
