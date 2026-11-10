#!/usr/bin/env python3
"""Copy ABOUT.txt into the build output, next to the built .fap.

The About screen reads its text from /ext/pocsag/ABOUT.txt at runtime
rather than carrying it in .rodata, because a FAP is loaded into RAM
whole: a string literal for a screen opened once still costs RAM for
the whole session. Publishing the file beside the .fap keeps the two in
step, so whatever is installed matches the build it came from.

Run by fbt through fap_extbuild in application.fam; also usable by hand:

    python tools/export_about.py <source ABOUT.txt> <destination>
"""

from __future__ import annotations

import os
import shutil
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {os.path.basename(sys.argv[0])} <source> <destination>", file=sys.stderr)
        return 2

    source, destination = sys.argv[1], sys.argv[2]
    if not os.path.isfile(source):
        print(f"{source}: not found", file=sys.stderr)
        return 1

    os.makedirs(os.path.dirname(os.path.abspath(destination)), exist_ok=True)
    shutil.copyfile(source, destination)
    return 0


if __name__ == "__main__":
    sys.exit(main())
