# Release candidates and the 1.0 freeze

Pre-1.0 the ABI may change between minor versions. From 1.0 it is append-only within the major version
(`abi-policy.md`). This page is the procedure for getting there, and for every release before it. The checks are code
(`scripts/release-check.py`, `scripts/abi-check.py`), so the checklist cannot drift from the repository.

## What is enforced automatically

| Check | Where | What it catches |
|---|---|---|
| `abi_check` (ctest, every CI run) | `scripts/abi-check.py` against `docs/abi/baseline.json` | a removed or changed function, a changed or moved struct field, a shrunk struct, a changed `#define` value, a library that exports something the header does not declare (or the reverse). Additions pass |
| `abi` (ctest) | `tests/test_abi.cpp` | struct sizes and the offsets the Java FFM binding hard-codes, with the compiler's own `offsetof` |
| `release_check` (ctest) | `scripts/release-check.py` | the version copies disagreeing (header, vcpkg manifest), the Java binding expecting another ABI version |
| `compat_generator` (ctest) | `scripts/compat-from-diag.py` | the table generator breaking on real `tessera-diag --json` output |
| `release.yml` | tag vs header | a tag that is not the header's version |

The baseline records what is in `tessera.h`. A struct may grow **only** by taking over bytes that were reserved
(`reserved*` fields) or by appending; a new field anywhere else is reported as a break. When an addition is intended, record it:
`python scripts/abi-check.py --update` and commit `docs/abi/baseline.json` with the change that adds it. Until 1.0 a deliberate
break is possible the same way (bump the version, update the baseline, say so in the commit); the check exists so that it is
never an accident. (Run against the project's own history, the check flags exactly one break across 0.2 to 0.12: the removal of
the `TESSERA_CONTEXT_*_INIT` macros during 0.3; every later version is a pure addition.)

## Release candidate (any release)

1. `python scripts/release-check.py --rc` passes: versions agree, no ABI break, the baseline is current, the tree is clean, and
   `docs/compat.md` says it was last updated at this version's ABI.
2. CI is green on the commit to be tagged (including the Windows, Linux and macOS jobs, `java-*`, `vcpkg`).
3. *Nightly* and a *Release* dry run (Actions, no tag) are green on that commit; for a minor release run *Long coexistence* once.
4. Regenerate the matrix on every machine you have (`tessera-diag --json`, then `scripts/compat-from-diag.py`), paste the
   tables into `docs/compat.md`, update its "Last updated at ABI" line. A machine that was not run stays "not run".
5. `ROADMAP.md`: every item you ticked has a test or measurement behind it, and every gap is written next to the item.
6. Tag `vX.Y.Z-rcN`? The release workflow accepts only `vX.Y.Z`; use a *Release* dry run for candidates and tag the final one.

## The freeze (0.x to 1.0.0)

Preconditions (the owner's decisions are in `decisions-1.0.md`, and each unchecked box is a reason not to freeze):

- `python scripts/release-check.py --freeze` passes. It requires, beyond `--rc`: the header says 1.0.0; **no open item** (`[ ]` or
  `[~]`) in ROADMAP 10.1 to 10.5, where a gap the owner accepts is marked `ACCEPTED-FOR-1.0` on its line (a reviewable edit);
  the CMake package version file uses `SameMajorVersion`.
- The baseline equals the header: `python scripts/abi-check.py` reports no pending additions after `--update`.
- Hardware gate from ROADMAP 10.5: at least AMD or Intel on Windows, and macOS on a real machine, are recorded in
  `docs/compat.md` (or explicitly accepted).

Procedure (one commit, reviewed, then the tag):

1. `python scripts/release-check.py --set-version 1.0.0` (writes the header and the vcpkg manifest; Maven, CMake, Conan and the
   jar read the header).
2. In `CMakeLists.txt` change `COMPATIBILITY SameMinorVersion` to `SameMajorVersion` (the package version file); the soname is
   already the major version.
3. `python scripts/abi-check.py --update`; review the diff of `docs/abi/baseline.json`: it is the frozen ABI.
4. In `abi-policy.md` replace "Before 1.0 the ABI may change between minor versions" by the 1.x promise; in the release notes
   in `release.yml` drop the "Pre-1.0" sentence; in `docs/releasing.md` and `README.md` the same.
5. `python scripts/release-check.py --freeze`, then CI, nightly, release dry run.
6. `git tag v1.0.0 && git push --tags`; review the draft release, publish by hand.
7. After the tag: any ABI change is an addition that passes `abi_check`; anything else needs a new major version (2.0), a new
   `TESSERA_ABI_VERSION` and a new soname. The Java binding's `EXPECTED_ABI` moves with `TESSERA_ABI_VERSION`.

## If a freeze turns out to be wrong

Append-only is the safety valve: a wrong or missing field is fixed by a new field and a new function, and the old one stays
(documented as deprecated, never removed in 1.x). That is why the structs carry `reserved` space and every new entry point has
been added next to the old one, never in place of it.
