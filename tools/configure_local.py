"""Generate private local credentials. Never prints passwords or stages files."""
from __future__ import annotations

import argparse
import getpass
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "sketch_may4a" / "LocalConfig.h"
ACCESS = ROOT / "private" / "access.txt"


def import_windows_wifi() -> tuple[str, str]:
    result = subprocess.run(
        ["netsh", "wlan", "show", "interfaces"], capture_output=True, text=True,
        errors="replace", check=True,
    )
    ssid = None
    for line in result.stdout.splitlines():
        key, sep, value = line.partition(":")
        if sep and key.strip() == "SSID":
            ssid = value.strip()
            break
    if not ssid:
        raise RuntimeError("No active Windows Wi-Fi connection. Use the ESP32 setup page instead.")
    approved_temp = Path.home() / "AppData" / "Local" / "Temp" / "opencode"
    with tempfile.TemporaryDirectory(prefix="pc-tuner-wifi-", dir=approved_temp) as directory:
        subprocess.run(
            ["netsh", "wlan", "export", "profile", f"name={ssid}",
             f"folder={directory}", "key=clear"],
            capture_output=True, check=True,
        )
        ns = {"w": "http://www.microsoft.com/networking/WLAN/profile/v1"}
        for path in Path(directory).glob("*.xml"):
            profile = ET.parse(path)
            name = profile.findtext(".//w:SSID/w:name", namespaces=ns)
            if name == ssid:
                password = profile.findtext(".//w:sharedKey/w:keyMaterial", namespaces=ns)
                if not password:
                    raise RuntimeError("This Wi-Fi profile has no exportable WPA password.")
                return ssid, password
    raise RuntimeError("Could not export the active Wi-Fi profile.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--import-windows-wifi", action="store_true")
    group.add_argument("--prompt-wifi", action="store_true")
    parser.add_argument("--force", action="store_true", help="Replace existing private bootstrap configuration")
    args = parser.parse_args()
    if HEADER.exists() and not args.force:
        print("Private configuration already exists; left unchanged.")
        print("Lightweight firmware does not use legacy web/hotspot passwords.")
        return
    ssid = password = ""
    if args.import_windows_wifi:
        ssid, password = import_windows_wifi()
    elif args.prompt_wifi:
        ssid = input("2.4 GHz Wi-Fi SSID: ")
        password = getpass.getpass("Wi-Fi password (not echoed): ")
    if ssid and (len(ssid.encode("utf-8")) > 32 or
                 any(ord(ch) < 32 or ord(ch) == 127 for ch in ssid) or
                 not (re.fullmatch(r"[\x20-\x7E]{8,63}", password) or
                      re.fullmatch(r"[0-9a-fA-F]{64}", password))):
        raise ValueError("Use a 1..32 byte SSID and an ASCII WPA passphrase or 64-digit hex key.")
    values = {
        "WIFI_SSID": ssid, "WIFI_PASSWORD": password,
        "SETUP_SSID": "PC-Tuner-Setup",
    }
    lines = [
        "#pragma once", "// PRIVATE: generated locally; excluded by .gitignore.",
        "namespace local_config {",
    ]
    for key, value in values.items():
        # Emit UTF-8 bytes using escapes: independent of the compiler code page.
        literal = '"' + "".join(
            chr(byte) if 32 <= byte <= 126 and byte not in (34, 92)
            else f"\\{byte:03o}" for byte in value.encode("utf-8")
        ) + '"'
        lines.append(f"constexpr char {key}[] = {literal};")
    lines.append("}")
    HEADER.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    ACCESS.parent.mkdir(parents=True, exist_ok=True)
    ACCESS.write_text(
        "PRIVATE — do not share or commit this file.\n\n"
        "Setup hotspot: PC-Tuner-Setup with the board's MAC suffix\n"
        "Setup hotspot: OPEN, no password\n"
        "Setup address: http://192.168.4.1\n\n"
        "Web page: no device login\n\n"
        f"Bootstrap Wi-Fi: {ssid if ssid else 'Configure using the setup page'}\n"
        "Passwords are compiled into firmware; protect the board and binaries.\n"
        "Wi-Fi configured in the web page is stored on the ESP32 in NVS.\n",
        encoding="utf-8",
    )
    print("Private bootstrap configuration generated; no web/hotspot password is used.")
    print("No passwords were printed. No Git commands were run.")


if __name__ == "__main__":
    main()
