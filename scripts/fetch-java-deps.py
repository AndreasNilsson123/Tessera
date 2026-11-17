#!/usr/bin/env python3
"""Downloads the third-party jars the optional Java integration tests need (LWJGL, JavaFX) into third_party/java/.

Only the integration tests use them; the library and its own tests need nothing from here. Versions are pinned, files come
from Maven Central over HTTPS, and every download is verified against the SHA-1 Maven Central publishes next to it before
it is moved into place. Nothing is executed. Re-running skips files that are already present and verified.

    python scripts/fetch-java-deps.py            # LWJGL + JavaFX for this platform
    python scripts/fetch-java-deps.py --lwjgl    # only LWJGL
    python scripts/fetch-java-deps.py --javafx   # only JavaFX
"""
import argparse
import hashlib
import os
import platform
import sys
import urllib.request

LWJGL_VERSION = "3.3.4"
JAVAFX_VERSION = "21.0.5"
REPO = "https://repo1.maven.org/maven2"


def host():
    system = platform.system().lower()
    machine = platform.machine().lower()
    arm = machine in ("arm64", "aarch64")
    if system == "windows":
        return {"lwjgl": "natives-windows" + ("-arm64" if arm else ""), "javafx": "win"}
    if system == "darwin":
        return {"lwjgl": "natives-macos" + ("-arm64" if arm else ""), "javafx": "mac-aarch64" if arm else "mac"}
    return {"lwjgl": "natives-linux" + ("-arm64" if arm else ""), "javafx": "linux-aarch64" if arm else "linux"}


def artifacts(want_lwjgl, want_javafx):
    h = host()
    out = []
    if want_lwjgl:
        for name in ("lwjgl", "lwjgl-opengl"):
            base = f"{REPO}/org/lwjgl/{name}/{LWJGL_VERSION}/{name}-{LWJGL_VERSION}"
            out.append(base + ".jar")
            out.append(base + f"-{h['lwjgl']}.jar")
    if want_javafx:
        for name in ("javafx-base", "javafx-graphics"):
            out.append(f"{REPO}/org/openjfx/{name}/{JAVAFX_VERSION}/{name}-{JAVAFX_VERSION}-{h['javafx']}.jar")
    return out


def fetch(url, timeout=60):
    with urllib.request.urlopen(url, timeout=timeout) as response:  # https only: REPO is https
        return response.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lwjgl", action="store_true")
    parser.add_argument("--javafx", action="store_true")
    parser.add_argument("--dest", default=None, help="default: <repo>/third_party/java")
    args = parser.parse_args()
    both = not args.lwjgl and not args.javafx
    dest = args.dest or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "third_party", "java")
    dest = os.path.abspath(dest)
    os.makedirs(dest, exist_ok=True)

    failed = 0
    for url in artifacts(both or args.lwjgl, both or args.javafx):
        name = url.rsplit("/", 1)[1]
        target = os.path.join(dest, name)
        try:
            expected = fetch(url + ".sha1").decode("ascii").split()[0].strip().lower()
            if os.path.exists(target) and hashlib.sha1(open(target, "rb").read()).hexdigest() == expected:
                print(f"ok (already present)  {name}")
                continue
            data = fetch(url)
            actual = hashlib.sha1(data).hexdigest()
            if actual != expected:
                print(f"CHECKSUM MISMATCH     {name}: expected {expected}, got {actual}", file=sys.stderr)
                failed += 1
                continue
            partial = target + ".part"
            with open(partial, "wb") as f:
                f.write(data)
            os.replace(partial, target)
            print(f"downloaded            {name} ({len(data) // 1024} KiB, sha1 {actual[:12]}...)")
        except Exception as e:  # noqa: BLE001 - report and carry on with the other files
            print(f"FAILED                {name}: {e}", file=sys.stderr)
            failed += 1
    print(f"-> {dest}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
