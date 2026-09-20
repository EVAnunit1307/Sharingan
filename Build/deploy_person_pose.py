"""Stage the camera/radar update using the already trusted Pi SSH host key.

Activation is explicit; old checkout and sensor calibration remain rollback.
Never opens flight-controller or motor interfaces.
"""
import argparse
import json
from pathlib import Path
import shlex
import socket
import time
import paramiko

ROOT=Path(__file__).resolve().parents[1]
BASE='/home/evanl1307/HTN2026/sensor-test'
OLD=BASE+'/pi-sensor-pull-test-028ad24'
NEW=BASE+'/person-pose-20260920'
PY='/home/evanl1307/HTN2026/.venv/bin/python'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ip',default='172.20.10.3')
    parser.add_argument('--activate',action='store_true')
    args=parser.parse_args()
    client=paramiko.SSHClient();client.load_system_host_keys()
    client.set_missing_host_key_policy(paramiko.RejectPolicy())
    client.connect('larp-pi.local',username='evanl1307',
        key_filename=str(ROOT/'Saved/PiSensorPullTest/ssh/pi-sensor-test'),
        sock=socket.create_connection((args.ip,22),timeout=5),allow_agent=False,look_for_keys=False,timeout=5)
    def remote(code):
        _,stdout,stderr=client.exec_command(shlex.quote(PY)+' -c '+shlex.quote(code),timeout=20)
        text=stdout.read().decode();errors=stderr.read().decode();rc=stdout.channel.recv_exit_status()
        if rc:raise RuntimeError(errors or text)
        return text
    remote(f"from pathlib import Path;import shutil;src=Path({OLD!r})/'SensorRig/CV';dst=Path({NEW!r})/'SensorRig/CV';assert src.is_dir();shutil.copytree(src,dst,dirs_exist_ok=True)")
    files=['camera_dashboard.py','radar_service.py','tracking_math.py']
    with client.open_sftp() as sftp:
        for name in files:
            sftp.put(str(ROOT/'SensorRig/CV'/name),NEW+'/SensorRig/CV/'+name)
    remote(f"import py_compile;from pathlib import Path;[py_compile.compile(str(Path({NEW!r})/'SensorRig/CV'/n),doraise=True) for n in {files!r}]")
    result=dict(staged=True,path=NEW,activated=False,host=args.ip,files=files)
    if args.activate:
        code=f'''
import os,signal,subprocess,time,json
from pathlib import Path
base=Path({BASE!r})
previous=[]
for entry in Path('/proc').iterdir():
 if not entry.name.isdigit():continue
 try:cmd=(entry/'cmdline').read_bytes().replace(b'\\x00',b' ').decode()
 except (OSError,UnicodeDecodeError):continue
 if ('pi_camera_stream.py' in cmd or 'camera_dashboard.py' in cmd) and (' -c ' not in cmd):
  if {OLD!r} not in cmd and {NEW!r} not in cmd:raise RuntimeError('Unrecognized camera owner; not replacing it')
  previous.append((int(entry.name),cmd))
for pid,_ in previous:os.kill(pid,signal.SIGTERM)
for _ in range(60):
 if not any(Path('/proc',str(pid)).exists() for pid,_ in previous):break
 time.sleep(.1)
else:raise RuntimeError('Previous sensor owner did not stop')
log=open(base/'person-pose.log','ab',buffering=0)
proc=subprocess.Popen([{PY!r},'-u',{NEW+'/SensorRig/CV/pi_camera_stream.py'!r},'--radar','--quest-port','8765'],
 cwd='/home/evanl1307/HTN2026',stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
(base/'camera_dashboard.pid').write_text(str(proc.pid))
print(json.dumps(dict(pid=proc.pid,previous=previous)))
'''
        result.update(json.loads(remote(code)),activated=True)
    client.close()
    out=ROOT/'Saved/PersonPose/pi-deployment.json';out.parent.mkdir(parents=True,exist_ok=True)
    out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
