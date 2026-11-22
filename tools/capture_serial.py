"""Capture ESP32 logs on Windows using .NET; no global pyserial installation."""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM11")
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--reset", action="store_true",
                        help="Pulse EN to reboot the board; does not erase settings.")
    parser.add_argument("--output", type=Path, default=ROOT / ".local" / "serial-monitor.log")
    args = parser.parse_args()
    if not re.fullmatch(r"COM[1-9][0-9]*", args.port, re.I) or not 1 <= args.seconds <= 300:
        raise ValueError("Use a COM port and a capture duration of 1..300 seconds.")
    powershell = shutil.which("powershell") or shutil.which("pwsh")
    if not powershell:
        raise RuntimeError("Windows PowerShell/.NET SerialPort is required for capture.")
    reset = """
    $port.DtrEnable = $false
    $port.RtsEnable = $true
    [System.Threading.Thread]::Sleep(100)
    $port.RtsEnable = $false
    """ if args.reset else ""
    # Use the already available SerialPort API. No .ps1 file is executed and
    # no execution-policy setting or device-ownership workaround is applied.
    script = f"""
    $ErrorActionPreference = 'Stop'
    $port = [System.IO.Ports.SerialPort]::new('{args.port}', 115200,
        [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $port.DtrEnable = $false
    $port.RtsEnable = $false
    $port.ReadTimeout = 250
    try {{
        $port.Open()
        {reset}
        $timer = [System.Diagnostics.Stopwatch]::StartNew()
        while ($timer.Elapsed.TotalSeconds -lt {args.seconds}) {{
            try {{ [Console]::WriteLine($port.ReadLine()) }}
            catch [System.TimeoutException] {{}}
        }}
    }} finally {{
        if ($port.IsOpen) {{ $port.Close() }}
        $port.Dispose()
    }}
    """
    result = subprocess.run([powershell, "-NoProfile", "-Command", script],
                            capture_output=True, text=True, errors="replace",
                            timeout=args.seconds + 15)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result.stdout + result.stderr, encoding="utf-8")
    print(result.stdout, end="")
    if result.returncode:
        print(result.stderr, file=sys.stderr, end="")
        print(f"Could not capture {args.port}. Close Serial Monitor/Plotter and other port users.",
              file=sys.stderr)
    print(f"Serial log: {args.output}")
    return result.returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"Serial capture error: {error}", file=sys.stderr)
        sys.exit(1)
