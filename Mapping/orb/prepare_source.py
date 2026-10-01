"""Add export accessors to one pinned upstream checkout; never change tracking logic."""
import argparse
from pathlib import Path
import subprocess

REVISION = '4452a3c4ab75b1cde34e5505a36ec3f9edcdc4c4'

def prepare(source):
    source = Path(source)
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION:
        raise ValueError('Unexpected ORB-SLAM3 revision: ' + revision)
    header = source/'include/System.h'
    original = header.read_text()
    if 'Atlas* ExportAtlas() const;' not in original:
        marker = '\nprivate:\n'
        if original.count(marker) != 1:
            raise ValueError('Unexpected System.h layout')
        header.write_text(original.replace(marker, '\n    // WALLHACK: stopped-worker research export only.\n'
            '    Atlas* ExportAtlas() const;\n    Tracking* ExportTracker() const;\n'
            '    void WaitForExport();\n' + marker))
    # Upstream generates this small g2o configuration file through its CMake build.
    (source/'Thirdparty/g2o/config.h').write_text('#pragma once\n// OpenMP and shared g2o disabled.\n')
    # Modern libc++ supplies these standard types without the old GCC TR1 paths.
    for path in source.rglob('*'):
        if path.suffix not in ('.h', '.hpp', '.cpp', '.cc'):
            continue
        old = path.read_text()
        new = old.replace('<stdint-gcc.h>', '<cstdint>').replace('<tr1/unordered_map>', '<unordered_map>')
        new = new.replace('<tr1/memory>', '<memory>').replace('std::tr1::', 'std::')
        if new != old:
            path.write_text(new)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    prepare(parser.parse_args().source)
