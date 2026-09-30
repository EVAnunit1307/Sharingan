"""Conservative offline keyframe experiment; original captures are never deleted."""
import math


def sharpest_windows(rows, window_seconds=2/3):
    """Keep the sharpest observed frame in each time window plus both endpoints.

    This scores relative sharpness locally, without treating a textureless wall
    as proof of blur or discarding an entire time interval. It does not certify
    geometric overlap: benchmark reconstruction before changing capture defaults.
    """
    if window_seconds <= 0:
        raise ValueError('Window must be positive')
    if not rows:
        return []
    origin = rows[0]['host_capture_mono_ns']
    best = {}
    for index, row in enumerate(rows):
        bucket = math.floor((row['host_capture_mono_ns']-origin)/1e9/window_seconds)
        score = row.get('sharpness', 0)
        if not isinstance(score, (float, int)) or not math.isfinite(score):
            score = 0
        if bucket not in best or score > best[bucket][0]:
            best[bucket] = (score, index)
    return sorted({0, len(rows)-1, *(entry[1] for entry in best.values())})
