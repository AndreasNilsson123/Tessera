#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Tessera contributors
"""Release-candidate and freeze checks (docs/freeze.md). Everything here is derived from files in the checkout.

  release-check.py                    consistency: the version lives in one place and every copy agrees   (runs in ctest)
  release-check.py --tag v0.13.0      the above, and the tag must equal the header version
  release-check.py --rc               release-candidate gate: consistency + ABI baseline up to date + clean git tree +
                                      the compatibility file describes this ABI
  release-check.py --freeze           what must be true before 0.x becomes 1.0.0 (see docs/freeze.md): everything in --rc, the
                                      header says 1.0.0, no open item in ROADMAP 10.1-10.5, the baseline equals the header
  release-check.py --set-version X.Y.Z   write the version into the header and the vcpkg manifest (the only two copies)
  release-check.py --self-test

Exit status 0 = every check passed; 1 = at least one failed (each failure is printed with what to do).
"""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "tessera", "tessera.h")
VCPKG = os.path.join(ROOT, "packaging", "vcpkg", "ports", "tessera", "vcpkg.json")
JAVA_NATIVES = os.path.join(ROOT, "bindings", "java", "src", "main", "java", "io", "github", "andreasnilsson123",
                            "tessera", "internal", "Natives.java")
COMPAT = os.path.join(ROOT, "docs", "compat.md")
ROADMAP = os.path.join(ROOT, "ROADMAP.md")


def read(path):
    with open(path, encoding="utf-8", newline="") as f:  # keep the file's own line endings when it is written back
        return f.read()


def header_version(text=None):
    text = read(HEADER) if text is None else text
    parts = [int(re.search(r"#define TESSERA_VERSION_%s (\d+)" % p, text).group(1)) for p in ("MAJOR", "MINOR", "PATCH")]
    return tuple(parts)


def header_abi(text=None):
    text = read(HEADER) if text is None else text
    return int(re.search(r"#define TESSERA_ABI_VERSION (\d+)", text).group(1))


def fmt(v):
    return "%d.%d.%d" % v


