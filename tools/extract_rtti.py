#!/usr/bin/env python3
"""
tools/extract_rtti.py - C++ RTTI & VTable Cross-Referencing Tool for FTL
Parses Itanium C++ ABI typeinfo structures (_ZTI, _ZTS, _ZTV) and compares
recovered member functions and vtables against known FTL: Hyperspace layouts.
"""

import sys
import os
import json
import re

# Known FTL core classes to track
CORE_FTL_CLASSES = [
    "CApp",
    "ShipManager",
    "WorldManager",
    "CrewMember",
    "ShipBlueprint",
    "WeaponBlueprint",
    "DroneBlueprint",
    "CombatControl",
    "SpaceManager",
    "CommandGui",
    "ResourceControl",
    "ScoreKeeper",
    "CompleteShip",
    "Projectile",
    "Laser",
    "Missile",
    "BeamWeapon",
    "BombProject",
    "SystemBox",
    "ShieldSystem",
    "WeaponSystem",
    "OxygenSystem",
    "MedbaySystem",
    "CloneSystem",
    "MindSystem",
    "HackingSystem",
    "CloakingSystem",
    "BatterySystem"
]

def demangle_itanium_simple(mangled: str) -> str:
    """
    Lightweight Itanium C++ demangler fallback when c++filt / llvm-cxxfilt is not available.
    Handles common FTL patterns like _ZTV9CApp -> vtable for CApp.
    """
    prefix = ""
    target = mangled
    if target.startswith("__"):
        target = target[1:]
    
    if target.startswith("_ZTV"):
        prefix = "vtable for "
        target = target[4:]
    elif target.startswith("_ZTI"):
        prefix = "typeinfo for "
        target = target[4:]
    elif target.startswith("_ZTS"):
        prefix = "typeinfo name for "
        target = target[4:]
    elif target.startswith("_ZN"):
        # Nested name: e.g. _ZN4CApp6OnInitEv
        # Basic regex parsing
        m = re.findall(r'(\d+)([A-Za-z0-9_]+)', target[3:])
        if m:
            parts = []
            for length_str, name in m:
                l = int(length_str)
                parts.append(name[:l])
            return "::".join(parts)

    m = re.match(r'^(\d+)([A-Za-z0-9_]+)', target)
    if m:
        l = int(m.group(1))
        name = m.group(2)[:l]
        return prefix + name

    return mangled


def process_vtables_and_rtti(triage_dir: str, output_path: str):
    vtables_file = os.path.join(triage_dir, "vtables.json")
    if not os.path.exists(vtables_file):
        print(f"[-] Error: {vtables_file} not found. Run analyze_macho.py first.")
        return

    with open(vtables_file, "r") as f:
        vtables = json.load(f)

    catalog = {}
    for vt in vtables:
        raw_name = vt["name"]
        demangled = demangle_itanium_simple(raw_name)
        addr = vt["address"]

        matched_class = None
        for cls in CORE_FTL_CLASSES:
            if cls in demangled:
                matched_class = cls
                break

        catalog[raw_name] = {
            "demangled": demangled,
            "address": hex(addr),
            "matched_core_class": matched_class
        }

    with open(output_path, "w") as f:
        json.dump(catalog, f, indent=2)

    print(f"[*] Processed {len(vtables)} vtables.")
    matched = [v for v in catalog.values() if v["matched_core_class"]]
    print(f"[*] Matched {len(matched)} core FTL classes.")
    for m in matched[:10]:
        print(f"    - {m['demangled']} @ {m['address']}")


def main():
    if len(sys.argv) < 2:
        print("Usage: python extract_rtti.py <triage_reports_dir> [output_file]")
        sys.exit(1)

    triage_dir = sys.argv[1]
    out_file = sys.argv[2] if len(sys.argv) > 2 else os.path.join(triage_dir, "rtti_catalog.json")
    process_vtables_and_rtti(triage_dir, out_file)


if __name__ == "__main__":
    main()
