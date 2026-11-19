# Releasing

Identity: Maven `io.github.andreasnilsson123:tessera` (+ `tessera-lwjgl`), Java package `io.github.andreasnilsson123.tessera`,
license Apache-2.0, copyright "Tessera contributors". Pre-1.0: the ABI may change between minor versions
(`abi-policy.md`); do not call anything 1.0 until the decisions in ROADMAP 10.5 are made.

## What a release contains

| Artifact | Built by | Verified |
|---|---|---|
| `tessera-<v>-<OS>-<arch>.zip/.tar.gz`: library, headers, CMake package, `tessera-diag`, licenses | `cpack` per platform | Windows package contents inspected; Linux/macOS not run |
| `tessera-<v>.jar`: one multi-release jar, natives for Windows, Linux (x86_64) and macOS (aarch64), `module-info`, license files | `scripts/assemble-release.py` | **Run on a Windows + Linux pair**: the assembled jar runs `SelfTest` on Windows (JNI and FFM) and on Linux (JNI) from its embedded natives, and as a named module; output is byte-for-byte reproducible. The macOS native is taken from CI and has never been run |
| `-sources.jar`, `-javadoc.jar`, `tessera-lwjgl-<v>.jar` (+ both), POMs | same | built and inspected |
| `tessera-<v>.cdx.json` (CycloneDX SBOM: no third-party code is embedded) | same | generated |
| `SHA256SUMS`, `maven/` bundle with `.md5/.sha1/.sha256` (and `.asc` when signing) | same | generated; **signing and Central upload untested** |
| Conan recipe `packaging/conan` | `conan create` | **verified** (build + consumer test on Linux) |
| vcpkg port `packaging/vcpkg/ports/tessera` | - | **untested** (vcpkg could not be run here) |

## Cutting a release

1. Bump `TESSERA_VERSION_*` in `include/tessera/tessera.h` (the Maven version, jar manifest, CMake package and Conan recipe all
   read it) and `packaging/vcpkg/ports/tessera/vcpkg.json`; update `ROADMAP.md` and notes.
2. `git tag v<version> && git push --tags`. The `Release` workflow checks the tag against the header, builds every platform,
   assembles the artifacts and creates a **draft** GitHub release. Review it, then publish by hand.
3. A dry run without a tag: *Actions -> Release -> Run workflow* (builds and assembles, no release).
4. Local assembly (what the workflow runs), given each platform's `tessera.jar` and native library:

   ```
   python scripts/assemble-release.py --base-jar linux/tessera.jar --native linux-x86_64=linux/libtessera.so \
       --native windows-x86_64=win/tessera.dll --native macos-aarch64=mac/libtessera.dylib \
       --lwjgl-jar linux/tessera-lwjgl.jar --lwjgl-classpath lwjgl.jar:lwjgl-opengl.jar --out dist
   ```

## Publishing to Maven Central (one-time setup, then manual)

1. Sign in to the Central Portal (central.sonatype.com) with GitHub and **verify the namespace** `io.github.andreasnilsson123`
   (it asks you to create a temporary public repository with a given name; no domain needed).
2. Create a user token (Portal -> Account -> Generate User Token) and a GPG key; publish the public key to a key server.
3. Add repository secrets: `GPG_PRIVATE_KEY` (armored), `GPG_PASSPHRASE`, `CENTRAL_USERNAME`, `CENTRAL_PASSWORD`.
4. Run the workflow with `publish = true`. It uploads the signed bundle as *user managed*: nothing is released until you press
   **Publish** in the Portal. Central versions are immutable.

This path has not been exercised; the first upload will probably need adjustments.

## Conan and vcpkg

```
conan create packaging/conan --build=missing         # then: requires = "tessera/0.7.0"
TESSERA_LOCAL_SOURCE=$PWD vcpkg install tessera --overlay-ports=packaging/vcpkg/ports   # untested
```

For a real vcpkg port, tag a release, replace `SHA512 0` in the portfile (vcpkg prints the expected value) and submit it to the
vcpkg registry; for Conan Center, submit the recipe to `conan-center-index` once the repository is public and tagged.

## Reproducibility and integrity

Assembly uses fixed timestamps and sorted entries, javadoc runs with `-notimestamp`: the same inputs give the same bytes
(checked: two runs, identical `SHA256SUMS`). The native libraries themselves are not bit-reproducible across machines.
Verify downloads with `SHA256SUMS`; signed artifacts also carry `.asc` files.
