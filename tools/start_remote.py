"""Safely relay a real ESP32 through HTTPS. Uses only Python's standard library."""
from __future__ import annotations

import argparse
import ipaddress
import re
import shutil
import socket
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

PRIVATE_LANS = tuple(ipaddress.IPv4Network(value) for value in
                     ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def target_address(target: str, *, resolver=socket.getaddrinfo,
                   computer_addresses: set[str] | None = None) -> str:
    """Validate and pin one private IPv4 target before making any HTTP request."""
    try:
        uri = urllib.parse.urlsplit(target)
        if uri.scheme != "http" or not uri.hostname or uri.username is not None or \
           uri.password is not None or uri.query or uri.fragment or \
           uri.path not in ("", "/") or uri.port not in (None, 80) or \
           any(ord(ch) <= 32 for ch in target):
            raise ValueError()
    except ValueError:
        raise ValueError("Target must be the real ESP32 router-LAN HTTP URL on port 80, without credentials, query, or fragment.") from None
    if uri.hostname.lower() == "localhost":
        raise ValueError("A local simulator must never be tunneled.")
    try:
        records = resolver(uri.hostname, 80, type=socket.SOCK_STREAM)
        addresses = {ipaddress.ip_address(record[4][0]) for record in records}
    except (OSError, ValueError):
        raise ValueError("Cannot resolve the ESP32. Use its numeric station IP from Serial Monitor.") from None
    if not addresses or any(not isinstance(address, ipaddress.IPv4Address) or
                            not any(address in network for network in PRIVATE_LANS)
                            for address in addresses):
        raise ValueError("Target must resolve only to private IPv4 LAN addresses.")
    if ipaddress.IPv4Address("192.168.4.1") in addresses:
        raise ValueError("Use the router-LAN station address, not the setup hotspot.")
    if computer_addresses is None:
        try:
            computer_addresses = {record[4][0] for record in
                                  socket.getaddrinfo(socket.gethostname(), 80,
                                                     type=socket.SOCK_STREAM)}
        except OSError:
            computer_addresses = set()
    if any(str(address) in computer_addresses for address in addresses):
        raise ValueError("Target is this computer, not the ESP32. Local simulators must never be tunneled.")
    # Pin the verified IPv4 address. cloudflared will not re-resolve an unchecked
    # IPv6/public address or a changed DNS record later.
    return f"http://{sorted(addresses)[0]}/"


def check_device(target: str, *, opener=None) -> None:
    """Read-only firmware preflight. Authentication is enforced by the tunnel."""
    opener = opener or urllib.request.build_opener(
        urllib.request.ProxyHandler({}), NoRedirect(),
    )
    try:
        with opener.open(target, timeout=8) as response:
            if response.status != 200 or response.headers.get("X-PC-Tuner-Device") != "esp32-pc-tuner":
                raise ValueError("The target is not the updated ESP32 firmware; no tunnel was started.")
            response.read(65536)
        with opener.open(urllib.parse.urljoin(target, "api/status"), timeout=8) as response:
            if response.status != 200 or \
               response.headers.get("X-PC-Tuner-Device") != "esp32-pc-tuner" or \
               not response.read(4096).startswith(b"ESP32 PC Tuner"):
                raise ValueError("The lightweight ESP32 status preflight failed.")
    except (OSError, urllib.error.URLError) as error:
        raise ValueError(f"Cannot reach the ESP32 on the home LAN: {error}") from None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, help="ESP32 station URL, e.g. http://pc-tuner.local")
    parser.add_argument("--allowed-email", action="append", default=[],
                        help="Your email for an additional one-time-PIN gate; repeat for several people.")
    parser.add_argument("--check-only", action="store_true", help="Verify the ESP32 without starting a tunnel.")
    args = parser.parse_args()
    if not args.check_only and not args.allowed_email:
        raise ValueError("This firmware has no device login. Use --allowed-email; "
                         "an unprotected public tunnel is not permitted.")
    for email in args.allowed_email:
        if not re.fullmatch(r"[^@\s,]+@[^@\s,]+\.[^@\s,]+", email):
            raise ValueError("Use individual allowed email addresses, not wildcard domains or passwords.")
    address = target_address(args.target)
    check_device(address)
    print(f"ESP32 identity and lightweight API verified at {address}", flush=True)
    if args.check_only:
        print("Check only: no Internet tunnel or system service was started.")
        return 0
    cloudflared = shutil.which("cloudflared")
    if not cloudflared:
        raise ValueError("Install cloudflared: winget install --id Cloudflare.cloudflared")
    for name in ("config.yml", "config.yaml"):
        if (Path.home() / ".cloudflared" / name).exists():
            raise ValueError("An existing Cloudflare config was found. Use its named tunnel; no configuration was changed.")
    command = [cloudflared, "tunnel", "--no-autoupdate", "--loglevel", "info",
               "--metrics", "127.0.0.1:0", "--proxy-keepalive-connections", "2",
               "--proxy-keepalive-timeout", "10s", "--url", address]
    if args.allowed_email:
        help_result = subprocess.run([cloudflared, "tunnel", "--help"], capture_output=True,
                                     text=True, errors="replace", check=True)
        if "--allowed-mail" not in help_result.stdout:
            raise ValueError("This cloudflared lacks email protection. Update it or use Cloudflare Access.")
        for email in args.allowed_email:
            command.extend(["--allowed-mail", email])
    print("Starting a temporary HTTPS tunnel. Keep the home host awake and on the ESP32 LAN.", flush=True)
    print("Open the printed HTTPS URL from another network. Press Ctrl+C to stop.", flush=True)
    return subprocess.run(command).returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nTunnel stopped.")
        sys.exit(0)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Remote access error: {error}", file=sys.stderr)
        sys.exit(1)
