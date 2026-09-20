# Portable validation evidence

These reports were copied from the running Pi's `HTN2026/sensor-test/reports/`
so a GitHub checkout contains the useful evidence without requiring that sibling
directory. They are observations from this bench, not universal accuracy claims.

| Report | What it establishes |
|---|---|
| `combined_runtime_validation.json` | 30 fresh camera samples; median 10 detections/s, 24.1 capture fps, 51.75 ms inference; radar live at 11.2 Hz; mirror correction preserved |
| `combined_browser_validation.json` | Desktop/mobile/Quest-layout checks, fullscreen, real WebSocket receipt, sensor-independent failures, frozen-data expiry, HTTP recovery and correct left/right plotting |
| `quest_handoff_validation.json` | Four seconds / 40 live WebSocket packets with advancing camera/radar frames; HTTP pages/assets and JPEG present; current mounting and manifest |
| `camera_evaluation_summary.json` | 525-frame development replay summary, config, label corrections and manifest hash; no per-frame imagery/boxes included |
| `radar_browser_validation.json` | Earlier dedicated radar-page browser checks |

At handoff, all 39 unit tests pass. Native Unreal build/deployment, a physical
headset test, actual through-drywall accuracy and moving-drone behavior were not
validated. The camera replay uses development data, not held-out people/rooms or
box annotations. The raw images remain local to the original Pi; a new clone can
run the application/unit tests but cannot reproduce that replay without its data.

Run from the repository root with the correct Python environment:

```sh
.venv/bin/python -m unittest discover -s SensorRig/CV/tests -v
.venv/bin/python SensorRig/CV/check_handoff.py --url http://<pi-ip>:8766 --seconds 4
```

For optional browser regression checks, install `CV/requirements-dev.txt` and a
Chromium executable (or install Playwright's Chromium). The script injects failure
fixtures into its own browser only, keeps real scene/configuration unchanged,
and writes screenshots plus a report to the output directory:

```sh
.venv/bin/python -m pip install -r SensorRig/CV/requirements-dev.txt
.venv/bin/python SensorRig/CV/tests/check_dashboard_browser.py \
  --url http://<pi-ip>:8766 --browser /usr/bin/chromium \
  --out /tmp/sharingan-browser-validation
```

On the original Pi, the environment lives one directory above the repository:
use `../.venv/bin/python`. This browser check expects the physically corrected
bench configuration (`invert_x: true`) and a running camera/radar station.
