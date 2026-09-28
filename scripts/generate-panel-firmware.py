#!/usr/bin/env python3

from pathlib import Path
import argparse
import hashlib
import json
import struct
import sys

MAGIC = b"PANEL-FIRMWARE\x00"
VERSION = 1

EXPECTED = {
    "anbernic,rg34xx-panel":
        "1400dd6c9b9d64bc618da7b544a029c5718af4a8d712894b343280796dc2bcea",
    "anbernic,rg34xx-sp-panel":
        "929432ef1971f7332c8bd3cad6837c1bb6d0a8d5f7176e206e1eea554a8b2c09",
    "anbernic,rg34xx-sp-v2-panel":
        "3ecd82f153a07ce86097a6cf3326484b235a9d6c5556e5ad56299a490096d3c2",
    "anbernic,rg35xx-plus-rev6-panel":
        "f7d3f0cd98be93c95e2af2361f8f309871ae6aea4f9374c1843ac1ca35a7f699",
    "anbernic,rg35xx-sp-v2-panel":
        "2251794f1454376faac2c50438a517c1ef76501509b6f4d505a2c5d64bcf6e4f",
    "anbernic,rg40xx-panel":
        "519eae7a9dfe39435581cc53559b10362dc00e7f58a1c87ad0770a61299d39ed",
    "anbernic,rg40xx-v2-panel":
        "95bc84fdbbabdaa29a4a5934155c519a5855f19f4d26e7a4b957bb8176a1b743",
    "anbernic,rgcubexx-panel":
        "c8f38b80281e3d90dc68a6bd2c92ccc235e85c3cb4c2ef6db8606b7cf37658d7",
}


def encode(conf):
    out = bytearray(MAGIC)
    out.append(VERSION)

    c = bytearray()

    c += struct.pack(">H", conf["width_mm"])
    c += struct.pack(">H", conf["height_mm"])
    c += struct.pack(">H", conf["rotation"])

    c += bytes.fromhex(conf["reserved_1_hex"])
    c += bytes.fromhex(conf["reserved_2_hex"])

    c += struct.pack(">H", conf["delays"]["reset"])
    c += struct.pack(">H", conf["delays"]["init"])
    c += struct.pack(">H", conf["delays"]["sleep"])
    c += struct.pack(">H", conf["delays"]["backlight"])

    c += bytes.fromhex(conf["reserved_3_hex"])

    c += struct.pack(">H", conf["dsi"]["lanes"])
    c += struct.pack(">H", conf["dsi"]["format"])
    c += struct.pack(">I", conf["dsi"]["mode_flags"])

    c += struct.pack(">I", conf["bus_flags"])

    c += bytes.fromhex(conf["reserved_4_hex"])

    c += bytes([
        conf["preferred_timing"],
        len(conf["timings"]),
    ])

    if len(c) != 48:
        raise ValueError(f"invalid config size: {len(c)}")

    out += c

    for t in conf["timings"]:
        timing = bytearray()

        for key in ("hactive", "hfp", "hslen", "hbp",
                    "vactive", "vfp", "vslen", "vbp"):
            timing += struct.pack(">H", t[key])

        timing += struct.pack(">I", t["dclk"])
        timing += struct.pack(">I", t["flags"])
        timing += bytes.fromhex(t["reserved_hex"])

        if len(timing) != 32:
            raise ValueError(f"invalid timing size: {len(timing)}")

        out += timing

    out += bytes.fromhex(conf["init_sequence_hex"])

    return bytes(out)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--presets", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)

    preset_files = sorted(args.presets.glob("*.json"))

    found = {p.stem for p in preset_files}
    expected = set(EXPECTED)

    if found != expected:
        print("ERROR: preset set mismatch", file=sys.stderr)
        print("missing:", sorted(expected - found), file=sys.stderr)
        print("extra:", sorted(found - expected), file=sys.stderr)
        return 1

    failures = 0

    for preset in preset_files:
        conf = json.loads(preset.read_text(encoding="utf-8"))
        name = preset.stem

        if conf.get("filename") != name:
            print(f"ERROR: filename mismatch in {preset}", file=sys.stderr)
            failures += 1
            continue

        blob = encode(conf)
        digest = hashlib.sha256(blob).hexdigest()

        target = args.output / f"{name}.panel"
        target.write_bytes(blob)

        ok = digest == EXPECTED[name]

        print(
            f"{name:38} "
            f"{len(blob):4} bytes "
            f"{digest} "
            f"{'PASS' if ok else 'FAIL'}"
        )

        if not ok:
            failures += 1

    if failures:
        print(f"\nFAIL: {failures}")
        return 1

    print(f"\nPASS: {len(EXPECTED)}/{len(EXPECTED)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
