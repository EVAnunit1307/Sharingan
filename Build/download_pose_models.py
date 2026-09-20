"""Fetch pinned official MediaPipe task bundles; verify every byte before use."""
import argparse
import hashlib
from pathlib import Path
from urllib.request import urlopen

MODELS={
    'lite':'59929e1d1ee95287735ddd833b19cf4ac46d29bc7afddbbf6753c459690d574a',
    'full':'5134a3aad27a58b93da0088d431f366da362b44e3ccfbe3462b3827a839011b1',
}

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=Path('Saved/PersonPose/models'))
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    for variant,expected in MODELS.items():
        destination=args.output/f'pose_landmarker_{variant}.task'
        if destination.exists() and hashlib.sha256(destination.read_bytes()).hexdigest()==expected:
            print(f'Verified {destination}');continue
        url=f'https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_{variant}/float16/1/pose_landmarker_{variant}.task'
        with urlopen(url,timeout=60) as response:data=response.read(64*1024*1024)
        if hashlib.sha256(data).hexdigest()!=expected:
            raise ValueError(f'Unexpected checksum for {url}; existing model retained')
        temporary=destination.with_suffix('.part');temporary.write_bytes(data);temporary.replace(destination)
        print(f'Verified {destination}')

if __name__=='__main__':main()
