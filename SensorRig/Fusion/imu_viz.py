import serial, struct, threading, time, math, json
from flask import Flask, jsonify, Response, request
from websockets.sync.client import connect as ws_connect

PORT = 'COM4'
BAUD = 115200
MSP_STATUS = 101
MSP_RAW_IMU = 102
MSP_MOTOR = 104
MSP_SET_MOTOR = 214
MSP_ATTITUDE = 108
MSP_ALTITUDE = 109

MOTOR_STOP = 1000
MOTOR_SAFE_CAP = 1300      # default ceiling — plenty to confirm spin direction/sound
MOTOR_ABS_MAX = 2000
HEARTBEAT_TIMEOUT = 0.6    # seconds — motor test mode auto-disarms if the page stops polling

CALIBRATION_SAMPLES = 20  # samples averaged at each reset for a cleaner reference

# --- Pi bridge: this drone IMU is now the real rig-pose source for the Pi's
# person-detection fusion pipeline (replaces the old fake_rig.py stub), and
# in turn receives the Pi's detections back so they can be plotted relative
# to the drone here. ---
BRIDGE_URL = 'ws://larp-pi.local:8765/'
RADAR_MAX_M = 20.0     # minimap / 3D marker visible range
DET_STALE_S = 1.5      # drop a detection from the display if not refreshed within this long

# --- real camera feed (replaces the old fake thermal-HUD canvas). Served by
# pi_camera_stream.py running on the Pi (MJPEG over plain HTTP, no auth) ---
PI_STREAM_URL = 'http://larp-pi.local:8766/stream'

app = Flask(__name__)
state = {
    'roll': 0.0, 'pitch': 0.0, 'yaw': 0.0, 'ok': False,
    'x': 0.0, 'y': 0.0, 'z': 0.0,        # cm, relative to last reset
    'speed': 0.0, 'moving': False,
    'baro': False, 'calibrating': False,
    'motors': [1000, 1000, 1000, 1000],  # live MSP_MOTOR readback (1000-2000 range), motors 1-4

    # --- motor test-mode override state (same mechanism as motor_test.py /
    # Betaflight's Motors tab — merged in here so you don't need a second
    # program fighting motor_test.py or Betaflight Configurator for COM4) ---
    'armed_test': False,
    'unlocked': False,
    'throttle': 1150,
    'num_motors': 4,
    'motor_on': [False] * 8,
    'motor_cmd': [MOTOR_STOP] * 8,
    'last_heartbeat': 0.0,
    'last_error': '',
}
lock = threading.Lock()

# --- position estimation state (gravity-compensated accel, gyro-gated ZUPT) ---
accel_scale = None      # raw units per 1g
gravity_world = None    # (gx,gy,gz) in g's, captured at calibration
vel = [0.0, 0.0, 0.0]   # m/s, world frame
pos = [0.0, 0.0, 0.0]   # m, world frame (x,y horizontal, z = up, accel-based)
lin_filt = [0.0, 0.0, 0.0]  # smoothed linear accel, m/s^2
still_count = 0
last_t = None

need_calibrate = True
calib_buf = []          # list of (ax,ay,az,roll,pitch,yaw) collected while calibrating
CALIB_WINDOW = 0.4      # seconds of "hold still" data to average at reset

baro_detected = False
baro_ref_cm = None      # altitude reading captured at reset, if baro present
baro_calib_buf = []

# --- bridge (Pi link) state ---
bridge_lock = threading.Lock()
bridge_connected = False
latest_detections = []       # list of {id, x, y, ...} in the Pi/rig's world frame (meters)
latest_detections_at = 0.0


def msp_request(ser, cmd, payload=b''):
    size = len(payload)
    body = bytes([size, cmd]) + payload
    checksum = 0
    for b in body:
        checksum ^= b
    ser.write(b'$M<' + body + bytes([checksum]))


def read_msp_response(ser, timeout=0.3):
    ser.timeout = timeout
    start = time.time()
    while time.time() - start < timeout:
        c = ser.read(1)
        if c == b'$':
            if ser.read(2) != b'M>':
                continue
            size_b = ser.read(1)
            if not size_b:
                return None
            size = size_b[0]
            cmd_b = ser.read(1)
            if not cmd_b:
                return None
            cmd = cmd_b[0]
            payload = ser.read(size)
            ser.read(1)  # checksum, not verified
            return cmd, payload
    return None


def body_to_world(vec, roll_deg, pitch_deg, yaw_deg):
    """Rotate a body-frame vector into world frame using aircraft ZYX Euler angles."""
    r = math.radians(roll_deg)
    p = math.radians(pitch_deg)
    y = math.radians(yaw_deg)
    cr, sr = math.cos(r), math.sin(r)
    cp, sp = math.cos(p), math.sin(p)
    cy, sy = math.cos(y), math.sin(y)
    x, yv, z = vec
    r00 = cy*cp
    r01 = cy*sp*sr - sy*cr
    r02 = cy*sp*cr + sy*sr
    r10 = sy*cp
    r11 = sy*sp*sr + cy*cr
    r12 = sy*sp*cr - cy*sr
    r20 = -sp
    r21 = cp*sr
    r22 = cp*cr
    return (
        r00*x + r01*yv + r02*z,
        r10*x + r11*yv + r12*z,
        r20*x + r21*yv + r22*z,
    )


def reset_position():
    """Start a short still-hold calibration window; finalized once enough
    samples land (see process_imu). Zeroes position/velocity immediately."""
    global need_calibrate, vel, pos, calib_buf, baro_calib_buf, baro_ref_cm, lin_filt
    need_calibrate = True
    calib_buf = []
    baro_calib_buf = []
    baro_ref_cm = None
    vel = [0.0, 0.0, 0.0]
    pos = [0.0, 0.0, 0.0]
    lin_filt = [0.0, 0.0, 0.0]


