#!/usr/bin/env python3
"""
Wallhack live fusion dashboard.

Combines the camera stream (with on-frame person/object detection, same
detector as pi_camera_stream.py) and the LD2450 radar reader (same framing
logic as ld2450_reader.py) into one process, and serves a single live
dashboard page showing both side by side plus a live person-count /
radar-target-count cross-check.

Run on the Pi (from the same folder pi_camera_stream.py lives in, so the
models/ path still resolves -- otherwise it falls back to HOG automatically,
same as before):

    python3 wallhack_dashboard.py

Then open, from any device on the same network as the Pi:
    http://larp-pi.local:8766/
  or, if mDNS isn't resolving for you:
    http://<the Pi's current IP>:8766/

IMPORTANT: stop any already-running pi_camera_stream.py / ld2450_reader.py
first -- this script owns both the camera and the serial port itself, and
two processes can't hold either at the same time.

One-time fix so this doesn't need sudo for the radar port:
    sudo usermod -aG dialout evanl1307
then log out of the SSH session and back in (group membership only takes
effect on a fresh login). After that, plain `python3 wallhack_dashboard.py`
can read /dev/serial0 without sudo.
"""
import math
import struct
import threading
import time

import cv2
import numpy as np
import serial
from flask import Flask, Response, jsonify

from picamera2 import Picamera2

# ---------------------------------------------------------------- camera --
FRAME_W, FRAME_H = 640, 480
STREAM_PORT = 8766
# Detection (YOLOv8n) runs in its OWN thread, decoupled from capture/encode/
# stream. This is the fix for the video feed hanging: YOLOv8n on a Pi 4 CPU
# can take 300ms-1s+ per frame, and running it inline in the same loop that
# feeds the MJPEG stream was stalling the stream for that entire time on
# every detection pass -- from the browser's point of view that looked like
# the /stream request just hanging ("pending") forever. Now capture_loop()
# always runs at full camera framerate and just draws whatever the latest
# finished detection result was, however old it is by a few hundred ms.

# The camera is mounted rotated -- correct it before detection runs (a
# person-detector trained on upright people performs badly on upside-down
# input, so this was very likely hurting camera accuracy too, not just the
# preview). Set False if you remount the ribbon/camera the right way up.
CAMERA_ROTATE_180 = True

# Rough monocular position estimate for each detected person: bearing comes
# from where the box sits left/right in the frame (reliable), distance comes
# from how tall the box is on the assumption of an average adult height
# (a rough guess, nowhere near as precise as the radar's real range
# measurement -- this is for a sanity-check overlay, not a hard number).
# CAMERA_HFOV_DEG default assumes a Pi Camera Module v2 (~62 deg horizontal
# FOV). If you have a v1, v3, or the HQ camera, change this to match --
# it changes both the bearing and distance math below.
CAMERA_HFOV_DEG = 62.0
ASSUMED_PERSON_HEIGHT_M = 1.65
_FOCAL_PX = (FRAME_W / 2.0) / math.tan(math.radians(CAMERA_HFOV_DEG / 2.0))


def estimate_person_position(x1, y1, x2, y2, frame_w):
    """Very rough camera-only position estimate for one detected person box,
    in the same x=lateral-mm / y=forward-mm frame the radar uses, so both
    can be plotted on the same scope."""
    box_h = y2 - y1
    if box_h <= 0:
        return None
    cx = (x1 + x2) / 2.0
    bearing_rad = math.atan((cx - frame_w / 2.0) / _FOCAL_PX)
    dist_mm = (ASSUMED_PERSON_HEIGHT_M * 1000.0 * _FOCAL_PX) / box_h
    return {
        "x": round(dist_mm * math.sin(bearing_rad)),
        "y": round(dist_mm * math.cos(bearing_rad)),
    }

# ---- Detector priority: YOLOv8n (best) > MobileNet-SSD (ok) > HOG (weakest) --
# Each one only gets used if the one above it failed to load, so you don't
# have to touch this file if a model file happens to be missing -- it just
# degrades to the next best thing and logs which one it picked.

