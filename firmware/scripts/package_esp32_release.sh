#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-firmware/.pio/build/heltec-hc33}"
output_dir="${2:-dist/firmware}"
mkdir -p "$output_dir"

# PlatformIO copies the ESP-IDF images into the build root after generating
# flasher_args.json. Resolve those copies while keeping the manifest's offsets.
python - "$build_dir" "$output_dir" <<'PY'
import json
import shutil
import subprocess
import sys
from pathlib import Path

build_dir = Path(sys.argv[1]).resolve()
output_dir = Path(sys.argv[2]).resolve()
manifest = json.loads((build_dir / "flasher_args.json").read_text())
fallbacks = {
    manifest["bootloader"]["file"]: "bootloader.bin",
    manifest["partition-table"]["file"]: "partitions.bin",
    manifest["app"]["file"]: "firmware.bin",
}

def image_path(name):
    original = build_dir / name
    copied = build_dir / fallbacks.get(name, name)
    for candidate in (original, copied):
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"Flash image {name} is missing from {build_dir}")

app = image_path(manifest["app"]["file"])
shutil.copyfile(app, output_dir / "live-stocking-ota.bin")

segments = []
for offset, name in sorted(manifest["flash_files"].items(), key=lambda item: int(item[0], 0)):
    segments.extend((offset, str(image_path(name))))

chip = manifest["extra_esptool_args"]["chip"]
subprocess.run([
    sys.executable, "-m", "esptool", "--chip", chip, "merge-bin",
    *manifest["write_flash_args"],
    "--output", str(output_dir / "live-stocking-flash.bin"),
    *segments,
], check=True)
PY
