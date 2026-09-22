#!/usr/bin/env python3
"""
Prepare the Morse HaLow Zephyr module for the nRF54 PlatformIO build.
"""
Import("env")
import os
import re
import shutil
import shlex
from pathlib import Path

PROJECT_DIR = Path(env.subst("$PROJECT_DIR")).resolve()
env.Replace(PROJECT_SRC_DIR=str(PROJECT_DIR / "nrf54" / "src"))

MORSE_CHIP = env.GetProjectOption("custom_morse_chip", "mm6108").lower()
if MORSE_CHIP not in ("mm6108", "mm8108"):
    print(f"ERROR: Unsupported custom_morse_chip: {MORSE_CHIP}")
    env.Exit(1)
MORSE_FIRMWARE = "mm8108b2-rl.mbin" if MORSE_CHIP == "mm8108" else "mm6108.mbin"

module_root = (PROJECT_DIR / "modules" / "mm-iot-zephyr").resolve()
fallback_root = Path(os.environ.get("MMIOT_ZEPHYR_ROOT", PROJECT_DIR / ".." / "mm-iot-zephyr")).expanduser().resolve()
use_prebuilt = env.GetProjectOption("custom_morse_prebuilt", "no").lower() in ("yes", "true", "1")


def is_complete_morse_module(path):
    return (
        path.exists()
        and (path / "CMakeLists.txt").exists()
        and (path / "components" / "morse_sm" / "hostap" / "morse_mbedtls_config.h").exists()
    )


if use_prebuilt:
    binary_root = PROJECT_DIR / "modules" / "mm-iot-zephyr-prebuilt"
    mm_root = Path(os.environ.get("MMIOT_ZEPHYR_BINARY_ROOT", binary_root)).expanduser().resolve()
    archive = mm_root / "zephyr/blobs/lib/mm6108/arm-cortex-m33f/libmorse.a"
    if MORSE_CHIP != "mm6108" or not archive.is_file() or not (mm_root / "include/mmwlan.h").is_file():
        print(f"ERROR: Complete MM6108 binary Morse module not found at {mm_root}")
        env.Exit(1)
else:
    mm_root = module_root if is_complete_morse_module(module_root) else fallback_root


def first_existing(paths):
    for path in paths:
        if path.exists():
            return path
    return None


def read_kconfig_string(path, option, required=True):
    pattern = re.compile(rf'^\s*{re.escape(option)}\s*=\s*"([^"]+)"\s*(?:#.*)?$')

    if not path.exists():
        print(f"ERROR: Kconfig file not found: {path}")
        env.Exit(1)

    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1)

    if required:
        print(f"ERROR: {option} is not configured in {path}")
        env.Exit(1)
    return None

env.setdefault("ENV", {})
env["ENV"]["EDGE_DEVICE_ENABLE_HALOW"] = "1"
env["ENV"]["MORSE_ZEPHYR_MODULE"] = str(mm_root)
os.environ["EDGE_DEVICE_ENABLE_HALOW"] = "1"
os.environ["MORSE_ZEPHYR_MODULE"] = str(mm_root)

bcf_filename = read_kconfig_string(
    PROJECT_DIR / "zephyr" / "prj.conf",
    "CONFIG_WIFI_MORSE_BCF",
)
# An environment-specific fragment selects both the Zephyr override and the
# matching BCF blob staged by this script.
extra_conf = env.GetProjectOption("custom_zephyr_extra_conf", "")
if extra_conf:
    extra_conf_path = (PROJECT_DIR / extra_conf).resolve()
    bcf_override = read_kconfig_string(
        extra_conf_path, "CONFIG_WIFI_MORSE_BCF", required=False
    )
    if bcf_override:
        bcf_filename = bcf_override
    board = env.BoardConfig()
    cmake_args = board.get("build.zephyr.cmake_extra_args", "")
    board.update(
        "build.zephyr.cmake_extra_args",
        cmake_args + " " + shlex.quote("-DEXTRA_CONF_FILE=" + str(extra_conf_path)),
    )

if Path(bcf_filename).name != bcf_filename:
    print(f"ERROR: CONFIG_WIFI_MORSE_BCF must contain a filename, got: {bcf_filename}")
    env.Exit(1)
project_bcf = PROJECT_DIR / bcf_filename

source_roots = [] if use_prebuilt else [mm_root]
if not use_prebuilt and fallback_root != mm_root:
    source_roots.append(fallback_root)

blob_dir = mm_root / "zephyr" / "blobs"
staged_files = [] if use_prebuilt else [
    (
        first_existing(
            [root / "zephyr" / "blobs" / "lib" / "mm6108" / "arm-cortex-m33f" / "libmorse.a" for root in source_roots]
            + [root / "submodules" / "mm-iot-sdk" / "framework" / "morselib" / "lib" / "arm-cortex-m33f" / "libmorse.a" for root in source_roots]
        ),
        blob_dir / "lib" / "mm6108" / "arm-cortex-m33f" / "libmorse.a",
        "libmorse.a",
    ),
    (
        first_existing(
            [root / "zephyr" / "blobs" / "firmware" / MORSE_FIRMWARE for root in source_roots]
            + [root / "submodules" / "mm-iot-sdk" / "framework" / "morsefirmware" / MORSE_FIRMWARE for root in source_roots]
        ),
        blob_dir / "firmware" / MORSE_FIRMWARE,
        MORSE_FIRMWARE,
    ),
    (
        first_existing(
            [project_bcf]
            + [root / "zephyr" / "blobs" / "firmware" / bcf_filename for root in source_roots]
            + [root / "submodules" / "mm-iot-sdk" / "framework" / "morsefirmware" / MORSE_CHIP / "bcfs" / bcf_filename for root in source_roots]
        ),
        blob_dir / "firmware" / bcf_filename,
        bcf_filename,
    ),
]

for src, dst, label in staged_files:
    if src is None:
        print(f"ERROR: {label} not found in project or Morse module checkouts")
        env.Exit(1)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if not dst.exists() or src.read_bytes() != dst.read_bytes():
        shutil.copyfile(src, dst)
        print(f"Staged {label}: {dst}")

sdk_bcf = mm_root / "submodules" / "mm-iot-sdk" / "framework" / "morsefirmware" / MORSE_CHIP / "bcfs" / bcf_filename
staged_bcf = blob_dir / "firmware" / bcf_filename
if use_prebuilt and (not (blob_dir / "firmware" / MORSE_FIRMWARE).is_file() or not staged_bcf.is_file()):
    print(f"ERROR: Morse firmware or {bcf_filename} is missing from binary module")
    env.Exit(1)
if not use_prebuilt and staged_bcf.exists():
    sdk_bcf.parent.mkdir(parents=True, exist_ok=True)
    if not sdk_bcf.exists() or staged_bcf.read_bytes() != sdk_bcf.read_bytes():
        shutil.copyfile(staged_bcf, sdk_bcf)
        print(f"Staged {bcf_filename} for SDK path: {sdk_bcf}")

env["ENV"]["MORSE_SM_USE_APP_BINARIES"] = "1"
os.environ["MORSE_SM_USE_APP_BINARIES"] = "1"

print(f"Using Morse Zephyr module: {mm_root} ({MORSE_CHIP}, BCF {bcf_filename}, prebuilt={use_prebuilt})")
