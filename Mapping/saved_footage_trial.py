"""Bounded saved-footage selection ablation, with fixed overlap acceptance.

The only capture-specific exclusion is an explicitly supplied, previously
reviewed bad-frame range. Sharpness/feature coverage are relative selection
heuristics, not a generic corruption detector or a physical accuracy score.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

import cv2
import numpy as np

from Mapping.room_demo import contained, digest, verify_assets
from Mapping.window_replay import load_trial, overlap_transform, replay


def dump(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')


def image_quality(image):
    if image is None or image.ndim != 3 or image.shape[2] != 3:
        raise ValueError('Image could not be decoded')
    gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
    gray = cv2.resize(gray, (320, 240), interpolation=cv2.INTER_AREA)
    sharpness = float(cv2.Laplacian(gray, cv2.CV_32F).var())
    corners = cv2.goodFeaturesToTrack(gray, 400, .01, 5)
    coverage = 0
    count = 0
    if corners is not None:
        xy = corners[:, 0]
        cells = np.column_stack([np.minimum((xy[:, 0] / 40).astype(int), 7),
                                 np.minimum((xy[:, 1] / 40).astype(int), 5)])
        coverage = len(np.unique(cells, axis=0)) / 48
        count = len(xy)
    return dict(sharpness=sharpness, corner_coverage=coverage, corners=count,
                dark_fraction=float((gray < 5).mean()), bright_fraction=float((gray > 250).mean()))


def choose_frames(anchors, metrics, excluded, mode, radius=6):
    if mode not in ('streak_only', 'quality_coverage'):
        raise ValueError('Unknown selection mode')
    if radius < 0:
        raise ValueError('Negative search radius')
    selected, decisions = [], []
    for anchor in anchors:
        candidates = [i for i in range(anchor - radius, anchor + radius + 1)
                      if i in metrics and metrics[i].get('decoded') and i not in excluded]
        if not candidates:
            raise ValueError(f'No valid nearby candidate for source row {anchor}')
        if mode == 'streak_only':
            best = min(candidates, key=lambda i: (abs(i - anchor), i))
        else:
            # Local ranks avoid confusing a low-texture view with motion blur.
            # Candidates remain inside a short, non-overlapping temporal bin.
            n = len(candidates)
            score = {i: .7 * sum(metrics[j]['sharpness'] <= metrics[i]['sharpness'] for j in candidates) / n
                     + .3 * sum(metrics[j]['corner_coverage'] <= metrics[i]['corner_coverage'] for j in candidates) / n
                     for i in candidates}
            best = max(candidates, key=lambda i: (score[i], -abs(i - anchor), -i))
        selected.append(best)
        decisions.append(dict(anchor=anchor, selected=best, candidates=candidates,
                              anchor_excluded=anchor in excluded, metrics=metrics[best]))
    if len(set(selected)) != len(selected) or selected != sorted(selected):
        raise ValueError('Candidate bins overlap or produce non-increasing selections')
    return selected, decisions


def prepare(session, output, excluded_ranges):
    session, output = Path(session), Path(output)
    if output.exists(): raise ValueError('Choose a new experiment directory')
    rows = [json.loads(line) for line in (session / 'frames.jsonl').read_text().splitlines()]
    anchors = list(range(54, 640, 15))
    if len(rows) <= max(anchors) + 6:
        raise ValueError('This fixed experiment needs the complete 658-frame couch capture')
    excluded = {i for low, high in excluded_ranges for i in range(low, high + 1)}
    candidates = sorted({i for a in anchors for i in range(a - 6, a + 7)})
    metrics = {}
    cv2.setNumThreads(2)
    for i in candidates:
        path = session / rows[i]['image']
        if not path.resolve().is_relative_to((session / 'images').resolve()):
            raise ValueError('Image outside recording')
        image = cv2.imread(str(path))
        metrics[i] = dict(decoded=image is not None,
            sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            **(image_quality(image) if image is not None else {}))
    output.mkdir(parents=True)
    plan = dict(schema=1, source_session=session.name, anchors=anchors,
        candidate_radius_frames=6, windows=[0, 8, 16], frames=24, resolution=504,
        variant='large', repeats=2, undistort=True, center_principal=False,
        excluded_ranges=excluded_ranges,
        exclusion_note='Previously visually reviewed colour-streak interval; no generic corruption detector is claimed.',
        selection_rule='Streak-only: nearest eligible frame. Quality: 70% local sharpness rank + 30% feature-cell coverage rank; nearest frame breaks ties.',
        acceptance='Unmodified Mapping.window_replay local/overlap screens. No parameter search or threshold relaxation after inference.',
        evaluation='Same nominal temporal bins and 24-image budget; changed-image comparisons are selection ablations, not paired pixel-error comparisons.',
        metric_scale_available=False, live=False, imu_used=False, cases={})
    for mode in ('streak_only', 'quality_coverage'):
        indices, decisions = choose_frames(anchors, metrics, excluded, mode)
        dest = output / mode / 'input' / session.name
        (dest / 'images').mkdir(parents=True)
        selected_rows = []
        for i in indices:
            row = dict(rows[i], source_row_index=i)
            selected_rows.append(row)
            shutil.copyfile(session / row['image'], dest / row['image'])
        manifest = json.loads((session / 'manifest.json').read_text())
        manifest.update(frames_saved=len(indices), kind='selected_ai_input_only',
                        source_frames_saved=len(rows), selection_case=mode)
        dump(dest / 'manifest.json', manifest)
        shutil.copyfile(session / 'camera-calibration.json', dest / 'camera-calibration.json')
        (dest / 'frames.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in selected_rows))
        timestamps = np.array([r['sensor_timestamp_ns'] for r in selected_rows])
        plan['cases'][mode] = dict(session=str(dest.relative_to(output)), indices=indices,
            changed_frames=sum(i != a for i, a in zip(indices, anchors)),
            max_selected_gap_seconds=float(np.diff(timestamps).max() / 1e9),
            median_sharpness=float(np.median([metrics[i]['sharpness'] for i in indices])),
            median_corner_coverage=float(np.median([metrics[i]['corner_coverage'] for i in indices])),
            decisions=decisions)
    plan['baseline_quality'] = dict(median_sharpness=float(np.median([metrics[i]['sharpness'] for i in anchors])),
        median_corner_coverage=float(np.median([metrics[i]['corner_coverage'] for i in anchors])))
    dump(output / 'plan.json', plan)
    dump(output / 'candidate-quality.json', metrics)
    print(json.dumps({name: {k: case[k] for k in ('indices','changed_frames','median_sharpness','median_corner_coverage')} for name, case in plan['cases'].items()}, indent=2))
    return plan


def run(args):
    root = args.root
    plan = json.loads((root / 'plan.json').read_text())
    config = json.loads((Path(__file__).parent / 'examples/room-chain-20261003/demo.json').read_text())
    verify_assets(args.source, args.model, dict(config, model=config['models']['large']))
    metrics = json.loads((root / 'candidate-quality.json').read_text())
    for mode, case in plan['cases'].items():
        session = contained(root, case['session'])
        rows = [json.loads(line) for line in (session / 'frames.jsonl').read_text().splitlines()]
        if [r.get('source_row_index') for r in rows] != case['indices']:
            raise ValueError('Prepared frame selection differs from plan')
        for row in rows:
            if digest(contained(session, row['image'])) != metrics[str(row['source_row_index'])]['sha256']:
                raise ValueError('Prepared input image checksum differs')
        for i, start in enumerate(plan['windows']):
            output = root / mode / f'window-{i}'
            if output.exists(): raise ValueError('Inference outputs already exist; use a fresh experiment copy')
    runs = []
    for mode, case in plan['cases'].items():
        for i, start in enumerate(plan['windows']):
            output = root / mode / f'window-{i}'
            cmd = [str(args.python), '-m', 'Mapping.da3_trial', '--source', str(args.source),
                '--model', str(args.model), '--session', str(root / case['session']),
                '--output', str(output), '--start-frame', str(start), '--stride', '1',
                '--frames', '24', '--resolution', '504', '--device', args.device,
                '--variant', 'large', '--repeats', '2', '--mps-memory-fraction', '.6', '--undistort']
            print('START', mode, i, flush=True)
            with (root / mode / f'window-{i}.log').open('w') as log:
                completed = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
            runs.append(dict(case=mode, window=i, returncode=completed.returncode, command=cmd))
            dump(root / 'runs.json', runs)
            if completed.returncode: raise RuntimeError(f'Inference failed: {mode}/{i}; see preserved log')
            print('DONE', mode, i, flush=True)


def evaluate(paths, output):
    result = replay(paths, output)
    result['pair_checks'] = []
    for first, second in zip(paths, paths[1:]):
        try:
            _, check = overlap_transform(load_trial(first), load_trial(second))
        except ValueError as exc:
            check = dict(accepted=False, error=str(exc))
        result['pair_checks'].append(check)
    # Surface-only fit is kept unchanged; all comparisons use the same screens.
    return result


def report(root, baseline):
    root = Path(root)
    plan = json.loads((root / 'plan.json').read_text())
    trials = []
    selections = dict(baseline=baseline, **{mode: [root / mode / f'window-{i}' for i in range(3)] for mode in plan['cases']})
    for name, paths in selections.items():
        value = evaluate(paths, root / (name + '-replay'))
        value.update(name=name, selected_indices=plan['anchors'] if name == 'baseline' else plan['cases'][name]['indices'])
        value['retained_views'] = len(value['poses'])
        value['retained_span_seconds'] = max((p['seconds'] for p in value['poses']), default=0)
        value['first_camera_rotation'] = load_trial(paths[0])['arrays']['extrinsics'][0, :, :3].tolist()
        points = value['points']
        if len(points) > 15000:
            value['points'] = [points[i] for i in np.linspace(0, len(points) - 1, 15000, dtype=int)]
        trials.append(value)
    summary = dict(plan=plan, trials=[{k: v for k, v in t.items() if k not in ('points', 'poses', 'images')} for t in trials],
        note='Same acceptance screens. No physical accuracy, IMU, live stream or production default change. Selected images differ between cases.')
    dump(root / 'comparison.json', summary)
    payload = dict(trials=trials, note=summary['note'])
    template = (Path(__file__).parent / 'static/selection-replay.html').read_text()
    encoded = json.dumps(payload, separators=(',', ':'), allow_nan=False).replace('<', '\\u003c')
    (root / 'index.html').write_text(template.replace('/*SELECTION_DATA*/null', encoded))
    for t in trials:
        print(t['name'], t['accepted_windows'], 'windows', t['retained_views'], 'selected poses', flush=True)
    return summary


def copy_inputs(bundle, output):
    """Verify the published experiment, then copy only inputs into a fresh run."""
    if output.exists(): raise ValueError('Choose a new output directory')
    for name, checksum in json.loads((bundle / 'SHA256SUMS.json').read_text()).items():
        if digest(contained(bundle, name)) != checksum:
            raise ValueError('Bundle checksum mismatch: ' + name)
    plan = json.loads((bundle / 'plan.json').read_text())
    output.mkdir(parents=True)
    for name in ('plan.json', 'candidate-quality.json'):
        shutil.copyfile(bundle / name, output / name)
    for case in plan['cases'].values():
        destination = contained(output, case['session'])
        source = contained(bundle, case['session'])
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, destination)
    print('Verified and copied selected inputs to', output)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    prep = sub.add_parser('prepare')
    prep.add_argument('--session', type=Path, required=True)
    prep.add_argument('--output', type=Path, required=True)
    prep.add_argument('--exclude-range', nargs=2, type=int, action='append', default=[])
    infer = sub.add_parser('run')
    for field in ('root', 'source', 'model', 'python'):
        infer.add_argument('--' + field, type=Path, required=True)
    infer.add_argument('--device', choices=('cpu', 'mps'), default='cpu')
    compare = sub.add_parser('report')
    compare.add_argument('--root', type=Path, required=True)
    compare.add_argument('--baseline', nargs=3, type=Path, required=True)
    copy = sub.add_parser('copy-inputs')
    copy.add_argument('--bundle', type=Path, default=Path(__file__).parent / 'examples/selection-followup-20261003')
    copy.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.command == 'prepare': prepare(args.session, args.output, args.exclude_range)
    elif args.command == 'run': run(args)
    elif args.command == 'report': report(args.root, args.baseline)
    else: copy_inputs(args.bundle, args.output)


if __name__ == '__main__': main()
