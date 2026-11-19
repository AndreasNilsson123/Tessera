#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Tessera contributors
"""Assembles the Java release artifacts from per-platform builds.

Each platform's build produces a `tessera.jar` that embeds only that platform's native library. This script takes the class
files from one of them and the native libraries from all of them, and writes (to --out):

    tessera-<v>.jar            multi-release jar with natives/<os>-<arch>/<library> for every platform given, module-info,
                               LICENSE/NOTICE, an Implementation-Version manifest
    tessera-<v>-sources.jar    the Java sources (and the license)
    tessera-<v>-javadoc.jar    javadoc of the exported package (needs a JDK's `javadoc`)
    tessera-lwjgl-<v>.jar (+ sources, javadoc)   the optional LWJGL adapter, if --lwjgl-jar is given
    *.pom                      Maven POMs for both artifacts
    tessera-<v>.cdx.json       CycloneDX SBOM
    SHA256SUMS                 of everything above
    maven/                     the same files in a Maven repository layout with .md5/.sha1/.sha256 (and .asc with --sign), ready
                               to zip and upload to the Central Portal

Everything is deterministic (fixed timestamps, sorted entries) so that the same inputs give the same bytes.

    python scripts/assemble-release.py --base-jar linux/tessera.jar --native linux-x86_64=linux/libtessera.so \\
        --native windows-x86_64=win/tessera.dll --native macos-aarch64=mac/libtessera.dylib --lwjgl-jar linux/tessera-lwjgl.jar \\
        --out dist
"""
import argparse
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import uuid
import zipfile

GROUP = "io.github.andreasnilsson123"
ARTIFACT = "tessera"
LWJGL_ARTIFACT = "tessera-lwjgl"
URL = "https://github.com/AndreasNilsson123/Tessera"
LWJGL_VERSION = "3.3.4"
PACKAGE = "io.github.andreasnilsson123.tessera"
EPOCH = (2026, 1, 1, 0, 0, 0)  # fixed ZIP timestamp: reproducible output
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
NATIVE_NAMES = {"windows": "tessera.dll", "linux": "libtessera.so", "macos": "libtessera.dylib"}


def project_version():
    text = open(os.path.join(ROOT, "include", "tessera", "tessera.h"), encoding="utf-8").read()
    parts = [re.search(r"#define TESSERA_VERSION_%s (\d+)" % p, text).group(1) for p in ("MAJOR", "MINOR", "PATCH")]
    return ".".join(parts)


def zip_write(zf, name, data):
    info = zipfile.ZipInfo(name, EPOCH)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    zf.writestr(info, data)


def manifest(version, title, extra=()):
    lines = ["Manifest-Version: 1.0", "Multi-Release: true", f"Implementation-Title: {title}", f"Implementation-Version: {version}",
             f"Implementation-Vendor: Tessera contributors", "Implementation-URL: " + URL] + list(extra)
    out = []
    for line in lines:  # 72-byte line limit with continuation lines
        raw = line.encode("utf-8")
        out.append(raw[:72].decode("utf-8"))
        raw = raw[72:]
        while raw:
            out.append(" " + raw[:71].decode("utf-8"))
            raw = raw[71:]
    return ("\r\n".join(out) + "\r\n\r\n").encode("utf-8")


def read_license_files():
    return {"META-INF/LICENSE": open(os.path.join(ROOT, "LICENSE"), "rb").read(), "META-INF/NOTICE": open(os.path.join(ROOT, "NOTICE"), "rb").read()}


def repack(base_jar, out_path, natives, version, title, extra_manifest=()):
    entries = {}
    with zipfile.ZipFile(base_jar) as z:
        for n in z.namelist():
            if n.endswith("/") or n.startswith("natives/") or n == "META-INF/MANIFEST.MF":
                continue
            entries[n] = z.read(n)
    for key, path in natives.items():
        osname = key.split("-")[0]
        entries[f"natives/{key}/{NATIVE_NAMES[osname]}"] = open(path, "rb").read()
    entries.update(read_license_files())
    entries["META-INF/MANIFEST.MF"] = manifest(version, title, extra_manifest)
    with zipfile.ZipFile(out_path, "w") as zf:
        zip_write(zf, "META-INF/MANIFEST.MF", entries.pop("META-INF/MANIFEST.MF"))
        for n in sorted(entries):
            zip_write(zf, n, entries[n])


def sources_jar(out_path, version, roots, title):
    entries = {}
    for r in roots:
        for dirpath, _dirs, files in os.walk(r):
            for f in files:
                if f.endswith(".java"):
                    full = os.path.join(dirpath, f)
                    entries[os.path.relpath(full, r).replace(os.sep, "/")] = open(full, "rb").read()
    entries.update(read_license_files())
    with zipfile.ZipFile(out_path, "w") as zf:
        zip_write(zf, "META-INF/MANIFEST.MF", manifest(version, title + " sources"))
        for n in sorted(entries):
            zip_write(zf, n, entries[n])


