"""Record verified artifacts and explicit device-validation limits for handoff."""
import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--installed-apk', type=Path, help='APK pulled from the Quest after installation; omit for a pending candidate')
parser.add_argument('--headset-report', type=Path, help='Observed device/wearer results for this APK, as JSON')
args = parser.parse_args()
evidence = root / 'Saved/NavigationVerification'
package = json.loads((evidence / 'package-verification.json').read_text())
assert package['passed'], 'Package verification must pass first'
reports = sorted((evidence / 'TestRuns').glob('*/Report/index.json'))
assert reports, 'Automation report missing'
tests = json.loads(reports[-1].read_text(encoding='utf-8-sig'))
assert len(tests['tests']) >= 75 and all(t['state'] == 'Success' for t in tests['tests'])

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest().upper()

installed_hash = digest(args.installed_apk) if args.installed_apk else None
if installed_hash:
    assert installed_hash == package['apk_sha256'], 'Installed APK differs from verified candidate'
headset = {
    'installation_verified': bool(installed_hash),
    'runtime_guidance_verified': False,
    'measured_game_fps': None,
    'permission_interaction_verified': False,
    'physical_connected_rooms_moved_obstacle_test': 'Pending wearer',
    'notes': 'No runtime or wearer evidence has been recorded for this APK.'
}
previous_path = evidence / 'delivery.json'
if previous_path.exists():
    previous = json.loads(previous_path.read_text(encoding='utf-8-sig'))
    if previous.get('apk_sha256') == package['apk_sha256']:
        headset = previous.get('headset_validation', headset)
        installed_hash = installed_hash or previous.get('installed_apk_sha256')
if args.headset_report:
    observed = json.loads(args.headset_report.read_text(encoding='utf-8-sig'))
    assert observed.get('apk_sha256') == package['apk_sha256'], 'Headset evidence belongs to another APK'
    headset = observed['headset_validation']
headset['installation_verified'] = bool(installed_hash)
signing = (evidence / 'signing.txt').read_text(encoding='utf-8-sig')
assert 'Verified using v2 scheme (APK Signature Scheme v2): true' in signing
paths = [p for p in (root / 'Source/HandoffQuestHUD').rglob('*') if p.suffix in {'.h', '.cpp', '.cs'}]
paths += list((root / 'Config').glob('*.ini')) + [root / 'HandoffQuestHUD.uproject']
paths += list((root / 'Content/Materials').glob('*.uasset')) + list((root / 'Content/People').glob('*.uasset'))
result = {
    'recorded_at_utc': datetime.now(timezone.utc).isoformat(),
    'package': 'com.wallhack.questhud.navigation',
    'apk': str(evidence / 'Package/Android_ASTC/HandoffQuestHUD-arm64.apk'),
    'apk_sha256': package['apk_sha256'],
    'native_sha256': package['native_sha256'],
    'signature_verified': True,
    'installed_apk_sha256': installed_hash,
    'package_checks_passed': len(package['checks']),
    'automation_report': str(reports[-1]),
    'tests_passed': len(tests['tests']),
    'test_warnings': tests.get('succeededWithWarnings', 0),
    'full_cook_log': str(evidence / 'FullPackage.log'),
    'render_evidence': str(evidence / 'Render'),
    'rollback': {p.name: digest(p) for p in evidence.glob('Rollback-*.apk')},
    'source_config_material_sha256': {p.relative_to(root).as_posix(): digest(p) for p in sorted(paths)},
    'headset_validation': headset
}
(evidence / 'delivery.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps({k: result[k] for k in ('apk', 'apk_sha256', 'tests_passed', 'package_checks_passed')}, indent=2))