# YOLOv8n, COCO 80-class (including "person"), ONNX export, 640x640 input.
# Much more accurate than the old MobileNet-SSD below -- that model is a
# 2016-era 20-class VOC detector and is known to confuse people with
# furniture at odd angles, which is exactly the "detecting a chair" problem.
YOLO_MODEL_PATH = "models/yolov8n.onnx"
YOLO_INPUT_SIZE = 640
YOLO_CONF_THRESH = 0.45
YOLO_NMS_THRESH = 0.45
COCO_CLASSES = [
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush",
]

# MobileNet-SSD model files -- fallback #2 if YOLOv8n isn't available.
PROTOTXT = "models/MobileNetSSD_deploy.prototxt"
MODEL = "models/MobileNetSSD_deploy.caffemodel"
CLASSES = [
    "background", "aeroplane", "bicycle", "bird", "boat", "bottle", "bus",
    "car", "cat", "chair", "cow", "diningtable", "dog", "horse", "motorbike",
    "person", "pottedplant", "sheep", "sofa", "train", "tvmonitor",
]

picam2 = Picamera2()
cam_config = picam2.create_video_configuration(main={"size": (FRAME_W, FRAME_H), "format": "RGB888"})
picam2.configure(cam_config)
picam2.start()
time.sleep(1.0)  # let auto-exposure/white-balance settle

yolo_net = None
net = None
hog = None
try:
    yolo_net = cv2.dnn.readNetFromONNX(YOLO_MODEL_PATH)
    print("[detect] YOLOv8n loaded -- COCO 80-class detector (best available)")
except Exception as e:
    print(f"[detect] YOLOv8n not available ({e}) -- trying MobileNet-SSD")
    try:
        net = cv2.dnn.readNetFromCaffe(PROTOTXT, MODEL)
        print("[detect] MobileNet-SSD loaded -- general object detection (20 classes incl. person)")
    except Exception as e2:
        print(f"[detect] MobileNet-SSD not available ({e2}) -- falling back to HOG person detector")
        hog = cv2.HOGDescriptor()
        hog.setSVMDetector(cv2.HOGDescriptor_getDefaultPeopleDetector())

cam_lock = threading.Lock()
cam_state = {"jpeg": None, "person_count": 0, "cam_positions": [], "ts": 0.0}

# Raw (rotated, BGR) frame most recently captured -- detect_loop reads this,
# capture_loop writes it every iteration. Kept separate from cam_state so a
# slow detection pass never blocks the fast capture/encode/stream path.
frame_lock = threading.Lock()
latest_frame = {"frame": None}

# Most recently *finished* detection result. capture_loop draws these boxes
# onto whatever the current frame is -- cheap (just rectangles/text), never
# waits on inference itself.
detect_lock = threading.Lock()
detect_state = {"boxes": [], "person_count": 0, "cam_positions": []}


def yolo_detect(frame):
    """Run YOLOv8n and return a list of (x1, y1, x2, y2, class_id, confidence)
    in the original frame's pixel coordinates."""
    h, w = frame.shape[:2]
    scale = YOLO_INPUT_SIZE / max(h, w)
    nh, nw = int(round(h * scale)), int(round(w * scale))
    resized = cv2.resize(frame, (nw, nh))
    canvas = np.zeros((YOLO_INPUT_SIZE, YOLO_INPUT_SIZE, 3), dtype=np.uint8)
    canvas[0:nh, 0:nw] = resized

    blob = cv2.dnn.blobFromImage(canvas, scalefactor=1 / 255.0,
                                  size=(YOLO_INPUT_SIZE, YOLO_INPUT_SIZE),
                                  swapRB=True, crop=False)
    yolo_net.setInput(blob)
    output = yolo_net.forward()       # (1, 84, 8400)
    output = output[0].T              # (8400, 84): [cx, cy, bw, bh, 80 class scores...]

    boxes, scores, class_ids = [], [], []
    class_scores_all = output[:, 4:]
    best_class_ids = np.argmax(class_scores_all, axis=1)
    best_scores = class_scores_all[np.arange(len(output)), best_class_ids]
    keep = best_scores >= YOLO_CONF_THRESH
    for row, cid, score in zip(output[keep], best_class_ids[keep], best_scores[keep]):
        cx, cy, bw, bh = row[0], row[1], row[2], row[3]
        x1 = (cx - bw / 2) / scale
        y1 = (cy - bh / 2) / scale
        boxes.append([int(x1), int(y1), int(bw / scale), int(bh / scale)])
        scores.append(float(score))
        class_ids.append(int(cid))

    results = []
    if boxes:
        indices = cv2.dnn.NMSBoxes(boxes, scores, YOLO_CONF_THRESH, YOLO_NMS_THRESH)
        for i in np.array(indices).flatten():
            x, y, bw_, bh_ = boxes[i]
            x1, y1 = max(0, x), max(0, y)
            x2, y2 = min(w, x + bw_), min(h, y + bh_)
            results.append((x1, y1, x2, y2, class_ids[i], scores[i]))
    return results


