#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Tessera contributors
"""Builds the tables of docs/compat.md from `tessera-diag --json` output, so a new machine is one command.

  tessera-diag --json > mybox.json
  python scripts/compat-from-diag.py mybox.json --label "Windows 11, AMD Radeon RX 7800" --suites "wgl, soak" > rows.md

Several files can be given (each with its own --label, in order); the output is two Markdown tables to paste into
docs/compat.md: the "Verified" rows (one per backend that exists) and the "Optional features, by driver" table (one column per
backend). Only what the diag run actually did is reported; a case that was refused shows the driver's message.
The tool never invents a result: a backend that was not available is listed as such and gets no feature column.

  --suites  free text for the "Suites run" cell (the diag run itself is always named)
  --check   exit 1 if any file does not look like tessera-diag output (used by CI on the stored samples)
"""
import argparse
import json
import re
import sys

FEATURES = [
    # (row title, how to compute it from the cases/backend)
    ("Highest GL core profile", "core_gl"),
    ("OpenGL ES contexts", "es"),
    ("Debug context", ["core 3.3 + debug"]),
    ("Forward-compatible", ["core 3.3 + forward-compatible"]),
    ("Robust access / reset notification", ["core 3.3 + robust access", "core 3.3 + robust + reset notification"]),
    ("`KHR_no_error`", ["core 3.3 + no-error"]),
    ("Release-flush control", ["core 3.3 + release-flush"]),
    ("4x / 8x MSAA", ["core 3.3, 4x MSAA", "core 3.3, 8x MSAA"]),
    ("sRGB framebuffer", ["core 3.3, sRGB framebuffer"]),
    ("Float colour buffer", ["core 3.3, float colour buffer"]),
    ("Stereo", ["core 3.3, stereo"]),
    ("Double-buffered", ["core 3.3, double-buffered"]),
    ("Pbuffer / no-drawable (FBO only)", ["core 3.3, pbuffer surface", "core 3.3, no surface (FBO only)"]),
    ("Swap control (window)", "swap"),
]


def cell_for(case):
    if case is None:
        return "not run"
    if case["result"] == "success":
        return "ok"
    msg = (case.get("message") or case["result"]).strip().rstrip(".")
    return "**refused** (%s)" % (msg[:70] + ("..." if len(msg) > 70 else ""))


def version_key(v):
    return tuple(int(x) for x in re.findall(r"\d+", v))


def feature(backend, how):
    cases = {c["case"]: c for c in backend.get("cases", [])}
    if how == "core_gl":
        best = None
        for name, c in cases.items():
            m = re.match(r"^OpenGL (\d+\.\d+) core$", name)
            if m and c["result"] == "success" and (best is None or version_key(m.group(1)) > version_key(best)):
                best = m.group(1)
        return best or "none"
    if how == "es":
        best = None
        for name, c in cases.items():
            m = re.match(r"^OpenGL ES (\d+\.\d+)$", name)
            if m and c["result"] == "success" and (best is None or version_key(m.group(1)) > version_key(best)):
                best = m.group(1)
        return ("yes (%s)" % best) if best else "no"
    if how == "swap":
        s = backend.get("swapControl")
        if not s:
            return "not reported"
        parts = [n for n, k in (("control", "control"), ("adaptive", "adaptive"), ("read-back", "readback")) if s.get(k)]
        return " + ".join(parts) if parts else "none"
    cells = [cell_for(cases.get(n)) for n in how]
    # two identical refusals are said once; "ok / ok" keeps its shape (it matches the hand-written table)
    return cells[0] if len(set(cells)) == 1 and cells[0] != "ok" else " / ".join(cells)


def load(path):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    if not isinstance(d, dict) or "tessera" not in d or "backends" not in d:
        raise SystemExit("%s does not look like tessera-diag --json output" % path)
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--label", action="append", default=[], help="platform description for each file, in order")
    ap.add_argument("--suites", action="append", default=[], help="'Suites run' text for each file, in order")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    docs = [load(p) for p in args.files]
    if args.check:
        print("ok: %d file(s)" % len(docs))
        return 0

    rows, columns = [], []
    for i, d in enumerate(docs):
        label = args.label[i] if i < len(args.label) else "%s (fill in the machine)" % d.get("platform", "?")
        suites = args.suites[i] if i < len(args.suites) else ""
        for b in d["backends"]:
            if not b.get("available"):
                continue
            cases = b.get("cases", [])
            granted = sum(1 for c in cases if c["result"] == "success")
            gpu = "%s, %s (%s)" % (b.get("renderer", "?"), b.get("version", "?"), b.get("vendor", "?"))
            run = "`tessera-diag` (%d of %d strict cases granted)" % (granted, len(cases))
            if suites:
                run = suites + "; " + run
            rows.append("| %s | %s | %s | %s | pass (diag %s, ABI %s) |" % (
                label, b["backend"].upper(), gpu, run, d["tessera"], d.get("abi", "?")))
            columns.append((("%s, %s" % (b["backend"].upper(), b.get("renderer", "?"))), b))

    out = ["## Verified (rows)", "", "| Platform | Backend | GPU / driver | Suites run | Result |", "|---|---|---|---|---|"]
    out += rows or ["| (no backend was available) | | | | |"]
    out += ["", "## Optional features, by driver", ""]
    if columns:
        out.append("| Feature | " + " | ".join(c[0] for c in columns) + " |")
        out.append("|---|" + "---|" * len(columns))
        for title, how in FEATURES:
            out.append("| %s | " % title + " | ".join(feature(b, how) for _, b in columns) + " |")
    else:
        out.append("(no backend was available)")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
