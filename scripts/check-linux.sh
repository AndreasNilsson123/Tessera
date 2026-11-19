#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Tessera contributors
# Reproduce the Linux CI jobs locally (works in WSL, including with the repo on /mnt/c).
#
#   scripts/check-linux.sh                 # the "ci" preset
#   scripts/check-linux.sh asan-ubsan tsan # several presets, in order
#
# The tree is copied to a scratch directory first: building on /mnt/c is slow, and a build tree created
# from Linux must not end up inside a Windows checkout. Override the compiler with CC/CXX as usual.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="${TESSERA_CHECK_DIR:-$HOME/.cache/tessera-check}"
presets=("$@")
[ ${#presets[@]} -gt 0 ] || presets=(ci)

rm -rf "$work"
mkdir -p "$work"
tar -C "$root" --exclude=./build --exclude='./cmake-build-*' --exclude=./.idea --exclude=./.git -cf - . |
    tar -C "$work" -xf -
cd "$work"

for preset in "${presets[@]}"; do
    echo "=== preset: $preset"
    cmake --preset "$preset" > "configure-$preset.log" 2>&1 || { tail -30 "configure-$preset.log"; exit 1; }
    cmake --build --preset "$preset" > "build-$preset.log" 2>&1 ||
        { grep -E "error|Error" "build-$preset.log" | sort -u | head -40; exit 1; }

    run=()
    if [ "$preset" = "tsan" ] && command -v setarch > /dev/null; then
        # Older TSan runtimes abort under high-entropy ASLR (default on newer kernels, e.g. WSL2).
        run=(setarch "$(uname -m)" -R)
    fi
    "${run[@]}" ctest --preset "$preset"
done
