"""Compile the ESP32 sketch locally. Never uploads, commits, or pushes."""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from build_web import build

ROOT = Path(__file__).resolve().parents[1]
FQBN = "esp32:esp32:esp32:PartitionScheme=huge_app,FlashSize=4M,CPUFreq=240,PSRAM=disabled"


def find_cli(explicit: str | None) -> str:
    if explicit:
        if not Path(explicit).is_file():
            raise FileNotFoundError("The supplied Arduino CLI path does not exist.")
        return explicit
    installed = shutil.which("arduino-cli")
    if installed:
        return installed
    for base in (Path(os.environ.get("ProgramFiles", r"C:\Program Files")),
                 Path(os.environ.get("LOCALAPPDATA", "")) / "Programs"):
        candidate = base / "Arduino IDE" / "resources" / "app" / "lib" / \
            "backend" / "resources" / "arduino-cli.exe"
        if candidate.is_file():
            return str(candidate)
    raise FileNotFoundError("Install Arduino IDE/CLI, or pass --cli with the executable path.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli")
    parser.add_argument("--build-path", type=Path)
    args = parser.parse_args()
    if not (ROOT / "sketch_may4a" / "LocalConfig.h").is_file():
        raise RuntimeError("Run python tools/configure_local.py before compiling.")
    build()
    # ESP32 core 2.0.10 has unquoted Windows file_opts hooks. Use a space-free
    # build path without modifying the user's installed board package.
    approved_temp = Path.home() / "AppData" / "Local" / "Temp" / "opencode"
    suffix = hashlib.sha256(str(ROOT).encode()).hexdigest()[:8]
    build_path = args.build_path or (
        approved_temp / f"pc-tuner-arduino-{suffix}" if os.name == "nt"
        else ROOT / ".local" / "arduino"
    )
    if os.name == "nt" and " " in str(build_path):
        raise ValueError("ESP32 core 2.0.10 requires a space-free Windows build path.")
    build_path.mkdir(parents=True, exist_ok=True)
    local = ROOT / ".local"
    local.mkdir(exist_ok=True)
    log_path = local / "firmware-build.log"
    command = [find_cli(args.cli), "compile", "--fqbn", FQBN,
               "--build-path", str(build_path), "--warnings", "all",
               str(ROOT / "sketch_may4a")]
    print("Compiling ESP32 Dev Module / Huge APP. No hardware will be flashed.", flush=True)
    with log_path.open("w", encoding="utf-8") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    output = re.sub(r"\x1b\[[0-9;]*m", "", log_path.read_text(encoding="utf-8", errors="replace"))
    if result.returncode:
        print(output[-12000:])
        print(f"Build failed; full local log: {log_path}")
        return result.returncode
    for line in output.splitlines():
        if line.startswith(("Sketch uses ", "Global variables use ")):
            print(line)
    artifacts = local / "firmware"
    artifacts.mkdir(exist_ok=True)
    for pattern in ("*.bin", "*.elf", "*.map"):
        for path in build_path.glob(pattern):
            shutil.copy2(path, artifacts / path.name)
    print(f"PASS: actual ESP32 firmware compiled. Private build output: {artifacts}")
    print(f"Build log: {log_path}")
    print("Firmware contains device secrets; do not share or commit build artifacts.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Build error: {error}", file=sys.stderr)
        sys.exit(1)