def finalize_calibration():
    global accel_scale, gravity_world, need_calibrate, last_t, baro_ref_cm
    n = len(calib_buf)
    ax = sum(s[0] for s in calib_buf) / n
    ay = sum(s[1] for s in calib_buf) / n
    az = sum(s[2] for s in calib_buf) / n
    roll = sum(s[3] for s in calib_buf) / n
    pitch = sum(s[4] for s in calib_buf) / n
    yaw = sum(s[5] for s in calib_buf) / n
    accel_scale = math.sqrt(ax*ax + ay*ay + az*az)
    g_body = (ax/accel_scale, ay/accel_scale, az/accel_scale)
    gravity_world = body_to_world(g_body, roll, pitch, yaw)
    if baro_calib_buf:
        baro_ref_cm = sum(baro_calib_buf) / len(baro_calib_buf)
    need_calibrate = False
    last_t = time.time()


def process_imu(ax, ay, az, gx, gy, gz, roll, pitch, yaw, alt_cm, now):
    global vel, pos, still_count, last_t, need_calibrate, lin_filt

    if need_calibrate:
        calib_buf.append((ax, ay, az, roll, pitch, yaw))
        if alt_cm is not None:
            baro_calib_buf.append(alt_cm)
        if len(calib_buf) >= CALIBRATION_SAMPLES:
            finalize_calibration()
        return

    if accel_scale is None:
        return

    if last_t is None:
        last_t = now
        return
    dt = now - last_t
    last_t = now
    if dt <= 0 or dt > 0.5:
        return

    ag_body = (ax/accel_scale, ay/accel_scale, az/accel_scale)
    ag_world = body_to_world(ag_body, roll, pitch, yaw)

    lin = (
        (ag_world[0] - gravity_world[0]) * 9.81,
        (ag_world[1] - gravity_world[1]) * 9.81,
        (ag_world[2] - gravity_world[2]) * 9.81,
    )

    # light exponential smoothing to cut sensor noise before integrating
    alpha = 0.35
    for i in range(3):
        lin_filt[i] = alpha*lin[i] + (1-alpha)*lin_filt[i]
    lin_s = lin_filt

    lin_mag = math.sqrt(sum(c*c for c in lin_s))
    if lin_mag < 0.2:
        lin_s = (0.0, 0.0, 0.0)

    raw_mag = math.sqrt(ax*ax + ay*ay + az*az)
    accel_still = abs(raw_mag/accel_scale - 1.0) < 0.03
    gyro_mag = math.sqrt(gx*gx + gy*gy + gz*gz)
    gyro_still = gyro_mag < 25  # raw units; not calibrated to deg/s, just a relative gate
    stationary_now = accel_still and gyro_still

    if stationary_now:
        still_count += 1
    else:
        still_count = 0

    for i in range(3):
        vel[i] += lin_s[i] * dt
        vel[i] *= 0.985  # gentle anti-drift leak

    if still_count > 5:
        vel[:] = [0.0, 0.0, 0.0]

    for i in range(3):
        pos[i] += vel[i] * dt

    speed = math.sqrt(sum(v*v for v in vel))
    with lock:
        state['x'] = pos[0] * 100.0
        state['y'] = pos[1] * 100.0
        if baro_detected and baro_ref_cm is not None and alt_cm is not None:
            state['z'] = alt_cm - baro_ref_cm   # trust the barometer for height
        else:
            state['z'] = pos[2] * 100.0
        state['speed'] = speed
        state['moving'] = not stationary_now
        state['calibrating'] = False


def poller():
    global baro_detected
    ser = None
    checked_sensors = False
    while True:
        try:
            if ser is None:
                ser = serial.Serial(PORT, BAUD, timeout=0.5)
                time.sleep(0.3)
                checked_sensors = False

            if not checked_sensors:
                msp_request(ser, MSP_STATUS)
                r = read_msp_response(ser)
                if r and r[0] == MSP_STATUS and len(r[1]) >= 6:
                    _, _, sensor_bits = struct.unpack('<HHH', r[1][:6])
                    baro_detected = bool(sensor_bits & 0x02)
                    with lock:
                        state['baro'] = baro_detected
                    checked_sensors = True

            with lock:
                state['calibrating'] = need_calibrate

            msp_request(ser, MSP_ATTITUDE)
            resp = read_msp_response(ser)
            roll = pitch = yaw = None
            if resp:
                cmd, payload = resp
                if cmd == MSP_ATTITUDE and len(payload) >= 6:
                    r, p, y = struct.unpack('<hhh', payload[:6])
                    roll, pitch, yaw = r/10.0, p/10.0, float(y)
                    with lock:
                        state['roll'] = roll
                        state['pitch'] = pitch
                        state['yaw'] = yaw
                        state['ok'] = True
            else:
                with lock:
                    state['ok'] = False

            msp_request(ser, MSP_MOTOR)
            resp_m = read_msp_response(ser)
            if resp_m:
                cmd_m, payload_m = resp_m
                if cmd_m == MSP_MOTOR and len(payload_m) >= 8:
                    m1, m2, m3, m4 = struct.unpack('<HHHH', payload_m[:8])
                    with lock:
                        state['motors'] = [m1, m2, m3, m4]

            # --- motor test-mode override: re-sent every loop, exactly like
            # motor_test.py / Betaflight Configurator's Motors tab do, since a
            # single MSP_SET_MOTOR only holds until the FC's own override
            # timeout lapses ---
            with lock:
                stale = (time.time() - state['last_heartbeat']) > HEARTBEAT_TIMEOUT
                if stale and state['armed_test']:
                    state['armed_test'] = False
                    state['motor_on'] = [False] * 8
                    state['last_error'] = 'Watchdog: lost contact with the dashboard — motors stopped.'
                cap = MOTOR_ABS_MAX if state['unlocked'] else MOTOR_SAFE_CAP
                thr = max(MOTOR_STOP, min(cap, state['throttle']))
                cmds = [
                    thr if (state['armed_test'] and state['motor_on'][i]) else MOTOR_STOP
                    for i in range(8)
                ]
                state['motor_cmd'] = cmds
            msp_request(ser, MSP_SET_MOTOR, struct.pack('<8H', *cmds))
            read_msp_response(ser, timeout=0.05)  # drain ack if the FC sends one, don't stall the loop on it

            msp_request(ser, MSP_RAW_IMU)
            resp2 = read_msp_response(ser)
            ax = ay = az = gx = gy = gz = None
            if resp2:
                cmd2, payload2 = resp2
                if cmd2 == MSP_RAW_IMU and len(payload2) >= 12:
                    ax, ay, az, gx, gy, gz = struct.unpack('<hhhhhh', payload2[:12])

            alt_cm = None
            if baro_detected:
                msp_request(ser, MSP_ALTITUDE)
                resp3 = read_msp_response(ser)
                if resp3:
                    cmd3, payload3 = resp3
                    if cmd3 == MSP_ALTITUDE and len(payload3) >= 4:
                        (alt_raw,) = struct.unpack('<i', payload3[:4])
                        alt_cm = float(alt_raw)

            if roll is not None and ax is not None:
                process_imu(ax, ay, az, gx, gy, gz, roll, pitch, yaw, alt_cm, time.time())
        except Exception:
            with lock:
                state['ok'] = False
            try:
                if ser:
                    ser.close()
            except Exception:
                pass
            ser = None
            time.sleep(1)
        time.sleep(0.015)


