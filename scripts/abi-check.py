#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Tessera contributors
"""Makes docs/abi-policy.md executable.

Reads include/tessera/tessera.h and compares its ABI with a stored baseline (docs/abi/baseline.json):

  * functions: every baseline function must still exist with the same signature (new ones are additions);
  * structs:   every non-reserved baseline field keeps its name, type and offset; the size never shrinks; the space that
               was reserved may be taken over by new fields (that is how the ABI grows);
  * macros:    every baseline #define keeps its value (new ones are additions);
  * library:   with --lib, the exported tessera* symbols of the built library are exactly the header's functions.

Usage:
  abi-check.py                      compare the header with the baseline            (exit 1 on a break)
  abi-check.py --lib <file>         also check the exports of a built library
  abi-check.py --update             write the baseline from the header (a deliberate act: part of a release/ABI bump)
  abi-check.py --dump               print what was extracted (debugging)
  abi-check.py --self-test          check the extractor and the comparison on small inputs

Struct layout is computed with the natural-alignment rules for the fixed-width types the header uses; test_abi.cpp pins the
same layouts with the compiler's own offsetof, so the two cannot drift apart silently.
"""
import argparse
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "tessera", "tessera.h")
BASELINE = os.path.join(ROOT, "docs", "abi", "baseline.json")

# size == alignment for every scalar the header uses
SCALARS = {
    "int8_t": 1, "uint8_t": 1, "char": 1,
    "int16_t": 2, "uint16_t": 2,
    "int32_t": 4, "uint32_t": 4, "float": 4, "TesseraResult": 4, "TesseraEnum": 4,
    "int64_t": 8, "uint64_t": 8, "double": 8, "TesseraHandle": 8, "TesseraContext": 8,
    "size_t": 8,  # the header does not use it in a struct; 64-bit baseline
}
POINTER = 8

IGNORED_MACROS = re.compile(r"^(TESSERA_H|TESSERA_API|TESSERA_CALL|TESSERA_VERSION_(MAJOR|MINOR|PATCH)|TESSERA_ABI_VERSION|"
                            r"TESSERA_EXPORT.*)$")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def squash(s):
    return re.sub(r"\s+", " ", s).strip()


def parse_functions(text):
    out = {}
    for m in re.finditer(r"TESSERA_API\s+(.*?)\bTESSERA_CALL\s+(\w+)\s*\((.*?)\)\s*;", text, flags=re.S):
        ret, name, params = squash(m.group(1)), m.group(2), squash(m.group(3))
        params = "void" if params == "" else re.sub(r"\s*\*\s*", "* ", params).replace("* ,", "*,")
        params = re.sub(r"\s+", " ", params).strip()
        out[name] = {"return": re.sub(r"\s*\*\s*", "*", ret), "params": params}
    return out


