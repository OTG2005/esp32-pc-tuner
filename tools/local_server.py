"""Loopback-only UI simulator using the firmware's compiled C++ control logic."""
from __future__ import annotations

import argparse
import atexit
import json
import os
import subprocess
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def native_executable() -> Path:
    manifest = ROOT / ".local" / "build" / "native-current.json"
    if manifest.is_file():
        path = (ROOT / json.loads(manifest.read_text(encoding="utf-8"))["driver"]).resolve()
        path.relative_to((ROOT / ".local").resolve())
    else:
        path = ROOT / ".local" / "build" / \
            ("native_driver.exe" if os.name == "nt" else "native_driver")
    if not path.is_file():
        raise RuntimeError("Run python tools/run_local_tests.py to build the simulator first.")
    return path


class NativeControl:
    def __init__(self, executable: Path):
        self.process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.lock = threading.Lock()
        self.started = time.monotonic()

    def rpc(self, text: str) -> dict:
        data = text.encode("utf-8")
        with self.lock:
            self.process.stdin.write(str(len(data)).encode() + b"\n" + data)
            self.process.stdin.flush()
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError("Native simulator exited")
            return json.loads(line)

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=5)
        for pipe in (self.process.stdin, self.process.stdout, self.process.stderr):
            if pipe:
                pipe.close()


def make_server(port: int = 8765, executable: Path | None = None) -> ThreadingHTTPServer:
    native = NativeControl(executable or native_executable())
    mock_wifi = {"connected": True, "ssid": "LOCAL-TEST-NETWORK", "ip": "127.0.0.1",
                 "rssi": -48, "setup_ap": False, "setup_ip": "192.168.4.1",
                 "hostname": "pc-tuner.local", "service_ready": True, "credentials_pending": False}

    def status(value: dict) -> str:
        c = value["controls"]
        return ("LOCAL SIMULATION — not an ESP32\n"
                f"lights={'on' if c['lights'] else 'off'} brightness={c['brightness']} "
                f"color={c['color']} effect={c['effect']}\n"
                f"sensitivity={c['sensitivity']} source={c['source']} volume={c['volume']} "
                f"mute={'on' if c['muted'] else 'off'} downmix={c['downmix']}\n"
                f"Wi-Fi=simulated SSID={mock_wifi['ssid']} IP=127.0.0.1 "
                f"revision={value['revision']}\n")

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args): pass

        def setup(self) -> None:
            super().setup()
            self.connection.settimeout(2)

        def send_data(self, code: int, body: bytes, mime: str) -> None:
            self.send_response(code)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("X-Frame-Options", "DENY")
            self.send_header("Referrer-Policy", "no-referrer")
            self.send_header("X-PC-Tuner-Device", "simulator")
            if code == 429:
                self.send_header("Retry-After", "60")
            self.send_header("Content-Security-Policy",
                "default-src 'self'; script-src 'self' 'unsafe-inline'; "
                "style-src 'self' 'unsafe-inline'; connect-src 'self'; "
                "img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
            self.end_headers()
            self.wfile.write(body)

        def send_text(self, code: int, value: str) -> None:
            self.send_data(code, value.encode("utf-8"), "text/plain; charset=utf-8")

        def fail(self, code: int, message: str) -> None:
            self.send_text(code, message)

        def body(self, maximum: int, mime: str) -> str | None:
            try:
                size = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                self.fail(400, "invalid content length"); return None
            if size <= 0 or size >= maximum:
                self.fail(413, "request body is empty or too large"); return None
            try:
                raw = self.rfile.read(size)
                if len(raw) != size:
                    self.fail(408, "request body timed out"); return None
                value = raw.decode("utf-8")
            except TimeoutError:
                self.fail(408, "request body timed out"); return None
            except UnicodeDecodeError:
                self.fail(400, "request must contain valid UTF-8"); return None
            if "\0" in value:
                self.fail(400, "NUL bytes are not allowed"); return None
            return value

        def do_GET(self) -> None:
            if self.path == "/":
                page = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
                page = page.replace("<h1>PC Tuner</h1>",
                    "<h1>PC Tuner — LOCAL SIMULATION</h1><p>These controls do not operate an ESP32.</p>")
                self.send_data(200, page.encode(), "text/html; charset=utf-8")
            elif self.path == "/api/status":
                self.send_text(200, status(native.rpc("status")))
            else:
                self.fail(404, "route not found")

        def do_POST(self) -> None:
            if self.path not in ("/api/command", "/api/wifi"):
                self.fail(404, "route not found"); return
            if self.path == "/api/command":
                text = self.body(96, "text/plain")
                if text is None: return
                reply = native.rpc("command:" + text)
                if not reply["ok"]:
                    self.fail(400 if reply.get("invalid") else 409,
                              reply.get("error") or reply["message"]); return
                if text.strip().lower() in ("status", "wifi status"):
                    self.send_text(200, status(reply))
                elif "help" in reply:
                    self.send_text(200, "".join(reply["help"]))
                else:
                    self.send_text(200, reply["message"])
            else:
                value = self.body(384, "application/x-www-form-urlencoded")
                if value is None: return
                fields = urllib.parse.parse_qs(value, keep_blank_values=True)
                ssid = fields.get("ssid", [""])[0]
                password = fields.get("password", [""])[0]
                allow_open = not password
                if "\0" in ssid or "\0" in password or "\n" in ssid or "\r" in ssid:
                    self.fail(400, "invalid Wi-Fi fields"); return
                reply = native.rpc(f"validate-wifi:{int(allow_open)}\n{ssid}\n{password}")
                if not reply["ok"]:
                    self.fail(400, reply["error"]); return
                mock_wifi["ssid"] = ssid
                self.send_text(202, "SIMULATION: Wi-Fi input accepted; no network was changed.")

    try:
        server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    except OSError:
        native.close()
        raise
    server.native = native
    atexit.register(native.close)
    return server


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    server = make_server(args.port)
    print(f"LOCAL SIMULATION: http://localhost:{server.server_port}", flush=True)
    print("No login; loopback simulation only.", flush=True)
    try:
        server.serve_forever()
    finally:
        server.native.close()
        server.server_close()
