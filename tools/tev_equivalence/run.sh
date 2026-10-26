#!/usr/bin/env bash
# run.sh
#
# Checks that webos_tev_specialize.c generates shaders that render exactly like
# the port's generic TEV shader: random variants (same const-block format as
# pc_gx_tev.c) are rendered both ways with Mesa's software OpenGL ES (llvmpipe,
# float render target) and compared pixel by pixel.
#
# template.frag is a variant the port generated on the TV; refresh it from a
# gpu_hot_*.frag dump (minus its first comment line) if default.frag changes.
#
# Requirements (Linux host): gcc, python3, Mesa EGL + GLES (libegl1-mesa-dev,
# libgles2-mesa-dev). Usage: tools/tev_equivalence/run.sh [variant count]

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="${here}/../../src/webos"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

python3 "${here}/gen_variants.py" "${here}/template.frag" "${work}/variants" "${1:-400}"
gcc -O1 -Wall -Wextra -I"${src}" -o "${work}/harness" "${here}/harness.c" "${src}/webos_tev_specialize.c" \
    -lEGL -lGLESv2 -lm
"${work}/harness" "${here}/template.frag" "${work}"/variants/*.frag