def parse_macros(text):
    out = {}
    text = re.sub(r"\\[ \t]*\r?\n", " ", text)  # line continuations
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(TESSERA_\w+)(?![\w(])[ \t]+(.+?)[ \t]*$", text, flags=re.M):
        name, value = m.group(1), squash(m.group(2))
        if IGNORED_MACROS.match(name):
            continue
        out[name] = value
    return out


def layout(name, body, known):
    """Returns {size, align, fields:[{name,type,offset,size}]} for a struct body (comments already removed)."""
    fields, offset, max_align = [], 0, 1
    for stmt in body.split(";"):
        stmt = squash(stmt)
        if not stmt:
            continue
        m = re.match(r"^(.*?)\s+(\w+(?:\s*\[\s*\w+\s*\])?(?:\s*,\s*\w+(?:\s*\[\s*\w+\s*\])?)*)$", stmt)
        if not m:
            raise SystemExit("cannot parse a field of %s: %r" % (name, stmt))
        ctype = m.group(1).replace(" *", "*").strip()
        for decl in m.group(2).split(","):
            decl = decl.strip()
            am = re.match(r"^(\w+)\s*(?:\[\s*(\w+)\s*\])?$", decl)
            fname, count = am.group(1), am.group(2)
            if ctype.endswith("*"):
                scalar = POINTER
            elif ctype in SCALARS:
                scalar = SCALARS[ctype]
            elif ctype in known:  # a struct declared earlier: its own size and alignment
                scalar = known[ctype]["align"]
            else:
                raise SystemExit("unknown type %r in %s.%s (teach scripts/abi-check.py its size)" % (ctype, name, fname))
            n = int(count, 0) if count else 1
            each = known[ctype]["size"] if ctype in known else scalar
            offset = (offset + scalar - 1) // scalar * scalar
            fields.append({"name": fname, "type": ctype + ("[%d]" % n if count else ""), "offset": offset,
                           "size": each * n})
            offset += each * n
            max_align = max(max_align, scalar)
    size = (offset + max_align - 1) // max_align * max_align
    return {"size": size, "align": max_align, "fields": fields}


def parse_structs(text):
    out = {}
    for m in re.finditer(r"typedef\s+struct\s+\w+\s*\{(.*?)\}\s*(\w+)\s*;", text, flags=re.S):
        out[m.group(2)] = layout(m.group(2), m.group(1), out)
    return out


def extract(path=HEADER):
    with open(path, encoding="utf-8") as f:
        return extract_text(f.read())


def extract_text(text):
    text = strip_comments(text)
    # the static inline helpers contain braces and are not ABI; drop them before looking for structs/functions
    text = re.sub(r"static\s+inline\s+[^{;]*\{.*?\n\}", " ", text, flags=re.S)
    return {"functions": parse_functions(text), "structs": parse_structs(text), "macros": parse_macros(text)}


def is_reserved(field_name):
    return field_name.startswith("reserved")


def compare(base, cur):
    """Returns (breaks, additions)."""
    breaks, adds = [], []
    for name, sig in base["functions"].items():
        if name not in cur["functions"]:
            breaks.append("function removed: %s" % name)
        elif cur["functions"][name] != sig:
            breaks.append("function signature changed: %s\n    was: %s %s(%s)\n    now: %s %s(%s)" % (
                name, sig["return"], name, sig["params"], cur["functions"][name]["return"], name,
                cur["functions"][name]["params"]))
    adds += ["function added: %s" % n for n in cur["functions"] if n not in base["functions"]]

    for sname, sbase in base["structs"].items():
        scur = cur["structs"].get(sname)
        if scur is None:
            breaks.append("struct removed: %s" % sname)
            continue
        if scur["size"] < sbase["size"]:
            breaks.append("struct shrank: %s (%d -> %d bytes)" % (sname, sbase["size"], scur["size"]))
        curf = {f["name"]: f for f in scur["fields"]}
        for f in sbase["fields"]:
            if is_reserved(f["name"]):
                continue
            c = curf.get(f["name"])
            if c is None:
                breaks.append("struct field removed: %s.%s" % (sname, f["name"]))
            elif (c["type"], c["offset"]) != (f["type"], f["offset"]):
                breaks.append("struct field changed: %s.%s was %s at %d, now %s at %d" % (
                    sname, f["name"], f["type"], f["offset"], c["type"], c["offset"]))
        # Taken-over reserved space must stay inside the old reserved bytes: nothing may be added *between* old fields.
        old_reserved = [(f["offset"], f["offset"] + f["size"]) for f in sbase["fields"] if is_reserved(f["name"])]
        old_names = {f["name"] for f in sbase["fields"]}
        for f in scur["fields"]:
            if f["name"] in old_names:
                continue
            inside = any(lo <= f["offset"] and f["offset"] + f["size"] <= hi for lo, hi in old_reserved)
            beyond = f["offset"] >= sbase["size"]
            if not (inside or beyond):
                breaks.append("struct field %s.%s (offset %d) is neither in the old reserved space nor appended" % (
                    sname, f["name"], f["offset"]))
            else:
                adds.append("struct field added: %s.%s (%s at %d)" % (sname, f["name"], f["type"], f["offset"]))
    adds += ["struct added: %s" % n for n in cur["structs"] if n not in base["structs"]]

    for name, value in base["macros"].items():
        if name not in cur["macros"]:
            breaks.append("macro removed: %s" % name)
        elif cur["macros"][name] != value:
            breaks.append("macro value changed: %s was %s, now %s" % (name, value, cur["macros"][name]))
    adds += ["macro added: %s" % n for n in cur["macros"] if n not in base["macros"]]
    return breaks, adds


def pe_exports(path):
    """Export-table names of a PE file, read directly (no objdump/dumpbin needed, so it works in any ctest environment)."""
    import struct
    with open(path, "rb") as f:
        d = f.read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("%s is not a PE file" % path)
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt_size = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)  # data directories
    export_rva, _ = struct.unpack_from("<II", d, dd)
    secs = [struct.unpack_from("<8sIIII", d, opt + opt_size + 40 * i) for i in range(nsec)]

    def off(rva):
        for _, vsize, va, rsize, raw in secs:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise SystemExit("bad RVA in %s" % path)

    if export_rva == 0:
        return set()
    e = off(export_rva)
    count, _, names_rva = struct.unpack_from("<III", d, e + 24)
    names = set()
    for i in range(count):
        n = off(struct.unpack_from("<I", d, off(names_rva) + 4 * i)[0])
        names.add(d[n:d.index(b"\0", n)].decode("ascii"))
    return {n for n in names if n.startswith("tessera")}


