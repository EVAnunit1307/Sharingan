"""Export a versioned parser/render fixture using an actual pose-model result."""
import json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
base=json.loads((root/'GroundStation/tests/fixtures/spatial_people.json').read_text())
pose=json.loads((root/'Saved/PersonPose/benchmark/full-pose.json').read_text())['poses'][0]
pose['age_ms']=0
base['spatial_people'].update(tracks_version=1,rig_pose_valid=True,tracks=[dict(id=41,
    right_m=0,forward_m=3,velocity_right_mps=.5,velocity_forward_mps=0,
    height_m=1.65,height_source='assumed',facing_deg=pose['facing_deg'] or 0,pose_yaw_deg=0,
    facing_source='camera',person_evidence='camera_observed',position_source='radar_matched',
    valid_for_ms=500,sample_key='R/1/7/1',pose=pose)])
(root/'GroundStation/tests/fixtures/articulated_people.json').write_text(json.dumps(base,indent=2)+'\n')
