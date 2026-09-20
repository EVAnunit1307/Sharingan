"""Record explicitly labelled bench trials; never infer the scene's ground truth."""
import json
import math
from pathlib import Path
import threading
import time
import uuid

DEFAULT_DIRECTORY = Path(__file__).resolve().parents[1] / 'Radar' / 'trials'


def summarize(rows, right_m=None, forward_m=None, outages=0):
    errors = []
    if right_m is not None:
        for row in rows:
            # Multiple detections cannot be associated with ground truth here.
            if len(row['targets']) == 1:
                target = row['targets'][0]
                errors.append(math.hypot(target['right_m']-right_m, target['forward_m']-forward_m))
    hits = sum(bool(row['targets']) for row in rows)
    return dict(frames=len(rows), frames_with_target=hits,
        target_frame_fraction=round(hits/len(rows),4) if rows else None,
        ambiguous_frames=sum(len(row['targets'])>1 for row in rows), outage_polls=outages,
        position_samples=len(errors),
        mean_position_error_m=round(sum(errors)/len(errors),3) if errors else None,
        max_position_error_m=round(max(errors),3) if errors else None,
        through_wall_verified=False)


class RadarTrials:
    def __init__(self, radar, directory=DEFAULT_DIRECTORY):
        self.radar = radar
        self.directory = Path(directory)
        self.lock = threading.Lock()
        self.current = {'status': 'idle'}
        self.reports = []
        for path in sorted(self.directory.glob('*.json'), key=lambda p: p.stat().st_mtime)[-20:]:
            try:
                report = json.loads(path.read_text())
                if isinstance(report, dict) and 'summary' in report and 'label' in report:
                    self.reports.append({key: value for key, value in report.items() if key != 'frames'})
            except (OSError, ValueError):
                continue

    def start(self, label, right_m=None, forward_m=None, duration_s=12):
        if label not in ('empty', 'person_clear', 'person_behind_drywall'):
            raise ValueError('Choose empty, person_clear, or person_behind_drywall')
        if type(duration_s) not in (int, float) or not math.isfinite(duration_s) or not 5 <= duration_s <= 30:
            raise ValueError('Trial duration must be 5–30 seconds')
        for value in (right_m, forward_m):
            if value is not None and (type(value) not in (float, int) or not math.isfinite(value) or abs(value)>10):
                raise ValueError('Known positions must be finite metre values within 10m')
        if (right_m is None) != (forward_m is None):
            raise ValueError('Provide both known coordinates, or leave both empty')
        if label == 'empty' and right_m is not None:
            raise ValueError('Empty-room trials do not have a target position')
        with self.lock:
            if self.current['status'] in ('countdown', 'recording'):
                raise ValueError('A radar trial is already running')
            trial_id = uuid.uuid4().hex[:12]
            self.current = dict(status='countdown', id=trial_id, label=label, remaining_s=5)
        worker = threading.Thread(target=self.record, args=(trial_id,label,right_m,forward_m,duration_s), daemon=True)
        worker.start()
        return self.snapshot()

    def record(self, trial_id, label, right_m, forward_m, duration):
        report = dict(id=trial_id, label=label, expected_right_m=right_m, expected_forward_m=forward_m,
                      duration_s=duration, created_at=time.time(),
                      truth_source='operator label; physical wall/occupancy not independently verified', frames=[])
        try:
            self.directory.mkdir(parents=True, exist_ok=True)
            for left in range(5, 0, -1):
                with self.lock:
                    self.current.update(remaining_s=left)
                time.sleep(1)
            start = time.monotonic()
            report['config'] = self.radar.snapshot()['config']
            last_frame = None
            outages = 0
            while time.monotonic()-start < duration:
                snapshot = self.radar.snapshot()
                elapsed = time.monotonic()-start
                with self.lock:
                    self.current.update(status='recording', remaining_s=round(max(0,duration-elapsed),1))
                if snapshot['config'] != report['config']:
                    raise ValueError('Mount configuration changed during the trial; repeat it')
                if snapshot['status'] != 'live':
                    outages += 1
                elif snapshot['frame_id'] != last_frame:
                    last_frame = snapshot['frame_id']
                    report['frames'].append(dict(t=round(elapsed,3), frame_id=last_frame,
                                                 targets=snapshot['targets'], raw=snapshot['raw_targets']))
                time.sleep(.04)
            report['summary'] = summarize(report['frames'], right_m, forward_m, outages)
            (self.directory/f'{trial_id}.json').write_text(json.dumps(report,indent=2)+'\n')
            brief={key:value for key,value in report.items() if key!='frames'}
            with self.lock:
                self.reports.append(brief)
                self.reports=self.reports[-20:]
                self.current=dict(status='complete',id=trial_id,summary=report['summary'])
        except Exception as exc:
            with self.lock:
                self.current=dict(status='error',id=trial_id,error=str(exc))

    def snapshot(self):
        with self.lock:
            return dict(current=dict(self.current), reports=list(self.reports))
