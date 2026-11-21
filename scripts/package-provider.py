#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Tessera contributors
"""Packs an optional GL provider (an ANGLE or Mesa build) as a SEPARATE release artifact, with what the SBOM needs (ADR 0015).

Tessera does not ship ANGLE or Mesa in its library or its jar. A provider is a folder of someone else's binaries that a user
can point Tessera at (TESSERA_EGL_LIBRARY=<absolute path of libEGL>); this script turns such a folder into

    tessera-<tessera version>-<name>-<os>-<arch>.zip      the files, their licence texts, PROVIDER.json and a README
    tessera-<tessera version>-<name>-<os>-<arch>.provider.json   the SBOM fragment assemble-release.py merges into the CycloneDX file

so the release says exactly which third-party binaries it offers, from where, under which licence, with which hashes.
It refuses a folder without a licence file: shipping binaries without their licence is not a thing this project does.

    python scripts/package-provider.py --name angle --upstream-version 2.1.0 --upstream-url https://chromium.googlesource.com/angle/angle \\
        --license BSD-3-Clause --os windows --arch x86_64 --dir angle-build --entry libEGL.dll --out dist
"""
import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "tessera", "tessera.h")
LICENSE_NAMES = re.compile(r"^(licen[cs]e|copying|notice|third_party_notices|licenses\.)", re.I)
FIXED_TIME = (2026, 1, 1, 0, 0, 0)


def tessera_version():
    t = open(HEADER, encoding="utf-8").read()
    return ".".join(re.search(r"#define TESSERA_VERSION_%s (\d+)" % p, t).group(1) for p in ("MAJOR", "MINOR", "PATCH"))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def list_files(folder):
    out = []
    for dirpath, _, names in os.walk(folder):
        for n in names:
            full = os.path.join(dirpath, n)
            out.append((os.path.relpath(full, folder).replace(os.sep, "/"), full))
    return sorted(out)


