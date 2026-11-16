#!/bin/zsh -l
# Capture current FTL frame + 5 diorama planes from the Quest into workspace/captures/<time>/
# Optional: workspace/taps.txt with lines "x y" (game coords) are tapped first, 2s apart.
cd "$(dirname "$0")"
export ANDROID_HOME="$HOME/Library/Android/sdk"
export PATH="$ANDROID_HOME/platform-tools:/opt/homebrew/bin:/usr/local/bin:$PATH"
F=/sdcard/Android/data/com.subsetgames.ftl/files
OUT="workspace/captures/$(date +%H%M%S)"
mkdir -p "$OUT"
exec > >(tee "$OUT/capture.log") 2>&1
adb shell touch $F/.planes
if [ -f workspace/taps.txt ]; then
  while read -r x y; do
    [ -z "$x" ] && continue
    echo "tap $x $y"; adb shell "echo '$x $y' > $F/.tap_cmd"; sleep 2
  done < workspace/taps.txt
  mv workspace/taps.txt "$OUT/taps_done.txt"
fi
sleep 1
adb logcat -c
adb shell touch $F/.dump_frame
sleep 3
for f in latest.ppm plane0.png plane1.png plane2.png plane3.png plane4.png; do adb pull $F/$f "$OUT/$f" >/dev/null && echo "pulled $f"; done
adb logcat -d | grep -E "Diorama|Dumped" | tail -20
echo "== DONE $OUT"
