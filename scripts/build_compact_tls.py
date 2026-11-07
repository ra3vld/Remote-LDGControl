"""Build the pinned SDK's TLS sources with bounded record buffers.

Changing an application -D cannot change Arduino's precompiled Mbed TLS.
Compile the identical ESP-IDF 5.5.5 submodule with record-size overrides;
leave crypto, X.509 and all ABI-affecting options in the SDK.
"""
import hashlib
from pathlib import Path
import tarfile
import urllib.request

COMMIT = "9d669eadb1955d348986b9280156710aaaadf79f"
ARCHIVE_SHA256 = "45f5e3ca387dfc1dbc41bd221f56971a6d254eb1a88d8a0faadfa9e0d193719d"
SOURCES = [
    "debug.c", "mps_reader.c", "mps_trace.c", "ssl_cache.c",
    "ssl_ciphersuites.c", "ssl_client.c", "ssl_cookie.c",
    "ssl_debug_helpers_generated.c", "ssl_msg.c", "ssl_ticket.c", "ssl_tls.c",
    "ssl_tls12_client.c", "ssl_tls12_server.c", "ssl_tls13_keys.c",
    "ssl_tls13_server.c", "ssl_tls13_client.c", "ssl_tls13_generic.c",
    "ldg_tls_limits.c",
]


def source_directory(project):
    cache = Path(project) / ".pio" / "compact-tls"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / (COMMIT + ".tar.gz")
    if not archive.exists():
        url = "https://codeload.github.com/espressif/mbedtls/tar.gz/" + COMMIT
        with urllib.request.urlopen(url, timeout=60) as response:
            content = response.read(32 * 1024 * 1024 + 1)
        if hashlib.sha256(content).hexdigest() != ARCHIVE_SHA256:
            raise RuntimeError("Mbed TLS source archive digest mismatch")
        archive.write_bytes(content)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise RuntimeError("Cached Mbed TLS source archive digest mismatch")
    root = cache / ("mbedtls-" + COMMIT)
    if not root.exists():
        # Extract only regular files from the pinned upstream source directory.
        with tarfile.open(archive, "r:gz") as package:
            for member in package.getmembers():
                path = Path(member.name)
                if path.is_absolute() or ".." in path.parts:
                    raise RuntimeError("Unsafe Mbed TLS archive member")
                wanted = len(path.parts)>1 and (path.parts[1] in ("include", "library") or path.parts[1].startswith("LICENSE"))
                if member.isfile() and wanted:
                    target = cache / path
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(package.extractfile(member).read())
    return root


def build(env):
    if env["PIOENV"] == "native":
        return
    root = source_directory(env["PROJECT_DIR"])
    defines=env.get("CPPDEFINES",[])
    web_tls=any((isinstance(d,(tuple,list)) and d[0]=="TLS_ENABLED" and str(d[1])!="0") or d=="TLS_ENABLED" for d in defines)
    small_input=env["PIOENV"]=="esp32-3248s035c" and not web_tls
    sdk = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs"))
    version = (sdk / "esp32/include/esp_common/include/esp_idf_version.h").read_text()
    for name, expected in (("MAJOR", 5), ("MINOR", 5), ("PATCH", 5)):
        import re
        if not re.search(r"#define\s+ESP_IDF_VERSION_" + name + r"\s+" + str(expected) + r"\b", version):
            raise RuntimeError("Compact TLS sources must be updated together with ESP-IDF")
    (root / "library/ldg_tls_limits.c").write_text('''#include "mbedtls/ssl.h"
#if MBEDTLS_SSL_IN_CONTENT_LEN != '''+str(4096 if small_input else 16384)+''' || MBEDTLS_SSL_OUT_CONTENT_LEN != 4096
#error Unexpected TLS record buffer sizes
#endif
unsigned ldg_tls_in_content_len(void) { return MBEDTLS_SSL_IN_CONTENT_LEN; }
unsigned ldg_tls_out_content_len(void) { return MBEDTLS_SSL_OUT_CONTENT_LEN; }
''')
    tls = env.Clone()
    if small_input:tls.Append(CPPDEFINES=["LDG_TLS_SMALL_INPUT"])
    tls.Append(CPPPATH=[str(root / "library")])
    tls.Append(CPPDEFINES=[("MBEDTLS_USER_CONFIG_FILE", '\\"ldg_tls_config.h\\"')])
    library = tls.BuildLibrary(
        "$BUILD_DIR/compact-tls", str(root / "library"),
        src_filter=" ".join("+<" + name + ">" for name in SOURCES),
    )
    env.Prepend(LIBS=[library])


if "Import" in globals():
    Import("env")
    build(env)
