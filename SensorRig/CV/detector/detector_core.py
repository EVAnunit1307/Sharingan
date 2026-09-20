#!/usr/bin/env python3
"""
Shared person-detection core: model registry, preprocessing, output decode.

Both the offline bake-off and the live detector import from here, so a
protocol fix can't land in one and silently miss the other.

Every preprocessing mode here was verified empirically against a
known-person frame, not assumed from documentation. The one that bites:
YOLOX's official ONNX exports take RAW 0-255 BGR input. Feeding them
/255-normalised input yields objectness identically 0 and therefore zero
detections -- which reads as "this model is useless" rather than "the
harness is wrong".
"""
import os

import cv2
import numpy as np

MODELS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "models")

# norm: "div255"  -> RGB, /255         (YOLOv5/v6/v8/v11/v10)
#       "raw"     -> BGR, 0-255        (YOLOX official exports)
#       "meanstd" -> BGR, (x-mean)/std (NanoDet-Plus)
MODELS = {
    "yolov8n_crowdhuman": dict(file="yolov8n_crowdhuman.onnx",     size=640, fam="v8_single", norm="div255"),
    "yolov8n_person":     dict(file="yolov8n_person_deepghs.onnx", size=640, fam="v8_single", norm="div255"),
    "yolov8n_coco":       dict(file="yolov8n_deepghs.onnx",        size=640, fam="v8_multi",  norm="div255"),
    "yolo11n_mikelud":    dict(file="yolo11n_mikelud.onnx",        size=640, fam="v8_multi",  norm="div255"),
    "yolo11n_deepghs":    dict(file="yolo11n_deepghs.onnx",        size=640, fam="v8_multi",  norm="div255"),
    "yolov10n":           dict(file="yolov10n_onnxcommunity.onnx", size=640, fam="v10",       norm="div255"),
    "yolov5n":            dict(file="yolov5n.onnx",                size=640, fam="v5",   norm="div255", fp16=True),
    "yolov5s":            dict(file="yolov5s.onnx",                size=640, fam="v5",   norm="div255", fp16=True),
    "yolov6n":            dict(file="yolov6n.onnx",                size=640, fam="v6",   norm="div255"),
    "yolox_nano":         dict(file="yolox_nano.onnx",             size=416, fam="yolox", norm="raw"),
    "yolox_tiny":         dict(file="yolox_tiny.onnx",             size=416, fam="yolox", norm="raw"),
    "yolox_s":            dict(file="yolox_s_opencvzoo.onnx",      size=640, fam="yolox", norm="raw"),
    "nanodet_plus_m":     dict(file="nanodet-plus-m_416.onnx",     size=416, fam="nanodet", norm="meanstd"),
}

NANODET_MEAN = np.array([103.53, 116.28, 123.675], np.float32)
NANODET_STD = np.array([57.375, 57.12, 58.395], np.float32)

_GRID_CACHE = {}


def letterbox(im, sz, pad=114):
    """Aspect-preserving resize into sz x sz, padded bottom/right.

    Padding only bottom/right (not centred) keeps the mapping back to
    original coordinates a single divide by `r`, with no offset term to
    get wrong.
    """
    h, w = im.shape[:2]
    r = min(sz / h, sz / w)
    nh, nw = int(round(h * r)), int(round(w * r))
    out = np.full((sz, sz, 3), pad, np.uint8)
    out[:nh, :nw] = cv2.resize(im, (nw, nh), interpolation=cv2.INTER_LINEAR)
    return out, r


def preprocess(img, m):
    lb, r = letterbox(img, m["size"])
    if m["norm"] == "div255":
        x = lb[:, :, ::-1].astype(np.float32) / 255.0
    elif m["norm"] == "raw":
        x = lb.astype(np.float32)
    else:
        x = (lb.astype(np.float32) - NANODET_MEAN) / NANODET_STD
    x = x.transpose(2, 0, 1)[None]
    return (x.astype(np.float16) if m.get("fp16") else x.astype(np.float32)), r


def grids_for(sz, strides):
    key = (sz, tuple(strides))
    if key not in _GRID_CACHE:
        g, s = [], []
        for st in strides:
            n = -(-sz // st)   # ceil: NanoDet's stride-64 level on 416 is 7x7, not 6x6
            yv, xv = np.meshgrid(np.arange(n), np.arange(n), indexing="ij")
            grid = np.stack((xv, yv), 2).reshape(-1, 2)
            g.append(grid)
            s.append(np.full((grid.shape[0], 1), st, np.float32))
        _GRID_CACHE[key] = (np.concatenate(g).astype(np.float32), np.concatenate(s))
    return _GRID_CACHE[key]


def decode(out, m):
    """-> (scores, boxes_xyxy) in letterboxed pixels, PERSON class only."""
    a = np.asarray(out, np.float32)
    sq = a[0] if a.ndim == 3 else a
    fam = m["fam"]

    if fam in ("v8_single", "v8_multi"):
        att = sq.T if sq.shape[0] < sq.shape[1] else sq
        sc = att[:, 4]                       # class 0 is person in both layouts
        cxcy, wh = att[:, :2], att[:, 2:4]
        return sc, np.concatenate([cxcy - wh / 2, cxcy + wh / 2], 1)

    if fam == "v5":
        sc = sq[:, 4] * sq[:, 5]             # objectness * person
        cxcy, wh = sq[:, :2], sq[:, 2:4]
        return sc, np.concatenate([cxcy - wh / 2, cxcy + wh / 2], 1)

    if fam == "v6":
        sc = sq[:, 5]                        # objectness is identically 1
        cxcy, wh = sq[:, :2], sq[:, 2:4]
        return sc, np.concatenate([cxcy - wh / 2, cxcy + wh / 2], 1)

    if fam == "v10":                         # NMS-free, already xyxy
        keep = sq[:, 5] == 0
        return sq[keep, 4], sq[keep, :4]

    if fam == "yolox":
        grid, st = grids_for(m["size"], [8, 16, 32])
        xy = (sq[:, :2] + grid) * st
        wh = np.exp(sq[:, 2:4]) * st
        return sq[:, 4] * sq[:, 5], np.concatenate([xy - wh / 2, xy + wh / 2], 1)

    if fam == "nanodet":
        sc = sq[:, 0]
        reg = sq[:, 80:].reshape(-1, 4, 8)
        e = np.exp(reg - reg.max(-1, keepdims=True))
        p = e / e.sum(-1, keepdims=True)
        dist = (p * np.arange(8, dtype=np.float32)).sum(-1)
        grid, st = grids_for(m["size"], [8, 16, 32, 64])
        cx, cy = (grid[:, 0:1] + 0.5) * st, (grid[:, 1:2] + 0.5) * st
        d = dist * st
        return sc, np.concatenate([cx - d[:, 0:1], cy - d[:, 1:2],
                                   cx + d[:, 2:3], cy + d[:, 3:4]], 1)

    raise ValueError(fam)


def nms(boxes, scores, thr=0.45, top_k=20):
    if len(boxes) == 0:
        return []
    idx = cv2.dnn.NMSBoxes(
        [[float(b[0]), float(b[1]), float(b[2] - b[0]), float(b[3] - b[1])] for b in boxes],
        [float(s) for s in scores], 0.0, thr)
    return list(np.array(idx).flatten()[:top_k]) if len(idx) else []
