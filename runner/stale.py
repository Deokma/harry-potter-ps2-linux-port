#!/usr/bin/env python3
"""Generated-code batches, for runner/build.sh: python3 -I runner/stale.py LIST_DIR...

Each list_*.cpp includes generated files. A batch whose file has changed loses its object, so it recompiles, and a file that's gone leaves its
list. So a small change to a generator rebuilds a few batches rather than all of them (with gen.sh, which keeps unchanged files' times).
"""
import os
import re
import sys
from pathlib import Path

include = re.compile(r'#include "(.*)"')
for folder in sys.argv[1:]:
    rebuilt = 0
    for lst in sorted(Path(folder).glob("list_*.cpp")):
        # a batch whose files are all gone is left holding one empty line
        lines = [l for l in lst.read_text().splitlines() if l.strip()]
        kept = [l for l in lines if os.path.exists(include.match(l).group(1))]
        obj = lst.with_suffix(".o")
        stale = len(kept) != len(lines)
        if stale:
            lst.write_text("\n".join(kept) + "\n")
        if not stale and obj.exists():
            built = obj.stat().st_mtime
            stale = any(os.stat(include.match(l).group(1)).st_mtime > built for l in kept)
        if stale and obj.exists():
            obj.unlink()
            rebuilt += 1
    if rebuilt:
        print(f"{folder}: {rebuilt} batches out of date")
