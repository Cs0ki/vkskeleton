#!/usr/bin/env bash
# Build build/vkskeleton.so (32-bit, for Sven Co-op's 32-bit engine).
#
# Needs: gcc + binutils with -m32 support, python3, Vulkan and X11 headers
# (libvulkan-dev, libx11-dev), and the 32-bit runtime libraries libc6:i386
# and libx11-6:i386.  Optional: spirv-val (spirv-tools) to validate the
# generated shaders.  No shader compiler is needed: tools/gen_shaders.py
# emits the SPIR-V directly.
set -euo pipefail
cd "$(dirname "$0")"

mkdir -p build
python3 tools/gen_shaders.py src/shaders_spirv.h

if command -v spirv-val >/dev/null 2>&1; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    python3 - "$tmp" <<'PY'
import re, sys
from pathlib import Path
text = Path("src/shaders_spirv.h").read_text()
for name in ("vk_vertex_spirv", "vk_fragment_spirv", "vk_skin_vertex_spirv"):
    body = re.search(r"static const uint32_t " + name + r"\[\] = \{(.*?)\};",
                     text, re.S).group(1)
    words = [int(w, 16) for w in re.findall(r"0x[0-9a-fA-F]+", body)]
    Path(sys.argv[1], name + ".spv").write_bytes(
        b"".join(w.to_bytes(4, "little") for w in words))
PY
    spirv-val --target-env vulkan1.0 "$tmp/vk_vertex_spirv.spv"
    spirv-val --target-env vulkan1.0 "$tmp/vk_fragment_spirv.spv"
    spirv-val --target-env vulkan1.0 "$tmp/vk_skin_vertex_spirv.spv"
    echo "SPIR-V validation: OK"
else
    echo "SPIR-V validation: skipped (spirv-val not installed)"
fi

# src/compat: minimal 32-bit glibc header shims, so the 64-bit host headers
# can be used for the -m32 build without installing gcc-multilib headers.
# SSE2 instead of the i386 default x87 FPU (perf: x87 float copies dominated
# the per-vertex path).
gcc -m32 -O3 -msse2 -mfpmath=sse -fPIC -Wall -Wno-missing-braces \
    -Isrc -Isrc/compat -I/usr/include/x86_64-linux-gnu \
    -c src/vkskeleton.c -o build/vkskeleton.o
ld -m elf_i386 -shared -o build/vkskeleton.so build/vkskeleton.o \
    /usr/lib/i386-linux-gnu/libc.so.6 \
    /usr/lib/i386-linux-gnu/libm.so.6 \
    /usr/lib/i386-linux-gnu/libX11.so.6
echo "built: $(pwd)/build/vkskeleton.so"