def library_exports(lib):
    """Exported tessera* symbols of a shared library (PE, ELF or Mach-O), using the platform's own tools."""
    names = set()
    if lib.lower().endswith(".dll"):
        return pe_exports(lib)
    flags = ["-gU"] if sys.platform == "darwin" else ["-D", "--defined-only"]
    out = subprocess.run(["nm"] + flags + [lib], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-2] in ("T", "t", "W", "D", "B", "R", "S", "V"):
            sym = parts[-1].lstrip("_") if sys.platform == "darwin" else parts[-1]
            if sym.startswith("tessera"):
                names.add(sym)
    return names


def check_library(lib, cur):
    exported, declared = library_exports(lib), set(cur["functions"])
    problems = ["exported but not declared in the header: %s" % n for n in sorted(exported - declared)]
    problems += ["declared but not exported: %s" % n for n in sorted(declared - exported)]
    return problems


def self_test():
    h = """
    typedef struct A { uint32_t structSize; uint32_t kind; /* c */ uint64_t big; int32_t a, b; uint64_t reserved[2]; } A;
    TESSERA_API TesseraResult TESSERA_CALL f(const A* a,
                                              int32_t* out);
    #define TESSERA_X 3u
    """
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".h", delete=False) as t:
        t.write(h)
    try:
        base = extract(t.name)
    finally:
        os.unlink(t.name)
    a = base["structs"]["A"]
    assert [f["offset"] for f in a["fields"]] == [0, 4, 8, 16, 20, 24, ] and a["size"] == 40, a
    assert base["functions"]["f"]["params"] == "const A* a, int32_t* out", base["functions"]["f"]
    assert base["macros"] == {"TESSERA_X": "3u"}, base["macros"]

    import copy
    ok = copy.deepcopy(base)  # take 8 bytes of the reserved space and append a function and a macro
    ok["structs"]["A"]["fields"][5:] = [{"name": "newField", "type": "uint64_t", "offset": 24, "size": 8},
                                        {"name": "reserved", "type": "uint64_t[1]", "offset": 32, "size": 8}]
    ok["functions"]["g"] = {"return": "void", "params": "void"}
    ok["macros"]["TESSERA_Y"] = "4"
    breaks, adds = compare(base, ok)
    assert not breaks and len(adds) == 3, (breaks, adds)

    def broken(mutate):
        c = copy.deepcopy(base)
        mutate(c)
        return compare(base, c)[0]

    assert broken(lambda c: c["functions"].pop("f"))
    assert broken(lambda c: c["functions"]["f"].update(params="int32_t x"))
    assert broken(lambda c: c["structs"]["A"].update(size=32))
    assert broken(lambda c: c["structs"]["A"]["fields"][2].update(offset=12))
    assert broken(lambda c: c["structs"]["A"]["fields"].pop(1))
    assert broken(lambda c: c["structs"]["A"]["fields"].insert(
        1, {"name": "mid", "type": "uint32_t", "offset": 4, "size": 4}))  # in the middle of live fields
    assert broken(lambda c: c["macros"].update(TESSERA_X="4u"))
    assert broken(lambda c: c["macros"].pop("TESSERA_X"))
    cont = extract_text("#define TESSERA_L 1 | \\\n    2\n")
    assert cont["macros"] == {"TESSERA_L": "1 | 2"}, cont["macros"]
    print("abi-check self-test: ok")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lib", help="a built shared library whose exports are compared with the header")
    ap.add_argument("--update", action="store_true", help="rewrite the baseline from the header")
    ap.add_argument("--dump", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--header", default=HEADER)
    ap.add_argument("--baseline", default=BASELINE)
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return 0
    cur = extract(args.header)
    if args.dump:
        print(json.dumps(cur, indent=1))
        return 0
    if args.update:
        os.makedirs(os.path.dirname(args.baseline), exist_ok=True)
        with open(args.baseline, "w", encoding="utf-8", newline="\n") as f:
            json.dump(cur, f, indent=1, sort_keys=True)
            f.write("\n")
        print("baseline written: %s (%d functions, %d structs, %d macros)" % (
            args.baseline, len(cur["functions"]), len(cur["structs"]), len(cur["macros"])))
        return 0

    with open(args.baseline, encoding="utf-8") as f:
        base = json.load(f)
    breaks, adds = compare(base, cur)
    problems = check_library(args.lib, cur) if args.lib else []
    for a in adds:
        print("  addition (record it with --update at the next ABI bump): " + a)
    for b in breaks + problems:
        print("ABI BREAK: " + b)
    if breaks or problems:
        print("\nThe ABI is not append-only (docs/abi-policy.md). If the break is deliberate (pre-1.0 only), bump the version, "
              "run `python scripts/abi-check.py --update` and explain it in the commit.")
        return 1
    print("abi-check: ok (%d functions, %d structs, %d macros%s)" % (
        len(cur["functions"]), len(cur["structs"]), len(cur["macros"]),
        ", exports match" if args.lib else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