def run_detection(frame):
    """Pure detection pass -- NO drawing here. Returns (boxes, person_count,
    cam_positions) where boxes is a list of (x1, y1, x2, y2, label_name,
    conf, color) ready to be drawn onto any frame of the same size. Kept
    separate from drawing so this (the slow part) can run in its own thread
    while capture_loop (the fast part) just draws the latest result."""
    h, w = frame.shape[:2]
    person_count = 0
    cam_positions = []
    boxes = []
    if yolo_net is not None:
        for (x1, y1, x2, y2, class_id, conf) in yolo_detect(frame):
            label_name = COCO_CLASSES[class_id] if class_id < len(COCO_CLASSES) else str(class_id)
            if label_name == "person":
                person_count += 1
                pos = estimate_person_position(x1, y1, x2, y2, w)
                if pos is not None:
                    cam_positions.append(pos)
            color = (140, 220, 60) if label_name == "person" else (60, 180, 255)
            boxes.append((x1, y1, x2, y2, f"{label_name}: {conf*100:.0f}%", color))
    elif net is not None:
        blob = cv2.dnn.blobFromImage(frame, 0.007843, (300, 300), 127.5)
        net.setInput(blob)
        detections = net.forward()
        for i in range(detections.shape[2]):
            conf = float(detections[0, 0, i, 2])
            if conf < 0.5:
                continue
            idx = int(detections[0, 0, i, 1])
            box = detections[0, 0, i, 3:7] * [w, h, w, h]
            x1, y1, x2, y2 = box.astype(int)
            label_name = CLASSES[idx]
            if label_name == "person":
                person_count += 1
                pos = estimate_person_position(x1, y1, x2, y2, w)
                if pos is not None:
                    cam_positions.append(pos)
            color = (140, 220, 60) if label_name == "person" else (60, 180, 255)
            boxes.append((int(x1), int(y1), int(x2), int(y2), f"{label_name}: {conf*100:.0f}%", color))
    else:
        found, _ = hog.detectMultiScale(frame, winStride=(8, 8), padding=(4, 4), scale=1.05)
        person_count = len(found)
        for (x, y, bw, bh) in found:
            pos = estimate_person_position(x, y, x + bw, y + bh, w)
            if pos is not None:
                cam_positions.append(pos)
            boxes.append((int(x), int(y), int(x + bw), int(y + bh), "person", (140, 220, 60)))
    return boxes, person_count, cam_positions


