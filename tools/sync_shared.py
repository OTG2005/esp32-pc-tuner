"""Synchronize shared C++ headers/sources into both Arduino sketch folders.

Arduino IDE only compiles source files located inside each sketch folder.
Edit files in shared/ and run this script to update controller_node/ and led_node/.
"""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]
SHARED_DIR = ROOT / "shared"
TARGETS = [ROOT / "controller_node", ROOT / "led_node"]

SHARED_FILES = [
    "ControlProtocol.h",
    "ControlProtocol.cpp",
    "LocalConfig.example.h",
    "NetworkValidation.h",
    "NodeProtocol.h",
    "NodeProtocol.cpp",
    "NodeUdp.h",
    "NodeUdp.cpp",
    "RuntimeResources.h",
    "RuntimeResources.cpp",
    "TunerWiFi.h",
    "TunerWiFi.cpp",
]


def sync() -> None:
    for target in TARGETS:
        target.mkdir(parents=True, exist_ok=True)
        for filename in SHARED_FILES:
            src = SHARED_DIR / filename
            if not src.exists():
                raise FileNotFoundError(f"Missing shared file: {src}")
            shutil.copy2(src, target / filename)
    print(f"Synced {len(SHARED_FILES)} shared files to controller_node/ and led_node/.")


if __name__ == "__main__":
    sync()
