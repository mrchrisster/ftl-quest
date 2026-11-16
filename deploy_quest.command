#!/bin/zsh -l
# Build FTL Quest debug APK, install to Quest 3, launch, enable diorama planes.
cd "$(dirname "$0")"
LOG="$PWD/workspace/deploy.log"
exec > >(tee "$LOG") 2>&1
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
export ANDROID_HOME="$HOME/Library/Android/sdk"
export PATH="$ANDROID_HOME/platform-tools:/opt/homebrew/bin:/usr/local/bin:$PATH"
F=/sdcard/Android/data/io.github.mrchrisster.ftlquest/files
echo "== $(date)"; echo "== adb devices"; adb devices
echo "== build"
(cd android && gradle assembleDebug --console=plain) || { echo "BUILD FAILED"; exit 1; }
echo "== install"
adb install -r android/app/build/outputs/apk/debug/app-debug.apk || { echo "INSTALL FAILED"; exit 1; }
echo "== launch"
adb logcat -c
adb shell am start -S -n io.github.mrchrisster.ftlquest/.MainActivity
adb shell touch $F/.planes
sleep 15
echo "== logcat (Diorama / errors)"
adb logcat -d | grep -E "Diorama|FATAL|SIGSEGV|Abort|eagl_present_renderbuffer frame" | tail -60
echo "== DONE"