def bridge_receiver(ws):
    """Reads whatever the Pi's bridge broadcasts back at us (mainly fused
    person-detection packets from infer_stream.py) on its own thread, since
    the same sync websocket connection can be read from one thread while
    bridge_client() sends on another."""
    global latest_detections, latest_detections_at
    for message in ws:
        try:
            packet = json.loads(message)
        except json.JSONDecodeError:
            continue
        dets = packet.get('detections')
        if dets is not None:
            with bridge_lock:
                latest_detections = dets
                latest_detections_at = time.time()


def bridge_client():
    """Publishes this drone's real IMU-derived pose to the Pi's bridge as the
    rig-pose source (what fake_rig.py used to fake), and spins up
    bridge_receiver() on the same connection to pull detections back.
    Auto-reconnects on any drop, same pattern proven in fake_rig.py."""
    global bridge_connected
    while True:
        try:
            with ws_connect(BRIDGE_URL) as ws:
                bridge_connected = True
                print(f"[bridge] connected to {BRIDGE_URL}", flush=True)
                t = threading.Thread(target=bridge_receiver, args=(ws,), daemon=True)
                t.start()
                while True:
                    with lock:
                        s = dict(state)
                    packet = {
                        't': time.time(),
                        'rig': {
                            'x': s['x'] / 100.0,
                            'y': s['y'] / 100.0,
                            'heading_deg': s['yaw'],
                            'tracking_ok': bool(s['ok']) and not s['calibrating'],
                            'tracking_note': 'real drone IMU (MSP over COM4)',
                        },
                    }
                    ws.send(json.dumps(packet))
                    time.sleep(0.1)
        except Exception as e:
            bridge_connected = False
            print(f"[bridge] disconnected ({type(e).__name__}: {e}), retrying in 1s...", flush=True)
            time.sleep(1)


threading.Thread(target=poller, daemon=True).start()
threading.Thread(target=bridge_client, daemon=True).start()


@app.route('/attitude')
def attitude():
    # This poll doubles as the motor-test watchdog heartbeat — as long as the
    # page is open and polling (every 50ms), test mode stays armed.
    with lock:
        state['last_heartbeat'] = time.time()
        d = dict(state)
    with bridge_lock:
        fresh = time.time() - latest_detections_at < DET_STALE_S
        d['detections'] = latest_detections if fresh else []
        d['bridge_ok'] = bridge_connected
    return jsonify(d)


@app.route('/reset', methods=['POST'])
def reset():
    reset_position()
    return jsonify({'ok': True})


