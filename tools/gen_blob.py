#!/usr/bin/env python3
"""Turn a binary into a C source file exposing it as a byte array.

Used to embed the PS5 installer helper in the payload, the same way the web
assets are embedded, so the daemon can hand the helper to the loader at runtime.
"""

import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: gen_blob.py <binary> <symbol> <output.c>", file=sys.stderr)
        return 2

    source = Path(sys.argv[1])
    symbol = sys.argv[2]
    output = Path(sys.argv[3])

    data = source.read_bytes()
    output.parent.mkdir(parents=True, exist_ok=True)

    with output.open("w") as out:
        out.write("/* Generated from %s - do not edit. */\n" % source.name)
        out.write("#include <stddef.h>\n\n")
        out.write("const unsigned char %s[] = {\n" % symbol)
        for offset in range(0, len(data), 16):
            chunk = data[offset:offset + 16]
            out.write("    " + " ".join("0x%02x," % b for b in chunk) + "\n")
        out.write("};\n\n")
        out.write("const size_t %s_size = %d;\n" % (symbol, len(data)))

    print("  [GEN]  %s (%d bytes)" % (output, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
