"""Deterministic camera/radar association in the configured sensor reference frame.

The sensor packet additions described in Docs/quest-ground-station-integration.md
are prerequisites. Source sample timestamps are compared only with other samples
from the same Pi session; local monotonic time is used exclusively for ageing.
No confidence or person identity is inferred from a radar observation alone.
"""
from collections import deque
from dataclasses import dataclass
import hashlib
import json
import math
import time
from .tracking import PeopleTracker
from SensorRig.CV.tracking_math import assignment


CAMERA_TTL_MS = 750.0
RADAR_TTL_MS = 500.0
REFERENCES = {"radar", "configured_drone_reference"}


def number(value):
    try:
        return type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        return False


def identity(value):
    return type(value) is int and 0 <= value <= 2147483647


def rotate(right, forward, yaw_deg):
    """Clockwise yaw in the Pi's right/forward convention."""
    angle = math.radians(yaw_deg)
    return (math.cos(angle) * right + math.sin(angle) * forward,
            -math.sin(angle) * right + math.cos(angle) * forward)


@dataclass(frozen=True)
class FusionConfig:
    camera_offset_right_m: float = 0.0
    camera_offset_forward_m: float = 0.0
    alignment_confirmed: bool = False
    max_skew_ms: float = 150.0
    max_bearing_error_deg: float = 8.0
    box_margin_deg: float = 3.0
    ambiguity_margin_deg: float = 3.0
    confirmation_frames: int = 2
    max_people: int = 8
    camera_offset_up_m: float = -.014
    camera_height_m: float | None = None
    camera_pitch_deg: float = 0.
    camera_fx_px: float = 0.
    camera_fy_px: float = 0.
    camera_cx_px: float = 0.
    camera_cy_px: float = 0.
    rig_motion_mode: str = 'stationary'

    def __post_init__(self):
        if self.rig_motion_mode not in ('stationary','untracked','left_controller'):
            raise ValueError('Rig mode must be stationary, untracked, or left_controller')
        if not number(self.camera_offset_up_m) or abs(self.camera_offset_up_m)>2:
            raise ValueError('Invalid camera vertical offset')
        if self.camera_height_m is not None and (not number(self.camera_height_m) or not .1<=self.camera_height_m<=4):
            raise ValueError('Camera lens height must be 0.1–4 metres or null')
        if not number(self.camera_pitch_deg) or abs(self.camera_pitch_deg)>45:
            raise ValueError('Invalid camera pitch')
        for key in ('camera_fx_px','camera_fy_px','camera_cx_px','camera_cy_px'):
            if not number(getattr(self,key)) or not 0<=getattr(self,key)<=16384:
                raise ValueError('Invalid camera intrinsics')
        for name in ("camera_offset_right_m", "camera_offset_forward_m"):
            if not number(getattr(self, name)) or abs(getattr(self, name)) > 2:
                raise ValueError(f"{name} must be finite and within two metres")
        if type(self.alignment_confirmed) is not bool:
            raise ValueError("alignment_confirmed must be boolean")
        for name, maximum in (("max_skew_ms", 500), ("max_bearing_error_deg", 30),
                              ("box_margin_deg", 15), ("ambiguity_margin_deg", 15)):
            value = getattr(self, name)
            if not number(value) or not 0 < value <= maximum:
                raise ValueError(f"Invalid {name}")
        if type(self.confirmation_frames) is not int or not 2 <= self.confirmation_frames <= 10:
            raise ValueError("confirmation_frames must be between two and ten")
        if type(self.max_people) is not int or not 1 <= self.max_people <= 8:
            raise ValueError("max_people must be between one and eight")


@dataclass
class Sample:
    frame_id: int
    generation: int
    capture_ms: float
    received_at: float
    source_age_ms: float
    values: list
    metadata: dict

    def age(self, now):
        return self.source_age_ms + max(0.0, now - self.received_at) * 1000.0

    def refresh_age(self, reported_age, now):
        # Duplicates may report a higher age, but can never make a sample younger.
        self.source_age_ms = max(self.age(now), reported_age)
        self.received_at = now


