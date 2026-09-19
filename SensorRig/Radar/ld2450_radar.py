#!/usr/bin/env python3
"""
Live radar-view web page for the HLK-LD2450 — same idea as the classic
"$10 radar tracks you through a wall" demo, just served as a web page
instead of needing a dedicated TFT screen + ESP32.

Run on the Pi:
    python3 ld2450_radar.py

Then open http://<pi-ip-or-hostname>:8767 from any phone/laptop on the
same network (works over SSH-less local WiFi — doesn't need your PC at
all). Great for a two-person through-wall test: one person stands on
each side, whoever's got the browser open watches the dot move.
"""
import threading
import time
import struct

import serial
from flask import Flask, jsonify, Response

PORT = "/dev/serial0"
BAUD = 256000
HEADER = b"\xAA\xFF\x03\x00"
FOOTER = b"\x55\xCC"
FRAME_LEN = 30
WEB_PORT = 8767

# LD2450 spec: ~120 deg FOV, up to 6m forward, +/-3m lateral
MAX_RANGE_M = 6.0
FOV_DEG = 120.0

app = Flask(__name__)
lock = threading.Lock()
targets = []          # list of {id, x_m, y_m, speed_cms, dist_m}
sensor_ok = False
last_frame_at = 0.0


def decode_signed(raw):
    magnitude = raw & 0x7FFF
    return magnitude if (raw & 0x8000) else -magnitude


def parse_frame(frame):
    result = []
    for i in range(3):
        off = 4 + i * 8
        x_raw, y_raw, spd_raw, res = struct.unpack_from("<HHHH", frame, off)
        x, y, spd = decode_signed(x_raw), decode_signed(y_raw), decode_signed(spd_raw)
        if x != 0 or y != 0:
            dist = (x * x + y * y) ** 0.5
            result.append({
                "id": i,
                "x_m": x / 1000.0,
                "y_m": y / 1000.0,
                "speed_cms": spd,
                "dist_m": dist / 1000.0,
            })
    return result


def reader_loop():
    global targets, sensor_ok, last_frame_at
    buf = bytearray()
    while True:
        try:
            ser = serial.Serial(PORT, BAUD, timeout=1)
            print(f"[radar] listening on {PORT} @ {BAUD} baud", flush=True)
            while True:
                chunk = ser.read(64)
                if chunk:
                    buf += chunk
                while True:
                    idx = buf.find(HEADER)
                    if idx == -1 or len(buf) < idx + FRAME_LEN:
                        break
                    frame = bytes(buf[idx:idx + FRAME_LEN])
                    if frame[-2:] == FOOTER:
                        found = parse_frame(frame)
                        with lock:
                            targets = found
                            sensor_ok = True
                            last_frame_at = time.time()
                        del buf[:idx + FRAME_LEN]
                    else:
                        del buf[:idx + 1]
        except Exception as e:
            print(f"[radar] error: {e} — retrying in 1s", flush=True)
            with lock:
                sensor_ok = False
            time.sleep(1)


threading.Thread(target=reader_loop, daemon=True).start()


@app.route("/targets")
def get_targets():
    with lock:
        fresh = (time.time() - last_frame_at) < 1.0
        return jsonify({
            "ok": sensor_ok and fresh,
            "targets": targets if fresh else [],
        })


