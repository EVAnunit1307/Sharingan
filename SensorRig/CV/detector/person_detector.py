#!/usr/bin/env python3
"""
Production person detector: ONNX model + per-track temporal confirmation.

Two ideas do the work here:

1. A single-frame threshold has to sit high enough that no cabinet ever
   crosses it, which throws away real people. Furniture false positives are
   sporadic and jump around; a real person persists in roughly the same
   place across frames. So confirmation is per TRACK, not per frame --
   detections are associated across frames by IOU, and a track is only
   declared once it has been hit K times out of the last M frames.

2. Hysteresis: a high threshold to OPEN a track, a lower one to SUSTAIN it.
   Once we know someone is there, we can afford to believe weaker evidence,
   which is what keeps a person detected when they turn their back or step
   half out of frame.

Thresholds come from detector_config.json and are checked by replaying
development clips through this class. Strong observations confirm immediately;
lower scores require temporal evidence. See ../README.md for measured limits.
"""
import json
import os
import time

import numpy as np
import onnxruntime as ort

try:
    from .detector_core import MODELS, MODELS_DIR, decode, nms, preprocess
except ImportError:
    from detector_core import MODELS, MODELS_DIR, decode, nms, preprocess

ort.set_default_logger_severity(3)
HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(HERE, "detector_config.json")


def iou(a, b):
    ix1, iy1 = max(a[0], b[0]), max(a[1], b[1])
    ix2, iy2 = min(a[2], b[2]), min(a[3], b[3])
    iw, ih = max(0.0, ix2 - ix1), max(0.0, iy2 - iy1)
    inter = iw * ih
    ua = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / ua if ua > 0 else 0.0


class Track:
    __slots__ = ("id", "box", "score", "hist", "misses", "confirmed", "last_seen")

    def __init__(self, tid, box, score, hit, now):
        self.id = tid
        self.box = box
        self.score = score
        self.hist = [hit]
        self.misses = 0
        self.confirmed = False
        self.last_seen = now


