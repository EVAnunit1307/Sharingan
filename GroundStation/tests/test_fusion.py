import json
import math
import unittest

from GroundStation.fusion import FusionConfig, FusionEngine


def person(track_id=1, bearing=0, confidence=.9, forward=2, clipped=False):
    focal = 640 / (2 * math.tan(math.radians(31)))
    center = 320 + focal * math.tan(math.radians(bearing))
    return dict(id=track_id, score=confidence, observed=True,
                box=[center - 18, 20, center + 18, 480 if clipped else 450],
                x_m=None if clipped else forward * math.tan(math.radians(bearing)),
                y_m=None if clipped else forward,
                range_source="unavailable" if clipped else "monocular_estimate")


def target(track_id=7, bearing=0, forward=3):
    return dict(id=track_id, observed=True, right_m=forward * math.tan(math.radians(bearing)),
                forward_m=forward)


def packet(frame=1, people=None, targets=None):
    return dict(schema_version=1, source_session_id="pi-session", camera_generation=1,
                camera_frame_id=frame, camera_capture_mono_ms=1000 + frame * 100,
                camera_age_ms=0, camera_frame_width=640, camera_frame_height=480,
                camera_hfov_deg=62, camera_connected=True,
                camera_people=[person()] if people is None else people,
                radar=dict(status="live", generation=1, frame_id=frame,
                           capture_mono_ms=1000 + frame * 100, age_ms=0, position_units="m",
                           reference_origin="radar", coordinate_frame="drone_body_2d",
                           config=dict(invert_x=True, mount_level=True, mounting_confirmed=True,
                                       yaw_deg=0, offset_right_m=0, offset_forward_m=0),
                           targets=[target()] if targets is None else targets))