INDEX_HTML = f"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>LD2450 Radar</title>
<style>
  html,body{{margin:0;height:100%;background:#0b1220;font-family:'SF Mono',Menlo,monospace;color:#e7edf7;overflow:hidden;}}
  #wrap{{display:flex;flex-direction:column;align-items:center;padding:14px;box-sizing:border-box;height:100%;}}
  h1{{font-size:13px;letter-spacing:3px;color:#4fd1ff;text-transform:uppercase;margin:0 0 10px;}}
  #targetRow{{display:flex;gap:10px;width:100%;max-width:520px;margin-bottom:10px;}}
  .tcard{{flex:1;border:1px solid #1e2a3a;border-radius:8px;padding:8px 10px;background:rgba(20,26,38,0.6);text-align:center;}}
  .tcard.active{{border-color:#3cdc8c;background:rgba(60,220,140,0.08);}}
  .tcard .lbl{{font-size:10px;color:#5a6b80;letter-spacing:1px;}}
  .tcard .dist{{font-size:20px;color:#3cdc8c;font-weight:bold;margin-top:2px;}}
  .tcard .spd{{font-size:10px;color:#5a6b80;margin-top:2px;}}
  #status{{position:fixed;top:12px;right:14px;padding:5px 12px;border-radius:14px;font-size:11px;letter-spacing:1px;}}
  .ok{{background:rgba(60,220,140,0.15);color:#3cdc8c;border:1px solid #3cdc8c;}}
  .bad{{background:rgba(220,60,60,0.15);color:#e05a5a;border:1px solid #e05a5a;}}
  canvas{{max-width:100%;}}
  #caption{{font-size:10px;color:#5a6b80;letter-spacing:1px;margin-top:8px;text-transform:uppercase;}}
</style>
</head>
<body>
<div id="status" class="bad">CONNECTING…</div>
<div id="wrap">
  <h1>LD2450 · 24GHz mmWave Radar</h1>
  <div id="targetRow"></div>
  <canvas id="radar" width="520" height="360"></canvas>
  <div id="caption">RANGE {MAX_RANGE_M:.0f}m &middot; FOV {FOV_DEG:.0f}&deg; &middot; RING 1m</div>
</div>
<script>
const MAX_RANGE = {MAX_RANGE_M};
const FOV_DEG = {FOV_DEG};
const cv = document.getElementById('radar');
const ctx = cv.getContext('2d');
const W = cv.width, H = cv.height;
const cx = W/2, cy = H - 20;
const R = Math.min(cx, cy) - 20;

function buildCards(n){{
  const row = document.getElementById('targetRow');
  row.innerHTML = '';
  for(let i=0;i<3;i++){{
    const d = document.createElement('div');
    d.className = 'tcard';
    d.id = 'tcard'+i;
    d.innerHTML = `<div class="lbl">T${{i+1}}</div><div class="dist" id="tdist${{i}}">--</div><div class="spd" id="tspd${{i}}">NO TARGET</div>`;
    row.appendChild(d);
  }}
}}
buildCards();

function draw(targets){{
  ctx.clearRect(0,0,W,H);

  // range rings (arcs within the FOV wedge)
  const halfFov = FOV_DEG/2 * Math.PI/180;
  ctx.strokeStyle = '#1e2a3a';
  ctx.fillStyle = '#3a4a5f';
  ctx.font = '9px monospace';
  ctx.textAlign = 'center';
  for(let rm=1; rm<=MAX_RANGE; rm++){{
    const rr = R * (rm/MAX_RANGE);
    ctx.beginPath();
    ctx.arc(cx, cy, rr, -Math.PI/2 - halfFov, -Math.PI/2 + halfFov);
    ctx.stroke();
    ctx.fillText(rm+'m', cx + rr*Math.sin(halfFov) + 12, cy - rr*Math.cos(halfFov));
  }}

  // FOV boundary lines + angle labels
  ctx.strokeStyle = '#26333f';
  [-halfFov, 0, halfFov].forEach(a=>{{
    ctx.beginPath();
    ctx.moveTo(cx, cy);
    ctx.lineTo(cx + R*Math.sin(a), cy - R*Math.cos(a));
    ctx.stroke();
  }});

  // sensor position marker
  ctx.fillStyle = '#4fd1ff';
  ctx.beginPath();
  ctx.moveTo(cx, cy-9); ctx.lineTo(cx+6, cy+3); ctx.lineTo(cx-6, cy+3);
  ctx.closePath(); ctx.fill();

  // targets
  const t = performance.now()/1000;
  targets.forEach((tg, i)=>{{
    const px = cx + (tg.x_m/MAX_RANGE) * R;
    const py = cy - (tg.y_m/MAX_RANGE) * R;
    const pulse = 4 + 1.5*Math.sin(t*5 + i);
    ctx.fillStyle = 'rgba(255,138,60,0.25)';
    ctx.beginPath(); ctx.arc(px,py,pulse+7,0,Math.PI*2); ctx.fill();
    ctx.fillStyle = '#ff8a3c';
    ctx.beginPath(); ctx.arc(px,py,pulse,0,Math.PI*2); ctx.fill();
    ctx.fillStyle = '#e8eef5';
    ctx.font = '10px monospace';
    ctx.fillText(tg.dist_m.toFixed(2)+'m', px, py-12);
  }});
  ctx.textAlign = 'left';
}}

async function poll(){{
  try{{
    const r = await fetch('/targets');
    const d = await r.json();
    const s = document.getElementById('status');
    s.textContent = d.ok ? 'LIVE' : 'NO SIGNAL';
    s.className = d.ok ? 'ok' : 'bad';

    for(let i=0;i<3;i++){{
      const tg = d.targets[i];
      const distEl = document.getElementById('tdist'+i);
      const spdEl = document.getElementById('tspd'+i);
      const card = document.getElementById('tcard'+i);
      if(tg){{
        distEl.textContent = tg.dist_m.toFixed(2)+'m';
        spdEl.textContent = tg.speed_cms+' cm/s';
        card.classList.add('active');
      }} else {{
        distEl.textContent = '--';
        spdEl.textContent = 'NO TARGET';
        card.classList.remove('active');
      }}
    }}
    draw(d.targets || []);
  }}catch(e){{
    document.getElementById('status').textContent = 'SERVER DOWN';
    document.getElementById('status').className = 'bad';
  }}
}}
setInterval(poll, 100);
draw([]);
</script>
</body>
</html>
"""


@app.route("/")
def index():
    return Response(INDEX_HTML, mimetype="text/html")


if __name__ == "__main__":
    print(f"Open http://<this-pi>:{WEB_PORT} in a browser on the same network")
    app.run(host="0.0.0.0", port=WEB_PORT, threaded=True)
