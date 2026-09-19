import serial, serial.tools.list_ports, struct, threading, time
from flask import Flask, jsonify, Response, request

# ============================================================================
# MOTOR BENCH TEST DASHBOARD
#
# Talks MSP (MultiWii Serial Protocol) to your flight controller over COM4,
# same wire-level protocol as imu_viz.py — but this sends MSP_SET_MOTOR
# override commands instead of just reading attitude. This is exactly the
# mechanism Betaflight/iNav Configurator uses on its "Motors" tab.
#
# IMPORTANT:
#   - Only one program can hold COM4 open at a time. Close imu_viz.py (or any
#     other program using the port) before running this.
#   - This only works with props OFF and the FC in its normal disarmed state
#     (not receiving an "armed" signal from your radio/receiver). If your RX
#     is connected and could arm the FC, unplug it before bench testing.
#   - There's a software watchdog: if this browser tab stops talking to the
#     server for more than ~0.6s (closed, refreshed, crashed, phone locked,
#     tab backgrounded), all motors are forced to 0 and test mode disarms.
# ============================================================================

PORT = 'COM4'
BAUD = 115200

MSP_MOTOR = 104
MSP_SET_MOTOR = 214

MOTOR_STOP = 1000
MOTOR_SAFE_CAP = 1300      # default ceiling — plenty to confirm spin direction/sound
MOTOR_ABS_MAX = 2000
HEARTBEAT_TIMEOUT = 0.6    # seconds

app = Flask(__name__)
lock = threading.Lock()
state = {
    'connected': False,
    'armed_test': False,
    'unlocked': False,
    'throttle': 1150,
    'num_motors': 4,
    'motor_on': [False] * 8,
    'motor_cmd': [MOTOR_STOP] * 8,
    'motor_reported': [None] * 8,
    'last_heartbeat': 0.0,
    'last_error': '',
}

ser = None


def msp_request(s, cmd, payload=b''):
    size = len(payload)
    body = bytes([size, cmd]) + payload
    checksum = 0
    for b in body:
        checksum ^= b
    s.write(b'$M<' + body + bytes([checksum]))


def read_msp_response(s, timeout=0.05):
    s.timeout = timeout
    start = time.time()
    while time.time() - start < timeout:
        c = s.read(1)
        if c == b'$':
            if s.read(2) != b'M>':
                continue
            size_b = s.read(1)
            if not size_b:
                return None
            size = size_b[0]
            cmd_b = s.read(1)
            if not cmd_b:
                return None
            cmd = cmd_b[0]
            payload = s.read(size)
            s.read(1)  # checksum, not verified
            return cmd, payload
    return None


def print_port_list():
    ports = list(serial.tools.list_ports.comports())
    print(f"[diag] Serial ports Windows currently sees: {len(ports)}", flush=True)
    found_target = False
    for p in ports:
        marker = " <-- this is PORT" if p.device.upper() == PORT.upper() else ""
        if marker:
            found_target = True
        print(f"[diag]   {p.device}: {p.description} (hwid: {p.hwid}){marker}", flush=True)
    if not found_target:
        print(f"[diag] *** {PORT} was NOT in that list. Your FC is probably enumerating under a "
              f"different COM port now (check Device Manager) — update PORT at the top of this "
              f"file to match, or MSP writes will go nowhere even though the port 'opens'. ***", flush=True)


