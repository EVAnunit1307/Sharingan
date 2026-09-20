#!/usr/bin/env python3
"""Camera + radar dashboard using the shared detector and radar mount config."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'CV'))
from camera_dashboard import main

if __name__ == '__main__':
    main(['--radar', *sys.argv[1:]])
