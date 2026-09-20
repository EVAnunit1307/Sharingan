"""Browser checks against the live station. Synthetic failure data stays in the browser."""
import argparse
import shutil
from copy import deepcopy
import json
from pathlib import Path
from playwright.sync_api import sync_playwright

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--url',default='http://127.0.0.1:8766')
parser.add_argument('--out',type=Path,default=Path('/tmp/sharingan-browser-validation'))
parser.add_argument('--browser',default=shutil.which('chromium'))
args=parser.parse_args()
BASE=args.url.rstrip('/')
OUT=args.out
OUT.mkdir(parents=True,exist_ok=True)
with sync_playwright() as p:
    browser=p.chromium.launch(executable_path=args.browser,headless=True,args=['--no-sandbox','--disable-dev-shm-usage'])
    page=browser.new_page(viewport={'width':1440,'height':1100})
    errors=[]
    page.on('pageerror',lambda e:errors.append(str(e)))
    page.goto(BASE,wait_until='domcontentloaded')
    page.wait_for_function("document.getElementById('radar-status').textContent === 'RADAR LIVE' && document.getElementById('camera-status').textContent === 'CAMERA LIVE'",timeout=15000)
    baseline=page.request.get(BASE+'/detections').json()
    assert baseline['radar']['config']['invert_x'] is True
    assert page.locator('#mirror').is_checked()
    ids=page.evaluate("Array.from(document.querySelectorAll('[id]')).map(e=>e.id)")
    assert len(ids)==len(set(ids))
    page.screenshot(path=str(OUT/'combined_desktop.png'),full_page=True)
    page.locator('#check-bridge').click()
    page.wait_for_function("document.getElementById('bridge-check').textContent.startsWith('Telemetry received')",timeout=10000)
    assert page.locator('#quest-url').input_value()==BASE+'/quest'
    page.set_viewport_size({'width':390,'height':844})
    assert not page.evaluate('document.documentElement.scrollWidth>innerWidth')
    page.screenshot(path=str(OUT/'combined_mobile.png'),full_page=True)
    page.set_viewport_size({'width':1365,'height':768})
    page.goto(BASE+'/quest',wait_until='domcontentloaded')
    page.wait_for_function("document.getElementById('radar-status').textContent === 'RADAR LIVE'",timeout=15000)
    page.screenshot(path=str(OUT/'quest_dashboard.png'),full_page=True)
    for selector in ('#feed','#scope'):
        box=page.locator(selector).bounding_box()
        assert box['y']+box['height']<=768, (selector,box)
    assert page.locator('#fullscreen').bounding_box()['height']>=44
    page.locator('#fullscreen').click()
    page.wait_for_function('Boolean(document.fullscreenElement)',timeout=3000)
    page.locator('#fullscreen').click()
    page.wait_for_function('!document.fullscreenElement',timeout=3000)

    mode=['camera_off'];sequence=[100000]
    def fixture(route):
        if mode[0]=='network_off':
            route.abort();return
        d=deepcopy(baseline)
        if mode[0]!='frozen':sequence[0]+=1
        d.update(frame_id=sequence[0],frame_age_ms=1,camera_connected=True,status='live',person_count=0,people=[],error=None)
        d['radar'].update(frame_id=sequence[0],age_ms=1,status='live',error=None,raw_targets=[],targets=[
            {'id':99,'right_m':-1.2,'forward_m':2.3,'range_m':2.595,'bearing_deg':-27.55,'radial_speed_mps':0.}])
        if mode[0]=='camera_off':d.update(camera_connected=False,status='stale')
        elif mode[0]=='radar_off':d['radar'].update(status='disconnected',targets=[])
        route.fulfill(json=d)
    page.route('**/detections',fixture)
    page.wait_for_function("document.getElementById('camera-count').textContent === '—' && document.getElementById('radar-count').textContent === '1'",timeout=5000)
    assert '-1.20 right' in page.locator('#targets').inner_text()
    # The corrected position is plotted on the left; it is not mirrored again.
    assert page.evaluate("""() => { const c=document.getElementById('scope'), s=Math.min((c.width-110)/12,(c.height-130)/6); const rgb=c.getContext('2d').getImageData(Math.round(c.width/2-1.2*s),Math.round(c.height-80-2.3*s),1,1).data; return rgb[0]===136 && rgb[1]===201 && rgb[2]===242; }""")
    mode[0]='radar_off'
    page.wait_for_function("document.getElementById('radar-count').textContent === '—' && document.getElementById('camera-status').textContent === 'CAMERA LIVE'",timeout=5000)
    mode[0]='live'
    page.wait_for_function("document.getElementById('radar-count').textContent === '1'",timeout=5000)
    mode[0]='frozen'
    page.wait_for_function("document.getElementById('radar-count').textContent === '—' && document.getElementById('camera-count').textContent === '—'",timeout=5000)
    mode[0]='network_off'
    page.wait_for_function("document.getElementById('radar-status').textContent === 'RADAR CONNECTION LOST'",timeout=5000)
    page.unroute('**/detections')
    page.wait_for_function("document.getElementById('radar-status').textContent === 'RADAR LIVE' && document.getElementById('camera-status').textContent === 'CAMERA LIVE'",timeout=5000)
    assert not errors,errors
    result=dict(javascript_errors=errors,unique_element_ids=True,mobile_no_overflow=True,quest_camera_and_map_visible=True,
                quest_controls_at_least_44px=True,full_screen_enter_exit=True,websocket_received=True,
                camera_failure_preserves_radar=True,radar_failure_preserves_camera=True,
                frozen_frame_ids_clear_both=True,http_failure_clears_both=True,reconnect_recovers=True,
                corrected_left_right_preserved=True,headset_tested=False)
    (OUT/'combined_browser_validation.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
    browser.close()