def _age(value):
    if not number(value) or value < 0:
        raise ValueError("Source observation age must be finite and nonnegative")
    return float(value)


def _sample_fields(frame_id, generation, capture_ms):
    if not identity(frame_id) or not identity(generation) or not number(capture_ms) or capture_ms < 0:
        raise ValueError("Invalid source frame identity or capture time")


def _mount(radar):
    reference = radar.get("reference_origin")
    config = radar.get("config")
    if not isinstance(reference, str) or reference not in REFERENCES or not isinstance(config, dict):
        raise ValueError("An explicit sensor reference and mount configuration are required")
    values = {"reference_origin": reference}
    for key in ("invert_x", "mount_level", "mounting_confirmed"):
        if type(config.get(key)) is not bool:
            raise ValueError(f"Missing mount field: {key}")
        values[key] = config[key]
    for key in ("yaw_deg", "offset_right_m", "offset_forward_m"):
        if not number(config.get(key)):
            raise ValueError(f"Invalid mount field: {key}")
        values[key] = float(config[key])
    return values


def _people(packet):
    width, height = packet.get("camera_frame_width"), packet.get("camera_frame_height")
    hfov = packet.get("camera_hfov_deg")
    if not identity(width) or not identity(height) or not 1 <= width <= 16384 or not 1 <= height <= 16384:
        raise ValueError("Invalid camera dimensions")
    if not number(hfov) or not 1 < hfov < 179:
        raise ValueError("Invalid camera horizontal field of view")
    values = packet.get("camera_people")
    if not isinstance(values, list) or len(values) > 128:
        raise ValueError("Camera observations must be a bounded array")
    output, seen = [], set()
    focal = width / (2 * math.tan(math.radians(hfov / 2)))
    for value in values:
        if not isinstance(value, dict) or value.get("observed") is not True:
            continue
        camera_id, score, box = value.get("id"), value.get("score"), value.get("box")
        if not identity(camera_id) or not number(score) or not 0 <= score <= 1:
            continue
        if camera_id in seen:
            raise ValueError("Duplicate camera track identity")
        seen.add(camera_id)
        if not isinstance(box, list) or len(box) != 4 or not all(number(v) for v in box):
            continue
        left, top, right, bottom = box
        if not (0 <= left < right <= width and 0 <= top < bottom <= height):
            continue
        angle = lambda pixel: math.degrees(math.atan((pixel - width / 2) / focal))
        fallback = None
        x, y = value.get("x_m"), value.get("y_m")
        # Never accept a monocular range for a clipped box, even from a faulty sender.
        if (value.get("range_source") == "monocular_estimate" and number(x) and number(y)
                and y > 0 and left > 3 and top > 3 and right < width - 3 and bottom < height - 3):
            fallback = (float(x), float(y))
        output.append(dict(id=camera_id, confidence=float(score), fallback=fallback,
                           bearing=angle((left + right) / 2),
                           left_bearing=angle(left), right_bearing=angle(right)))
    return output, dict(width=width, height=height, hfov=float(hfov))


def _radar_targets(radar, use_measurements=False):
    values = radar.get("targets")
    if not isinstance(values, list) or len(values) > 32:
        raise ValueError("Radar observations must be a bounded array")
    output, seen = [], set()
    for value in values:
        if not isinstance(value, dict) or value.get("observed") is not True:
            continue
        target_id, right, forward = value.get("id"), value.get("right_m"), value.get("forward_m")
        if use_measurements and number(value.get('raw_right_m')) and number(value.get('raw_forward_m')):
            # These are accepted, confirmed returns before sensor-frame smoothing.
            # Filtering happens after the tracked rig transform on Quest.
            right,forward=value['raw_right_m'],value['raw_forward_m']
        if not identity(target_id) or not number(right) or not number(forward):
            continue
        if target_id in seen:
            raise ValueError("Duplicate radar track identity")
        seen.add(target_id)
        output.append(dict(id=target_id, right_m=float(right), forward_m=float(forward)))
    return output


