#!/bin/sh
# Download the Khronos OpenXR loader for Android (Apache-2.0) into deps/oxr.
# Pinned to the version this port is tested with.
set -e
V=${OPENXR_VERSION:-1.1.49}
cd "$(dirname "$0")/.."
mkdir -p deps/oxr
curl -fL -o deps/openxr_loader.aar \
  "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$V/openxr_loader_for_android-$V.aar"
unzip -oq deps/openxr_loader.aar -d deps/oxr
rm deps/openxr_loader.aar
echo "OpenXR loader $V -> deps/oxr"
