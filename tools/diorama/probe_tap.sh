#!/bin/zsh
# probe_tap.sh x y out.png : restart FTL, wait for the first screen, tap (x,y) in touch space, dump frame.
cd "$(dirname "$0")/../.."
F=/sdcard/Android/data/io.github.mrchrisster.ftlquest/files
adb shell am force-stop io.github.mrchrisster.ftlquest; sleep 1
adb shell am start -n io.github.mrchrisster.ftlquest/.MainActivity >/dev/null; adb shell touch $F/.planes
sleep ${BOOT_WAIT:-14}
adb shell "echo '$1 $2' > $F/.tap_cmd"; sleep 5
adb shell touch $F/.dump_frame; sleep 2
adb pull $F/latest.ppm workspace/probe.ppm >/dev/null && python3 -c "
from PIL import Image; Image.open('workspace/probe.ppm').resize((320,200)).save('$3')"
