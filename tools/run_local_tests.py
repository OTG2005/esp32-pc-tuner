"""Test the shared C++ controls and the lightweight, no-login web interface."""
from __future__ import annotations
import gzip
import io
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import unittest
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".local" / "build" / f"native-{os.getpid()}"
sys.path.insert(0, str(ROOT / "tools"))
from local_server import make_server
from build_web import build
from start_remote import target_address, check_device


def msvc_environment():
    if shutil.which("cl"):
        return dict(os.environ)
    setup = Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat")
    if not setup.is_file():
        vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
            "Microsoft Visual Studio/Installer/vswhere.exe"
        result = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
            capture_output=True, text=True, check=True)
        setup = Path(result.stdout.strip()) / "Common7/Tools/VsDevCmd.bat"
    result = subprocess.run(f'call "{setup}" -no_logo -arch=x64 -host_arch=x64 >nul && set',
        shell=True, capture_output=True, text=True, errors="replace", check=True)
    environment = {}
    for line in result.stdout.splitlines():
        key, separator, value = line.partition("=")
        if separator and key:
            environment[key] = value
    return environment


def compile_native():
    BUILD.mkdir(parents=True, exist_ok=True)
    environment = msvc_environment() if os.name == "nt" else dict(os.environ)
    path = next(value for key, value in environment.items() if key.upper() == "PATH")
    compiler = shutil.which("cl" if os.name == "nt" else "g++", path=path)
    if not compiler:
        raise RuntimeError("Install a native C++ compiler.")
    suffix = ".exe" if os.name == "nt" else ""
    for name in ("native_tests", "native_driver"):
        sources = [str(ROOT / "tests" / f"{name}.cpp"),
                   str(ROOT / "sketch_may4a/ControlProtocol.cpp"),
                   str(ROOT / "sketch_may4a/IrControl.cpp")]
        executable = BUILD / (name + suffix)
        command = [compiler, "/nologo", "/std:c++17", "/EHsc", "/W4", "/WX",
                   "/D_CRT_SECURE_NO_WARNINGS", *sources,
                   f"/Fe:{executable}", f"/Fo:{BUILD}{os.sep}"] if os.name == "nt" else \
                  [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", *sources,
                   "-o", str(executable)]
        result = subprocess.run(command, env=environment, cwd=BUILD,
                                capture_output=True, text=True, errors="replace")
        (BUILD / f"{name}-build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
    subprocess.run([str(BUILD / ("native_tests" + suffix))], check=True)
    (BUILD.parent / "native-current.json").write_text(json.dumps({
        "driver": (BUILD / ("native_driver" + suffix)).relative_to(ROOT).as_posix()
    }), encoding="utf-8")


class HttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = make_server(0)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.server.native.close()

    @classmethod
    def request(cls, path, method="GET", body=None):
        data = None
        if isinstance(body, dict):
            data = urllib.parse.urlencode(body).encode()
        elif isinstance(body, str):
            data = body.encode()
        elif isinstance(body, bytes):
            data = body
        request = urllib.request.Request(cls.base + path, data=data, method=method)
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, response.read().decode()
        except urllib.error.HTTPError as error:
            return error.code, error.read().decode()

    def test_no_login_and_small_offline_page(self):
        code, page = self.request("/")
        self.assertEqual(code, 200)
        self.assertIn("LOCAL SIMULATION", page)
        self.assertNotIn("loginForm", page)
        self.assertNotIn("<script src=", page)
        self.assertLess(len(page.encode()), 8000)
        self.assertNotIn("setInterval", page)
        self.assertNotIn("setTimeout", page)
        self.assertEqual(self.request("/api/status")[0], 200)

    def test_old_admin_routes_removed(self):
        for route, method in (("/api/login", "POST"), ("/api/logout", "POST"),
                              ("/api/catalog", "GET")):
            self.assertEqual(self.request(route, method, "ignored" if method == "POST" else None)[0], 404)

    def test_every_button_and_ir_command(self):
        page = (ROOT / "web/index.html").read_text(encoding="utf-8")
        commands = re.findall(r'data-c="([^"]+)"', page)
        catalog = self.server.native.rpc("catalog")
        self.assertEqual(len(catalog["colors"]), 23)
        self.assertEqual(len(catalog["effects"]), 10)
        self.assertEqual(len(catalog["ir"]), 41)
        commands += [item["command"] for item in catalog["ir"]]
        commands += ["brightness 0", "brightness 255", "volume 0", "volume 127",
                     "sensitivity 0.1", "sensitivity 3", "color #00AACC", "settings reset"]
        for command in commands:
            with self.subTest(command=command):
                self.assertEqual(self.request("/api/command", "POST", command)[0], 200)
        print(f"PASS: {len(commands)} lightweight button/IR/boundary command cases")

    def test_invalid_commands_do_not_change_controls(self):
        for command in ("lights yes", "brightness 256", "volume -1", "effect unknown",
                        "pc power", "source microphone", "source automatic"):
            before = self.server.native.rpc("status")["controls"]
            self.assertEqual(self.request("/api/command", "POST", command)[0], 400)
            self.assertEqual(before, self.server.native.rpc("status")["controls"])

    def test_input_bounds_and_no_get_side_effects(self):
        self.assertEqual(self.request("/api/command", "POST", "x" * 96)[0], 413)
        self.assertEqual(self.request("/api/command", "POST", "lights on\0")[0], 400)
        self.assertEqual(self.request("/api/command", "GET")[0], 404)
        self.assertEqual(self.request("/api/command", "POST", b"\xff")[0], 400)

    def test_router_form_and_no_password_echo(self):
        for fields in ({"ssid": "", "password": "12345678"},
                       {"ssid": "s" * 33, "password": "12345678"},
                       {"ssid": "home", "password": "short"}):
            self.assertEqual(self.request("/api/wifi", "POST", fields)[0], 400)
        password = "only-a-test-password"
        code, reply = self.request("/api/wifi", "POST", {"ssid": "test-home", "password": password})
        self.assertEqual(code, 202)
        self.assertNotIn(password, reply)
        self.assertNotIn(password, self.request("/api/status")[1])
        self.assertEqual(self.request("/api/wifi", "POST", {"ssid": "open-home", "password": ""})[0], 202)

    def test_ble_and_web_share_state(self):
        reply = self.server.native.rpc("ble:lights off\ncolor rose-red\nsensitivity balanced\n")
        self.assertEqual(reply["accepted"], 3)
        self.assertIn("lights=off", self.request("/api/status")[1])
        self.assertEqual(self.request("/api/command", "POST", "lights on")[0], 200)
        self.assertTrue(self.server.native.rpc("ble:status\n")["controls"]["lights"])

    def test_read_only_queries_do_not_change_revision(self):
        before = self.server.native.rpc("status")
        for command in ("status", "wifi status", "help"):
            self.assertEqual(self.request("/api/command", "POST", command)[0], 200)
        after = self.server.native.rpc("status")
        self.assertEqual(before["revision"], after["revision"])
        self.assertEqual(before["controls"], after["controls"])

    def test_playback_needs_transport(self):
        try:
            for mode in ("test:disconnected", "test:connected-no-transport"):
                self.server.native.rpc(mode)
                for command in ("audio play", "audio pause", "audio stop", "audio next", "audio previous"):
                    self.assertEqual(self.request("/api/command", "POST", command)[0], 409)
                self.assertEqual(self.request("/api/command", "POST", "color red")[0], 200)
        finally:
            self.server.native.rpc("test:connected")

    def test_parallel_commands_are_not_lost(self):
        self.request("/api/command", "POST", "brightness 0")
        with ThreadPoolExecutor(max_workers=4) as pool:
            replies = list(pool.map(lambda _: self.request("/api/command", "POST", "brightness up"), range(6)))
        self.assertTrue(all(code == 200 for code, _ in replies))
        self.assertEqual(self.server.native.rpc("status")["controls"]["brightness"], 120)

    def test_gzip_roundtrip_matches_source(self):
        page = (ROOT / "web/index.html").read_bytes()
        header = (ROOT / "sketch_may4a/WebPage.h").read_text(encoding="utf-8")
        compressed = header.split("WEB_PAGE_GZIP[] PROGMEM = {", 1)[1].split("};", 1)[0]
        data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})", compressed))
        self.assertEqual(gzip.decompress(data), page)
        self.assertLess(len(data), 2500)