class FusionEngine:
    """Single-owner state machine. Call ingest on receipt, snapshot on relay ticks.

    Invalid packets raise ValueError without renewing observation freshness.
    disconnect() clears all observations immediately. Returned dictionaries have
    no references to mutable internal state and are JSON serializable.
    """

    def __init__(self, config=None, clock=time.monotonic):
        self.config = config or FusionConfig()
        self.clock = clock
        self.session = None
        self.mount = None
        self.reference_id = None
        self.camera = None
        self.radar = deque(maxlen=128)
        self.matches = {}
        self.confirmations = {}
        self.camera_high_water = -1
        self.radar_high_water = -1
        self.camera_generation = None
        self.radar_generation = None
        self.camera_geometry = None
        self.status = "waiting_for_sensor"
        self.tracker = PeopleTracker()

    def disconnect(self):
        self.camera = None
        self.radar.clear()
        self.confirmations.clear()
        self.matches.clear()
        self.camera_high_water = self.radar_high_water = -1
        self.status = "sensor_disconnected"
        self.tracker.reset()

    def set_config(self, config):
        if not isinstance(config, FusionConfig):
            raise ValueError("Expected FusionConfig")
        if config != self.config:
            self.config = config
            self.disconnect()
            self.reference_id = self._reference_id() if self.mount else None

    def _reference_id(self):
        basis = dict(mount=self.mount, camera_offset_right_m=self.config.camera_offset_right_m,
                     camera_offset_forward_m=self.config.camera_offset_forward_m,
                     camera_offset_up_m=self.config.camera_offset_up_m,
                     camera_height_m=self.config.camera_height_m,camera_pitch_deg=self.config.camera_pitch_deg,
                     rig_motion_mode=self.config.rig_motion_mode)
        return hashlib.sha256(json.dumps(basis, sort_keys=True).encode()).hexdigest()[:24]

    def ingest(self, packet, now=None):
        now = self.clock() if now is None else now
        if (not number(now) or not isinstance(packet, dict)
                or type(packet.get("schema_version")) is not int or packet["schema_version"] != 1):
            raise ValueError("Expected version-one sensor packet")
        session = packet.get("source_session_id")
        radar = packet.get("radar")
        if not isinstance(session, str) or not 1 <= len(session) <= 128 or not isinstance(radar, dict):
            raise ValueError("Missing sensor session or radar metadata")
        mount = _mount(radar)
        cg, rg = packet.get("camera_generation"), radar.get("generation")
        if not identity(cg) or not identity(rg):
            raise ValueError("Missing sensor reconnect generations")

        # Parse fully before mutating state, including independent sensor failures.
        camera = None
        if packet.get("camera_connected") is True:
            cf, ct = packet.get("camera_frame_id"), packet.get("camera_capture_mono_ms")
            _sample_fields(cf, cg, ct)
            people, geometry = _people(packet)
            camera = Sample(cf, cg, ct, now, _age(packet.get("camera_age_ms")), people, geometry)
        rs = None
        if radar.get("status") == "live":
            rf, rt = radar.get("frame_id"), radar.get("capture_mono_ms")
            _sample_fields(rf, rg, rt)
            if radar.get("position_units") != "m":
                raise ValueError("Radar position_units must be metres")
            targets = _radar_targets(radar,self.config.rig_motion_mode=='left_controller')
            rs = Sample(rf, rg, rt, now, _age(radar.get("age_ms")), targets, {})

        same_reference = session == self.session and mount == self.mount
        if same_reference:
            for generation, previous in ((cg, self.camera_generation), (rg, self.radar_generation)):
                if previous is not None and generation < previous:
                    raise ValueError("Sensor generation moved backwards")
            if (rs and rg == self.radar_generation and self.radar
                    and rs.frame_id > self.radar_high_water and rs.capture_ms <= self.radar[-1].capture_ms):
                raise ValueError("Radar capture time must advance within a generation")
            if (camera and cg == self.camera_generation and self.camera
                    and camera.frame_id > self.camera_high_water and camera.capture_ms <= self.camera.capture_ms):
                raise ValueError("Camera capture time must advance within a generation")

        if session != self.session or mount != self.mount:
            self.disconnect()
            self.session, self.mount = session, mount
            self.camera_generation = self.radar_generation = None
            self.camera_geometry = None
            self.reference_id = self._reference_id()
        if cg != self.camera_generation:
            if self.camera_generation is not None:
                self.tracker.reset()
            self.camera = None
            self.camera_high_water = -1
            self.confirmations.clear()
            self.matches.clear()
            self.camera_generation = cg
        if rg != self.radar_generation:
            if self.radar_generation is not None:
                self.tracker.reset()
            self.radar.clear()
            self.radar_high_water = -1
            self.confirmations.clear()
            self.matches.clear()
            self.radar_generation = rg

        if rs is None:
            self.radar.clear()
            self.confirmations.clear()
            self.matches.clear()
        elif rs.frame_id > self.radar_high_water:
            self.radar.append(rs)
            self.radar_high_water = rs.frame_id
        elif self.radar and rs.frame_id == self.radar[-1].frame_id:
            self.radar[-1].refresh_age(rs.source_age_ms, now)
        self._prune(now)

        if camera is None:
            self.camera = None
            self.matches.clear()
            self.confirmations.clear()
            self.status = "camera_unavailable"
        elif camera.frame_id > self.camera_high_water:
            if camera.metadata != self.camera_geometry:
                self.confirmations.clear()
                self.matches.clear()
                self.camera_geometry = camera.metadata
            self.camera = camera
            self.camera_high_water = camera.frame_id
            self._associate(now)
            self.status = "live"
        elif self.camera and camera.frame_id == self.camera.frame_id:
            self.camera.refresh_age(camera.source_age_ms, now)
        return self.snapshot(now)

    def _prune(self, now):
        self.radar = deque((sample for sample in self.radar if sample.age(now) <= 1000), maxlen=128)

    def _associate(self, now):
        self.matches = {}
        can_match = (self.config.alignment_confirmed and self.mount["mount_level"]
                     and self.mount["mounting_confirmed"] and self.camera.age(now) <= CAMERA_TTL_MS)
        candidates = [r for r in self.radar if r.age(now) <= RADAR_TTL_MS
                      and abs(r.capture_ms - self.camera.capture_ms) <= self.config.max_skew_ms]
        if not can_match or not candidates:
            self.confirmations.clear()
            return
        radar = min(candidates, key=lambda r: (abs(r.capture_ms - self.camera.capture_ms), -r.frame_id))
        edges = []
        for person in self.camera.values:
            for target in radar.values:
                x = target["right_m"] - self.config.camera_offset_right_m
                y = target["forward_m"] - self.config.camera_offset_forward_m
                # Radar has already been mounted into the reference frame. The
                # parallel camera shares its yaw, but not its raw radar X mirror.
                x, y = rotate(x, y, -self.mount["yaw_deg"])
                if y <= 0:
                    continue
                bearing = math.degrees(math.atan2(x, y))
                error = abs(bearing - person["bearing"])
                if (error <= self.config.max_bearing_error_deg
                        and person["left_bearing"] - self.config.box_margin_deg <= bearing
                        <= person["right_bearing"] + self.config.box_margin_deg):
                    edges.append((error, person["id"], target["id"]))
        edges.sort()
        # Bounded global assignment, while refusing locally ambiguous pairs.
        # A maximum of eight camera candidates and three LD2450 returns keeps
        # the deterministic bitmask solver bounded on the relay thread.
        camera_ids=[p['id'] for p in self.camera.values[:8]]
        radar_ids=[r['id'] for r in radar.values[:3]]
        costs=[[next((e[0] for e in edges if e[1]==c and e[2]==r),math.inf)
                for r in radar_ids] for c in camera_ids]
        assigned={camera_ids[i]:radar_ids[j] for i,j in assignment(costs,30.).items()}
        confirmations = {}
        for person in self.camera.values:
            ranked = [e for e in edges if e[1] == person["id"]]
            if not ranked:
                continue
            best = ranked[0]
            if assigned.get(person['id']) != best[2]:
                continue
            reverse = [e for e in edges if e[2] == best[2]]
            if reverse[0] != best:
                continue
            if any(len(items) > 1 and items[1][0] - items[0][0] < self.config.ambiguity_margin_deg
                   for items in (ranked, reverse)):
                continue
            key = (radar.generation, best[2])
            old_key, count = self.confirmations.get(person["id"], (None, 0))
            count = min(count + 1, self.config.confirmation_frames) if old_key == key else 1
            confirmations[person["id"]] = (key, count)
            if count >= self.config.confirmation_frames:
                target = next(t for t in radar.values if t["id"] == best[2])
                self.matches[person["id"]] = (radar, target)
        self.confirmations = confirmations

    def snapshot(self, now=None):
        now = self.clock() if now is None else now
        if not number(now):
            raise ValueError("Invalid local time")
        self._prune(now)
        camera = self.camera
        fresh = camera is not None and camera.age(now) <= CAMERA_TTL_MS
        status = self.status if fresh or camera is None else "camera_stale"
        people = []
        unmatched = unpositioned = 0
        associated = set()
        for observation in sorted(camera.values, key=lambda p: (-p["confidence"], p["id"])) if fresh else []:
            fallback = observation["fallback"]
            pose=self.tracker.poses.get(('C',camera.generation,observation['id']))
            if pose and pose.get('floor_position') and pose['age_ms']+(now-pose['received_at'])*1000<=350:
                floor=pose['floor_position']
                fallback=(floor['right_m'],floor['forward_m'])
            fallback = rotate(*fallback, self.mount["yaw_deg"]) if fallback else None
            fallback = dict(right_m=fallback[0] + self.config.camera_offset_right_m,
                            forward_m=fallback[1] + self.config.camera_offset_forward_m) if fallback else None
            match = self.matches.get(observation["id"])
            if match and match[0].age(now) > RADAR_TTL_MS:
                match = None
            if not match:
                unmatched += 1
            if not match and fallback is None:
                unpositioned += 1
                continue
            if len(people) >= self.config.max_people:
                continue
            sample, target = match if match else (None, None)
            if target:
                associated.add((sample.generation, target["id"]))
            position = target or fallback
            people.append(dict(camera_id=observation["id"], camera_generation=camera.generation,
                camera_frame_id=camera.frame_id, confidence=observation["confidence"],
                right_m=position["right_m"], forward_m=position["forward_m"],
                position_source="radar_matched" if match else "camera_estimate",
                radar_id=target["id"] if match else None,
                radar_generation=sample.generation if match else None,
                radar_frame_id=sample.frame_id if match else None,
                camera_age_ms=round(camera.age(now), 3),
                radar_age_ms=round(sample.age(now), 3) if match else None,
                fallback_position=fallback))
        latest_radar = (self.radar[-1] if self.radar and self.radar[-1].age(now) <= RADAR_TTL_MS
                        and self.mount["mount_level"] and self.mount["mounting_confirmed"] else None)
        dots = [dict(t, generation=latest_radar.generation, frame_id=latest_radar.frame_id,
                     age_ms=round(latest_radar.age(now), 3), classification="unverified_radar_target")
                for t in latest_radar.values
                if (latest_radar.generation, t["id"]) not in associated] if latest_radar else []
        tracks=self.tracker.snapshot(people,dots,camera if fresh else None,now,
            (self.session,self.reference_id,self.camera_generation,self.radar_generation),self.config,
            self.mount['yaw_deg'] if self.mount else 0.)
        return dict(version=1, tracks_version=1, tracks=tracks,track_decisions=[dict(d) for d in self.tracker.decisions],
                    rig_pose_valid=self.config.rig_motion_mode=='stationary',rig_motion_mode=self.config.rig_motion_mode,
                    source_session_id=self.session, reference_id=self.reference_id,
                    camera_frame_id=camera.frame_id if camera else self.camera_high_water,
                    camera_generation=self.camera_generation,
                    camera_age_ms=round(camera.age(now), 3) if camera else None,
                    reference_origin=self.mount["reference_origin"] if self.mount else None,
                    coordinate_frame="sensor_reference_2d", units="m", status=status,
                    alignment_confirmed=self.config.alignment_confirmed,
                    people=people, radar_targets=dots,
                    observed_people=len(camera.values) if fresh else 0,
                    unmatched_people=unmatched, unpositioned_people=unpositioned)
