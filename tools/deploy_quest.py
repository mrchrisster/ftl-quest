#!/usr/bin/env python3
"""
tools/deploy_quest.py - Automated Asset Extraction & Quest 3 ADB Deployment Tool
Extracts the unencrypted ARM64 Mach-O executable and ftl.dat from the user's legal IPA
and deploys them directly to the Quest 3 package files directory via ADB.
"""

import sys
import os
import zipfile
import subprocess
import shutil

WORKSPACE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Usage: python3 tools/deploy_quest.py [path/to/FTL.ipa]  (default: FTL_1.6.21_ios_8.0.ipa in the repo root)
IPA_PATH = sys.argv[1] if len(sys.argv) > 1 else os.path.join(WORKSPACE_DIR, "FTL_1.6.21_ios_8.0.ipa")

INPUTS_DIR = os.path.join(WORKSPACE_DIR, "workspace", "inputs")
QUEST_PACKAGE = "com.subsetgames.ftl"
QUEST_TARGET_DIR = f"/sdcard/Android/data/{QUEST_PACKAGE}/files"

def check_adb():
    try:
        out = subprocess.check_output(["adb", "devices"], encoding="utf-8")
        lines = [line.strip() for line in out.strip().split("\n")[1:] if line.strip()]
        devices = [l.split("\t")[0] for l in lines if "device" in l]
        return devices
    except Exception as e:
        print(f"[-] ADB not found or error checking devices: {e}")
        return []

def extract_assets():
    if not os.path.exists(IPA_PATH):
        print(f"[-] Error: IPA not found at {IPA_PATH}")
        sys.exit(1)

    os.makedirs(INPUTS_DIR, exist_ok=True)
    macho_dest = os.path.join(INPUTS_DIR, "FTL")
    dat_dest = os.path.join(INPUTS_DIR, "ftl.dat")

    print(f"[*] Extracting assets from {IPA_PATH}...")
    with zipfile.ZipFile(IPA_PATH, "r") as z:
        if not os.path.exists(macho_dest):
            print(f"  -> Extracting FTL binary...")
            with open(macho_dest, "wb") as f_out:
                f_out.write(z.read("Payload/FTL.app/FTL"))
            print(f"  [+] Saved {macho_dest} ({os.path.getsize(macho_dest)} bytes)")
        else:
            print(f"  [i] FTL binary already present.")

        if not os.path.exists(dat_dest):
            print(f"  -> Extracting ftl.dat (~231 MB)...")
            with open(dat_dest, "wb") as f_out:
                f_out.write(z.read("Payload/FTL.app/ftl.dat"))
            print(f"  [+] Saved {dat_dest} ({os.path.getsize(dat_dest)} bytes)")
        else:
            print(f"  [i] ftl.dat already present.")

    return macho_dest, dat_dest

def deploy_to_quest(macho_dest, dat_dest, device_id):
    print(f"\n[*] Preparing Quest 3 destination: {QUEST_TARGET_DIR} on device {device_id}...")
    subprocess.run(["adb", "-s", device_id, "shell", f"mkdir -p {QUEST_TARGET_DIR}/save"], check=True)

    print(f"[*] Pushing FTL binary to Quest 3...")
    subprocess.run(["adb", "-s", device_id, "push", macho_dest, f"{QUEST_TARGET_DIR}/FTL"], check=True)

    print(f"[*] Pushing ftl.dat resource archive to Quest 3 (this may take a few seconds)...")
    subprocess.run(["adb", "-s", device_id, "push", dat_dest, f"{QUEST_TARGET_DIR}/ftl.dat"], check=True)

    print(f"[+] All game assets successfully deployed to Quest 3!")
    print(f"    - Executable: {QUEST_TARGET_DIR}/FTL")
    print(f"    - Resources : {QUEST_TARGET_DIR}/ftl.dat")

def main():
    macho_path, dat_path = extract_assets()
    devices = check_adb()

    if not devices:
        print("\n[!] No active ADB device found.")
        print(f"    Extracted files are ready locally in: {INPUTS_DIR}")
        print("    When your Quest 3 is connected via USB/Wi-Fi with Developer Mode enabled, re-run:")
        print("    python tools/deploy_quest.py")
        return

    print(f"\n[+] Found {len(devices)} active Android / Quest device(s): {devices}")
    deploy_to_quest(macho_path, dat_path, devices[0])

if __name__ == "__main__":
    main()
