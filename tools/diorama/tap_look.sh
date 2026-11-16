#!/bin/zsh
# tap_look.sh [x y ...] -> taps each point (surface px, 1280x800), then dumps frame to workspace/look.png
cd "$(dirname "$0")/../.."
F=/sdcard/Android/data/io.github.mrchrisster.ftlquest/files
while [ $# -ge 2 ]; do adb shell "echo '$1 $2' > $F/.tap_cmd"; shift 2; sleep ${TAP_WAIT:-2}; done
adb shell touch $F/.dump_frame; sleep 2
adb pull $F/latest.ppm workspace/look.ppm >/dev/null && python3 -c "
from PIL import Image; Image.open('workspace/look.ppm').save('workspace/look.png')" && echo workspace/look.png
