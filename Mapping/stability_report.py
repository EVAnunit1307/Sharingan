"""Render an offline experiment comparison for the local dashboard."""
import argparse
from html import escape
import json
from pathlib import Path

from Mapping.geometry_quality import assess


def render(root, session, output):
    frames = [json.loads(line) for line in (session/'frames.jsonl').read_text().splitlines()]
    names = [Path(frame['image']).name for frame in frames]
    rows = []
    labels = {'sift-no-fallback':'SIFT · no fallback', 'xfeat-no-fallback':'XFeat · no fallback',
              'xfeat-strict':'XFeat · strict geometry', 'xfeat-global':'XFeat · global solver',
              'lighterglue-features':'XFeat + LighterGlue · no fallback',
              'lighterglue-strict':'XFeat + LighterGlue · strict geometry'}
    for label in labels:
        path = root/label/'report.json'
        if not path.is_file(): continue
        data = json.loads(path.read_text()); trials = data['trials']
        if len(trials) < 2: continue
        a,b = trials[:2]
        comparison = dict(b.get('repeatability', {'failure':'No comparison'}),
                          reference_views=a.get('registered_images',0),trial_views=b.get('registered_images',0))
        quality = assess(comparison,len(names))
        present = {p['image'] for p in a.get('camera_path',[])}
        bars = ''.join(f'<rect x="{i*2}" y="0" width="1.5" height="18" fill="{"#9ddebe" if name in present else "#29332f"}"/>' for i,name in enumerate(names))
        timeline = f'<svg role="img" aria-label="First build: green bars are recovered views, gray are missing" viewBox="0 0 {len(names)*2} 18">{bars}</svg>'
        metric = comparison.get('center_rmse_fraction_of_path_spread')
        error = 'Unavailable' if metric is None else f'{100*metric:.2f}%'
        counts = f'{a.get("registered_images",0)} / {b.get("registered_images",0)}'
        status = {'unstable':'Unstable','repeatable_partial':'Repeatable fragment',
                  'repeatable_unvalidated':'Repeatable; unvalidated','inconclusive':'Inconclusive'}[quality['status']]
        rows.append(f'<tr><th scope="row">{escape(labels[label])}</th><td>{counts}</td><td>{comparison.get("common_views",0)}</td><td>{error}</td><td>{status}</td><td>{timeline}</td></tr>')
    body = '''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WALLHACK · Stability experiments</title><style>
:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#0e1411;color:#edf2ee;font:16px/1.6 system-ui,sans-serif}main{max-width:1240px;margin:auto;padding:40px 26px}a{color:#9ddebe}h1{font-size:clamp(30px,4vw,48px);line-height:1.1;margin:24px 0}h2{font-size:22px;margin-top:36px}p{max-width:880px;color:#b5c3ba}strong{color:#edf2ee}.eyebrow{letter-spacing:.12em;font-size:12px}.table{overflow:auto;border:1px solid #34463b;border-radius:10px;margin:28px 0}table{width:100%;border-collapse:collapse;font-size:14px;min-width:920px}th,td{padding:16px;text-align:left;border-bottom:1px solid #29372f}thead{background:#1e2d24;color:#c6d7cb}tbody th{font-weight:500}td:last-child{min-width:230px}svg{width:100%;display:block}li{margin:10px 0;max-width:900px}code{overflow-wrap:anywhere;font-size:13px}.note{border-left:3px solid #d2a957;padding:14px 20px;background:#1f231b}footer{margin-top:40px;font-size:13px;color:#91a298}</style>
<main><a href="/map">← Mapping dashboard</a><p class="eyebrow">25 SEPTEMBER 2026 · EXISTING CHAIR RECORDING</p>
<h1>Coverage is improving.<br>Full-map shape is still unstable.</h1>
<p>We compared feature matchers, stricter geometry checks and two reconstruction solvers on the same <strong>222 saved images</strong>. Each setting below was built twice with seeds 42 and 43. Raw footage and the original dashboard map are preserved.</p>
<p class="note"><strong>Useful result:</strong> conventional SIFT and strict XFeat independently recover a repeatable fragment covering roughly the first 38 seconds. A blurred swing away from the chair at 37–39 seconds is a likely tracking break. A full 360° reconstruction has not passed validation.</p>
<div class="table"><table><thead><tr><th>Experiment</th><th>Views: run 1 / 2<br>out of 222 each</th><th>Common views</th><th>Path disagreement</th><th>Result</th><th>Run 1 coverage<br>0 → 74 seconds</th></tr></thead><tbody>ROWS</tbody></table></div>
<p><strong>How to read this:</strong> green bars show recovered views in the largest usable component; gray bars are missing views. Other disconnected fragments may exist. “Path disagreement” is 3D camera-position RMSE after aligning scale, rotation and translation, divided by the first run’s path spread. It is computed only on common views. Different rows can cover different parts of the recording, so a smaller percentage is not proof of better physical accuracy.</p>
<p>The automatic check flags more than 5% path disagreement, more than 5° p90 orientation disagreement, or fewer than 90% common views across the selected models. Below 80% recording coverage it reports a fragment. These are engineering screening thresholds, not measured accuracy limits.</p>
<h2>Changes now in the app</h2><p><strong>Experimental matching</strong> now performs the second build automatically, saves both models and reports disagreement in the viewer. Weak structure-less fallback registration is disabled. This adds build time and can expose disconnected fragments. SIFT’s ordinary build button keeps its previous behavior.</p>
<h2>Next experiment</h2><ol><li>Preserve overlapping views through the 37–39 second transition. Add light and try the existing sharper-frame/short-exposure controls; check actual brightness before recording.</li><li>Give the reflective, repetitive chair more distinct visual detail with removable matte paper markers, and keep textured background visible while moving slowly sideways.</li><li>After a full map is repeatable, compare independent dimensions. The rough 40 cm seat diameter is recorded but has not been used to claim metric accuracy.</li></ol>
<p>Keep the small AI depth model on the laptop, after camera-pose estimation. It can propose visible surfaces, but adding more inferred points does not repair an unstable camera path. FC IMU integration remains pending a real data connection; Quest is optional later.</p>
<footer>Local experiments only · Session <code>SESSION</code><br>Evidence: <code>ROOT</code><br>Framework references: <a href="https://github.com/verlab/accelerated_features">official XFeat / LighterGlue</a> · <a href="https://colmap.github.io/faq.html">COLMAP documentation</a></footer></main></html>'''
    output.write_text(body.replace('ROWS',''.join(rows)).replace('SESSION',escape(session.name)).replace('ROOT',escape(str(root))))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('root','session','output'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();render(a.root,a.session,a.output)