def draw_boxes(frame, boxes):
    """Cheap: just rectangles + text onto the given frame. No inference."""
    for (x1, y1, x2, y2, label, color) in boxes:
        cv2.rectangle(frame, (x1, y1), (x2, y2), color, 2)
        cv2.putText(frame, label, (x1, max(15, y1 - 6)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1, cv2.LINE_AA)
    return frame


def detect_loop():
    """Runs YOLOv8n (or whichever detector loaded) in its own thread, at
    whatever pace the model actually manages on this Pi -- never tied to the
    camera's frame rate, and never able to stall the video stream since it
    only ever reads latest_frame and writes detect_state."""
    while True:
        with frame_lock:
            frame = latest_frame["frame"]
        if frame is None:
            time.sleep(0.05)
            continue
        try:
            boxes, person_count, cam_positions = run_detection(frame)
            with detect_lock:
                detect_state["boxes"] = boxes
                detect_state["person_count"] = person_count
                detect_state["cam_positions"] = cam_positions
        except Exception as e:
            print(f"[detect] error: {e}", flush=True)
            time.sleep(0.5)


def capture_loop():
    while True:
        try:
            frame = picam2.capture_array()  # RGB888
            frame = cv2.cvtColor(frame, cv2.COLOR_RGB2BGR)
            if CAMERA_ROTATE_180:
                frame = cv2.rotate(frame, cv2.ROTATE_180)

            with frame_lock:
                latest_frame["frame"] = frame

            with detect_lock:
                boxes = detect_state["boxes"]
                person_count = detect_state["person_count"]
                cam_positions = detect_state["cam_positions"]

            draw_frame = frame.copy()
            draw_boxes(draw_frame, boxes)
            cv2.putText(draw_frame, time.strftime("%H:%M:%S"), (8, FRAME_H - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.45, (200, 200, 200), 1, cv2.LINE_AA)
            ok, jpeg = cv2.imencode(".jpg", draw_frame, [cv2.IMWRITE_JPEG_QUALITY, 75])
            if ok:
                with cam_lock:
                    cam_state["jpeg"] = jpeg.tobytes()
                    cam_state["person_count"] = person_count
                    cam_state["cam_positions"] = cam_positions
                    cam_state["ts"] = time.time()
        except Exception as e:
            print(f"[capture] error: {e}", flush=True)
            time.sleep(0.5)


def mjpeg_generator():
    while True:
        with cam_lock:
            jpeg = cam_state["jpeg"]
        if jpeg is not None:
            yield (b"--frame\r\nContent-Type: image/jpeg\r\n\r\n" + jpeg + b"\r\n")
        time.sleep(0.03)


# ----------------------------------------------------------------- radar --
RADAR_PORT = "/dev/serial0"
RADAR_BAUD = 256000
HEADER = b"\xAA\xFF\x03\x00"
FOOTER = b"\x55\xCC"
FRAME_LEN = 30

# ---- Calibration toggles -------------------------------------------------
# If a quick sanity check (stand dead-center, note x; step to your LEFT
# facing the sensor, confirm which way x moves; walk toward/away, confirm y
# behaves) shows the axes don't match reality, flip these -- no rewiring
# needed, this is purely how we interpret the numbers the sensor sends.
SWAP_XY = False    # sensor mounted rotated 90 deg from "upright, facing out"
INVERT_X = True    # trying this: if the camera on the same rig is mounted
                    # rolled 180 deg (upside down), a rigidly-mounted radar
                    # right next to it would see its own left/right flip the
                    # same way, while forward distance (y) is unaffected by
                    # a roll. Do the physical check above to confirm -- flip
                    # back to False if it turns out to make things worse.

# ---- Range gate -----------------------------------------------------------
# Reject frames outside the sensor's own physical envelope, plus a small
# near-field dead zone where mmWave radar reflections off the sensor's own
# housing/mount are common noise sources rather than real targets.
MIN_VALID_Y = 60      # mm -- near-field junk below this
MAX_VALID_Y = 6000    # mm -- spec max range
MAX_VALID_X = 3000    # mm -- spec max lateral

# ---- Tracker tuning ---------------------------------------------------
EMA_ALPHA = 0.35        # smoothing: higher = snappier/more jitter, lower = smoother/more lag
CONFIRM_HITS = 3        # consecutive matches needed before a target is reported (kills 1-frame ghosts)
MAX_MISSES = 5          # frames a track can go unmatched before being dropped (avoids flicker)
GATE_DIST = 450         # mm -- how close a new reading must be to an existing track to "belong" to it

radar_lock = threading.Lock()
radar_state = {"targets": [], "ts": 0.0, "connected": False}


def decode_signed(raw):
    magnitude = raw & 0x7FFF
    return magnitude if (raw & 0x8000) else -magnitude


def parse_radar_frame(frame):
    """Decode the 3 fixed target slots, apply axis calibration, and drop
    anything outside the sensor's real envelope before it ever reaches the
    tracker."""
    detections = []
    for i in range(3):
        off = 4 + i * 8
        x_raw, y_raw, spd_raw, res = struct.unpack_from("<HHHH", frame, off)
        x, y, spd = decode_signed(x_raw), decode_signed(y_raw), decode_signed(spd_raw)
        if x == 0 and y == 0:
            continue
        if SWAP_XY:
            x, y = y, x
        if INVERT_X:
            x = -x
        if not (MIN_VALID_Y <= y <= MAX_VALID_Y):
            continue
        if abs(x) > MAX_VALID_X:
            continue
        detections.append({"x": x, "y": y, "spd": spd})
    return detections


class _Track:
    __slots__ = ("id", "x", "y", "spd", "hits", "misses", "confirmed")

    def __init__(self, tid, x, y, spd):
        self.id = tid
        self.x, self.y, self.spd = float(x), float(y), float(spd)
        self.hits = 1
        self.misses = 0
        self.confirmed = False


class Tracker:
    """Lightweight nearest-neighbor multi-target tracker with hit/miss
    debouncing and EMA smoothing. This is what actually fixes jitter and
    phantom blips: the LD2450 hands us raw, unfiltered per-frame detections
    with no guarantee a "slot" stays the same physical person between
    frames -- we do that association and filtering ourselves."""

    def __init__(self):
        self.tracks = []
        self._next_id = 1

    def update(self, detections):
        unmatched = list(range(len(detections)))
        for track in self.tracks:
            best_j, best_d = None, None
            for j in unmatched:
                d = ((track.x - detections[j]["x"]) ** 2 + (track.y - detections[j]["y"]) ** 2) ** 0.5
                if best_d is None or d < best_d:
                    best_d, best_j = d, j
            if best_j is not None and best_d <= GATE_DIST:
                det = detections[best_j]
                track.x = EMA_ALPHA * det["x"] + (1 - EMA_ALPHA) * track.x
                track.y = EMA_ALPHA * det["y"] + (1 - EMA_ALPHA) * track.y
                track.spd = EMA_ALPHA * det["spd"] + (1 - EMA_ALPHA) * track.spd
                track.hits += 1
                track.misses = 0
                if track.hits >= CONFIRM_HITS:
                    track.confirmed = True
                unmatched.remove(best_j)
            else:
                track.misses += 1

        for j in unmatched:
            det = detections[j]
            self.tracks.append(_Track(self._next_id, det["x"], det["y"], det["spd"]))
            self._next_id += 1

        self.tracks = [t for t in self.tracks if t.misses <= MAX_MISSES]

        return [
            {"id": t.id, "x": round(t.x), "y": round(t.y), "spd": round(t.spd)}
            for t in self.tracks if t.confirmed
        ]


def radar_loop():
    tracker = Tracker()
    while True:
        try:
            ser = serial.Serial(RADAR_PORT, RADAR_BAUD, timeout=1)
            print(f"[radar] listening on {RADAR_PORT} @ {RADAR_BAUD} baud")
            buf = bytearray()
            while True:
                buf += ser.read(64)
                while True:
                    idx = buf.find(HEADER)
                    if idx == -1 or len(buf) < idx + FRAME_LEN:
                        break
                    frame = bytes(buf[idx:idx + FRAME_LEN])
                    if frame[-2:] == FOOTER:
                        detections = parse_radar_frame(frame)
                        targets = tracker.update(detections)
                        with radar_lock:
                            radar_state["targets"] = targets
                            radar_state["ts"] = time.time()
                            radar_state["connected"] = True
                        del buf[:idx + FRAME_LEN]
                    else:
                        del buf[:idx + 1]
        except Exception as e:
            print(f"[radar] error: {e} -- retrying in 2s", flush=True)
            with radar_lock:
                radar_state["connected"] = False
            time.sleep(2)


# ------------------------------------------------------------------ app ---
app = Flask(__name__)
threading.Thread(target=capture_loop, daemon=True).start()
threading.Thread(target=detect_loop, daemon=True).start()
threading.Thread(target=radar_loop, daemon=True).start()


@app.route("/stream")
def stream():
    return Response(mjpeg_generator(), mimetype="multipart/x-mixed-replace; boundary=frame")


@app.route("/radar.json")
def radar_json():
    with radar_lock:
        targets = list(radar_state["targets"])
        radar_ts = radar_state["ts"]
        connected = radar_state["connected"]
    with cam_lock:
        person_count = cam_state["person_count"]
        cam_positions = list(cam_state["cam_positions"])
        cam_ts = cam_state["ts"]
    return jsonify({
        "targets": targets,
        "radar_ts": radar_ts,
        "radar_connected": connected,
        "person_count": person_count,
        "cam_positions": cam_positions,
        "cam_ts": cam_ts,
        "now": time.time(),
    })


DASHBOARD_HTML = """<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Wallhack Live Fusion</title>
<style>
  :root{
    --bg:#04070a; --panel:#0a1310; --panel2:#0d1815; --ink:#e7f5ee;
    --dim:#7fa38f; --dim2:#425a4e; --phosphor:#46e37f; --t2:#eaf6ef; --t3:#5c7c6c;
    --warn:#ffb454; --bad:#ff5f5f; --cam:#5fd0ff;
    --line: rgba(231,245,238,0.14); --line-strong: rgba(231,245,238,0.32);
  }
  *{ box-sizing:border-box; }
  body{
    margin:0; background:var(--bg); color:var(--ink);
    font-family: ui-monospace, "SFMono-Regular", Menlo, Consolas, monospace;
    padding: 18px clamp(14px,3vw,32px) 40px;
  }
  h1{ font-size:20px; margin:0 0 4px; letter-spacing:-0.01em; }
  .kicker{ font-size:11px; letter-spacing:0.18em; text-transform:uppercase; color:var(--dim); display:flex; align-items:center; gap:8px; margin-bottom:18px; }
  .dot{ width:7px; height:7px; border-radius:50%; background:var(--phosphor); box-shadow:0 0 8px var(--phosphor); flex:none; }
  .dot.bad{ background:var(--bad); box-shadow:0 0 8px var(--bad); }
  .grid{ display:grid; grid-template-columns:1.3fr 1fr; gap:18px; }
  @media (max-width:820px){ .grid{ grid-template-columns:1fr; } }
  .panel{ border:1px solid var(--line-strong); background:var(--panel); padding:12px; }
  .panel h2{ font-size:11px; letter-spacing:0.12em; text-transform:uppercase; color:var(--dim); margin:0 0 10px; }
  .cam-frame img{ width:100%; display:block; background:#000; }
  svg{ width:100%; height:auto; display:block; }
  .readout{ margin-top:16px; font-size:13px; }
  .readout .row{ display:flex; justify-content:space-between; padding:7px 0; border-bottom:1px solid var(--line); }
  .readout .row:last-child{ border-bottom:none; }
  .badge{ font-size:11px; padding:3px 8px; border:1px solid var(--line-strong); }
  .badge.ok{ color:var(--phosphor); border-color:var(--phosphor); }
  .badge.warn{ color:var(--warn); border-color:var(--warn); }
  .target-line{ font-size:12.5px; padding:5px 0; display:flex; gap:10px; }
  .swatch{ width:8px; height:8px; border-radius:50%; flex:none; margin-top:3px; }
  .legend{ display:flex; gap:16px; flex-wrap:wrap; font-size:11px; color:var(--dim); margin-top:10px; }
  .legend span{ display:inline-flex; align-items:center; gap:6px; }
  .legend .m-radar{ width:9px; height:9px; border-radius:50%; background:var(--phosphor); }
  .legend .m-cam{ width:9px; height:9px; border-radius:50%; border:1.5px dashed var(--cam); background:none; }
</style>
</head>
<body>
  <div class="kicker"><span class="dot" id="live-dot"></span><span id="conn-label">CONNECTING&hellip;</span></div>
  <h1>Wallhack &mdash; Live Fusion</h1>

  <div class="grid">
    <div class="panel cam-frame">
      <h2>Camera (live, with detection, corrected upright)</h2>
      <img src="/stream" alt="live camera feed">
    </div>
    <div class="panel">
      <h2>LD2450 radar + camera estimate (live)</h2>
      <svg id="scope" viewBox="0 0 300 310"></svg>
      <div class="legend">
        <span><i class="m-radar"></i> radar target (confirmed + smoothed)</span>
        <span><i class="m-cam"></i> camera position estimate (rough)</span>
      </div>
    </div>
  </div>

  <div class="panel readout">
    <div class="row"><span>Camera: people detected</span><span id="cam-count">&mdash;</span></div>
    <div class="row"><span>Radar: targets reported</span><span id="radar-count">&mdash;</span></div>
    <div class="row"><span>Fusion check</span><span id="fusion-badge" class="badge">&mdash;</span></div>
    <div id="target-list" style="margin-top:10px;"></div>
  </div>

<script>
(function(){
  var OX=150, OY=300, SC=130/3000;
  function toScreen(x,y){ return [OX + x*SC, OY - y*SC]; }
  var COLORS = ['#46e37f', '#eaf6ef', '#5c7c6c'];

  function ringMarkup(){
    var parts = [];
    parts.push('<defs><clipPath id="c1"><polygon points="150,300 280,225 280,40 20,40 20,225"/></clipPath></defs>');
    parts.push('<g clip-path="url(#c1)"><rect x="0" y="0" width="300" height="310" fill="#071009"/>');
    for (var r=43.3; r<270; r+=43.3){
      parts.push('<circle cx="150" cy="300" r="'+r.toFixed(1)+'" fill="none" stroke="#1d3327" stroke-width="1"/>');
    }
    parts.push('<line x1="150" y1="300" x2="150" y2="40" stroke="#1d3327" stroke-width="1" stroke-dasharray="2 6"/></g>');
    parts.push('<polygon points="150,300 280,225 280,40 20,40 20,225" fill="none" stroke="rgba(231,245,238,0.32)" stroke-width="1.2"/>');
    parts.push('<path d="M135,300 L150,275 L165,300 Z" fill="#7fa38f"/>');
    return parts.join('');
  }

  var scopeEl = document.getElementById('scope');
  function renderScope(targets, camPositions){
    var parts = [ringMarkup()];
    (camPositions || []).forEach(function(p){
      var s = toScreen(p.x, p.y);
      parts.push('<circle cx="'+s[0].toFixed(1)+'" cy="'+s[1].toFixed(1)+'" r="9" fill="none" stroke="var(--cam)" stroke-width="1.6" stroke-dasharray="3 3" opacity="0.85"/>');
    });
    targets.forEach(function(t){
      var s = toScreen(t.x, t.y);
      var color = COLORS[(t.id - 1) % COLORS.length];
      parts.push('<circle cx="'+s[0].toFixed(1)+'" cy="'+s[1].toFixed(1)+'" r="7" fill="'+color+'" stroke="#04070a" stroke-width="1.5"/>');
    });
    scopeEl.innerHTML = parts.join('');
  }

  function pad(v, n){ v = String(v); while (v.length < n) v = ' ' + v; return v; }

  var connLabel = document.getElementById('conn-label');
  var liveDot = document.getElementById('live-dot');
  var camCount = document.getElementById('cam-count');
  var radarCount = document.getElementById('radar-count');
  var fusionBadge = document.getElementById('fusion-badge');
  var targetList = document.getElementById('target-list');
  var failCount = 0;

  function poll(){
    fetch('/radar.json', {cache:'no-store'}).then(function(r){ return r.json(); }).then(function(data){
      failCount = 0;
      liveDot.className = 'dot' + (data.radar_connected ? '' : ' bad');
      connLabel.textContent = data.radar_connected ? 'LIVE' : 'RADAR NOT RESPONDING';

      renderScope(data.targets, data.cam_positions);
      camCount.textContent = data.person_count;
      radarCount.textContent = data.targets.length;

      if (data.targets.length === data.person_count){
        fusionBadge.textContent = 'MATCH';
        fusionBadge.className = 'badge ok';
      } else {
        fusionBadge.textContent = 'MISMATCH';
        fusionBadge.className = 'badge warn';
      }

      targetList.innerHTML = data.targets.length ? data.targets.map(function(t){
        var color = COLORS[(t.id - 1) % COLORS.length];
        var xs = (t.x >= 0 ? '+' : '') + t.x, ss = (t.spd >= 0 ? '+' : '') + t.spd;
        return '<div class="target-line"><span class="swatch" style="background:'+color+'"></span>' +
          '<span>T'+t.id+'</span><span>x:'+pad(xs,6)+'mm</span><span>y:'+pad(t.y,5)+'mm</span><span>spd:'+pad(ss,5)+'cm/s</span></div>';
      }).join('') : '<div style="color:var(--dim2);">(no targets)</div>';
    }).catch(function(){
      failCount++;
      if (failCount > 3){
        liveDot.className = 'dot bad';
        connLabel.textContent = 'DASHBOARD UNREACHABLE';
      }
    });
  }

  poll();
  setInterval(poll, 150);
})();
</script>
</body>
</html>"""


@app.route("/")
def index():
    return DASHBOARD_HTML


if __name__ == "__main__":
    print(f"Dashboard at http://<this-pi>:{STREAM_PORT}/")
    app.run(host="0.0.0.0", port=STREAM_PORT, threaded=True)
