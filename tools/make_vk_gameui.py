#!/usr/bin/env python3
"""Write a copy of Sven Co-op's gameui_english.txt in which the Options ->
Video "Renderer" dropdown entry (#GameUI_OpenGL) reads "Vulkan".

usage: make_vk_gameui.py <stock gameui_english.txt> <output path>

Keeps the original encoding (UTF-16LE with BOM) and CRLF line endings and
changes nothing else.  Exit status 1 (nothing written) unless exactly one
GameUI_OpenGL entry is found.
"""
import re
import sys
from pathlib import Path


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    raw = src.read_bytes()
    if raw[:2] == b"\xff\xfe":
        enc, bom = "utf-16-le", b"\xff\xfe"
    elif raw[:2] == b"\xfe\xff":
        enc, bom = "utf-16-be", b"\xfe\xff"
    else:
        enc, bom = "utf-8", b""
    text = raw[len(bom):].decode(enc)
    new, n = re.subn(r'^("GameUI_OpenGL"\s+)"OpenGL"', r'\1"Vulkan"', text,
                     flags=re.M)
    if n != 1:
        return 1
    dst.write_bytes(bom + new.encode(enc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