def abi_module():
    spec = importlib.util.spec_from_file_location("abi_check", os.path.join(ROOT, "scripts", "abi-check.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ---------------------------------------------------------------------------------------------------------- checks
# Each check returns a list of problems (empty = passed).

def check_versions(tag=None):
    problems, v = [], header_version()
    vc = json.loads(read(VCPKG))["version"]
    if vc != fmt(v):
        problems.append("vcpkg manifest says %s, the header says %s (run: release-check.py --set-version %s)" % (vc, fmt(v), fmt(v)))
    m = re.search(r"EXPECTED_ABI = (\d+);", read(JAVA_NATIVES))
    if not m or int(m.group(1)) != header_abi():
        problems.append("Java Natives.EXPECTED_ABI (%s) differs from TESSERA_ABI_VERSION (%d)" % (m and m.group(1), header_abi()))
    if tag is not None and tag != "v" + fmt(v):
        problems.append("tag %s does not match the header (v%s)" % (tag, fmt(v)))
    return problems


def check_baseline(strict):
    """strict: the baseline must equal the header (no additions pending); otherwise additions are fine, breaks are not."""
    mod = abi_module()
    with open(mod.BASELINE, encoding="utf-8") as f:
        base = json.load(f)
    cur = mod.extract()
    breaks, adds = mod.compare(base, cur)
    problems = list(breaks)
    if strict and adds:
        problems.append("the ABI baseline is behind the header (%d addition(s), e.g. %s): run scripts/abi-check.py --update and "
                        "commit docs/abi/baseline.json" % (len(adds), adds[0]))
    return problems


def check_clean_tree():
    try:
        out = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return ["cannot run git"]
    return ["the working tree is not clean (%d path(s), e.g. %s)" % (len(out.splitlines()), out.splitlines()[0])] if out.strip() else []


def check_compat_current():
    v = header_version()
    m = re.search(r"Last updated at ABI (\d+)\.(\d+)\.(\d+)", read(COMPAT))
    if not m:
        return ["docs/compat.md has no 'Last updated at ABI X.Y.Z' line"]
    if tuple(int(x) for x in m.groups()) != v:
        return ["docs/compat.md was last updated at ABI %s.%s.%s, the header is %s: re-run the matrix "
                "(scripts/compat-from-diag.py) and update the line" % (*m.groups(), fmt(v))]
    return []


def open_items(text, sections):
    """Unticked '- [ ]' and half-done '- [~]' items inside the given ROADMAP subsections ('### 10.x' headings). The owner can
    accept a known gap by writing ACCEPTED-FOR-1.0 on its line: a deliberate, reviewable act in the diff."""
    found = []
    for sec in sections:
        m = re.search(r"^### %s\b.*?(?=^### |^## |\Z)" % re.escape(sec), text, flags=re.S | re.M)
        if not m:
            found.append("%s: section not found" % sec)
            continue
        for line in m.group(0).splitlines():
            if re.match(r"^\s*- \[[ ~]\]", line) and "ACCEPTED-FOR-1.0" not in line:
                found.append("%s: %s" % (sec, re.sub(r"\*\*|`", "", line.strip())[:110]))
    return found


def check_freeze_ready():
    problems = []
    v = header_version()
    if v != (1, 0, 0):
        problems.append("the header says %s; the freeze release is 1.0.0 (run: release-check.py --set-version 1.0.0)" % fmt(v))
    if "COMPATIBILITY SameMajorVersion" not in read(os.path.join(ROOT, "CMakeLists.txt")):
        problems.append("CMakeLists.txt: the package version file must say COMPATIBILITY SameMajorVersion from 1.0 on "
                        "(it says SameMinorVersion while the ABI may still change between minors)")
    problems += ["open in ROADMAP: " + i for i in open_items(read(ROADMAP), ["10.1", "10.2", "10.3", "10.4", "10.5"])]
    return problems


# ------------------------------------------------------------------------------------------------------- set-version

def set_version(new):
    m = re.match(r"^(\d+)\.(\d+)\.(\d+)$", new)
    if not m:
        raise SystemExit("version must be X.Y.Z")
    text = read(HEADER)
    for name, value in zip(("MAJOR", "MINOR", "PATCH"), m.groups()):
        text, n = re.subn(r"(#define TESSERA_VERSION_%s )\d+" % name, r"\g<1>" + value, text)
        assert n == 1
    with open(HEADER, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    vc = read(VCPKG)
    vc, n = re.subn(r'("version": ")[^"]+(")', r"\g<1>" + new + r"\g<2>", vc, count=1)
    assert n == 1
    with open(VCPKG, "w", encoding="utf-8", newline="") as f:
        f.write(vc)
    print("version set to %s in the header and the vcpkg manifest (Maven, CMake, Conan and the jar read the header)" % new)


def self_test():
    assert header_version("#define TESSERA_VERSION_MAJOR 1\n#define TESSERA_VERSION_MINOR 2\n#define TESSERA_VERSION_PATCH 3\n") == (1, 2, 3)
    assert fmt((0, 12, 0)) == "0.12.0"
    t = "## 10. x\n### 10.1 a\n- [x] done\n- [ ] open one\n- [~] half\n### 10.2 b\n- [x] fine\n## 11\n- [ ] not in range\n"
    assert open_items(t, ["10.1"]) == ["10.1: - [ ] open one", "10.1: - [~] half"], open_items(t, ["10.1"])
    assert open_items(t, ["10.2"]) == []
    assert open_items(t.replace("- [~] half", "- [~] half ACCEPTED-FOR-1.0"), ["10.1"]) == ["10.1: - [ ] open one"]
    assert open_items(t, ["10.9"]) == ["10.9: section not found"]
    print("release-check self-test: ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag")
    ap.add_argument("--rc", action="store_true")
    ap.add_argument("--freeze", action="store_true")
    ap.add_argument("--set-version")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return 0
    if args.set_version:
        set_version(args.set_version)
        return 0

    checks = [("versions agree", lambda: check_versions(args.tag)),
              ("ABI baseline (no break%s)" % ("; up to date" if (args.rc or args.freeze) else ""),
               lambda: check_baseline(args.rc or args.freeze))]
    if args.rc or args.freeze:
        checks += [("clean git tree", check_clean_tree), ("compat.md describes this ABI", check_compat_current)]
    if args.freeze:
        checks += [("ready to freeze (1.0.0, ROADMAP 10.x closed)", check_freeze_ready)]

    failed = 0
    for name, fn in checks:
        problems = fn()
        print("%s %s" % ("PASS" if not problems else "FAIL", name))
        for p in problems:
            print("       - " + p)
        failed += bool(problems)
    print("\nversion %s, ABI %d: %s" % (fmt(header_version()), header_abi(), "all checks passed" if not failed else
                                         "%d check(s) failed" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