class FusionTests(unittest.TestCase):
    def setUp(self):
        self.engine = FusionEngine(FusionConfig(alignment_confirmed=True))

    def confirm(self, first=None, second=None):
        self.engine.ingest(first or packet(), now=0)
        return self.engine.ingest(second or packet(2), now=.1)

    def test_two_distinct_camera_frames_confirm_and_keep_camera_identity(self):
        first = self.engine.ingest(packet(), now=0)["people"][0]
        second = self.engine.ingest(packet(2), now=.1)["people"][0]
        self.assertEqual(first["position_source"], "camera_estimate")
        self.assertEqual(first["forward_m"], 2)
        self.assertEqual(second["position_source"], "radar_matched")
        self.assertEqual(second["forward_m"], 3)
        self.assertEqual(first["camera_id"], second["camera_id"])
        self.assertEqual(second["radar_id"], 7)

    def test_duplicate_frame_cannot_confirm_or_renew_freshness(self):
        p = packet()
        self.engine.ingest(p, now=0)
        duplicate = self.engine.ingest(p, now=.6)
        self.assertEqual(duplicate["people"][0]["position_source"], "camera_estimate")
        self.assertAlmostEqual(duplicate["people"][0]["camera_age_ms"], 600)
        self.assertEqual(self.engine.ingest(p, now=.751)["people"], [])

    def test_empty_frame_removes_people_and_older_frame_cannot_restore_them(self):
        self.confirm()
        self.assertEqual(self.engine.ingest(packet(3, people=[]), now=.2)["people"], [])
        self.assertEqual(self.engine.ingest(packet(2), now=.3)["people"], [])

    def test_clipped_people_need_radar_not_monocular_depth(self):
        a, b = packet(people=[person(clipped=True)]), packet(2, people=[person(clipped=True)])
        first = self.engine.ingest(a, now=0)
        self.assertEqual(first["people"], [])
        self.assertEqual(first["unpositioned_people"], 1)
        self.assertEqual(self.engine.ingest(b, now=.1)["people"][0]["position_source"], "radar_matched")
        self.assertEqual(self.engine.snapshot(now=.601)["people"], [])

    def test_sender_cannot_invent_clipped_monocular_range(self):
        p = person(clipped=True)
        p.update(x_m=0, y_m=2, range_source="monocular_estimate")
        result = self.engine.ingest(packet(people=[p], targets=[]), now=0)
        self.assertEqual(result["people"], [])

    def test_radar_expiry_falls_back_without_changing_identity(self):
        matched = self.confirm()["people"][0]
        fallback = self.engine.snapshot(now=.601)["people"][0]
        self.assertEqual(fallback["position_source"], "camera_estimate")
        self.assertEqual(fallback["camera_id"], matched["camera_id"])
        self.assertEqual(fallback["forward_m"], 2)
        self.assertEqual(self.engine.snapshot(now=.851)["people"], [])

    def test_source_ages_and_relay_residence_both_count(self):
        p = packet()
        p["camera_age_ms"] = 700
        self.engine.ingest(p, now=10)
        self.assertEqual(self.engine.snapshot(now=10.051)["people"], [])

    def test_camera_disconnect_does_not_create_radar_people(self):
        p = packet()
        p["camera_connected"] = False
        result = self.engine.ingest(p, now=0)
        self.assertEqual(result["people"], [])
        self.assertEqual(len(result["radar_targets"]), 1)
        self.assertEqual(result["radar_targets"][0]["classification"], "unverified_radar_target")

    def test_matched_radar_is_not_also_a_dot(self):
        self.assertEqual(self.confirm()["radar_targets"], [])

    def test_radar_failure_immediately_uses_camera_fallback(self):
        self.confirm()
        p = packet(2)
        p["radar"]["status"] = "disconnected"
        result = self.engine.ingest(p, now=.2)
        self.assertEqual(result["people"][0]["position_source"], "camera_estimate")
        self.assertEqual(result["radar_targets"], [])

    def test_radar_reconnect_and_reused_id_need_new_confirmation(self):
        self.confirm()
        p = packet(3)
        p["radar"].update(generation=2, frame_id=1)
        self.assertEqual(self.engine.ingest(p, now=.2)["people"][0]["position_source"], "camera_estimate")
        old = packet(4)
        with self.assertRaises(ValueError):
            self.engine.ingest(old, now=.3)

    def test_camera_reconnect_resets_identity_and_confirmation(self):
        self.confirm()
        p = packet(3)
        p.update(camera_generation=2, camera_frame_id=1)
        person_out = self.engine.ingest(p, now=.2)["people"][0]
        self.assertEqual(person_out["camera_generation"], 2)
        self.assertEqual(person_out["position_source"], "camera_estimate")

    def test_source_session_change_clears_previous_associations(self):
        self.confirm()
        p = packet()
        p["source_session_id"] = "new-pi-session"
        self.assertEqual(self.engine.ingest(p, now=.2)["people"][0]["position_source"], "camera_estimate")

    def test_mount_change_changes_registration_and_clears_matches(self):
        old = self.confirm()
        p = packet(3)
        p["radar"]["config"]["yaw_deg"] = 5
        new = self.engine.ingest(p, now=.2)
        self.assertNotEqual(old["reference_id"], new["reference_id"])
        self.assertEqual(new["people"][0]["position_source"], "camera_estimate")

    def test_disconnected_upstream_hides_all_contacts_immediately(self):
        self.confirm()
        self.engine.disconnect()
        result = self.engine.snapshot(now=.2)
        self.assertEqual(result["people"], [])
        self.assertEqual(result["radar_targets"], [])

    def test_mount_not_confirmed_prevents_radar_positions(self):
        a, b = packet(), packet(2)
        a["radar"]["config"]["mounting_confirmed"] = False
        b["radar"]["config"]["mounting_confirmed"] = False
        result = self.confirm(a, b)
        self.assertEqual(result["people"][0]["position_source"], "camera_estimate")
        self.assertEqual(result["radar_targets"], [])

    def test_alignment_confirmation_is_required(self):
        self.engine = FusionEngine()
        self.assertEqual(self.confirm()["people"][0]["position_source"], "camera_estimate")

    def test_camera_offset_affects_matching_and_fallback_once(self):
        self.engine = FusionEngine(FusionConfig(camera_offset_right_m=.5,
                                               camera_offset_forward_m=.2, alignment_confirmed=True))
        a, b = packet(), packet(2)
        for p in (a, b):
            p["radar"]["targets"][0].update(right_m=.5, forward_m=3.2)
        result = self.confirm(a, b)["people"][0]
        self.assertEqual(result["position_source"], "radar_matched")
        self.assertEqual(result["right_m"], .5)
        self.assertEqual(result["forward_m"], 3.2)
        self.assertEqual(result["fallback_position"], dict(right_m=.5, forward_m=2.2))

    def test_existing_radar_mirror_is_not_applied_again(self):
        a = packet(people=[person(bearing=-12)], targets=[target(bearing=-12)])
        b = packet(2, people=[person(bearing=-12)], targets=[target(bearing=-12)])
        self.assertLess(self.confirm(a, b)["people"][0]["right_m"], 0)

    def test_parallel_camera_uses_mount_yaw_without_rotating_radar_twice(self):
        self.engine = FusionEngine(FusionConfig(camera_offset_right_m=.2,
                                               camera_offset_forward_m=.3, alignment_confirmed=True))
        a, b = packet(), packet(2)
        for p in (a, b):
            p["radar"]["config"]["yaw_deg"] = 90
            p["radar"]["targets"][0].update(right_m=3.2, forward_m=.3)
        result = self.confirm(a, b)["people"][0]
        self.assertEqual(result["position_source"], "radar_matched")
        self.assertEqual(result["right_m"], 3.2)
        self.assertEqual(result["forward_m"], .3)
        self.assertAlmostEqual(result["fallback_position"]["right_m"], 2.2)
        self.assertAlmostEqual(result["fallback_position"]["forward_m"], .3)

    def test_two_people_have_independent_one_to_one_matches(self):
        people = [person(1, -15), person(2, 15)]
        targets = [target(7, 15), target(8, -15)]
        result = self.confirm(packet(people=people, targets=targets), packet(2, people=people, targets=targets))
        self.assertEqual([(p["camera_id"], p["radar_id"]) for p in result["people"]], [(1, 8), (2, 7)])

    def test_crossing_people_with_ambiguous_bearings_fall_back(self):
        people = [person(1, -1), person(2, 1)]
        targets = [target(7, -.5), target(8, .5)]
        result = self.confirm(packet(people=people, targets=targets), packet(2, people=people, targets=targets))
        self.assertTrue(all(p["position_source"] == "camera_estimate" for p in result["people"]))

    def test_ambiguous_radar_targets_do_not_confirm(self):
        targets = [target(7, -1), target(8, 1)]
        result = self.confirm(packet(targets=targets), packet(2, targets=targets))
        self.assertEqual(result["people"][0]["position_source"], "camera_estimate")

    def test_angular_gate_rejects_different_person(self):
        targets = [target(bearing=20)]
        result = self.confirm(packet(targets=targets), packet(2, targets=targets))
        self.assertEqual(result["people"][0]["position_source"], "camera_estimate")

    def test_capture_skew_rejects_fresh_but_unaligned_samples(self):
        a, b = packet(), packet(2)
        a["radar"]["capture_mono_ms"] -= 300
        b["radar"]["capture_mono_ms"] -= 300
        self.assertEqual(self.confirm(a, b)["people"][0]["position_source"], "camera_estimate")

    def test_nearest_buffered_sample_is_used_instead_of_latest_radar(self):
        self.confirm()
        old = packet(3)
        old["camera_frame_id"] = 2
        old["camera_capture_mono_ms"] = 1200
        self.engine.ingest(old, now=.2)
        incoming = packet(4, targets=[target(bearing=20)])
        incoming["camera_capture_mono_ms"] = 1300
        result = self.engine.ingest(incoming, now=.3)["people"][0]
        self.assertEqual(result["position_source"], "radar_matched")
        self.assertEqual(result["radar_frame_id"], 3)

    def test_people_cap_uses_confidence_then_stable_id(self):
        people = [person(i, confidence=.8 if i < 9 else .9) for i in range(10)]
        result = self.engine.ingest(packet(people=people, targets=[]), now=0)
        self.assertEqual([p["camera_id"] for p in result["people"]], [9, 0, 1, 2, 3, 4, 5, 6])
        self.assertEqual(result["observed_people"], 10)

    def test_invalid_source_metadata_does_not_renew_previous_people(self):
        self.confirm()
        for mutate in (lambda p: p.update(camera_capture_mono_ms=float("nan")),
                       lambda p: p.update(camera_age_ms=-1),
                       lambda p: p.update(camera_hfov_deg=float("inf")),
                       lambda p: p.update(camera_frame_id=True),
                       lambda p: p.update(schema_version=True),
                       lambda p: p["radar"].update(position_units="mm")):
            p = packet(3)
            mutate(p)
            with self.assertRaises(ValueError):
                self.engine.ingest(p, now=.8)
        self.assertEqual(self.engine.snapshot(now=.851)["people"], [])

    def test_nonfinite_positions_and_bool_ids_are_not_rendered(self):
        p = packet(people=[person()], targets=[target()])
        p["camera_people"][0]["id"] = True
        p["radar"]["targets"][0]["right_m"] = float("nan")
        result = self.engine.ingest(p, now=0)
        self.assertEqual(result["people"], [])
        self.assertEqual(result["radar_targets"], [])

    def test_invalid_monotonic_order_is_atomic(self):
        self.confirm()
        before = self.engine.snapshot(now=.1)
        bad = packet(3)
        bad["camera_capture_mono_ms"] = 1
        with self.assertRaises(ValueError):
            self.engine.ingest(bad, now=.1)
        self.assertEqual(self.engine.snapshot(now=.1), before)

    def test_output_is_json_and_callers_cannot_mutate_internal_state(self):
        result = self.confirm()
        json.dumps(result, allow_nan=False)
        result["people"][0]["right_m"] = 999
        result["people"][0]["fallback_position"]["right_m"] = 999
        self.assertEqual(self.engine.snapshot(now=.1)["people"][0]["right_m"], 0)

    def test_invalid_configuration_is_rejected(self):
        for kwargs in (dict(camera_offset_right_m=float("nan")), dict(alignment_confirmed=1),
                       dict(confirmation_frames=1), dict(max_people=9), dict(max_skew_ms=-1)):
            with self.assertRaises(ValueError):
                FusionConfig(**kwargs)

    def test_invalid_reference_type_is_reported_as_packet_validation_error(self):
        p = packet()
        p["radar"]["reference_origin"] = []
        with self.assertRaises(ValueError):
            self.engine.ingest(p, now=0)

    def test_oversized_numeric_value_is_reported_as_packet_validation_error(self):
        p = packet()
        p["camera_age_ms"] = 10 ** 1000
        with self.assertRaises(ValueError):
            self.engine.ingest(p, now=0)


if __name__ == "__main__":
    unittest.main()