def control_loop():
    """Runs continuously at ~20Hz: computes the commanded value for every
    motor from current UI state, enforces the watchdog + safety cap, and
    re-sends MSP_SET_MOTOR every tick (a single send would only spin the
    motor until the FC's own override timeout lapses)."""
    global ser
    tick = 0
    last_diag_print = 0.0
    motor_ack_ever_seen = False
    while True:
        loop_start = time.time()
        try:
            if ser is None:
                print(f"[diag] Opening {PORT} @ {BAUD}...", flush=True)
                ser = serial.Serial(PORT, BAUD, timeout=0.05)
                time.sleep(0.3)
                print(f"[diag] {PORT} opened successfully.", flush=True)

            with lock:
                now = time.time()
                stale = (now - state['last_heartbeat']) > HEARTBEAT_TIMEOUT
                if stale and state['armed_test']:
                    state['armed_test'] = False
                    state['motor_on'] = [False] * 8
                    state['last_error'] = 'Watchdog: lost contact with the dashboard — motors stopped.'

                cap = MOTOR_ABS_MAX if state['unlocked'] else MOTOR_SAFE_CAP
                throttle = max(MOTOR_STOP, min(cap, state['throttle']))
                cmds = [
                    throttle if (state['armed_test'] and state['motor_on'][i]) else MOTOR_STOP
                    for i in range(8)
                ]
                state['motor_cmd'] = cmds

            msp_request(ser, MSP_SET_MOTOR, struct.pack('<8H', *cmds))
            set_ack = read_msp_response(ser, timeout=0.05)  # drain ack if the FC sends one

            tick += 1
            if tick % 5 == 0:
                msp_request(ser, MSP_MOTOR)
                resp = read_msp_response(ser, timeout=0.05)
                if resp and resp[0] == MSP_MOTOR and len(resp[1]) >= 16:
                    reported = list(struct.unpack('<8H', resp[1][:16]))
                    with lock:
                        state['motor_reported'] = reported
                    if not motor_ack_ever_seen:
                        motor_ack_ever_seen = True
                        print(f"[diag] FC responded to MSP_MOTOR for the first time: {reported[:4]}...", flush=True)

            with lock:
                state['connected'] = True

            # Once a second, print exactly what's being sent/seen so this is
            # visible directly in the terminal without needing a screenshot.
            if loop_start - last_diag_print > 1.0:
                last_diag_print = loop_start
                with lock:
                    a, mo, mc, mr, err = state['armed_test'], list(state['motor_on']), list(state['motor_cmd']), state['motor_reported'], state['last_error']
                print(f"[diag] armed={a} motor_on={mo[:4]} sending_cmd={mc[:4]} "
                      f"fc_reports_back={mr[:4] if mr[0] is not None else 'NEVER RESPONDED'} "
                      f"set_motor_ack={'yes' if set_ack else 'no'} last_error={err!r}", flush=True)

        except Exception as e:
            print(f"[diag] *** SERIAL ERROR: {type(e).__name__}: {e} — closing and retrying in 1s ***", flush=True)
            with lock:
                state['connected'] = False
                state['armed_test'] = False
                state['motor_on'] = [False] * 8
                state['last_error'] = f'{type(e).__name__}: {e}'
            try:
                if ser:
                    ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(1)

        elapsed = time.time() - loop_start
        time.sleep(max(0, 0.05 - elapsed))


print_port_list()
threading.Thread(target=control_loop, daemon=True).start()


@app.route('/status')
def status():
    # A status poll doubles as the watchdog heartbeat — as long as the page
    # is open and polling, test mode stays alive.
    with lock:
        state['last_heartbeat'] = time.time()
        d = dict(state)
    return jsonify(d)


@app.route('/arm', methods=['POST'])
def arm():
    body = request.get_json(force=True, silent=True) or {}
    want = bool(body.get('arm'))
    with lock:
        state['armed_test'] = want
        state['last_heartbeat'] = time.time()
        if want:
            state['last_error'] = ''
        else:
            state['motor_on'] = [False] * 8
    return jsonify({'ok': True, 'armed_test': want})


@app.route('/motor', methods=['POST'])
def motor():
    body = request.get_json(force=True, silent=True) or {}
    idx = int(body.get('index', -1))
    on = bool(body.get('on'))
    with lock:
        if 0 <= idx < 8 and state['armed_test']:
            state['motor_on'][idx] = on
            state['last_heartbeat'] = time.time()
    return jsonify({'ok': True})


@app.route('/all', methods=['POST'])
def all_motors():
    body = request.get_json(force=True, silent=True) or {}
    on = bool(body.get('on'))
    with lock:
        if state['armed_test']:
            n = state['num_motors']
            state['motor_on'] = [on if i < n else False for i in range(8)]
            state['last_heartbeat'] = time.time()
    return jsonify({'ok': True})


@app.route('/throttle', methods=['POST'])
def throttle():
    body = request.get_json(force=True, silent=True) or {}
    try:
        v = int(body.get('value', MOTOR_STOP))
    except (TypeError, ValueError):
        v = MOTOR_STOP
    with lock:
        cap = MOTOR_ABS_MAX if state['unlocked'] else MOTOR_SAFE_CAP
        state['throttle'] = max(MOTOR_STOP, min(cap, v))
        state['last_heartbeat'] = time.time()
        result = state['throttle']
    return jsonify({'ok': True, 'throttle': result})


@app.route('/unlock', methods=['POST'])
def unlock():
    body = request.get_json(force=True, silent=True) or {}
    with lock:
        state['unlocked'] = bool(body.get('unlocked'))
        if not state['unlocked']:
            state['throttle'] = min(state['throttle'], MOTOR_SAFE_CAP)
    return jsonify({'ok': True, 'unlocked': state['unlocked']})