class ToolTests(unittest.TestCase):
    def test_microphone_capture_and_fallback_removed(self):
        sketch = (ROOT / "sketch_may4a/sketch_may4a.ino").read_text(encoding="utf-8")
        for name in ("microphoneInput", "readMicrophone", "microphoneConfig",
                     "MIC_I2S_PORT", "microphoneBuffer", "RX_MODE"):
            self.assertNotIn(name, sketch)
        page = (ROOT / "web/index.html").read_text(encoding="utf-8")
        self.assertNotIn('data-c="source microphone"', page)
        self.assertNotIn('data-c="source automatic"', page)
        self.assertIn("Bluetooth audio only", page)

    def test_private_files_still_ignored_and_unchanged(self):
        header = ROOT / "sketch_may4a/LocalConfig.h"
        if not header.exists():
            self.skipTest("No private configuration in a clean checkout.")
        before = header.read_bytes()
        subprocess.run([sys.executable, str(ROOT / "tools/configure_local.py")],
                       cwd=ROOT, capture_output=True, check=True)
        self.assertEqual(header.read_bytes(), before)
        paths = ["sketch_may4a/LocalConfig.h", "private/access.txt", ".local/firmware/test.bin"]
        result = subprocess.run(["git", "check-ignore", "--", *paths], cwd=ROOT,
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.splitlines(), paths)

    def test_remote_unsafe_targets_and_no_ungated_tunnel(self):
        for target in ("http://localhost:8765", "http://127.0.0.1", "http://192.168.4.1",
                       "https://192.168.100.33", "http://8.8.8.8"):
            result = subprocess.run([sys.executable, str(ROOT / "tools/start_remote.py"),
                "--target", target, "--check-only"], capture_output=True, text=True, timeout=15)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Remote access error:", result.stderr)
        result = subprocess.run([sys.executable, str(ROOT / "tools/start_remote.py"),
            "--target", "http://192.168.100.33"], capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--allowed-email", result.stderr)

    def test_remote_private_ip_pinning(self):
        def resolve(_host, port, **_kwargs):
            return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", ("192.168.100.33", port))]
        self.assertEqual(target_address("http://pc-tuner.local", resolver=resolve,
                                        computer_addresses={"192.168.100.3"}), "http://192.168.100.33/")
        with self.assertRaisesRegex(ValueError, "this computer"):
            target_address("http://pc-tuner.local", resolver=resolve,
                           computer_addresses={"192.168.100.33"})

    def test_remote_lite_firmware_preflight(self):
        class Response:
            status = 200
            def __init__(self, marker): self.headers = {"X-PC-Tuner-Device": marker}
            def __enter__(self): return self
            def __exit__(self, *_args): pass
            def read(self, _limit): return b"ESP32 PC Tuner\nfixture"
        class Opener:
            def __init__(self, marker="esp32-pc-tuner"): self.marker = marker
            def open(self, _url, **_kwargs): return Response(self.marker)
        check_device("http://192.168.100.33/", opener=Opener())
        with self.assertRaisesRegex(ValueError, "not the updated ESP32"):
            check_device("http://192.168.100.33/", opener=Opener("simulator"))


if __name__ == "__main__":
    build()
    compile_native()
    page = (ROOT / "web/index.html").read_text(encoding="utf-8")
    script = re.search(r"<script>(.*?)</script>", page, re.S).group(1)
    js = BUILD / "web-script.js"
    js.write_text(script, encoding="utf-8")
    subprocess.run(["node", "--check", str(js)], check=True)
    suite = unittest.TestSuite([unittest.defaultTestLoader.loadTestsFromTestCase(HttpTests),
                               unittest.defaultTestLoader.loadTestsFromTestCase(ToolTests)])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
