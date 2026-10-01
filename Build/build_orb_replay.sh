#!/usr/bin/env bash
# Optional local research build. Does not change the Pi or launch recording.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
research="$repo_root/Saved/MappingResearch/orb-slam3"
revision=4452a3c4ab75b1cde34e5505a36ec3f9edcdc4c4
mkdir -p "$research"
command -v cmake >/dev/null || { echo 'Install cmake and boost first (macOS: brew install cmake boost).'; exit 1; }
fetch_archive() {
  local name="$1" url="$2" digest="$3"
  if [[ ! -f "$research/$name.tar.gz" ]]; then
    curl --fail --location --retry 2 "$url" --output "$research/$name.tar.gz"
  fi
  python3 - "$research/$name.tar.gz" "$digest" <<'PY'
import hashlib, pathlib, sys
if hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest() != sys.argv[2]:
    raise SystemExit('Downloaded dependency checksum mismatch')
PY
  if [[ ! -d "$research/$name" ]]; then tar -xzf "$research/$name.tar.gz" -C "$research"; fi
}
if [[ ! -d "$research/source/.git" ]]; then
  git init "$research/source"
  git -C "$research/source" remote add origin https://github.com/UZ-SLAMLab/ORB_SLAM3.git
  git -C "$research/source" fetch --depth 1 origin "$revision"
  git -C "$research/source" checkout --detach FETCH_HEAD
fi
python3 "$repo_root/Mapping/orb/prepare_source.py" "$research/source"
fetch_archive eigen-3.4.0 https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz 8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72
fetch_archive opencv-4.11.0 https://github.com/opencv/opencv/archive/refs/tags/4.11.0.tar.gz 9a7c11f924eff5f8d8070e297b322ee68b9227e003fd600d4b8122198091665f
if [[ ! -f "$research/prefix/lib/cmake/opencv4/OpenCVConfig.cmake" ]]; then
  cmake -S "$research/opencv-4.11.0" -B "$research/opencv-build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$research/prefix" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_LIST=core,imgproc,imgcodecs,calib3d,features2d,highgui \
    -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_opencv_apps=OFF \
    -DBUILD_JAVA=OFF -DBUILD_opencv_python3=OFF -DWITH_FFMPEG=OFF -DWITH_GSTREAMER=OFF \
    -DWITH_COCOA=OFF -DWITH_OPENEXR=OFF -DWITH_VTK=OFF -DWITH_AVFOUNDATION=OFF \
    -DWITH_WEBP=OFF -DWITH_TIFF=OFF -DWITH_OPENJPEG=OFF -DWITH_IPP=OFF -DWITH_ITT=OFF \
    -DBUILD_ZLIB=ON -DBUILD_PNG=ON -DBUILD_JPEG=ON
  cmake --build "$research/opencv-build" -j 6
  cmake --install "$research/opencv-build"
fi
cmake -S "$repo_root/Mapping/orb" -B "$research/build" \
  -DORB_SOURCE="$research/source" -DEIGEN_SOURCE="$research/eigen-3.4.0" \
  -DOpenCV_DIR="$research/prefix/lib/cmake/opencv4" -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
cmake --build "$research/build" -j 5
if [[ ! -f "$research/source/Vocabulary/ORBvoc.txt" ]]; then
  tar -xzf "$research/source/Vocabulary/ORBvoc.txt.tar.gz" -C "$research/source/Vocabulary"
fi
echo 'Tracking replay built. See Mapping/orb/README.md for recording replay.'