# --- motor test-mode routes (verbatim mechanism from motor_test.py) ---

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
<title>Drone IMU Live</title>
<style>
  html,body{margin:0;height:100%;background:#1b2432;overflow:hidden;font-family:'SF Mono',Menlo,monospace;color:#e8eef5;}
  #hud{position:fixed;top:16px;left:16px;background:rgba(10,14,22,0.6);border:1px solid #1e2a3a;border-radius:10px;padding:14px 18px;backdrop-filter:blur(6px);min-width:190px;}
  #hud h1{font-size:13px;letter-spacing:2px;color:#4fd1ff;margin:0 0 8px 0;text-transform:uppercase;}
  #hud h2{font-size:11px;letter-spacing:2px;color:#5a6b80;margin:14px 0 6px 0;text-transform:uppercase;border-top:1px solid #1e2a3a;padding-top:10px;display:flex;justify-content:space-between;}
  #hud .row{display:flex;justify-content:space-between;gap:24px;font-size:14px;padding:2px 0;}
  #hud .row span:first-child{color:#5a6b80;}
  #hud .big{font-size:22px;color:#3cdc8c;font-weight:bold;text-align:center;margin-top:4px;}
  #hud .caption{font-size:10px;color:#5a6b80;text-align:center;margin-top:2px;}
  #baroTag{font-size:9px;padding:1px 6px;border-radius:8px;border:1px solid #5a6b80;color:#5a6b80;}
  #baroTag.on{border-color:#3cdc8c;color:#3cdc8c;}
  #resetBtn{margin-top:12px;width:100%;padding:8px;background:#132030;border:1px solid #4fd1ff;color:#4fd1ff;border-radius:6px;cursor:pointer;font-family:inherit;font-size:12px;letter-spacing:1px;text-transform:uppercase;}
  #resetBtn:hover{background:#1c3348;}
  #resetBtn:active{transform:scale(0.97);}
  #status{position:fixed;top:16px;right:16px;padding:6px 12px;border-radius:20px;font-size:12px;letter-spacing:1px;}
  #bridgeBadge{position:fixed;top:52px;right:16px;padding:6px 12px;border-radius:20px;font-size:12px;letter-spacing:1px;}
  .ok{background:rgba(60,220,140,0.15);color:#3cdc8c;border:1px solid #3cdc8c;}
  .bad{background:rgba(220,60,60,0.15);color:#e05a5a;border:1px solid #e05a5a;}
  .moving{background:rgba(255,200,60,0.15) !important;color:#ffc83c !important;border-color:#ffc83c !important;}
  .calib{background:rgba(79,209,255,0.15) !important;color:#4fd1ff !important;border-color:#4fd1ff !important;}
  #radarWrap{position:fixed;bottom:16px;right:16px;border:1px solid #1e2a3a;border-radius:10px;background:rgba(10,14,22,0.6);backdrop-filter:blur(6px);padding:8px;text-align:center;z-index:5;}
  #radarWrap .cap{font-size:9px;letter-spacing:1px;color:#5a6b80;text-transform:uppercase;margin-top:4px;}
  canvas{display:block;}
  #holoPane{position:fixed;top:0;left:0;width:100vw;height:50vh;background:#050a10;overflow:hidden;border-bottom:1px solid #123;}
  #piCam{width:100%;height:100%;object-fit:cover;display:block;filter:saturate(1.05) contrast(1.05);}
  #camStatus{position:fixed;top:8px;right:16px;padding:5px 11px;border-radius:14px;font-size:11px;letter-spacing:1px;z-index:6;}
  #camStatus.ok{background:rgba(60,220,140,0.15);color:#3cdc8c;border:1px solid #3cdc8c;}
  #camStatus.bad{background:rgba(220,60,60,0.15);color:#e05a5a;border:1px solid #e05a5a;}
  #dronePane{position:fixed;top:50vh;left:0;width:100vw;height:50vh;background:#1b2432;}
  #paneLabel{position:fixed;top:8px;left:50%;transform:translateX(-50%);font-size:10px;letter-spacing:3px;color:#2a6b7a;text-transform:uppercase;z-index:5;pointer-events:none;}
  #hud,#status,#bridgeBadge{z-index:10;}

  /* --- motor test-mode panel (merged from motor_test.py) --- */
  #motorPanel{position:fixed;bottom:16px;left:16px;width:300px;max-height:calc(100vh - 32px);overflow-y:auto;background:rgba(10,14,22,0.75);border:1px solid #1e2a3a;border-radius:10px;padding:14px 16px;backdrop-filter:blur(6px);z-index:10;}
  #motorPanel h1{font-size:12px;letter-spacing:2px;color:#4fd1ff;margin:0 0 8px 0;text-transform:uppercase;}
  #motorPanel .warn{background:rgba(255,200,60,0.08);border:1px solid #6b5a1e;border-radius:8px;padding:8px 10px;font-size:11px;color:#ffc83c;margin-bottom:10px;line-height:1.4;}
  #motorPanel .pillrow{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:10px;}
  #motorPanel .pill{display:inline-block;padding:3px 9px;border-radius:14px;font-size:10px;letter-spacing:0.5px;border:1px solid #5a6b80;color:#5a6b80;}
  #motorPanel .pill.ok{border-color:#3cdc8c;color:#3cdc8c;background:rgba(60,220,140,0.1);}
  #motorPanel .pill.bad{border-color:#e05a5a;color:#e05a5a;background:rgba(220,60,60,0.1);}
  #motorPanel .pill.armed{border-color:#ffc83c;color:#ffc83c;background:rgba(255,200,60,0.15);animation:pulse 1.2s infinite;}
  @keyframes pulse{0%,100%{opacity:1;}50%{opacity:0.55;}}
  #motorPanel label{font-size:11px;color:#b7c3d1;}
  #motorPanel input[type=checkbox]{width:14px;height:14px;vertical-align:middle;margin-right:5px;}
  #motorPanel .row{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin-bottom:8px;}
  #motorPanel button{font-family:inherit;font-size:11px;letter-spacing:0.5px;text-transform:uppercase;border-radius:6px;padding:7px 12px;cursor:pointer;border:1px solid #4fd1ff;background:#132030;color:#4fd1ff;}
  #motorPanel button:hover:not(:disabled){background:#1c3348;}
  #motorPanel button:disabled{opacity:0.35;cursor:not-allowed;}
  #motorPanel button.go{border-color:#3cdc8c;color:#3cdc8c;}
  #motorPanel button.stop{border-color:#e05a5a;color:#e05a5a;}
  #motorPanel input[type=range]{width:150px;}
  #motorGrid{display:grid;grid-template-columns:repeat(2,1fr);gap:8px;margin-top:8px;}
  .mcard{border:1px solid #1e2a3a;border-radius:8px;padding:8px;text-align:center;background:rgba(20,26,38,0.6);}
  .mcard.on{border-color:#3cdc8c;background:rgba(60,220,140,0.08);}
  .mlabel{font-size:11px;color:#b7c3d1;letter-spacing:1px;margin-bottom:4px;}
  .toggle{position:relative;display:inline-block;width:38px;height:21px;}
  .toggle input{opacity:0;width:0;height:0;}
  .slider{position:absolute;cursor:pointer;inset:0;background:#2a3a4d;border-radius:21px;transition:0.15s;}
  .slider:before{position:absolute;content:"";height:15px;width:15px;left:3px;bottom:3px;background:#8a97a8;border-radius:50%;transition:0.15s;}
  input:checked + .slider{background:#1a4a30;}
  input:checked + .slider:before{background:#3cdc8c;transform:translateX(17px);}
  input:disabled + .slider{opacity:0.4;cursor:not-allowed;}
  .mval{font-size:10px;color:#5a6b80;margin-top:4px;}
  #motorLog{font-size:10px;color:#e05a5a;min-height:12px;margin-top:6px;}
  #estop{position:fixed;bottom:16px;left:50%;transform:translateX(-50%);z-index:20;padding:16px 44px;font-size:15px;font-weight:bold;letter-spacing:3px;border-radius:12px;border:2px solid #e05a5a;background:#3a1414;color:#ff8a8a;box-shadow:0 0 24px rgba(224,90,90,0.5);cursor:pointer;font-family:inherit;}
  #estop:hover{background:#4a1818;}
</style>
</head>
<body>
<div id="holoPane"><img id="piCam" alt="Pi camera feed"></div>
<div id="camStatus" class="bad">CONNECTING…</div>
<div id="dronePane"></div>
<div id="paneLabel">Pi Camera // Live Uplink</div>
<div id="hud">
  <h1>Live IMU // Attitude</h1>
  <div class="row"><span>Roll</span><span id="r">0.0&deg;</span></div>
  <div class="row"><span>Pitch</span><span id="p">0.0&deg;</span></div>
  <div class="row"><span>Yaw</span><span id="y">0.0&deg;</span></div>

  <h2><span>Height off ground</span><span id="baroTag">NO BARO</span></h2>
  <div class="big" id="hz">0.0 cm</div>
  <div class="caption" id="hzCaption">estimated from IMU, drifts over time</div>

  <h2>Position (since reset)</h2>
  <div class="row"><span>X</span><span id="px">0.0 cm</span></div>
  <div class="row"><span>Y</span><span id="py">0.0 cm</span></div>
  <div class="row"><span>Z</span><span id="pz">0.0 cm</span></div>

  <h2>Motors (live)</h2>
  <div class="row"><span>M1</span><span id="m1">1000</span></div>
  <div class="row"><span>M2</span><span id="m2">1000</span></div>
  <div class="row"><span>M3</span><span id="m3">1000</span></div>
  <div class="row"><span>M4</span><span id="m4">1000</span></div>

  <h2>Pi bridge</h2>
  <div class="row"><span>Link</span><span id="bridgeState">connecting…</span></div>
  <div class="row"><span>People seen</span><span id="peopleCount">0</span></div>

  <button id="resetBtn">Reset to Ground</button>
</div>
<div id="radarWrap">
  <canvas id="radar" width="220" height="220"></canvas>
  <div class="cap">RADAR // 0-20m, drone-relative</div>
</div>
<div id="status" class="bad">NO SIGNAL</div>
<div id="bridgeBadge" class="bad">PI LINK DOWN</div>

<div id="motorPanel">
  <h1>Motor Test</h1>
  <div class="warn">&#9888; Props must be OFF. Keep hands/cables clear. Disconnect your RX first if it could arm the FC on its own.</div>
  <div class="pillrow">
    <span class="pill" id="armPill">SAFE</span>
    <span class="pill" id="wdPill">WATCHDOG OK</span>
  </div>
  <div class="row">
    <label><input type="checkbox" id="confirmChk"> Props removed, area clear</label>
  </div>
  <div class="row">
    <button id="armBtn" class="go" disabled>Arm Test Mode</button>
  </div>
  <div class="row">
    <label>Motors:
      <select id="numMotors">
        <option value="2">2</option>
        <option value="3">3</option>
        <option value="4" selected>4</option>
        <option value="6">6</option>
        <option value="8">8</option>
      </select>
    </label>
    <label><input type="checkbox" id="unlockChk"> Unlock (2000&micro;s)</label>
  </div>
  <div class="row">
    <label for="throttle">Throttle</label>
    <input type="range" id="throttle" min="1000" max="1300" value="1150">
    <span id="throttleVal">1150&micro;s</span>
  </div>
  <div class="row">
    <button id="allOn" disabled>All On</button>
    <button id="allOff" disabled>All Off</button>
  </div>
  <div id="motorGrid"></div>
  <div id="motorLog"></div>
</div>
<button id="estop">EMERGENCY STOP</button>
<script src="https://cdnjs.cloudflare.com/ajax/libs/three.js/r128/three.min.js"></script>
<script>
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x1b2432);
scene.fog = new THREE.FogExp2(0x1b2432, 0.006);
const camera = new THREE.PerspectiveCamera(55, innerWidth/(innerHeight/2), 0.1, 1000);
camera.position.set(0, 6, 16);
camera.lookAt(0,0,0);
const renderer = new THREE.WebGLRenderer({antialias:true});
renderer.setSize(innerWidth, innerHeight/2);
renderer.setPixelRatio(devicePixelRatio);
renderer.setClearColor(0x1b2432, 1);
document.getElementById('dronePane').appendChild(renderer.domElement);

scene.add(new THREE.AmbientLight(0xffffff, 1.4));
const key = new THREE.DirectionalLight(0x9fe8ff, 1.6);
key.position.set(4,8,5);
scene.add(key);
const fill = new THREE.DirectionalLight(0xffffff, 0.8);
fill.position.set(-4,3,4);
scene.add(fill);
const rim = new THREE.DirectionalLight(0xff9a5a, 0.9);
rim.position.set(-5,-2,-4);
scene.add(rim);

const grid = new THREE.GridHelper(80, 40, 0x4a5a70, 0x33404f);
grid.position.y = -2.2;
scene.add(grid);

const heightLineGeom = new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(0,-2.2,0), new THREE.Vector3(0,-2.2,0)]);
const heightLine = new THREE.Line(heightLineGeom, new THREE.LineBasicMaterial({color:0x4fd1ff, transparent:true, opacity:0.5}));
scene.add(heightLine);

const MAX_TRAIL = 300;
const trailPositions = new Float32Array(MAX_TRAIL*3);
const trailGeom = new THREE.BufferGeometry();
trailGeom.setAttribute('position', new THREE.BufferAttribute(trailPositions, 3));
const trail = new THREE.Line(trailGeom, new THREE.LineBasicMaterial({color:0x3cdc8c, transparent:true, opacity:0.6}));
scene.add(trail);
let trailPts = [];

const drone = new THREE.Group();
const bodyMat = new THREE.MeshStandardMaterial({color:0xe8edf3, metalness:0.35, roughness:0.45});
const accentMat = new THREE.MeshStandardMaterial({color:0x4fd1ff, emissive:0x1288b8, emissiveIntensity:0.8, metalness:0.2, roughness:0.3});
const armMat = new THREE.MeshStandardMaterial({color:0x8a97a8, metalness:0.5, roughness:0.4});
const propMat = new THREE.MeshStandardMaterial({color:0xc7d2e0, metalness:0.1, roughness:0.5, transparent:true, opacity:0.75});

const core = new THREE.Mesh(new THREE.BoxGeometry(1.1,0.28,1.1), bodyMat);
drone.add(core);
const canopy = new THREE.Mesh(new THREE.ConeGeometry(0.32,0.4,4), accentMat);
canopy.rotation.y = Math.PI/4;
canopy.position.y = 0.28;
drone.add(canopy);

// Order here matches Betaflight's own MSP_MOTOR index order (motor1..motor4),
// so propMeshes[i] always spins in step with the real motor[i] value.
const armPositions = [[1,1],[1,-1],[-1,1],[-1,-1]];
const propMeshes = [];
armPositions.forEach(([sx,sz])=>{
  const arm = new THREE.Mesh(new THREE.CylinderGeometry(0.05,0.05,1.5,8), armMat);
  arm.rotation.z = Math.PI/2;
  arm.rotation.y = Math.atan2(sz,sx);
  arm.position.set(sx*0.75, 0, sz*0.75);
  drone.add(arm);

  const motor = new THREE.Mesh(new THREE.CylinderGeometry(0.13,0.13,0.22,12), armMat);
  motor.position.set(sx*1.5, 0.05, sz*1.5);
  drone.add(motor);

  const prop = new THREE.Mesh(new THREE.BoxGeometry(1.3,0.02,0.09), propMat);
  prop.position.set(sx*1.5, 0.18, sz*1.5);
  drone.add(prop);
  propMeshes.push(prop);

  const led = new THREE.Mesh(new THREE.SphereGeometry(0.05,8,8), new THREE.MeshBasicMaterial({color: sx>0?0x3cdc8c:0xe05a5a}));
  led.position.set(sx*1.5, -0.08, sz*1.5);
  drone.add(led);
});

// The Raspberry Pi + camera riding on top of the airframe — this is the
// actual sensor doing the person-detection, so it gets called out clearly.
// Sized roughly to a Pi 4/5 board footprint (85mm x 56mm) at this scene's scale.
const piMat = new THREE.MeshStandardMaterial({color:0x2fbf5e, emissive:0x0f5c28, emissiveIntensity:0.5, metalness:0.15, roughness:0.5});
const piBoard = new THREE.Mesh(new THREE.BoxGeometry(0.5, 0.06, 0.34), piMat);
piBoard.position.y = 0.34;
drone.add(piBoard);
const lensMat = new THREE.MeshStandardMaterial({color:0x14181f, metalness:0.6, roughness:0.2});
const piCam = new THREE.Mesh(new THREE.CylinderGeometry(0.06,0.06,0.08,16), lensMat);
piCam.rotation.x = Math.PI/2;
piCam.position.set(0.15, 0.34, 0.21);
drone.add(piCam);
const lensGlint = new THREE.Mesh(new THREE.CircleGeometry(0.035,16), new THREE.MeshBasicMaterial({color:0x4fd1ff}));
lensGlint.position.set(0.15, 0.34, 0.255);
drone.add(lensGlint);

scene.add(drone);

let target = {roll:0, pitch:0, yaw:0, x:0, y:0, z:0};
let current = {roll:0, pitch:0, yaw:0, x:0, y:0, z:0};
const POS_SCALE = 0.06;

// live motor throttle (raw MSP values, 1000=idle/off .. 2000=full) and the
// visual spin angle we integrate from them each frame, per motor
let motors = [1000, 1000, 1000, 1000];
const motorSpin = [0, 0, 0, 0];
const MOTOR_IDLE = 1000;
function motorOmega(v){
  // rad/s of visual spin — 0 at idle, fast-but-readable near full throttle.
  // Not real prop RPM (that would just be a blur); scaled for a clear,
  // proportional "is this motor actually being told to spin, and how hard" read.
  return Math.max(0, v - MOTOR_IDLE) / 1000 * 45;
}

// --- motor test-mode panel state (merged from motor_test.py) ---
let mt = { armed_test:false, num_motors:4, motor_on:Array(8).fill(false), motor_cmd:Array(8).fill(1000), unlocked:false, throttle:1150, last_error:'' };
let autoTestRunning = false;

function buildGrid(){
  const grid = document.getElementById('motorGrid');
  grid.innerHTML = '';
  for(let i=0;i<mt.num_motors;i++){
    const card = document.createElement('div');
    card.className = 'mcard';
    card.id = 'mcard'+i;
    card.innerHTML = `
      <div class="mlabel">M${i+1}</div>
      <label class="toggle">
        <input type="checkbox" class="mtoggle" data-idx="${i}">
        <span class="slider"></span>
      </label>
      <div class="mval" id="mval${i}">1000&micro;s</div>
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

function updateMotorPanel(d){
  mt = {
    armed_test: !!d.armed_test, num_motors: d.num_motors||4,
    motor_on: d.motor_on||Array(8).fill(false), motor_cmd: d.motor_cmd||Array(8).fill(1000),
    unlocked: !!d.unlocked, throttle: d.throttle||1150, last_error: d.last_error||'',
  };

  const armPill = document.getElementById('armPill');
  if(mt.armed_test){ armPill.textContent = 'TEST ARMED'; armPill.className = 'pill armed'; }
  else { armPill.textContent = 'SAFE'; armPill.className = 'pill'; }
  document.getElementById('wdPill').className = 'pill ok';

  document.getElementById('armBtn').textContent = mt.armed_test ? 'Disarm Test Mode' : 'Arm Test Mode';
  document.getElementById('armBtn').className = mt.armed_test ? 'stop' : 'go';
  ['allOn','allOff'].forEach(id => document.getElementById(id).disabled = !mt.armed_test);

  document.getElementById('unlockChk').checked = mt.unlocked;
  const cap = mt.unlocked ? 2000 : 1300;
  const tSlider = document.getElementById('throttle');
  tSlider.max = cap;
  if(document.activeElement !== tSlider) tSlider.value = mt.throttle;
  document.getElementById('throttleVal').textContent = mt.throttle + 'µs';

  for(let i=0;i<mt.num_motors;i++){
    const cb = document.querySelector(`.mtoggle[data-idx="${i}"]`);
    if(!cb) continue;
    if(document.activeElement !== cb) cb.checked = mt.motor_on[i];
    cb.disabled = !mt.armed_test;
    const card = document.getElementById('mcard'+i);
    if(card){
      card.classList.toggle('on', mt.motor_on[i]);
      document.getElementById('mval'+i).textContent = mt.motor_cmd[i] + 'µs';
    }
  }
  document.getElementById('motorLog').textContent = mt.last_error;
}

document.getElementById('confirmChk').addEventListener('change', (e)=>{
  document.getElementById('armBtn').disabled = !e.target.checked;
});
document.getElementById('armBtn').addEventListener('click', async ()=>{
  await fetch('/arm', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({arm: !mt.armed_test})});
});
document.getElementById('numMotors').addEventListener('change', async (e)=>{
  await fetch('/num_motors', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({value: parseInt(e.target.value)})});
  mt.num_motors = parseInt(e.target.value);
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

// person-detection markers — pooled meshes keyed by detection id, positioned
// relative to the drone (the Pi/camera is mounted on it, so "relative to the
// Pi" and "relative to the drone" are the same offset for our purposes).
// Detections arrive in meters (same world frame as the rig pose we publish);
// drone position (current.x/y) is in cm, so /100 brings it to meters too.
const detMat = new THREE.MeshStandardMaterial({color:0xff8a3c, emissive:0xcc5500, emissiveIntensity:1.1, metalness:0.1, roughness:0.4});
const detectionMeshes = {};
const DET_SCALE = POS_SCALE * 100.0;   // meters -> scene units, matching the drone's cm*POS_SCALE convention
const RADAR_MAX_M = 20.0;
let lastDetections = [];
let bridgeOk = false;

// --- real Pi camera feed (MJPEG <img>, replaces the old fake canvas HUD) ---
const CAM_STREAM_URL = '__PI_STREAM_URL__';
const piCamImg = document.getElementById('piCam');
const camStatusEl = document.getElementById('camStatus');
let camRetryTimer = null;
function setCamSrc(){
  piCamImg.src = CAM_STREAM_URL + (CAM_STREAM_URL.includes('?') ? '&' : '?') + 't=' + Date.now();
}
piCamImg.addEventListener('load', ()=>{
  camStatusEl.textContent = 'LIVE'; camStatusEl.className = 'ok';
});
piCamImg.addEventListener('error', ()=>{
  camStatusEl.textContent = 'NO FEED — retrying…'; camStatusEl.className = 'bad';
  clearTimeout(camRetryTimer);
  camRetryTimer = setTimeout(setCamSrc, 2000);
});
setCamSrc();

function updateDetectionMeshes(){
  const seen = new Set();
  const relX0 = current.x/100, relY0 = current.y/100;
  lastDetections.forEach(d=>{
    seen.add(d.id);
    let m = detectionMeshes[d.id];
    if(!m){
      m = new THREE.Mesh(new THREE.CapsuleGeometry(0.15, 0.55, 4, 8), detMat.clone());
      m.userData.grow = 0;
      scene.add(m);
      detectionMeshes[d.id] = m;
    }
    const relX = d.x - relX0, relY = d.y - relY0;
    m.position.set(relX*DET_SCALE, -1.6, -relY*DET_SCALE);
    m.userData.targetPos = m.position.clone();
    m.userData.stale = false;
  });
  Object.keys(detectionMeshes).forEach(id=>{
    if(!seen.has(id)) detectionMeshes[id].userData.stale = true;
  });
}

function animateDetectionMeshes(t){
  Object.entries(detectionMeshes).forEach(([id,m])=>{
    if(m.userData.stale){
      m.scale.multiplyScalar(0.85);
      if(m.scale.x < 0.05){ scene.remove(m); delete detectionMeshes[id]; }
      return;
    }
    m.userData.grow = Math.min(1, (m.userData.grow||0) + 0.08);
    const pulse = 1 + 0.08*Math.sin(t*4 + (id.length||0));
    m.scale.setScalar(m.userData.grow * pulse);
  });
}

async function poll(){
  try{
    const r = await fetch('/attitude');
    const d = await r.json();
    target.roll = d.roll; target.pitch = d.pitch; target.yaw = d.yaw;
    target.x = d.x; target.y = d.y; target.z = d.z;
    const s = document.getElementById('status');
    if(d.calibrating){ s.textContent='CALIBRATING…'; s.className='calib'; }
    else if(d.ok){
      s.textContent = d.moving ? 'MOVING' : 'LIVE';
      s.className = d.moving ? 'moving' : 'ok';
    } else { s.textContent='NO SIGNAL'; s.className='bad'; }

    const baroTag = document.getElementById('baroTag');
    baroTag.textContent = d.baro ? 'BARO' : 'NO BARO';
    baroTag.className = d.baro ? 'on' : '';
    document.getElementById('hzCaption').textContent = d.baro ? 'from barometer — stable' : 'estimated from IMU, drifts over time';

    document.getElementById('hz').textContent = d.z.toFixed(1)+' cm';
    document.getElementById('px').textContent = d.x.toFixed(1)+' cm';
    document.getElementById('py').textContent = d.y.toFixed(1)+' cm';
    document.getElementById('pz').textContent = d.z.toFixed(1)+' cm';

    if(Array.isArray(d.motors) && d.motors.length===4){
      motors = d.motors;
      ['m1','m2','m3','m4'].forEach((id,i)=>{
        document.getElementById(id).textContent = Math.round(motors[i]);
      });
    }

    updateMotorPanel(d);

    lastDetections = d.detections || [];
    bridgeOk = !!d.bridge_ok;
    const bb = document.getElementById('bridgeBadge');
    bb.textContent = bridgeOk ? 'PI LINKED' : 'PI LINK DOWN';
    bb.className = bridgeOk ? 'ok' : 'bad';
    document.getElementById('bridgeState').textContent = bridgeOk ? 'connected' : 'reconnecting…';
    document.getElementById('peopleCount').textContent = lastDetections.length;
    updateDetectionMeshes();
  }catch(e){
    const s = document.getElementById('status');
    s.textContent='SERVER DOWN'; s.className='bad';
  }
}
setInterval(poll, 50);

document.getElementById('resetBtn').addEventListener('click', async ()=>{
  await fetch('/reset', {method:'POST'});
  trailPts = [];
});

function lerp(a,b,t){return a+(b-a)*t;}

let lastFrameT = performance.now()/1000;

function animate(){
  requestAnimationFrame(animate);
  const nowT = performance.now()/1000;
  const dt = Math.min(0.1, Math.max(0, nowT - lastFrameT));
  lastFrameT = nowT;

  current.roll = lerp(current.roll, target.roll, 0.15);
  current.pitch = lerp(current.pitch, target.pitch, 0.15);
  current.yaw = lerp(current.yaw, target.yaw, 0.15);
  current.x = lerp(current.x, target.x, 0.2);
  current.y = lerp(current.y, target.y, 0.2);
  current.z = lerp(current.z, target.z, 0.2);

  drone.rotation.z = -current.roll * Math.PI/180;
  drone.rotation.x = current.pitch * Math.PI/180;
  drone.rotation.y = -current.yaw * Math.PI/180;

  drone.position.x = current.x * POS_SCALE;
  drone.position.y = current.z * POS_SCALE;
  drone.position.z = -current.y * POS_SCALE;

  heightLine.geometry.setFromPoints([
    new THREE.Vector3(drone.position.x, -2.2, drone.position.z),
    new THREE.Vector3(drone.position.x, drone.position.y, drone.position.z),
  ]);

  trailPts.push(drone.position.clone());
  if(trailPts.length > MAX_TRAIL) trailPts.shift();
  for(let i=0;i<trailPts.length;i++){
    trailPositions[i*3] = trailPts[i].x;
    trailPositions[i*3+1] = trailPts[i].y;
    trailPositions[i*3+2] = trailPts[i].z;
  }
  trail.geometry.setDrawRange(0, trailPts.length);
  trail.geometry.attributes.position.needsUpdate = true;

  // spin each propeller at a rate proportional to that motor's real live
  // MSP value — a motor Betaflight isn't driving stays still, one it's
  // driving hard spins visibly faster, in real time
  for(let i=0;i<propMeshes.length;i++){
    motorSpin[i] += motorOmega(motors[i]) * dt;
    propMeshes[i].rotation.y = motorSpin[i];
  }

  document.getElementById('r').textContent = current.roll.toFixed(1)+'°';
  document.getElementById('p').textContent = current.pitch.toFixed(1)+'°';
  document.getElementById('y').textContent = current.yaw.toFixed(1)+'°';

  const t = performance.now()/1000;
  animateDetectionMeshes(t);
  drawRadar();

  renderer.render(scene, camera);
}

function drawRadar(){
  const cv = document.getElementById('radar');
  const ctx = cv.getContext('2d');
  const W = cv.width, H = cv.height, cx = W/2, cy = H/2;
  const R = Math.min(cx,cy) - 8;
  ctx.clearRect(0,0,W,H);

  // range rings every 5m out to RADAR_MAX_M
  ctx.strokeStyle = '#1e2a3a';
  ctx.fillStyle = '#3a4a5f';
  ctx.font = '8px monospace';
  ctx.textAlign = 'center';
  for(let rm=5; rm<=RADAR_MAX_M; rm+=5){
    const rr = R * (rm/RADAR_MAX_M);
    ctx.beginPath(); ctx.arc(cx,cy,rr,0,Math.PI*2); ctx.stroke();
    ctx.fillText(rm+'m', cx + rr - 10, cy - 3);
  }
  ctx.strokeStyle = '#26333f';
  ctx.beginPath(); ctx.moveTo(cx,cy-R); ctx.lineTo(cx,cy+R); ctx.moveTo(cx-R,cy); ctx.lineTo(cx+R,cy); ctx.stroke();

  // drone at center, heading arrow shows current yaw
  const yawRad = current.yaw * Math.PI/180;
  ctx.save();
  ctx.translate(cx,cy);
  ctx.rotate(yawRad);
  ctx.fillStyle = '#4fd1ff';
  ctx.beginPath();
  ctx.moveTo(0,-11); ctx.lineTo(6,7); ctx.lineTo(0,3); ctx.lineTo(-6,7);
  ctx.closePath(); ctx.fill();
  ctx.restore();

  // detections, relative to the drone, scaled to the radar's pixel radius
  const relX0 = current.x/100, relY0 = current.y/100;
  const t = performance.now()/1000;
  lastDetections.forEach((d,i)=>{
    const relX = d.x - relX0, relY = d.y - relY0;
    const dist = Math.sqrt(relX*relX + relY*relY);
    if(dist > RADAR_MAX_M) return;
    const px = cx + (relX/RADAR_MAX_M) * R;
    const py = cy - (relY/RADAR_MAX_M) * R;
    const pulse = 3.5 + 1.5*Math.sin(t*5 + i);
    ctx.fillStyle = 'rgba(255,138,60,0.25)';
    ctx.beginPath(); ctx.arc(px,py,pulse+5,0,Math.PI*2); ctx.fill();
    ctx.fillStyle = '#ff8a3c';
    ctx.beginPath(); ctx.arc(px,py,pulse,0,Math.PI*2); ctx.fill();
    ctx.fillStyle = '#e8eef5';
    ctx.font = '8px monospace';
    ctx.fillText(dist.toFixed(1)+'m', px, py-9);
  });
  ctx.textAlign = 'left';
}
animate();

window.addEventListener('resize', ()=>{
  camera.aspect = innerWidth/(innerHeight/2);
  camera.updateProjectionMatrix();
  renderer.setSize(innerWidth, innerHeight/2);
});
</script>
</body>
</html>
"""


INDEX_HTML = INDEX_HTML.replace('__PI_STREAM_URL__', PI_STREAM_URL)


@app.route('/')
def index():
    return Response(INDEX_HTML, mimetype='text/html')


if __name__ == '__main__':
    print("Open http://127.0.0.1:5055 in your browser")
    app.run(host='127.0.0.1', port=5055)