@app.route('/num_motors', methods=['POST'])
def num_motors():
    body = request.get_json(force=True, silent=True) or {}
    try:
        n = int(body.get('value', 4))
    except (TypeError, ValueError):
        n = 4
    with lock:
        state['num_motors'] = max(1, min(8, n))
    return jsonify({'ok': True, 'num_motors': state['num_motors']})


@app.route('/estop', methods=['POST'])
def estop():
    with lock:
        state['armed_test'] = False
        state['motor_on'] = [False] * 8
        state['last_error'] = 'Emergency stop pressed.'
    return jsonify({'ok': True})


INDEX_HTML = """<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>Motor Bench Test</title>
<style>
  html,body{margin:0;min-height:100%;background:#1b2432;font-family:'SF Mono',Menlo,monospace;color:#e8eef5;}
  body{padding:20px 20px 120px 20px;box-sizing:border-box;}
  h1{font-size:16px;letter-spacing:2px;color:#4fd1ff;text-transform:uppercase;margin:0 0 2px 0;}
  .sub{font-size:11px;color:#5a6b80;margin-bottom:16px;}
  .card{background:rgba(10,14,22,0.6);border:1px solid #1e2a3a;border-radius:10px;padding:16px 18px;margin-bottom:14px;max-width:640px;}
  .row{display:flex;align-items:center;gap:10px;flex-wrap:wrap;}
  .pill{display:inline-block;padding:5px 12px;border-radius:20px;font-size:11px;letter-spacing:1px;border:1px solid #5a6b80;color:#5a6b80;}
  .pill.ok{border-color:#3cdc8c;color:#3cdc8c;background:rgba(60,220,140,0.1);}
  .pill.bad{border-color:#e05a5a;color:#e05a5a;background:rgba(220,60,60,0.1);}
  .pill.armed{border-color:#ffc83c;color:#ffc83c;background:rgba(255,200,60,0.15);animation:pulse 1.2s infinite;}
  @keyframes pulse{0%,100%{opacity:1;}50%{opacity:0.55;}}
  .warn{background:rgba(255,200,60,0.08);border:1px solid #6b5a1e;border-radius:8px;padding:10px 14px;font-size:12px;color:#ffc83c;margin-bottom:14px;max-width:640px;}
  label{font-size:12px;color:#b7c3d1;}
  input[type=checkbox]{width:16px;height:16px;vertical-align:middle;margin-right:6px;}
  button{font-family:inherit;font-size:12px;letter-spacing:1px;text-transform:uppercase;border-radius:6px;padding:9px 16px;cursor:pointer;border:1px solid #4fd1ff;background:#132030;color:#4fd1ff;}
  button:hover:not(:disabled){background:#1c3348;}
  button:disabled{opacity:0.35;cursor:not-allowed;}
  button.go{border-color:#3cdc8c;color:#3cdc8c;}
  button.go:hover:not(:disabled){background:#123322;}
  button.stop{border-color:#e05a5a;color:#e05a5a;}
  button.stop:hover:not(:disabled){background:#331414;}
  input[type=range]{width:260px;}
  #estop{position:fixed;bottom:24px;left:50%;transform:translateX(-50%);z-index:20;padding:18px 48px;font-size:16px;font-weight:bold;letter-spacing:3px;border-radius:12px;border:2px solid #e05a5a;background:#3a1414;color:#ff8a8a;box-shadow:0 0 24px rgba(224,90,90,0.5);}
  #estop:hover{background:#4a1818;}
  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:12px;max-width:640px;margin-top:10px;}
  .mcard{border:1px solid #1e2a3a;border-radius:8px;padding:10px;text-align:center;background:rgba(20,26,38,0.6);}
  .mcard.on{border-color:#3cdc8c;background:rgba(60,220,140,0.08);}
  .mlabel{font-size:12px;color:#b7c3d1;letter-spacing:1px;margin-bottom:6px;}
  .spinner{width:36px;height:36px;margin:0 auto 6px auto;border-radius:50%;border:3px solid #2a3a4d;border-top-color:#5a6b80;}
  .mcard.on .spinner{border-top-color:#3cdc8c;animation:spin linear infinite;}
  @keyframes spin{to{transform:rotate(360deg);}}
  .toggle{position:relative;display:inline-block;width:44px;height:24px;margin-top:4px;}
  .toggle input{opacity:0;width:0;height:0;}
  .slider{position:absolute;cursor:pointer;inset:0;background:#2a3a4d;border-radius:24px;transition:0.15s;}
  .slider:before{position:absolute;content:"";height:18px;width:18px;left:3px;bottom:3px;background:#8a97a8;border-radius:50%;transition:0.15s;}
  input:checked + .slider{background:#1a4a30;}
  input:checked + .slider:before{background:#3cdc8c;transform:translateX(20px);}
  input:disabled + .slider{opacity:0.4;cursor:not-allowed;}
  .mval{font-size:10px;color:#5a6b80;margin-top:4px;}
  #log{font-size:11px;color:#e05a5a;min-height:14px;margin-top:8px;}
  .cap{font-size:10px;color:#5a6b80;margin-top:6px;line-height:1.5;}
</style>
</head>
<body>
<h1>Motor Bench Test</h1>
<div class="sub">MSP override via COM4 &mdash; same mechanism as Betaflight/iNav Configurator's Motors tab</div>

<div class="warn">
  &#9888; Props must be OFF. Keep hands, cables and loose objects clear of the motors before arming test mode.
  Disconnect your radio receiver first if it could arm the FC on its own.
</div>

<div class="card row">
  <span class="pill" id="connPill">CONNECTING&hellip;</span>
  <span class="pill" id="armPill">SAFE</span>
  <span class="pill" id="wdPill">WATCHDOG OK</span>
</div>

<div class="card">
  <div class="row" style="margin-bottom:10px;">
    <label><input type="checkbox" id="confirmChk"> Propellers are removed and the area is clear</label>
  </div>
  <div class="row">
    <button id="armBtn" class="go" disabled>Arm Test Mode</button>
    <span class="cap" style="margin:0;">Nothing spins until this is on. The dashboard must stay open &mdash; motors auto-stop if it disconnects.</span>
  </div>
</div>

<div class="card">
  <div class="row" style="margin-bottom:12px;">
    <label>Motors:
      <select id="numMotors">
        <option value="2">2</option>
        <option value="3">3</option>
        <option value="4" selected>4</option>
        <option value="6">6</option>
        <option value="8">8</option>
      </select>
    </label>
    <label><input type="checkbox" id="unlockChk"> Unlock full range (up to 2000&micro;s)</label>
  </div>
  <div class="row">
    <label for="throttle">Test throttle</label>
    <input type="range" id="throttle" min="1000" max="1300" value="1150">
    <span id="throttleVal">1150&micro;s (~50%)</span>
  </div>
  <div class="row" style="margin-top:12px;">
    <button id="allOn" disabled>All On</button>
    <button id="allOff" disabled>All Off</button>
    <button id="autoTest" disabled>Auto Test (each motor, 1s)</button>
  </div>
  <div id="log"></div>
</div>

<div class="card">
  <div class="grid" id="motorGrid"></div>
  <div class="cap">M1&ndash;M8 mapping depends on your mixer &mdash; toggle one at a time first to confirm which physical motor spins and which direction, before assuming the numbering.</div>
</div>

<button id="estop">EMERGENCY STOP</button>

<script>
let st = { armed_test:false, num_motors:4, motor_on:Array(8).fill(false), motor_cmd:Array(8).fill(1000), unlocked:false, connected:false };
let autoTestRunning = false;

function buildGrid(){
  const grid = document.getElementById('motorGrid');
  grid.innerHTML = '';
  for(let i=0;i<st.num_motors;i++){
    const card = document.createElement('div');
    card.className = 'mcard';
    card.id = 'mcard'+i;
    card.innerHTML = `
      <div class="mlabel">M${i+1}</div>
      <div class="spinner"></div>
      <label class="toggle">
        <input type="checkbox" class="mtoggle" data-idx="${i}">
        <span class="slider"></span>
      </label>
      <div class="mval" id="mval${i}">0&micro;s</div>
    `;
    grid.appendChild(card);
  }
  grid.querySelectorAll('.mtoggle').forEach(cb=>{
    cb.addEventListener('change', async (e)=>{
      const idx = parseInt(e.target.dataset.idx);
      await fetch('/motor', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({index: idx, on: e.target.checked})});
    });
  });
}
buildGrid();

async function refresh(){
  try {
    const r = await fetch('/status');
    st = await r.json();
    const connPill = document.getElementById('connPill');
    connPill.textContent = st.connected ? 'COM4 CONNECTED' : 'COM4 NOT CONNECTED';
    connPill.className = 'pill ' + (st.connected ? 'ok' : 'bad');

    const armPill = document.getElementById('armPill');
    if(st.armed_test){ armPill.textContent = 'TEST ARMED'; armPill.className = 'pill armed'; }
    else { armPill.textContent = 'SAFE'; armPill.className = 'pill'; }

    document.getElementById('wdPill').className = 'pill ok';

    document.getElementById('armBtn').textContent = st.armed_test ? 'Disarm Test Mode' : 'Arm Test Mode';
    document.getElementById('armBtn').className = st.armed_test ? 'stop' : 'go';
    ['allOn','allOff','autoTest'].forEach(id => document.getElementById(id).disabled = !st.armed_test);

    document.getElementById('unlockChk').checked = st.unlocked;
    const cap = st.unlocked ? 2000 : 1300;
    const tSlider = document.getElementById('throttle');
    tSlider.max = cap;
    if(document.activeElement !== tSlider) tSlider.value = st.throttle;
    document.getElementById('throttleVal').textContent = st.throttle + 'µs (~' + Math.round((st.throttle-1000)/10) + '%)';

    for(let i=0;i<8;i++){
      const cb = document.querySelector(`.mtoggle[data-idx="${i}"]`);
      if(!cb) continue;
      if(document.activeElement !== cb) cb.checked = st.motor_on[i];
      cb.disabled = !st.armed_test;
      const card = document.getElementById('mcard'+i);
      card.classList.toggle('on', st.motor_on[i]);
      const spinner = card.querySelector('.spinner');
      const pct = Math.max(0, (st.motor_cmd[i]-1000)/10);
      spinner.style.animationDuration = pct > 0 ? Math.max(0.15, 1.2 - pct*0.011) + 's' : '0s';
      document.getElementById('mval'+i).textContent = st.motor_cmd[i] + 'µs';
    }

    document.getElementById('log').textContent = st.last_error || '';
  } catch(e) {
    document.getElementById('connPill').textContent = 'SERVER UNREACHABLE';
    document.getElementById('connPill').className = 'pill bad';
  }
}
setInterval(refresh, 150);
refresh();

document.getElementById('confirmChk').addEventListener('change', (e)=>{
  document.getElementById('armBtn').disabled = !e.target.checked;
});

document.getElementById('armBtn').addEventListener('click', async ()=>{
  await fetch('/arm', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({arm: !st.armed_test})});
});

document.getElementById('numMotors').addEventListener('change', async (e)=>{
  await fetch('/num_motors', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({value: parseInt(e.target.value)})});
  st.num_motors = parseInt(e.target.value);
  buildGrid();
});

document.getElementById('unlockChk').addEventListener('change', async (e)=>{
  await fetch('/unlock', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({unlocked: e.target.checked})});
});

document.getElementById('throttle').addEventListener('input', async (e)=>{
  await fetch('/throttle', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({value: parseInt(e.target.value)})});
});

document.getElementById('allOn').addEventListener('click', async ()=>{
  await fetch('/all', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({on: true})});
});
document.getElementById('allOff').addEventListener('click', async ()=>{
  await fetch('/all', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({on: false})});
});

async function sleep(ms){ return new Promise(res=>setTimeout(res, ms)); }

document.getElementById('autoTest').addEventListener('click', async ()=>{
  if(autoTestRunning) return;
  autoTestRunning = true;
  document.getElementById('autoTest').disabled = true;
  try {
    for(let i=0;i<st.num_motors;i++){
      if(!st.armed_test) break;
      await fetch('/motor', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({index:i, on:true})});
      await sleep(1000);
      await fetch('/motor', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({index:i, on:false})});
      await sleep(300);
    }
  } finally {
    autoTestRunning = false;
    document.getElementById('autoTest').disabled = !st.armed_test;
  }
});

async function doEstop(){
  navigator.sendBeacon && navigator.sendBeacon('/estop', new Blob([JSON.stringify({})], {type:'application/json'}));
  await fetch('/estop', {method:'POST'});
}
document.getElementById('estop').addEventListener('click', doEstop);

document.addEventListener('keydown', (e)=>{
  if(e.code === 'Space' || e.code === 'Escape'){ e.preventDefault(); doEstop(); }
});

document.addEventListener('visibilitychange', ()=>{
  if(document.hidden) doEstop();
});
window.addEventListener('beforeunload', ()=>{
  navigator.sendBeacon && navigator.sendBeacon('/estop', new Blob([JSON.stringify({})], {type:'application/json'}));
});
</script>
</body>
</html>
"""


@app.route('/')
def index():
    return Response(INDEX_HTML, mimetype='text/html')


if __name__ == '__main__':
    print("Open http://127.0.0.1:5056 in your browser", flush=True)
    app.run(host='127.0.0.1', port=5056)
