"""Capture current-user Quest evidence, retaining the full display and both halves.

Display captures are evidence for inspection, not automatic proof of eye visibility.
A blank frame (headset asleep/locked) never passes visual verification.
"""
import argparse
import hashlib
import io
import json
import re
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from PIL import Image, ImageStat

root = Path(__file__).resolve().parents[1] / 'Saved/NavigationVerification'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path)
parser.add_argument('--serial', default='340YC10GBN16H0')
parser.add_argument('--adb', default='C:/Users/kevin/AppData/Local/Android/Sdk/platform-tools/adb.exe')
args = parser.parse_args()
run = args.output or Path((root / 'CurrentHeadsetRun.txt').read_text(encoding='utf-8-sig').strip())
run.mkdir(parents=True, exist_ok=True)
apk_hash = hashlib.sha256((root / 'Installed-Navigation.apk').read_bytes()).hexdigest().upper()
report_path = run / 'headset-report.json'
previous = json.loads(report_path.read_text(encoding='utf-8-sig')) if report_path.exists() else {}
validation = previous.get('headset_validation', {}) if previous.get('apk_sha256') == apk_hash else {}

def query(*parts, required=True):
    result = subprocess.run([args.adb, '-s', args.serial, *parts], capture_output=True, timeout=25)
    if required and result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace'))
    return result.stdout

query('get-state')
user = query('shell', 'am', 'get-current-user').decode().strip()
processes = query('shell', 'ps', '-A', '-o', 'USER,PID,NAME').decode(errors='replace')
pid = next((parts[1] for line in processes.splitlines() if len(parts := line.split()) == 3
            and parts[2] == 'com.wallhack.questhud.navigation'
            and (parts[0].startswith(f'u{user}_') or
                 (parts[0].isdigit() and int(parts[0]) // 100000 == int(user)))), None)
activity = query('shell', 'dumpsys', 'activity', 'activities').decode(errors='replace')
(run / 'activities.txt').write_text(activity, encoding='utf-8')
log = query('logcat', '-d', f'--pid={pid}', '-s', 'UE').decode(errors='replace') if pid else ''
# Quest's shared logcat ring can evict startup evidence within seconds. Retain
# earlier lines only for this exact APK/process, never another Android profile/run.
old_log = run / 'app-logcat.txt'
if pid and validation.get('process_id') == int(pid) and validation.get('android_user') == user and old_log.exists():
    log = '\n'.join(dict.fromkeys((old_log.read_text(encoding='utf-8') + '\n' + log).splitlines())) + '\n'
(run / 'app-logcat.txt').write_text(log, encoding='utf-8')
frame = query('exec-out', 'screencap', '-p', required=False)
capture = {'valid_png': False, 'nonblank': False, 'both_eyes_verified': False}
if frame.startswith(b'\x89PNG'):
    im = Image.open(io.BytesIO(frame)).convert('RGB')
    im.save(run / 'display-full.png')
    capture.update(valid_png=True, dimensions=list(im.size),
                   nonblank=max(ImageStat.Stat(im).mean) > 1,
                   eye_layout='Unverified display layout; inspect full frame before treating halves as eyes.')
    for name, bounds in (('left', (0, 0, im.width // 2, im.height)),
                         ('right', (im.width // 2, 0, im.width, im.height))):
        half = im.crop(bounds)
        half.save(run / f'display-{name}-half.png')
        capture[f'{name}_half_nonblank'] = max(ImageStat.Stat(half).mean) > 1
    im.thumbnail((1600, 1000))
    im.save(run / 'launch-screen.jpg')
fps = [float(v) for v in re.findall(r'game_fps=([\d.]+)', log)]
settings = re.findall(r'Wallhack stereo: MobileHDR=(\d+) ShadingPath=(\d+) MultiView=(\d+) MobileAA=(\d+) Alpha=(\d+)', log)
validation.update(
    application_launch_verified=bool(pid) and any(
        any(marker in line for marker in ('topResumedActivity=', 'ResumedActivity:', 'Resumed:', 'mResumedActivity:'))
        and 'com.wallhack.questhud.navigation/' in line for line in activity.splitlines()),
    process_id=int(pid) if pid else None, android_user=user, display_capture=capture,
    scene_loading_verified='Wallhack navigation: MRUK loaded' in log,
    localization_and_live_checks_verified='ROOM LOCK / LIVE CHECKS' in log,
    resolved_stereo_settings=list(map(int, settings[-1])) if settings else None,
    resolved_stereo_settings_order=['MobileHDR', 'ShadingPath', 'MultiView', 'MobileAA', 'Alpha'],
    stereo_settings_verified=bool(settings) and settings[-1] == ('0', '0', '1', '3', '1'),
    measured_game_fps={'samples': len(fps), 'minimum': min(fps), 'maximum': max(fps), 'mean': sum(fps)/len(fps)} if fps else None,
    performance_scope='Captured process samples; workload must be described by the wearer.',
    evidence=str(run),
)
validation.setdefault('both_eyes_wearer_verified', False)
validation.setdefault('notes', 'Paired desktop eye-position captures do not establish Android multiview correctness. Quest visual check pending.')
report = {'recorded_at_utc': datetime.now(timezone.utc).isoformat(), 'apk_sha256': apk_hash, 'headset_validation': validation}
report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report, indent=2))