def javadoc_jar(out_path, version, src_root, packages_dir, title, javadoc, classpath=None):
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        files = []
        for dirpath, _dirs, fs in os.walk(packages_dir):
            if os.sep + "internal" in dirpath:
                continue
            files += [os.path.join(dirpath, f) for f in fs if f.endswith(".java")]
        cmd = [javadoc, "-quiet", "-notimestamp", "-Xdoclint:none", "--release", "11", "-encoding", "UTF-8", "-charset", "UTF-8", "-windowtitle", title,
               "-d", tmp, "-sourcepath", src_root]
        if classpath:
            cmd += ["-cp", os.pathsep.join(classpath)]
        cmd += sorted(files)
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stdout + result.stderr)
            raise SystemExit("javadoc failed")
        entries = {}
        for dirpath, _dirs, fs in os.walk(tmp):
            for f in fs:
                full = os.path.join(dirpath, f)
                entries[os.path.relpath(full, tmp).replace(os.sep, "/")] = open(full, "rb").read()
    entries.update(read_license_files())
    with zipfile.ZipFile(out_path, "w") as zf:
        zip_write(zf, "META-INF/MANIFEST.MF", manifest(version, title + " javadoc"))
        for n in sorted(entries):
            zip_write(zf, n, entries[n])


def pom(artifact, version, name, description, deps=()):
    dep_xml = ""
    if deps:
        dep_xml = "  <dependencies>\n" + "".join(
            f"    <dependency>\n      <groupId>{g}</groupId>\n      <artifactId>{a}</artifactId>\n      <version>{v}</version>\n"
            f"      <scope>{s}</scope>\n    </dependency>\n" for g, a, v, s in deps) + "  </dependencies>\n"
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 http://maven.apache.org/xsd/maven-4.0.0.xsd">
  <modelVersion>4.0.0</modelVersion>
  <groupId>{GROUP}</groupId>
  <artifactId>{artifact}</artifactId>
  <version>{version}</version>
  <packaging>jar</packaging>
  <name>{name}</name>
  <description>{description}</description>
  <url>{URL}</url>
  <licenses>
    <license>
      <name>Apache-2.0</name>
      <url>https://www.apache.org/licenses/LICENSE-2.0.txt</url>
      <distribution>repo</distribution>
    </license>
  </licenses>
  <developers>
    <developer>
      <name>Tessera contributors</name>
      <url>{URL}</url>
    </developer>
  </developers>
  <scm>
    <connection>scm:git:{URL}.git</connection>
    <developerConnection>scm:git:{URL}.git</developerConnection>
    <url>{URL}</url>
  </scm>
{dep_xml}</project>
"""


def sha(path, algo):
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sbom(path, version, jars):
    ref = f"pkg:maven/{GROUP}/{ARTIFACT}@{version}"
    doc = {
        "bomFormat": "CycloneDX", "specVersion": "1.5",
        "serialNumber": "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, ref)), "version": 1,
        "metadata": {"timestamp": "2026-01-01T00:00:00Z",
                     "component": {"type": "library", "bom-ref": ref, "name": ARTIFACT, "group": GROUP, "version": version,
                                   "purl": ref, "licenses": [{"license": {"id": "Apache-2.0"}}],
                                   "externalReferences": [{"type": "vcs", "url": URL}]}},
        "components": [{"type": "file", "name": os.path.basename(j), "hashes": [{"alg": "SHA-256", "content": sha(j, "sha256")}]} for j in jars],
        "dependencies": [{"ref": ref, "dependsOn": []}],  # the artifacts embed no third-party code (see THIRD_PARTY_NOTICES.md)
    }
    import json
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
        f.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base-jar", required=True, help="a built tessera.jar (its classes are used)")
    ap.add_argument("--native", action="append", default=[], metavar="OS-ARCH=PATH", help="e.g. linux-x86_64=libtessera.so (repeatable)")
    ap.add_argument("--lwjgl-jar", help="a built tessera-lwjgl.jar")
    ap.add_argument("--javadoc", default=None, help="path of the javadoc tool (default: JAVA_HOME/bin/javadoc or PATH)")
    ap.add_argument("--lwjgl-classpath", default=None, help="classpath for javadoc of the LWJGL adapter (the LWJGL jars)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", default=None)
    ap.add_argument("--sign", action="store_true", help="sign every artifact with gpg (needs a key in the keyring / GPG_* env)")
    args = ap.parse_args()

    version = args.version or project_version()
    if args.version and args.version != project_version():
        sys.exit(f"--version {args.version} does not match include/tessera/tessera.h ({project_version()})")
    natives = {}
    for item in args.native:
        key, _, path = item.partition("=")
        if not re.fullmatch(r"(windows|linux|macos)-(x86_64|aarch64)", key) or not os.path.isfile(path):
            sys.exit(f"bad --native {item!r}")
        natives[key] = path
    if not natives:
        sys.exit("give at least one --native")
    javadoc = args.javadoc or (os.path.join(os.environ["JAVA_HOME"], "bin", "javadoc") if os.environ.get("JAVA_HOME") else shutil.which("javadoc"))
    if not javadoc or not (os.path.isfile(javadoc) or os.path.isfile(javadoc + ".exe") or shutil.which(javadoc)):
        sys.exit("javadoc not found (set JAVA_HOME or pass --javadoc)")

    out = os.path.abspath(args.out)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    main_src = os.path.join(ROOT, "bindings", "java", "src", "main", "java")
    ffm_src = os.path.join(ROOT, "bindings", "java", "src", "ffm", "java")
    lwjgl_src = os.path.join(ROOT, "bindings", "java", "src", "lwjgl", "java")

    artifacts = []
    jar = os.path.join(out, f"{ARTIFACT}-{version}.jar")
    repack(args.base_jar, jar, natives, version, "Tessera")
    artifacts.append(jar)
    srcs = os.path.join(out, f"{ARTIFACT}-{version}-sources.jar")
    sources_jar(srcs, version, [main_src, ffm_src], "Tessera")
    artifacts.append(srcs)
    doc = os.path.join(out, f"{ARTIFACT}-{version}-javadoc.jar")
    javadoc_jar(doc, version, main_src, os.path.join(main_src, *PACKAGE.split(".")), "Tessera " + version, javadoc)
    artifacts.append(doc)
    poms = {ARTIFACT: pom(ARTIFACT, version, "Tessera", "OpenGL / OpenGL ES context creation that coexists safely with other OpenGL users in the same process (JavaFX, AWT, GLFW).")}

    if args.lwjgl_jar:
        ljar = os.path.join(out, f"{LWJGL_ARTIFACT}-{version}.jar")
        entries = {}
        with zipfile.ZipFile(args.lwjgl_jar) as z:
            for n in z.namelist():
                if not n.endswith("/") and n != "META-INF/MANIFEST.MF":
                    entries[n] = z.read(n)
        entries.update(read_license_files())
        with zipfile.ZipFile(ljar, "w") as zf:
            zip_write(zf, "META-INF/MANIFEST.MF", manifest(version, "Tessera LWJGL adapter", [f"Automatic-Module-Name: {PACKAGE}.lwjgl"]))
            for n in sorted(entries):
                zip_write(zf, n, entries[n])
        artifacts.append(ljar)
        lsrc = os.path.join(out, f"{LWJGL_ARTIFACT}-{version}-sources.jar")
        sources_jar(lsrc, version, [lwjgl_src], "Tessera LWJGL adapter")
        artifacts.append(lsrc)
        ldoc = os.path.join(out, f"{LWJGL_ARTIFACT}-{version}-javadoc.jar")
        classpath = [jar] + ([c for c in args.lwjgl_classpath.split(os.pathsep) if c] if args.lwjgl_classpath else [])
        javadoc_jar(ldoc, version, lwjgl_src, os.path.join(lwjgl_src, *PACKAGE.split(".")), "Tessera LWJGL adapter " + version, javadoc, classpath)
        artifacts.append(ldoc)
        poms[LWJGL_ARTIFACT] = pom(LWJGL_ARTIFACT, version, "Tessera LWJGL adapter",
                                   "Connects LWJGL's OpenGL bindings to Tessera contexts (a FunctionProvider and per-thread capabilities).",
                                   deps=[(GROUP, ARTIFACT, version, "compile"), ("org.lwjgl", "lwjgl-opengl", LWJGL_VERSION, "provided")])
    for artifact, text in poms.items():
        path = os.path.join(out, f"{artifact}-{version}.pom")
        open(path, "w", encoding="utf-8", newline="\n").write(text)
        artifacts.append(path)
    cdx = os.path.join(out, f"{ARTIFACT}-{version}.cdx.json")
    sbom(cdx, version, [a for a in artifacts if a.endswith(".jar")])
    artifacts.append(cdx)

    # Maven repository layout + checksums (+ signatures)
    maven = os.path.join(out, "maven")
    for a in artifacts:
        if a.endswith(".cdx.json"):
            continue
        base = os.path.basename(a)
        artifact = LWJGL_ARTIFACT if base.startswith(LWJGL_ARTIFACT + "-") else ARTIFACT
        d = os.path.join(maven, *GROUP.split("."), artifact, version)
        os.makedirs(d, exist_ok=True)
        dest = os.path.join(d, base)
        shutil.copyfile(a, dest)
        for algo, ext in (("md5", "md5"), ("sha1", "sha1"), ("sha256", "sha256")):
            open(dest + "." + ext, "w").write(sha(dest, algo) + "\n")
        if args.sign:
            subprocess.run(["gpg", "--batch", "--yes", "--armor", "--detach-sign", dest], check=True)

    with open(os.path.join(out, "SHA256SUMS"), "w", newline="\n") as f:
        for a in sorted(artifacts):
            f.write(f"{sha(a, 'sha256')}  {os.path.basename(a)}\n")
    print(f"assembled {len(artifacts)} artifacts for {version} in {out}")
    for a in sorted(artifacts):
        print(f"  {os.path.basename(a)}  ({os.path.getsize(a) // 1024} KiB)")
    print("natives embedded:", ", ".join(sorted(natives)))


if __name__ == "__main__":
    main()