class PersonDetector:
    def __init__(self, config_path=CONFIG_PATH, threads=2):
        with open(config_path) as f:
            cfg = json.load(f)
        self.cfg = cfg
        self.model = MODELS[cfg["model"]]
        self.t_on = cfg["t_on"]
        self.t_off = cfg["t_off"]
        self.k = cfg["k"]
        self.m = cfg["m"]
        self.max_misses = cfg.get("max_misses", 6)
        self.min_box_frac = cfg.get("min_box_frac", 0.0)
        self.hold_seconds = cfg.get("hold_seconds", 0.35)
        self.match_iou = cfg.get("match_iou", 0.2)
        self.instant_threshold = cfg.get("instant_threshold", 0.85)
        if not (0 <= self.t_off <= self.t_on <= self.instant_threshold <= 1):
            raise ValueError("Require 0 <= t_off <= t_on <= instant_threshold <= 1")
        if not (1 <= self.k <= self.m and self.hold_seconds > 0 and 0 < self.match_iou <= 1):
            raise ValueError("Invalid confirmation window, hold time, or match IoU")

        so = ort.SessionOptions()
        # Leave headroom: the camera capture/encode loop needs CPU too, and
        # starving it stalls the video stream even when detection keeps up.
        so.intra_op_num_threads = threads
        so.inter_op_num_threads = 1
        so.add_session_config_entry("session.intra_op.allow_spinning", "0")
        so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        self.sess = ort.InferenceSession(
            os.path.join(MODELS_DIR, self.model["file"]), so,
            providers=["CPUExecutionProvider"])
        self.iname = self.sess.get_inputs()[0].name

        self.tracks = []
        self._next_id = 0
        self.last_raw = []   # most recent pre-gate detections, for display/debug
        self._last_timestamp = None

    # -- raw, per-frame ----------------------------------------------------
    def detect_raw(self, frame):
        """All person boxes above the SUSTAIN threshold, before tracking."""
        x, r = preprocess(frame, self.model)
        out = self.sess.run(None, {self.iname: x})[0]
        sc, boxes = decode(out, self.model)
        keep = (sc >= self.t_off) & np.isfinite(sc) & np.isfinite(boxes).all(axis=1)
        sc, boxes = sc[keep], boxes[keep]
        if not len(sc):
            return []
        idx = nms(boxes, sc)
        h, w = frame.shape[:2]
        res = []
        for i in idx:
            b = boxes[i] / r
            b = [float(np.clip(b[0], 0, w)), float(np.clip(b[1], 0, h)),
                 float(np.clip(b[2], 0, w)), float(np.clip(b[3], 0, h))]
            if b[2] <= b[0] or b[3] <= b[1]:
                continue
            if (b[2] - b[0]) * (b[3] - b[1]) / (w * h) < self.min_box_frac:
                continue
            res.append({"box": b, "score": float(sc[i])})
        return res

    # -- tracked, temporally confirmed -------------------------------------
    def detect(self, frame, timestamp=None):
        """Consume one distinct frame, with its monotonic capture timestamp."""
        return self.update(self.detect_raw(frame), timestamp)

    def update(self, dets, timestamp=None):
        """Associate observations; explicit timestamps also support recorded clips.

        Low-confidence boxes can sustain an existing person without opening
        a new track. Coasting is bounded in seconds, independent of CPU speed.
        """
        now = time.monotonic() if timestamp is None else timestamp
        if not np.isfinite(now):
            raise ValueError("Timestamp must be finite")
        if self._last_timestamp is not None and now <= self._last_timestamp:
            raise ValueError("Each frame must have a strictly increasing timestamp")
        self._last_timestamp = now
        dets = [d for d in dets if np.isfinite(d["score"]) and d["score"] >= self.t_off
                and np.isfinite(d["box"]).all() and d["box"][2] > d["box"][0]
                and d["box"][3] > d["box"][1]]
        self.last_raw = dets
        self.tracks = [t for t in self.tracks if now - t.last_seen <= self.hold_seconds]
        used = set()
        matched = {}
        # Global strongest-overlap assignment avoids older tracks stealing a
        # better observation from a neighbour simply because they are first.
        pairs = sorted(((iou(t.box, d["box"]), ti, di)
                        for ti, t in enumerate(self.tracks)
                        for di, d in enumerate(dets)), reverse=True)
        for overlap, ti, di in pairs:
            if overlap >= self.match_iou and ti not in matched and di not in used:
                matched[ti] = di
                used.add(di)

        for ti, t in enumerate(self.tracks):
            if ti in matched:
                d = dets[matched[ti]]
                t.box, t.score, t.misses = d["box"], d["score"], 0
                t.last_seen = now
                # Confirmed tracks sustain on the lower threshold; unconfirmed
                # ones must clear the higher OPEN threshold to count as a hit.
                t.hist.append(d["score"] >= (self.t_off if t.confirmed else self.t_on))
            else:
                t.misses += 1
                t.hist.append(False)
            if len(t.hist) > self.m:
                t.hist.pop(0)
            if not t.confirmed and (sum(t.hist) >= self.k or
                                     (t.misses == 0 and t.score >= self.instant_threshold)):
                t.confirmed = True
            elif t.confirmed and sum(t.hist) == 0:
                t.confirmed = False

        for i, d in enumerate(dets):
            if i in used:
                continue
            if d["score"] >= self.t_on:
                track = Track(self._next_id, d["box"], d["score"], True, now)
                track.confirmed = self.k == 1 or d["score"] >= self.instant_threshold
                self.tracks.append(track)
                self._next_id += 1

        self.tracks = [t for t in self.tracks if t.misses <= self.max_misses]
        return [{"id": t.id, "box": t.box, "score": t.score,
                 "observed": t.misses == 0, "misses": t.misses,
                 "age_ms": round((now - t.last_seen) * 1000, 1)}
                for t in self.tracks if t.confirmed]

    def reset(self):
        self.tracks, self._next_id, self.last_raw = [], 0, []
        self._last_timestamp = None
