# generate_html_gz.py
# PlatformIO pre-build skripta: generira gzip kompresiran header iz MAIN_HTML
# Kliče se pred vsakim buildom (extra_scripts = pre:scripts/generate_html_gz.py)
# Ročna uporaba: python scripts/generate_html_gz.py

import gzip
import re
import os
import sys

# PlatformIO SCons okolje — deluje tako kot pre-script kot standalone
try:
    Import("env")
    PROJECT_DIR = env.subst("$PROJECT_DIR")
    IS_PIO = True
except Exception:
    PROJECT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    IS_PIO = False

WEBSERVER_CPP = os.path.join(PROJECT_DIR, "src", "webserver.cpp")
OUTPUT_HEADER = os.path.join(PROJECT_DIR, "src", "webserver_html_gz.h")


def extract_html(cpp_path):
    with open(cpp_path, "r", encoding="utf-8") as f:
        content = f.read()
    m = re.search(r'R"rawliteral\((.*?)\)rawliteral"', content, re.DOTALL)
    if not m:
        print("[generate_html_gz] NAPAKA: MAIN_HTML rawliteral ni najden v src/webserver.cpp!", file=sys.stderr)
        sys.exit(1)
    return m.group(1).encode("utf-8")


def bytes_to_c_array(data):
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    if lines:
        lines[-1] = lines[-1].rstrip(",")
    return "\n".join(lines)


def generate_html_gz(*args, **kwargs):
    raw        = extract_html(WEBSERVER_CPP)
    compressed = gzip.compress(raw, compresslevel=9)
    ratio      = len(compressed) / len(raw) * 100
    saving     = 100 - ratio

    print(f"[generate_html_gz] Originalna: {len(raw):,} bytes -> gzip: {len(compressed):,} bytes ({saving:.1f}% prihranek)")

    header = (
        "// webserver_html_gz.h — Gzip kompresiran MAIN_HTML\n"
        "// GENERIRANO SAMODEJNO — ne urejaj ročno!\n"
        "// Vir: src/webserver.cpp (MAIN_HTML rawliteral)\n"
        f"// Originalna: {len(raw):,} bytes -> gzip: {len(compressed):,} bytes ({saving:.1f}% prihranek)\n"
        "#pragma once\n"
        "#include <pgmspace.h>\n"
        "\n"
        "static const uint8_t MAIN_HTML_GZ[] PROGMEM = {\n"
        + bytes_to_c_array(compressed) + "\n"
        "};\n"
        "static const size_t MAIN_HTML_GZ_LEN = sizeof(MAIN_HTML_GZ);\n"
    )

    with open(OUTPUT_HEADER, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)

    print(f"[generate_html_gz] Zapisano: {OUTPUT_HEADER}")


# PlatformIO: `pre:` v extra_scripts pomeni da se ta skripta naloži pred buildom.
# Pokličemo generate_html_gz() TAKOJ — pred kompilacijo webserver.cpp.
# (AddPreAction("buildprog") bi zagotovil šele pred linkerjem, kar je prepozno.)
if IS_PIO:
    generate_html_gz()
else:
    # Standalone: python scripts/generate_html_gz.py
    generate_html_gz()
