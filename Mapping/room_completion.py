"""Offline depth -> semantics -> structure -> optional completion experiment.

No training, metric calibration, live poses or navigation occupancy is produced.
View-supported surfaces and completion hypotheses remain separately removable.
The evaluation holds out views from plane fitting, but DA3 itself has seen all
input images. These are consistency checks, never independent accuracy scores.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import time

import cv2
import numpy as np

from Mapping.ai_room_draft import project_points
from Mapping.da3_inspect import centers, diagnostics, jpeg_data
from Mapping.operator_sketch import category, fit_floor, map_basis
from Mapping.window_replay import load_trial


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')


def cells_from_points(points, views, size, minimum_views=2):
    buckets = defaultdict(set)
    for cell, view in zip(np.floor(np.asarray(points)[:, :2] / size).astype(int), views):
        buckets[tuple(int(x) for x in cell)].add(int(view))
    return {key for key, support in buckets.items() if len(support) >= minimum_views}


def complete_cells(seen, blocked=(), radius=2):
    """Fill only nearby gaps inside the evidence hull; never claim free space.

    A bounded closing operation leaves wide unknown regions empty. Blocking
    cells represent known object/wall/opening projections, not unseen obstacles.
    """
    if radius not in (1, 2, 3):
        raise ValueError('Use a bounded 1–3 cell radius')
    seen, blocked = set(seen), set(blocked)
    if len(seen) < 3:
        return set()
    points = np.array(sorted(seen), dtype=np.int32)
    low = points.min(0) - radius - 2
    high = points.max(0) + radius + 2
    width, height = high - low + 1
    if max(width, height) > 1024:
        raise ValueError('Completion grid exceeds bounded experiment size')
    shifted = points - low
    mask = np.zeros((height, width), np.uint8)
    mask[shifted[:, 1], shifted[:, 0]] = 1
    hull = np.zeros_like(mask)
    cv2.fillConvexPoly(hull, cv2.convexHull(shifted).reshape(-1, 2), 1)
    kernel = np.ones((2 * radius + 1, 2 * radius + 1), np.uint8)
    closed = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)
    distance = cv2.distanceTransform(1 - mask, cv2.DIST_L2, 5)
    yy, xx = np.where((closed > 0) & (mask == 0) & (hull > 0) & (distance <= radius))
    return {(int(x + low[0]), int(y + low[1])) for x, y in zip(xx, yy)} - blocked


def fit_walls(points, views, scale, max_planes=4):
    """Fit vertical wall patches on even views; report odd-view support."""
    points, views = np.asarray(points), np.asarray(views)
    if len(points) < 100:
        return []
    remaining = (views % 2 == 0)
    check = points[views % 2 == 1]
    rng = np.random.default_rng(31)
    tolerance = .025 * scale
    walls = []
    for _ in range(max_planes):
        ids = np.flatnonzero(remaining)
        if len(ids) < 100:
            break
        subset = ids[rng.choice(len(ids), min(5000, len(ids)), replace=False)]
        xy = points[subset, :2]
        best = np.zeros(len(xy), bool)
        for _ in range(160):
            a, b = xy[rng.choice(len(xy), 2, replace=False)]
            tangent = b - a
            if np.linalg.norm(tangent) < .12 * scale:
                continue
            tangent /= np.linalg.norm(tangent)
            normal = np.array([-tangent[1], tangent[0]])
            mask = np.abs((xy - a) @ normal) < tolerance
            if mask.sum() > best.sum():
                best = mask
        if best.sum() < 100:
            break
        anchor = xy[best].mean(0)
        _, _, vt = np.linalg.svd(xy[best] - anchor, full_matrices=False)
        tangent = vt[0]
        normal = np.array([-tangent[1], tangent[0]])
        inliers = remaining & (np.abs((points[:, :2] - anchor) @ normal) < tolerance)
        remaining[inliers] = False
        support = np.unique(views[inliers])
        extent = (points[inliers, :2] - anchor) @ tangent
        lo, hi = np.percentile(extent, [2, 98])
        if len(support) < 2 or hi - lo < .15 * scale:
            continue
        holdout_along = (check[:, :2] - anchor) @ tangent
        holdout_error = np.abs((check[:, :2] - anchor) @ normal)
        matched = (holdout_along >= lo) & (holdout_along <= hi) & (holdout_error < tolerance)
        walls.append(dict(id=f'wall-{len(walls)}',
            a=(anchor + lo * tangent).tolist(), b=(anchor + hi * tangent).tolist(),
            height=float(max(.05 * scale, np.percentile(points[inliers, 2], 95))),
            fit_views=support.tolist(), fit_points=int(inliers.sum()),
            heldout_points_within_tolerance=int(matched.sum()),
            heldout_median_residual=float(np.median(holdout_error[matched])) if matched.any() else None,
            evidence='Vertical plane fitted to labelled wall depth; panel interiors are interpolated',
            state='view_supported_estimate', validated=False))
    return walls


def room_envelope(points, scale):
    """An explicitly arbitrary rectangular envelope, not a room measurement."""
    if len(points) < 20:
        return None
    xy = np.asarray(points)[:, :2]
    low, high = np.percentile(xy, [2, 98], axis=0)
    xy = xy[((xy >= low) & (xy <= high)).all(1)]
    if len(xy) < 10:
        return None
    rect = cv2.minAreaRect(xy.astype(np.float32))
    if min(rect[1]) < .08 * scale:
        return None
    return dict(corners=cv2.boxPoints(rect).astype(float).tolist(),
        height=float(max(.3 * scale, np.percentile(points[:, 2], 95))),
        state='unsupported_envelope', measured_boundary=False,
        assumption='Rectangle around the middle 96% of visible points. Unseen room extent and wall height are unconstrained; this can cut through the real room.')


def extract(trial):
    value = load_trial(trial)
    a, r = value['arrays'], value['report']
    sem = dict(np.load(Path(trial) / 'semantics.npz', allow_pickle=False))
    sr = json.loads((Path(trial) / 'semantics.json').read_text())
    if sem['labels'].shape != a['depth'].shape or sem['scores'].shape != a['depth'].shape:
        raise ValueError('Semantic/depth image geometry differs')
    if not np.isfinite(sem['scores']).all():
        raise ValueError('Invalid semantic scores')
    # Cache provenance is tied to the exact processed image, not just its shape.
    model_hash = sr['model']['files']['model.safetensors']['sha256']
    expected_keys = [hashlib.sha256(im.tobytes() + model_hash.encode()).hexdigest() for im in a['images']]
    if expected_keys != sr['image_cache_keys']:
        raise ValueError('Semantic labels were not computed for these exact images')
    lookup = {int(k): category(v) for k, v in sr['id2label'].items()}
    h, w = a['depth'].shape[1:]
    ys, xs = np.mgrid[0:h:4, 0:w:4]
    points, views, cats, rgb = [], [], [], []
    for i in range(len(a['images'])):
        valid = (a['confidence'][i, ys, xs].ravel() >= np.percentile(a['confidence'][i], 40))
        valid &= (a['depth'][i, ys, xs].ravel() < np.percentile(a['depth'][i], 99))
        points.append(project_points(a['depth'][i], a['intrinsics'][i], a['extrinsics'][i], ys, xs)[valid])
        views.extend([i] * int(valid.sum()))
        cats.extend([lookup.get(int(l)) if s >= .6 else None for l, s in zip(
            sem['labels'][i, ys, xs].ravel()[valid], sem['scores'][i, ys, xs].ravel()[valid])])
        rgb.append(a['images'][i, ys, xs].reshape(-1, 3)[valid])
    return value, np.concatenate(points), np.asarray(views), np.asarray(cats, object), np.concatenate(rgb)


def build(trial, label=None):
    started = time.perf_counter()
    trial = Path(trial)
    value, points, views, cats, rgb = extract(trial)
    a, r = value['arrays'], value['report']
    scale = float(np.median(a['depth']))
    cc = centers(a['extrinsics'])
    floor_mask = (cats == 'floor')
    plane, floor_report = fit_floor(points[floor_mask], views[floor_mask], cc, scale)
    check = diagnostics(a)
    local_ok = (check['median_of_pair_medians_pixels'] is not None and
        check['median_of_pair_medians_pixels'] <= .02 * a['images'].shape[2] and
        check['pairs_with_at_least_8_matches'] >= max(2, len(a['images']) // 2))
    result = dict(name=trial.name, label=label or trial.name, model=r['model'],
        session=r['session_id'], indices=r['indices'], source_trial=str(trial),
        prediction_sha256=hashlib.sha256((trial / 'prediction.npz').read_bytes()).hexdigest(),
        semantic_model_sha256=json.loads((trial / 'semantics.json').read_text())['model']['files']['model.safetensors']['sha256'],
        units='arbitrary', metric_scale_available=False, live=False, imu_used=False,
        mapping_eligible=False, floor=floor_report, local_consistency_passed=local_ok,
        diagnostics=check, points=[], floor_cells=[], filled_cells=[], walls=[], objects=[],
        envelope=None, camera_path=[], floor_holdout={'unavailable': 'No supported floor'},
        completion_holdout=None, images=[jpeg_data(im) for im in a['images']],
        overlays=[], selected_span_seconds=r['selected_span_seconds'],
        inference_seconds=r['inference_seconds'], cell_size=.035 * scale,
        warning='Exploratory layout. All depth and positions are estimates. Completion never establishes free space, unseen people, or measured room dimensions.')
    if plane is None:
        result.update(state='no_supported_floor', postprocess_seconds=time.perf_counter() - started)
        return result
    rotation, origin = map_basis(plane, cc[0], a['extrinsics'][0, :, :3].T)
    mapped = (points - origin) @ rotation.T
    result['map_from_da3'] = dict(rotation=rotation.tolist(), origin=origin.tolist())
    result['camera_path'] = ((cc - origin) @ rotation.T).tolist()
    result['state'] = 'provisional_structure' if local_ok else 'unstable_alignment'
    # Hold out odd views from floor plane fitting. DA3 saw them: consistency only.
    train = floor_mask & (views % 2 == 0)
    test = floor_mask & (views % 2 == 1)
    train_floor, _ = fit_floor(points[train], views[train], cc, scale)
    if train_floor is not None and test.any():
        residual = np.abs((points[test] - train_floor['origin']) @ train_floor['normal']) / scale
        result['floor_holdout'] = dict(fit_views=np.unique(views[train]).tolist(),
            check_views=np.unique(views[test]).tolist(), median_fraction_of_depth=float(np.median(residual)),
            fraction_within_2_5_percent_depth=float((residual < .025).mean()),
            note='Views withheld from plane fitting only; shared DA3 inference and semantics, no ground truth.')
    else:
        result['floor_holdout'] = dict(unavailable='Insufficient floor support in disjoint views')
    size = result['cell_size']
    floor_valid = floor_mask & (np.abs(mapped[:, 2]) < .025 * scale)
    seen = cells_from_points(mapped[floor_valid], views[floor_valid], size)
    blocked_mask = np.isin(cats, ['object', 'wall', 'door', 'window']) & (mapped[:, 2] > .08 * scale)
    blocked = cells_from_points(mapped[blocked_mask], views[blocked_mask], size)
    fill = complete_cells(seen, blocked)
    result['floor_cells'] = [list(c) for c in sorted(seen)]
    result['filled_cells'] = [list(c) for c in sorted(fill)]
    # Same operation evaluated with training-view cells only; odd-view cells are
    # support checks. New areas absent from the odd views remain unverified.
    training_cells = cells_from_points(mapped[floor_valid & (views % 2 == 0)], views[floor_valid & (views % 2 == 0)], size)
    test_cells = cells_from_points(mapped[floor_valid & (views % 2 == 1)], views[floor_valid & (views % 2 == 1)], size)
    train_block = cells_from_points(mapped[blocked_mask & (views % 2 == 0)], views[blocked_mask & (views % 2 == 0)], size)
    test_block = cells_from_points(mapped[blocked_mask & (views % 2 == 1)], views[blocked_mask & (views % 2 == 1)], size)
    predicted = complete_cells(training_cells, train_block)
    result['completion_holdout'] = dict(added_cells=len(predicted),
        supported_by_other_views=len(predicted & test_cells),
        overlapping_other_view_object_or_wall=len(predicted & test_block),
        unsupported_by_other_views=len(predicted - test_cells),
        note='A projected object can stand above floor. These are ambiguity flags, not physical accuracy or collision labels.')
    wall_mask = (cats == 'wall') & (mapped[:, 2] > .08 * scale) & (mapped[:, 2] < 3 * scale)
    result['walls'] = fit_walls(mapped[wall_mask], views[wall_mask], scale)
    # Components describe observed object regions; boxes deliberately avoid
    # inventing a class, hidden object back face or calibrated dimensions.
    obj = cells_from_points(mapped[(cats == 'object') & (mapped[:, 2] > .08 * scale)], views[(cats == 'object') & (mapped[:, 2] > .08 * scale)], size)
    remaining = set(obj)
    while remaining:
        component = {remaining.pop()}; stack = list(component)
        while stack:
            x, y = stack.pop()
            neighbors = {(x + dx, y + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)} & remaining
            remaining -= neighbors; component |= neighbors; stack.extend(neighbors)
        if len(component) >= 6:
            grid = np.array(sorted(component))
            result['objects'].append(dict(minimum=(grid.min(0) * size).tolist(),
                maximum=((grid.max(0) + 1) * size).tolist(), cells=len(component),
                label='Object region', state='view_supported_estimate'))
    result['objects'] = sorted(result['objects'], key=lambda x: -x['cells'])[:12]
    keep = (mapped[:, 2] >= -.1 * scale) & (mapped[:, 2] < 3 * scale)
    result['envelope'] = room_envelope(mapped[keep], scale)
    sample = np.flatnonzero(keep)
    sample = sample[np.linspace(0, len(sample) - 1, min(14000, len(sample)), dtype=int)]
    result['points'] = np.column_stack([mapped[sample], rgb[sample], views[sample]]).round(6).tolist()
    sem_colors = {'floor':[104,157,130], 'wall':[132,159,196], 'object':[221,169,94], 'door':[194,136,213], 'window':[109,185,194]}
    sem = dict(np.load(trial / 'semantics.npz', allow_pickle=False))
    names = json.loads((trial / 'semantics.json').read_text())['id2label']
    for i, image in enumerate(a['images']):
        overlay = image.copy()
        for ident, name in names.items():
            cat = category(name)
            if cat not in sem_colors: continue
            mask = (sem['labels'][i] == int(ident)) & (sem['scores'][i] >= .6)
            overlay[mask] = (overlay[mask] * .5 + np.array(sem_colors[cat]) * .5).astype(np.uint8)
        result['overlays'].append(jpeg_data(overlay))
    result['postprocess_seconds'] = time.perf_counter() - started
    return result


def compact_packet(result):
    return dict(schema='wallhack.room_hypothesis.v1', map_id=result['session'] + '/' + result['name'],
        units='arbitrary', live=False, mapping_eligible=False, source='recorded_ai_chain',
        state=result['state'], floor_cells=result['floor_cells'], cell_size=result['cell_size'],
        walls=result['walls'], objects=result['objects'],
        completion=dict(state='hypothesis', floor_cells=result['filled_cells'], envelope=result['envelope']),
        unknown_is_free=False, people=[], metric_scale_available=False)


def generate(trials, output, labels=None):
    output = Path(output)
    if output.exists(): raise ValueError('Choose a new output directory')
    if labels is not None and len(labels) != len(trials): raise ValueError('One label per trial')
    values = [build(t, labels[i] if labels else None) for i, t in enumerate(trials)]
    output.mkdir(parents=True)
    for value in values:
        packet = json.dumps(compact_packet(value), separators=(',', ':'), allow_nan=False).encode()
        (output / (value['name'] + '-packet.json')).write_bytes(packet + b'\n')
        value['packet_bytes'] = len(packet) + 1
        value['packet_payload_seconds'] = {str(bps): 8 * (len(packet) + 1) / bps for bps in (9600, 57600, 115200, 1000000)}
    payload = dict(schema='wallhack.room_completion_comparison.v1', trials=values,
        evaluation_note='No model weights trained. Geometry/completion consistency only; no physical room ground truth.')
    encoded = json.dumps(payload, separators=(',', ':'), allow_nan=False).replace('<', '\\u003c')
    template = (Path(__file__).parent / 'static/room-completion.html').read_text()
    (output / 'index.html').write_text(template.replace('/*COMPLETION_DATA*/null', encoded))
    summary = {**payload, 'trials': [{k: v for k, v in item.items() if k not in ('points', 'images', 'overlays')} for item in values]}
    write_json(output / 'comparison.json', summary)
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trials', nargs='+', type=Path, required=True)
    parser.add_argument('--labels', nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = generate(args.trials, args.output, args.labels)
    for trial in result['trials']:
        print(json.dumps({k: trial[k] for k in ('name', 'state', 'floor_holdout', 'completion_holdout', 'packet_bytes')}))
