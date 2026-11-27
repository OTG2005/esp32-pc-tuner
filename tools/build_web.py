"""Embed the offline web panel into controller_node/WebPage.h. No credentials are embedded."""
from pathlib import Path
import gzip

ROOT = Path(__file__).resolve().parents[1]


def build() -> None:
    html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
    delimiter = "PC_TUNER_HTML"
    if f'){delimiter}"' in html:
        raise ValueError("Raw string delimiter occurs in HTML")
    header = (
        "#pragma once\n"
        "// Generated from web/index.html by tools/build_web.py. Do not edit.\n"
        "#include <Arduino.h>\n"
        f'const char WEB_PAGE[] PROGMEM = R"{delimiter}({html}){delimiter}";\n'
    )
    compressed = gzip.compress(html.encode("utf-8"), compresslevel=9, mtime=0)
    header += "\nconst uint8_t WEB_PAGE_GZIP[] PROGMEM = {\n"
    for offset in range(0, len(compressed), 16):
        header += "  " + ", ".join(f"0x{byte:02x}" for byte in compressed[offset:offset + 16]) + ",\n"
    header += "};\n"
    (ROOT / "controller_node" / "WebPage.h").write_text(header, encoding="utf-8", newline="\n")
    print(f"Embedded web panel into controller_node/WebPage.h: {len(html.encode('utf-8'))} bytes; gzip: {len(compressed)} bytes")


if __name__ == "__main__":
    build()