def zip_add(zf, name, data):
    info = zipfile.ZipInfo(name, FIXED_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    zf.writestr(info, data)


def package(args, version):
    files = list_files(args.dir)
    if not files:
        raise SystemExit("%s is empty" % args.dir)
    names = [n for n, _ in files]
    if args.entry not in names:
        raise SystemExit("the entry library %s is not in %s (it is what TESSERA_EGL_LIBRARY will point to)" % (args.entry, args.dir))
    if not any(LICENSE_NAMES.match(os.path.basename(n)) for n in names):
        raise SystemExit("no licence file in %s (LICENSE*, COPYING*, NOTICE*, THIRD_PARTY_NOTICES*): refusing to package binaries "
                         "without their licence" % args.dir)

    base = "tessera-%s-%s-%s-%s" % (version, args.name, args.os, args.arch)
    provider = {
        "name": args.name, "version": args.upstream_version, "upstream": args.upstream_url, "license": args.license,
        "os": args.os, "arch": args.arch, "entry": args.entry, "packagedFor": "tessera " + version,
        "files": [{"path": n, "sha256": sha256(p)} for n, p in files],
    }
    readme = (
        "Optional GL provider for Tessera %s: %s %s (%s), %s, %s-%s.\n\n"
        "These are third-party binaries, not part of Tessera. To use them, give Tessera the ABSOLUTE path of the entry library:\n\n"
        "    TESSERA_EGL_LIBRARY=<this folder>/%s\n\n"
        "and choose the backend: TESSERA_BACKEND=egl (the provider's GPU path) or TESSERA_BACKEND=software (its CPU renderer).\n"
        "Tessera loads the entry library by that path and lets it find its own dependencies in its own folder (and System32 on\n"
        "Windows), never in the current directory or PATH. PROVIDER.json lists every file with its SHA-256.\n"
        % (version, args.name, args.upstream_version, args.upstream_url, args.license, args.os, args.arch, args.entry))

    os.makedirs(args.out, exist_ok=True)
    zpath = os.path.join(args.out, base + ".zip")
    with zipfile.ZipFile(zpath, "w") as zf:
        for n, p in files:
            with open(p, "rb") as f:
                zip_add(zf, "%s/%s" % (args.name, n), f.read())
        zip_add(zf, "%s/PROVIDER.json" % args.name, (json.dumps(provider, indent=2, sort_keys=True) + "\n").encode())
        zip_add(zf, "%s/README.txt" % args.name, readme.encode())

    # The CycloneDX fragment: one library component for the provider, each binary a nested file component with its hash.
    ref = "pkg:generic/%s@%s?os=%s&arch=%s" % (args.name, args.upstream_version, args.os, args.arch)
    fragment = {
        "type": "library", "bom-ref": ref, "name": args.name, "version": args.upstream_version, "scope": "optional",
        "description": "Optional GL provider packaged for Tessera; not embedded in the Tessera library or jar.",
        "purl": ref, "licenses": [{"license": {"id": args.license}}],
        "externalReferences": [{"type": "vcs", "url": args.upstream_url},
                               {"type": "distribution", "url": base + ".zip"}],
        "hashes": [{"alg": "SHA-256", "content": sha256(zpath)}],
        "components": [{"type": "file", "name": f["path"], "hashes": [{"alg": "SHA-256", "content": f["sha256"]}]}
                       for f in provider["files"]],
    }
    fpath = os.path.join(args.out, base + ".provider.json")
    with open(fpath, "w", encoding="utf-8", newline="\n") as f:
        json.dump(fragment, f, indent=2, sort_keys=True)
        f.write("\n")
    print("wrote", zpath)
    print("wrote", fpath)
    return zpath, fpath


def self_test():
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "in")
        os.makedirs(src)
        for n, data in (("libEGL.dll", b"egl"), ("libGLESv2.dll", b"gles"), ("LICENSE", b"BSD")):
            open(os.path.join(src, n), "wb").write(data)

        def run(**over):
            a = argparse.Namespace(name="angle", upstream_version="2.1.0", upstream_url="https://example.invalid/angle",
                                   license="BSD-3-Clause", os="windows", arch="x86_64", dir=src, entry="libEGL.dll",
                                   out=os.path.join(d, "out"))
            for k, v in over.items():
                setattr(a, k, v)
            return package(a, "9.9.9")

        z1, f1 = run()
        b1 = open(z1, "rb").read()
        z2, _ = run()
        assert open(z2, "rb").read() == b1, "the zip must be reproducible"
        frag = json.load(open(f1))
        assert frag["scope"] == "optional" and len(frag["components"]) == 3 and frag["licenses"][0]["license"]["id"] == "BSD-3-Clause"
        with zipfile.ZipFile(z1) as zf:
            assert "angle/PROVIDER.json" in zf.namelist() and "angle/README.txt" in zf.namelist()
        for bad in ({"entry": "missing.dll"},):
            try:
                run(**bad)
            except SystemExit:
                pass
            else:
                raise AssertionError("a missing entry library must be refused")
        os.remove(os.path.join(src, "LICENSE"))
        try:
            run()
        except SystemExit:
            pass
        else:
            raise AssertionError("a folder without a licence must be refused")
    print("package-provider self-test: ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name")
    ap.add_argument("--upstream-version")
    ap.add_argument("--upstream-url")
    ap.add_argument("--license", help="SPDX id, e.g. BSD-3-Clause (ANGLE), MIT (Mesa)")
    ap.add_argument("--os")
    ap.add_argument("--arch")
    ap.add_argument("--dir")
    ap.add_argument("--entry", help="the libEGL a user points TESSERA_EGL_LIBRARY at, relative to --dir")
    ap.add_argument("--out", default="dist")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return 0
    for need in ("name", "upstream_version", "upstream_url", "license", "os", "arch", "dir", "entry"):
        if not getattr(args, need):
            ap.error("--%s is required" % need.replace("_", "-"))
    package(args, tessera_version())
    return 0


if __name__ == "__main__":
    sys.exit(main())
