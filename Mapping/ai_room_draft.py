"""Read-only, exploratory visualization of local AI predictions, including weak ones.

Writes a standalone draft viewer and its manifest, never an operator map or pose
packet. Confidence is a model ranking, not a probability of spatial correctness.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from Mapping.da3_inspect import centers, diagnostics, jpeg_data
from Mapping.operator_sketch import COLORS, category

CATEGORIES = ['unlabelled', *COLORS]
PALETTE = [[160, 160, 160], *COLORS.values()]


def project_points(depth, intrinsics, extrinsics, ys, xs):
    pixels = np.stack([xs, ys, np.ones_like(xs)], axis=-1).reshape(-1, 3)
    camera = (pixels @ np.linalg.inv(intrinsics).T) * depth[ys, xs].reshape(-1, 1)
    return (camera - extrinsics[:, 3]) @ extrinsics[:, :3]


def build_trial(path, max_points=60000):
    path = Path(path)
    report = json.loads((path / 'summary.json').read_text())
    if report.get('state') != 'complete':
        raise ValueError('Draft requires a completed AI prediction')
    if not 1000 <= max_points <= 90000:
        raise ValueError('Use a display budget between 1,000 and 90,000 points')
    with np.load(path / 'prediction.npz', allow_pickle=False) as stored:
        depth, confidence, ex, intr, images = [stored[k] for k in
            ('depth', 'confidence', 'extrinsics', 'intrinsics', 'images')]
    if depth.ndim != 3:
        raise ValueError('Expected one depth image per view')
    n, h, w = depth.shape
    expected = [(confidence, (n, h, w)), (ex, (n, 3, 4)),
                (intr, (n, 3, 3)), (images, (n, h, w, 3))]
    if not 2 <= n <= 24 or min(h, w) < 2 or any(a.shape != shape for a, shape in expected):
        raise ValueError('Prediction image and camera shapes disagree')
    if any(not np.isfinite(a).all() for a in (depth, confidence, ex, intr, images)) or (depth <= 0).any():
        raise ValueError('Prediction contains invalid geometry')
    if len(report['frames']) != n or len(report['indices']) != n:
        raise ValueError('Source-frame provenance does not match prediction')

    labels = scores = None
    names = {}
    sem_path, sem_report_path = path / 'semantics.npz', path / 'semantics.json'
    if sem_path.exists() != sem_report_path.exists():
        raise ValueError('Incomplete semantic prediction')
    if sem_path.exists():
        with np.load(sem_path, allow_pickle=False) as stored:
            labels, scores = stored['labels'], stored['scores']
        if labels.shape != depth.shape or scores.shape != depth.shape or not np.isfinite(scores).all():
            raise ValueError('Semantic prediction geometry disagrees')
        names = json.loads(sem_report_path.read_text())['id2label']

    threshold = np.percentile(confidence, 40)
    far = np.percentile(depth, 99)
    weak = (confidence < threshold) | (depth > far)
    lo, hi = np.percentile(depth, [2, 98])
    gray = np.uint8(255 * (1 - np.clip((depth - lo) / max(float(hi - lo), 1e-8), 0, 1)))
    stride = max(1, int(np.ceil(np.sqrt(n * h * w / max_points))))
    ys, xs = np.mgrid[0:h:stride, 0:w:stride]
    points, views, overlays, image_labels = [], [], [], []
    for i in range(n):
        world = project_points(depth[i], intr[i], ex[i], ys, xs)
        rgb = images[i, ys, xs].reshape(-1, 3)
        cats = np.zeros((h, w), np.uint8)
        overlay = images[i].copy()
        candidates = []
        if labels is not None:
            for ident in np.unique(labels[i]):
                name = names.get(str(int(ident)), 'unlabelled').strip()
                cat = category(name)
                if cat is None:
                    continue
                mask = labels[i] == ident
                cats[mask] = CATEGORIES.index(cat)
                alpha = np.where(scores[i][mask] >= .6, .45, .18)[:, None]
                overlay[mask] = np.rint(overlay[mask] * (1 - alpha) + np.array(COLORS[cat]) * alpha).astype(np.uint8)
                if mask.mean() >= .008:
                    candidates.append(dict(label=name, category=cat,
                        image_fraction=float(mask.mean()), weak_label=bool(np.median(scores[i][mask]) < .6)))
        chunk = np.column_stack([world, rgb, np.full(len(world), i),
            weak[i, ys, xs].ravel(), cats[ys, xs].ravel()])
        points.append(chunk)
        views.append(jpeg_data(np.concatenate([images[i], np.repeat(gray[i, :, :, None], 3, 2)], axis=1)))
        overlays.append(jpeg_data(overlay))
        image_labels.append(sorted(candidates, key=lambda c: -c['image_fraction'])[:8])
    points = np.concatenate(points)
    if len(points) > max_points:
        points = points[np.linspace(0, len(points) - 1, max_points, dtype=int)]
    rows = [[*[round(float(v), 5) for v in p[:3]], *[int(v) for v in p[3:]]] for p in points]
    origin = report['frames'][0]['sensor_timestamp_ns']
    check = diagnostics(dict(depth=depth, confidence=confidence, extrinsics=ex, intrinsics=intr, images=images))
    return dict(name=path.name, session_id=report['session_id'], kind='ai_room_hypothesis',
        model=report.get('model', 'depth-anything/DA3-SMALL'),
        pose_conditioned=report.get('pose_conditioned', False),
        undistorted=report.get('undistorted', False),
        center_principal=report.get('center_principal', False),
        pose_alignment=report.get('pose_alignment'),
        validated=False, live=False, units='arbitrary', metric_scale_available=False,
        pose_estimation=report.get('pose_estimation', 'camera_decoder'),
        mapping_eligible=False, source_trial=str(path.resolve()),
        prediction_sha256=hashlib.sha256((path / 'prediction.npz').read_bytes()).hexdigest(),
        points=rows, centers=centers(ex).round(5).tolist(), camera_rotations=ex[:, :, :3].round(7).tolist(),
        images=views, overlays=overlays,
        labels=image_labels, semantics_available=labels is not None,
        indices=[int(frame.get('source_row_index', index))
                 for frame, index in zip(report['frames'], report['indices'])],
        seconds=[(f['sensor_timestamp_ns'] - origin) / 1e9 for f in report['frames']],
        camera_timestamps_ns=[f['sensor_timestamp_ns'] for f in report['frames']],
        diagnostics=check, inference_seconds=report['inference_seconds'],
        weak_display_points=sum(p[7] for p in rows), display_points=len(rows),
        display_sampling_stride=stride,
        warning='Tentative visible-scene hypothesis. Shape and camera alignment may be wrong. Unseen room boundaries remain unknown.',
        confidence_note='Faint amber points include the lower 40% of depth confidence or farthest 1% of depth; rankings are not calibrated accuracy probabilities. All displayed geometry also depends on uncertain camera poses.')


def generate(trials, output, labels=None):
    output = Path(output)
    manifest = output.with_suffix('.json')
    if output.suffix.lower() != '.html' or output.exists() or manifest.exists():
        raise ValueError('Choose a new .html output; existing results are preserved')
    values = [build_trial(path) for path in trials]
    if not values:
        raise ValueError('Choose at least one completed trial')
    if labels is not None and (len(labels) != len(values) or any(not label.strip() for label in labels)):
        raise ValueError('Supply one nonempty display label per trial')
    for i, value in enumerate(values):
        value['display_label'] = labels[i] if labels is not None else value['name']
    payload = dict(schema='wallhack.ai_room_draft.v1', kind='exploratory_viewer',
        mapping_eligible=False, categories=CATEGORIES, palette=PALETTE, trials=values)
    template = (Path(__file__).parent / 'static/ai-room-draft.html').read_text()
    encoded = json.dumps(payload, separators=(',', ':'), allow_nan=False).replace('<', chr(92) + 'u003c')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(template.replace('/*DRAFT_DATA*/null', encoded))
    summary = {**payload, 'trials': [{k: v for k, v in item.items()
        if k not in ('points', 'images', 'overlays', 'centers')} for item in values]}
    manifest.write_text(json.dumps(summary, indent=2, allow_nan=False) + '\n')
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trials', nargs='+', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--labels', nargs='+', help='Optional human-readable names, one per trial')
    args = parser.parse_args()
    result = generate(args.trials, args.output, args.labels)
    print(json.dumps(dict(output=str(args.output), trials=[dict(name=t['name'],
        points=t['display_points'], weak_points=t['weak_display_points']) for t in result['trials']])))
